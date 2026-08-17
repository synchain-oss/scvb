// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioRing.h"

#include <cstdlib>

#include "ScvbJournal.h"

namespace scvb
{

u32 ringFramesFromEnv()
{
    const char* v = std::getenv("SCVB_RING_FRAMES");
    if (v == nullptr || *v == '\0')
    {
        return kDefaultRingFrames;
    }
    const unsigned long parsed = std::strtoul(v, nullptr, 10);
    if (parsed == 0 || parsed > 0x80000000u)
    {
        return kDefaultRingFrames;
    }
    const u32 value = static_cast<u32>(parsed);
    // 必须是 2^k(寻址靠 mask = ring_frames-1)。
    if ((value & (value - 1)) != 0)
    {
        return kDefaultRingFrames;
    }
    return value;
}

AudioRing::~AudioRing()
{
    // 几何快照与视图不在此释放:环对象由 LeakPool 进程寿命保活(v4),
    // 几何对象与其视图同寿命(泄漏策略);见 LeakPool.h 与 SegmentView::reset 注释。
}

void AudioRing::invalidate()
{
    // v5:发布空快照 → isOpen()==false;旧快照交还保活池(在途写读仍安全)。
    AudioRingGeometry* old = geom_.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr)
    {
        retiredGeom_.push_back(old);
    }
}

AudioRingHeader* AudioRing::header()
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    return g != nullptr ? g->header : nullptr;
}

float* AudioRing::data()
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    return g != nullptr ? g->data : nullptr;
}

u32 AudioRing::ringFrames() const
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    return g != nullptr ? g->frames : 0;
}

u32 AudioRing::channels() const
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    return g != nullptr ? g->channels : 0;
}

u32 AudioRing::mask() const
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    return (g != nullptr && g->frames > 0) ? g->frames - 1 : 0;
}

InitResult AudioRing::createForInput(u32 sampleRate, u32 ringFrames, u32 channels)
{
    if (channels != 1 && channels != 2)
    {
        invalidate();
        return InitResult::kFailed;
    }
    // v6:段容量恒按 stereo 预算(ringFrames × 2 声道)创建。命名段无法在存活视图下扩容,
    // 而宿主编排(freeze/RIP/导出)会反复 mono⇄stereo 重建;一次按最大容量建段,
    // 之后声道数切换永不需要扩容,从根上消灭「小视图 + 大几何」越界(v1..v5 根因)。
    const std::size_t capacityBytes =
        sizeof(AudioRingHeader) + static_cast<std::size_t>(ringFrames) * 2 * sizeof(float);

    // v5:全新几何快照(自有视图),完全就绪后经 geom_ 单点发布;
    // 旧快照交还 retiredGeom_ 保活——写/读线程拿到的永远是一套自洽的
    // {view, data, frames, channels},不再有「新视图 + 旧几何」的越界窗口。
    auto* g = new AudioRingGeometry();
    const auto r = SegmentBackendWin32::map(segmentAudioName(group_, channel_), capacityBytes, g->view);
    if (r != InitResult::kOk)
    {
        delete g; // 视图按泄漏策略不解映射,对象本身可安全释放
        invalidate();
        return r;
    }

    // v6:attach 到旧版残留的 mono 尺寸段时按实际容量降级声道数——几何永远 ≤ 实际映射。
    const std::size_t actualBytes = g->view.size; // map() 已回填真实段大小
    const std::size_t neededBytes =
        sizeof(AudioRingHeader) + static_cast<std::size_t>(ringFrames) * channels * sizeof(float);
    u32 effChannels = channels;
    if (actualBytes < neededBytes)
    {
        const std::size_t availFloats =
            actualBytes > sizeof(AudioRingHeader) ? (actualBytes - sizeof(AudioRingHeader)) / sizeof(float) : 0;
        const std::size_t availFrames = availFloats / ringFrames;
        effChannels = availFrames >= 2 ? 2u : (availFrames >= 1 ? 1u : 0u);
        if (effChannels == 0)
        {
            delete g;
            invalidate();
            return InitResult::kFailed; // 旧段连 mono 都放不下(如残留测试用小环)→ 安全失败
        }
        journal::log("core", "ringClamped",
                     journal::join({journal::kv("ch", channel_),
                                    journal::kv("requested", static_cast<unsigned long long>(channels)),
                                    journal::kv("effective", static_cast<unsigned long long>(effChannels)),
                                    journal::kv("viewBytes", static_cast<unsigned long long>(actualBytes))}));
    }
    auto* header = static_cast<AudioRingHeader*>(g->view.base);
    const bool created = g->view.created;

    // 几何字段写回(magic 发布前落盘):sample_rate/ring_frames/channels/写头/epoch。
    const auto writeGeometry = [&]() {
        header->sample_rate = sampleRate;
        header->ring_frames = ringFrames;
        header->channels = effChannels;
        header->write_head_samples.store(0, std::memory_order_relaxed);
        header->epoch.store(0, std::memory_order_relaxed);
    };

    // magic 后行不变式:几何字段先写、magic 最后 release 发布(created 与覆盖式重初始化两路径都经 initData)。
    const auto ir = SegmentBackendWin32::initHeader(g->view, &header->magic, &header->abi, nullptr,
                                                    sizeof(AudioRingHeader), [&]() { writeGeometry(); });
    if (ir != InitResult::kOk)
    {
        delete g;
        invalidate();
        return ir;
    }

    if (!created)
    {
        // attach 到已有有效段(自身重初始化 / 接管残段):几何字段重写后 epoch+1 作为新代发布标记(读方弃旧代)。
        writeGeometry();
        header->epoch.fetch_add(1, std::memory_order_release);
    }

    g->header = header;
    g->data = reinterpret_cast<float*>(static_cast<char*>(g->view.base) + sizeof(AudioRingHeader));
    g->frames = ringFrames;
    g->channels = effChannels;

    journal::log(
        "core", "ringCreate",
        journal::join({journal::kv("ch", channel_), journal::kv("frames", static_cast<unsigned long long>(ringFrames)),
                       journal::kv("chans", static_cast<unsigned long long>(effChannels)),
                       journal::kv("viewBytes", static_cast<unsigned long long>(actualBytes)),
                       journal::kv("created", static_cast<unsigned long long>(created ? 1 : 0))}));

    AudioRingGeometry* old = geom_.exchange(g, std::memory_order_acq_rel);
    if (old != nullptr)
    {
        retiredGeom_.push_back(old);
    }
    lastEpoch_ = -1;
    validFrom_ = 0;
    return InitResult::kOk;
}

