// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/AudioRing.h"

namespace scvb
{

void AudioRing::bind(AudioRingHeader* header, float* data)
{
    if (header == nullptr || data == nullptr)
    {
        binding_.store(nullptr, std::memory_order_release);
        return;
    }

    // 构造不可变绑定:magic/abi 校验 + 几何快照(bind 时读一次,此后只读快照,绝不回读段头)。
    auto b = std::make_unique<AudioRingBinding>();
    b->header = header;
    b->data = data;
    const bool magicOk = header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 sr = header->sample_rate;
    const u32 frames = header->ring_frames;
    const u32 channels = header->channels;
    const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
    const bool geoOk = framesPow2 && (channels == 1 || channels == 2);
    b->geo = AudioRingGeometry{sr, frames, channels};
    b->bound = magicOk && abiOk && geoOk;

    binding_.store(b.get(), std::memory_order_release);
    owned_.push_back(std::move(b));
}

void AudioRing::write(const AudioRingBinding* b, int64_t t0, const float* interleaved, int n) noexcept
{
    if (b == nullptr || !b->bound || n <= 0)
    {
        return;
    }
    // [B 线 M04b] 写本笔样本之前的 release fence:本线程更早发布的写头(上一笔 write() 末尾那条 release store,
    // 或 publishWriteHead)先于本笔样本可见。那条 release store 只约束它**之前**的访问;下一次 write() 的样本写在
    // 它之后,arm64 上可以先于它被读方看见 —— 读方读到的写头还停在 h(k−1) 时,第 k 笔、第 k+1 笔乃至更后面的样本
    // 都可能已经落进环里,「写头之后至多一笔在写」就不成立了。读方(A 线 ShmRingMixSource)认当前窗的下沿
    // `w + inflightGuard() − 环长`、拷完复核 `w2 + guard ≤ 下沿 + 环长`、上一代窗别名判据里新一代跨度的上沿
    // `写头 + guard`,都以这一条为前提(guard 不小于一笔的长度)。有这道 fence,读方只要拷到第 k+1 笔的任何一个
    // 样本,拷完之后的 acquire 栅栏 + 复核就必然读到写头 ≥ h(k),复核接得住。
    // A 线 A-16 预拷贝拷完复核里「拷贝期间写方前进不足半环」那一条(其头注 DEP-2)在 arm64 上同样靠它:拷到的
    // 样本是哪一笔写下的,复核读到的写头就不低于它前一笔发布的写头,写方真跑远了复核看得出来。
    // 放在开头而不是上一笔末尾:本线程先 publishWriteHead / bumpEpoch、再 write() 时同样成立,不必每条发布路径
    // 各补一道。与 bumpEpoch 那道的分工:那道管「epoch+1 先于新一代样本」(跨代);这道管「上一笔写头先于本笔样本」
    // (同一代里相邻两笔,含 [SL-523] 一块按段连写的几笔)。bumpEpoch 之后再 write() 时两道都会执行。
    // x86:release fence 只是编译器屏障,不生成指令(TSO 下写写本来不重排,x86 上原本就成立)。
    // arm64:每次 write() 一条 DMB ISH。这里只需要写写顺序(DMB ISHST 就够),但 C++ 没有对应的可移植写法;
    // write() 每块一到几次,开销可忽略。样本是普通 float(段布局冻结),与 PlayheadShot / MeterShot 的 seqlock
    // 写侧同一写法。判据:tests/core/test_audio_ring_stress.cpp。
    std::atomic_thread_fence(std::memory_order_release);
    const u32 mask = b->geo.ringFrames - 1;
    const u32 ch = b->geo.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c = 0; c < ch; ++c)
        {
            b->data[static_cast<std::size_t>(frame) * ch + c] = interleaved[static_cast<std::size_t>(i) * ch + c];
        }
    }
    b->header->write_head_samples.store(static_cast<u64>(t0 + n), std::memory_order_release);
}

void AudioRing::bumpEpoch(const AudioRingBinding* b) noexcept
{
    if (b != nullptr && b->bound)
    {
        b->header->epoch.fetch_add(1, std::memory_order_release);
        // [B 线 M04] 换代之后、写新一代数据之前的 release fence。调用方(InputProcessor 音频线程)
        // 紧接着就在同一线程里 write() 新一代样本;release RMW 只约束它**之前**的访问,挡不住这些
        // 样本写被提到 epoch+1 之前可见 —— arm64 上读方可能前后两次都读到旧 epoch(e1 == e2),
        // 样本却已是新一代,把跨代数据当本代收下。fence 让「先换代、后写数据」在写侧成立;读侧在
        // 样本循环之后、e2 之前的 acquire fence 归 A 线(ShmRingMixSource,A-5),两侧配齐才完整。
        // x86:release fence 只是编译器屏障,不生成指令(lock xadd 本身已是全屏障,x86 上原本就不撕裂)。
        // arm64:每次换代一条 DMB ISH;换代只在定位 / 循环回跳时发生,不在逐块路径上。
        // [B 线 M04b] A 线 A-16 读方预拷贝的跨代安全也依赖这道 fence(其头注 DEP-1)。
        // 同一代里相邻两笔之间的那道在 write() 开头。今天 bumpEpoch 之后本线程写新一代样本走的都是 write(),
        // 那道也会把 epoch+1 排在新样本之前;这一道仍保留,让 bumpEpoch 不依赖调用方接着走哪条写路径。
        std::atomic_thread_fence(std::memory_order_release);
    }
}

void AudioRing::publishWriteHead(const AudioRingBinding* b, int64_t pos) noexcept
{
    // 只发布写头、不写样本,这里不另加 fence:它之后本线程再 write() 时,由 write() 开头那道([B 线 M04b])
    // 把这次发布排在新样本之前。
    if (b != nullptr && b->bound)
    {
        b->header->write_head_samples.store(static_cast<u64>(pos), std::memory_order_release);
    }
}

} // namespace scvb