InitResult AudioRing::openForOutput()
{
    // v5:同 createForInput——全新快照完全就绪后单点发布;失败先 invalidate()。
    auto* g = new AudioRingGeometry();
    const auto r = SegmentBackendWin32::openExisting(segmentAudioName(group_, channel_), g->view);
    if (r != InitResult::kOk)
    {
        delete g;
        invalidate();
        return r;
    }
    auto* header = static_cast<AudioRingHeader*>(g->view.base);
    // 非阻塞单次校验:magic 未就绪(创建者尚未发布)即返回失败,由 [M] 25Hz 下一 tick 重试,
    // 不在消息线程里 Sleep 自旋(避免串行叠加拖慢 UI/心跳,review 建议 4)。
    if (header->magic.load(std::memory_order_acquire) != kScvbMagic)
    {
        delete g;
        invalidate();
        return InitResult::kFailed;
    }
    if (header->abi.load(std::memory_order_acquire) != kScvbAbi)
    {
        delete g;
        invalidate();
        return InitResult::kAbiMismatch;
    }
    // v6:几何容量校验——旧版 Input 可能留下「小段 + 大几何」的坏头,拒绝而非越界读
    // (拒绝后 pollChannels 下一 tick 重试;Input 侧 clamp 后头字段会回到合法值)。
    const u32 rf = header->ring_frames;
    const u32 nch = header->channels;
    const std::size_t actualBytes = g->view.size;
    const bool sane = rf >= 2 && (rf & (rf - 1)) == 0 && (nch == 1 || nch == 2) &&
                      actualBytes >= sizeof(AudioRingHeader) + static_cast<std::size_t>(rf) * nch * sizeof(float);
    if (!sane)
    {
        const u64 now = ::GetTickCount64();
        if (now - lastRejectLogMs_ >= 2000) // 25Hz 重试节流,避免刷日志
        {
            lastRejectLogMs_ = now;
            journal::log(
                "core", "ringReject",
                journal::join({journal::kv("ch", channel_), journal::kv("frames", static_cast<unsigned long long>(rf)),
                               journal::kv("chans", static_cast<unsigned long long>(nch)),
                               journal::kv("viewBytes", static_cast<unsigned long long>(actualBytes))}));
        }
        delete g;
        invalidate();
        return InitResult::kFailed;
    }
    g->header = header;
    g->data = reinterpret_cast<float*>(static_cast<char*>(g->view.base) + sizeof(AudioRingHeader));
    g->frames = header->ring_frames;
    g->channels = header->channels;
    journal::log(
        "core", "ringOpen",
        journal::join({journal::kv("ch", channel_), journal::kv("frames", static_cast<unsigned long long>(g->frames)),
                       journal::kv("chans", static_cast<unsigned long long>(g->channels)),
                       journal::kv("viewBytes", static_cast<unsigned long long>(g->view.size))}));
    AudioRingGeometry* old = geom_.exchange(g, std::memory_order_acq_rel);
    if (old != nullptr)
    {
        retiredGeom_.push_back(old);
    }
    lastEpoch_ = -1;
    validFrom_ = 0;
    return InitResult::kOk;
}

void AudioRing::writeBlock(i64 t0, const float* interleavedSrc, int frames)
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    if (g == nullptr || t0 < 0 || frames <= 0 || interleavedSrc == nullptr)
    {
        return;
    }
    const u32 nch = g->channels;
    const u32 m = g->frames - 1;
    for (int i = 0; i < frames; ++i)
    {
        const std::size_t base = (static_cast<std::size_t>(t0 + i) & m) * nch;
        for (u32 c = 0; c < nch; ++c)
        {
            g->data[base + c] = interleavedSrc[static_cast<std::size_t>(i) * nch + c];
        }
    }
    // 数据先写,写头后发布(01 §5.1 步骤 4)。
    g->header->write_head_samples.store(static_cast<u64>(t0 + frames), std::memory_order_release);
}

void AudioRing::bumpEpoch()
{
    AudioRingGeometry* g = geom_.load(std::memory_order_relaxed);
    if (g != nullptr)
    {
        g->header->epoch.fetch_add(1, std::memory_order_release);
    }
}

AudioRing::ReadStatus AudioRing::readBlock(i64 t0, int frames, float* interleavedDst)
{
    AudioRingGeometry* g = geom_.load(std::memory_order_acquire);
    if (g == nullptr || t0 < 0 || frames <= 0 || interleavedDst == nullptr)
    {
        return ReadStatus::kGap;
    }
    const u32 nch = g->channels;
    const u32 m = g->frames - 1;

    const u64 e1 = g->header->epoch.load(std::memory_order_acquire);
    const u64 w = g->header->write_head_samples.load(std::memory_order_acquire);
    if (e1 != static_cast<u64>(lastEpoch_))
    {
        lastEpoch_ = static_cast<i64>(e1);
        validFrom_ = t0; // 换代后本代有效数据从当前块起点起算(01 §5.2)
    }

    const u64 t0u = static_cast<u64>(t0);
    const bool covered =
        (t0 >= validFrom_) && (w >= t0u + static_cast<u64>(frames)) && (w - t0u <= static_cast<u64>(g->frames));
    if (!covered)
    {
        return ReadStatus::kGap;
    }

    for (int i = 0; i < frames; ++i)
    {
        const std::size_t base = (static_cast<std::size_t>(t0 + i) & m) * nch;
        for (u32 c = 0; c < nch; ++c)
        {
            interleavedDst[static_cast<std::size_t>(i) * nch + c] = g->data[base + c];
        }
    }

    // 读后 write_head 复查(R1):读中被写方套圈 → 弃块 + gapCount(调用方)+1。
    const u64 e2 = g->header->epoch.load(std::memory_order_acquire);
    const u64 w2 = g->header->write_head_samples.load(std::memory_order_acquire);
    if (e2 != e1 || w2 > t0u + static_cast<u64>(g->frames))
    {
        return ReadStatus::kGap;
    }
    return ReadStatus::kOk;
}

} // namespace scvb