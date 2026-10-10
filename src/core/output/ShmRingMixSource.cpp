// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/ShmRingMixSource.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace scvb::output
{

namespace
{

// 写头与时间线位置一律按 int64 运算。写头是 u64,真实取值远小于 2^62;夹到 int64 上限的 1/4,
// 后面的加减(写头 + 在途保护、位置 + 环长)不会溢出。
constexpr u64 kMaxPos = static_cast<u64>(std::numeric_limits<int64_t>::max() / 4);

int64_t asPos(u64 v) noexcept
{
    return static_cast<int64_t>(v < kMaxPos ? v : kMaxPos);
}

int64_t floorMod(int64_t a, int64_t m) noexcept
{
    const int64_t r = a % m;
    return r < 0 ? r + m : r;
}

// read() 每块 volatile 读一次段头 channels(只比对、不寻址)。它是 plain u32(布局冻结,不能改成 atomic),
// 靠「对齐的 32 位读写在 x86 / ARMv8 上是单次访问原子的」。把这个前提钉在编译期(偏移 16 本身由
// SegmentLayout.h 的 static_assert 钉住)。
static_assert(sizeof(AudioRingHeader::channels) == 4, "header channels must be a 32-bit field");
static_assert(offsetof(AudioRingHeader, channels) % alignof(u32) == 0, "header channels must be 4-byte aligned");
static_assert(alignof(AudioRingHeader) >= alignof(u32), "AudioRingHeader alignment must cover u32");

} // namespace

void ShmRingMixSource::bind(AudioRingHeader* header, float* data) noexcept
{
    if (header == nullptr || data == nullptr)
    {
        binding_.store(nullptr, std::memory_order_release);
        return;
    }

    // 构造不可变绑定:magic/abi 校验 + 几何快照(bind 时读一次,此后寻址只用快照,绝不按段头几何寻址;
    // [A-5] read() 每块只按值比对段头 channels,见那里)。
    auto b = std::make_unique<AudioRingBinding>();
    b->header = header;
    b->data = data;
    const bool magicOk = header->magic.load(std::memory_order_acquire) == kScvbMagic;
    const bool abiOk = header->abi.load(std::memory_order_acquire) == kScvbAbi;
    const u32 sr = header->sample_rate;
    const u32 frames = header->ring_frames;
    const u32 channels = header->channels;
    const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
    b->geo = AudioRingGeometry{sr, frames, channels};
    b->bound = magicOk && abiOk && framesPow2 && (channels == 1 || channels == 2);

    binding_.store(b.get(), std::memory_order_release);
    owned_.push_back(std::move(b));
}

void ShmRingMixSource::unbind() noexcept
{
    // 发布 nullptr(防悬垂);不触碰音频线程独占的代际状态
    // (由 read() 经绑定指针变化自行重置,避免 [M]/[A] 数据竞争)。
    binding_.store(nullptr, std::memory_order_release);
}

bool ShmRingMixSource::bound() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr && b->bound;
}

u32 ShmRingMixSource::channels() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.channels : 0;
}

u32 ShmRingMixSource::sampleRate() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.sampleRate : 0;
}

u32 ShmRingMixSource::ringFrames() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return b != nullptr ? b->geo.ringFrames : 0;
}

u64 ShmRingMixSource::writeHead() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return (b != nullptr && b->bound) ? b->header->write_head_samples.load(std::memory_order_acquire) : 0;
}

u64 ShmRingMixSource::epoch() const noexcept
{
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    return (b != nullptr && b->bound) ? b->header->epoch.load(std::memory_order_acquire) : 0;
}

// ---------------------------------------------------------------------------
// 代际状态
// ---------------------------------------------------------------------------

int64_t ShmRingMixSource::leadHiRef() const noexcept
{
    if (leadPrev_.samples > 0)
    {
        return leadPrev_.hi;
    }
    return leadCur_.samples > 0 ? leadCur_.hi : -1;
}

int64_t ShmRingMixSource::spanBound(int64_t prevN, int64_t ring) const noexcept
{
    // 新一代在读方首次看到它之前最多已写了多少帧。读方逐块观测(相邻两次 read() 之间只隔一块):
    // 写方换代发生在上一次观测之后,而写方相对读方的提前量不超过实测上界 —— 所以新一代的跨度
    // ≤ 上界 + 上一块的块长。宿主定位时冲刷预取轨重渲染的那一口(A-4 LS-6/LS-7)也在这个界内:
    // 冲刷后预取轨重新渲染到「Output 游标 + 提前量」为止。没量到过提前量时退到一个环长(等于不设界,
    // 上一代窗因此用不上)。
    const int64_t lead = leadHiRef();
    return (lead >= 0 ? lead : ring) + prevN;
}

void ShmRingMixSource::clearWindows() noexcept
{
    curAnchored_ = false;
    curVf_ = 0;
    anchorOnUnconfirmed_ = false;
    prevOn_ = false;
    prevBLowKnown_ = false;
    refHiValid_ = false;
}

void ShmRingMixSource::onRebind() noexcept
{
    // 新绑定 = 新的不可变快照(几何可能变了):两个窗都作废,代际从头来。读方自身时间线与跨代测量
    // (提前量、写方步长)保留 —— 它们描述的是宿主的调度关系,不是这条环里的内容。
    if (leadCur_.samples > 0)
    {
        leadPrev_ = leadCur_;
    }
    if (stepMaxCur_ > 0)
    {
        stepMaxPrev_ = stepMaxCur_;
    }
    leadCur_ = LeadStat{};
    stepMaxCur_ = 0;
    clearWindows();
    genSeen_ = false;
    gen_ = 0;
    headConfirmed_ = false;
    headTentative_ = false;
    headZero_ = false;
    headMonotonic_ = true;
    firstSeen_ = false;
    lastW_ = 0;
    curHi_ = 0;
    jumpedInGen_ = false;
    jumpedAtGenStart_ = false;
    syncUsed_ = false;
    genBacklog_ = 0;
    contiguousInGen_ = true;
    framesSinceGen_ = 0;
    primed_ = false;
    sawFail_ = false;
    lastFailWriteHead_ = 0;
}

void ShmRingMixSource::confirmHead(int64_t w, int64_t prevN, int64_t ring) noexcept
{
    headConfirmed_ = true;
    headZero_ = false;
    curHi_ = std::max(curHi_, w);
    // 新一代起点的下界:首次观测时它最多写了 spanBound 帧,此后读方又走了 framesSinceGen_ 帧,
    // 写方在这期间写的也算进去(写方相对读方的提前量有上界)。中间漏读过块就界定不了 ⇒ 上一代窗作废。
    if (prevOn_ && !prevBLowKnown_)
    {
        if (contiguousInGen_)
        {
            prevBLowKnown_ = true;
            prevBLow_ = w - spanBound(prevN, ring) - framesSinceGen_;
        }
        else
        {
            prevOn_ = false;
        }
    }
}

void ShmRingMixSource::firstObservation(int64_t w, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept
{
    firstSeen_ = true;
    lastW_ = w;
    if (w == 0)
    {
        // 规则 5:写头为 0 = 几何改写 / 新段(先写头归零、再 epoch+1,InputSession.cpp)。旧数据按旧布局
        // 写的,两个窗都清空;写头此后一动就一定是本代写的。
        clearWindows();
        headZero_ = true;
        headConfirmed_ = true;
        return;
    }
    if (refHiValid_ && w < refHi_)
    {
        // 规则 3 保守规则前半:同一代里写头只增不减,写头低于上一代最后观测到的头 ⇒ 一定是换代之后写的。
        confirmHead(w, prevN, ring);
    }
    else if (refHiValid_ && !jumped && !skipped && w > refHi_ + stepMaxPrev_)
    {
        // 上一次观测就在一块之前,上一代在一块之内一般走不了这么远 ⇒ 多半是新一代的头。但 stepMaxPrev_
        // 只是观测到的最大步长、不是上界:新一代第一段发布之前(R1)看到的也可能是上一代在最后一次观测
        // 之后又多写了一大笔的旧头(R5)。所以只「暂定」:保守规则可以先按它锚,写头第一次动时复核
        // (sameGeneration,与锚在未确认写头上同一套);别名判的跨度照样按它估(certify)。
        headTentative_ = true;
        curHi_ = w;
    }
    else
    {
        // 规则 3 保守规则后半:可能是 bump 之后、写之前的旧头 —— 先不确认,等它在新一代里动一次。
        curHi_ = w;
    }
}

void ShmRingMixSource::newGeneration(u64 e, int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN,
                                     int64_t ring) noexcept
{
    const bool had = genSeen_;
    const u64 d = had ? e - gen_ : 0;

    // 旧代的测量收尾(跨代保留)。
    if (leadCur_.samples > 0)
    {
        leadPrev_ = leadCur_;
    }
    if (stepMaxCur_ > 0)
    {
        stepMaxPrev_ = stepMaxCur_;
    }
    leadCur_ = LeadStat{};
    stepMaxCur_ = 0;

    // 规则 3 首句:观测到换代时先把当前窗降为上一代窗。条件:正好隔一代、当前窗已锚定且非空、
    // 写头不是 0(几何改写的旧数据不能再读),外加这次观测与上一次之间读方没有跳、也没有漏读 ——
    // 否则新一代在首次观测之前写了多少无从界定,别名判据做不了。
    const bool demote = had && d == 1 && curAnchored_ && curHi_ > curVf_ && w > 0 && !jumped && !skipped;
    if (demote)
    {
        // 上一代在最后一次被观测之后最多又写了一个步长(没观测到的尾段),它会覆盖
        // [hi + 步长 − 环长) 以下那些位置的同槽 —— 窗的下沿让开这一段。
        prevLo_ = std::max(curVf_, curHi_ + stepMaxPrev_ - ring);
        prevHi_ = curHi_;
        prevOn_ = prevLo_ < prevHi_;
    }
    else
    {
        prevOn_ = false;
    }
    prevBLowKnown_ = false;
    refHiValid_ = had && d == 1 && headConfirmed_ && headMonotonic_ && curHi_ > 0;
    refHi_ = curHi_;
    headMonotonic_ = true;

    gen_ = e;
    genSeen_ = true;
    genBacklog_ = had ? d : 0;
    curAnchored_ = false;
    curVf_ = 0;
    anchorOnUnconfirmed_ = false;
    headConfirmed_ = false;
    headTentative_ = false;
    headZero_ = false;
    firstSeen_ = false;
    curHi_ = 0;
    lastW_ = 0;
    jumpedInGen_ = jumped;
    jumpedAtGenStart_ = jumped;
    syncUsed_ = false;
    contiguousInGen_ = true;
    framesSinceGen_ = 0;
    // 新一代要重新等写方追上,交接期同样不算失准(旧实现同款)。
    primed_ = false;
    sawFail_ = false;
    lastFailWriteHead_ = 0;

    if (flux)
    {
        return; // 规则 2:装载期间正在换代,写头归属不明 —— 本块只按上一代窗读、不确认、不锚定
    }
    firstObservation(w, jumped, skipped, prevN, ring);
}

void ShmRingMixSource::sameGeneration(int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN,
                                      int64_t ring) noexcept
{
    if (jumped)
    {
        jumpedInGen_ = true; // 推迟锚定的前提:读方在写方单独换代之后自己跳了
    }
    if (skipped)
    {
        contiguousInGen_ = false;
    }
    if (flux)
    {
        return;
    }
    if (!firstSeen_)
    {
        firstObservation(w, jumped, skipped, prevN, ring); // 本代首次观测时写头归属不明,这是第一次明确的观测
        return;
    }
    if (w == 0)
    {
        if (lastW_ != 0)
        {
            // 规则 5:同一代里写头归 0 = 几何改写进行中(写头已归零、epoch 还没 +1)。清空所有窗。
            clearWindows();
            headZero_ = true;
            headConfirmed_ = true;
            headTentative_ = false;
            curHi_ = 0;
        }
        lastW_ = 0;
        return;
    }
    if (w == lastW_)
    {
        return;
    }
    if (headZero_ || !headConfirmed_)
    {
        // 换代(或归零)之后写头第一次动:此后的写头一定是本代写的。暂定的头(R5)也在这里复核。
        if (curAnchored_ && anchorOnUnconfirmed_ && (w < lastW_ || w - lastW_ > spanBound(prevN, ring)))
        {
            // 锚是对着一个可能属于上一代的写头下的,写头这一动(往回、或一下子远远超过提前量上界)
            // 与「那就是本代的头」对不上 ⇒ 这个锚不可信,按规则重来。
            curAnchored_ = false;
            curHi_ = 0;
        }
        anchorOnUnconfirmed_ = false;
        // 确认之前记下的写头(firstObservation 的 curHi_ = w)可能是上一代的旧头,不能算进本代的最大写头。
        curHi_ = 0;
        headTentative_ = false;
        confirmHead(w, prevN, ring);
    }
    else if (w < lastW_)
    {
        // 规则 5「其余回退」:同一代里写头往回走(不是 0)。写方协议里只有一个来源 —— 停走带静止重写:
        // 走带停着、宿主给的 t0 与上一块相同时写方不换代,照样写 [t0, t0+n) 并把写头发布到 t0+n
        // (InputProcessor 的跳变检测),这一块比上一块短,写头就往回退(A-3 同步族实测到)。写过的位置
        // 仍是这些位置的数据(时间线寻址),当前窗与本代最大写头 curHi_ 照旧,与旧实现逐块同读(I4)。
        // 但这一代的写头从此不再单调:换代后「新写头低于上一代最后观测到的头 ⇒ 一定是新一代写的」
        // (规则 3 保守规则前半)的前提没了 —— 上一代的旧头可能又退到了更低处。本代结束时不交出 refHi。
        headMonotonic_ = false;
    }
    else
    {
        const int64_t step = w - lastW_;
        stepMaxCur_ = std::max(stepMaxCur_, step);
        if (prevOn_ && step > spanBound(prevN, ring))
        {
            prevOn_ = false; // 规则 6 末句:写方单块产出超过上界 ⇒ 本代禁用上一代窗
        }
        curHi_ = std::max(curHi_, w);
    }
    lastW_ = w;
}

void ShmRingMixSource::tryAnchor(int64_t t0, int64_t w, int n, int64_t slack, int64_t ring) noexcept
{
    // 「写头相对读方的位置不超过实测提前量上界」:读方跳到的位置就是写方本代的起点时,写头只能领先
    // 它一个提前量;领先得更多,说明写方本代起点在读方前面(读写目标不一致)或者这个头根本不是本代的。
    // 没量到过提前量(刚绑定)时退到旧实现的套圈界(一个环长)。slack = 本段之后这一宿主块还剩的长度
    // (超长块分段读,SL-523):写方已经写完整个宿主块,写头相对本段自然多领先这一截。
    const int64_t lim = leadHiRef();
    const bool near = t0 <= w && (w - t0) <= (lim >= 0 ? lim : ring) + slack;
    // 锁步 = 上一代里写头从不领先读方超过一块(写方一次产出或读方一块,取大者),且曾恰好停在读方块尾。
    // 两条都要:写方领先时停调,读方追上冻住的写头那几块里「停在块尾」也会成立(那是追上,不是锁步)。
    // 锁步写方每次只比读方多写一块,「写头不超过上界」对它是精确的:写头领先读方 ≤ 上界 ⇔ 本代起点 ≤ t0。
    const bool lockstep = leadPrev_.samples > 0 && leadPrev_.lo <= 0 &&
                          leadPrev_.hi <= std::max<int64_t>(stepMaxPrev_, static_cast<int64_t>(n));

    // 规则 3 同步规则(读方在首次看到本代的那一块也跳了)与规则 4 推迟锚定(写方单独换代之后,读方
    // 第一次自己跳变):锚在读方自己的跳变落点之后的当前位置。每代只用一次 —— 读方之后的跳变与写方
    // 本代起点无关。积压了 ≥2 代时(中间有没见过的代),非锁步的写方先等写头确认是本代的再判(R3)。
    const bool syncRule = jumpedAtGenStart_;
    const bool deferredRule = jumpedInGen_ && !jumpedAtGenStart_;
    if ((syncRule || deferredRule) && !syncUsed_ && near && (genBacklog_ < 2 || headConfirmed_ || lockstep))
    {
        if (!curAnchored_)
        {
            anchorOnUnconfirmed_ = !headConfirmed_;
        }
        curVf_ = curAnchored_ ? std::min(curVf_, t0) : t0;
        curAnchored_ = true;
        syncUsed_ = true;
        return;
    }
    if (curAnchored_)
    {
        return;
    }
    // 锁步写方单独换代(读方没跳:写方恢复调用 / 重新 prepare / 新实例接手):锁步的写方与读方处理同一个
    // 宿主块,它的新一代就从读方当前所在的块开始(I4,旧实现在同步宿主上就是这样读的)。「锁步」=
    // 上一代里写头从不领先读方超过一块、且曾恰好停在读方块尾(见上面 lockstep);写方领先(预取)时不走这条 —— 那正是 H4。
    if (!jumpedInGen_ && lockstep && near)
    {
        curVf_ = t0;
        curAnchored_ = true;
        anchorOnUnconfirmed_ = !headConfirmed_;
        return;
    }
    // 规则 3 保守规则:写头已确认是本代写的,就锚在它上面(它之下的本代数据从哪开始无从得知,
    // 能接住的由上一代窗接)。只是暂定的头(R5)也先锚,但记成「锚在未确认写头上」,写头第一次动时复核。
    if ((headConfirmed_ || headTentative_) && !headZero_)
    {
        curVf_ = w;
        curAnchored_ = true;
        anchorOnUnconfirmed_ = !headConfirmed_;
    }
}

// ---------------------------------------------------------------------------
// 认证:一块要读的每一帧都在当前窗里,或在上一代窗里且无别名
// ---------------------------------------------------------------------------

bool ShmRingMixSource::prevAliasFree(int64_t a0, int64_t a1, int64_t bLow, int64_t wN, int64_t ring) const noexcept
{
    // 新一代可能写过的位置 ⊆ [bLow, wN)(wN = 写头 + 在途保护)。上一代窗里的一帧 x 只在「新一代写过某个
    // 与 x 同槽、却不是 x 的位置」时才会被换掉:[bLow, wN) 里与 x 同余(mod 环长)的点只有一个,
    // 它不是 x 本身 ⇔ x 落在 [bLow, wN) 之外、且 (x − bLow) mod 环长 < wN − bLow。
    // x 落在 [bLow, wN) 本身时,那一格就算被新一代写过,写的也是 x 自己(时间线寻址),照读。
    // 这就是设计稿规则 6 的「p−w < R − (2·上界 + 余量) 或 p−w ≥ R」的精确形式(两支分别对应
    // 余数落在 wN − bLow 之上的前后两段)。
    const int64_t lw = wN - bLow;
    if (lw >= ring)
    {
        return false; // 新一代可能已经写满一整圈:上一代窗里没有哪一帧还保得住
    }
    const auto clear = [lw, ring, bLow](int64_t p0, int64_t p1) {
        if (p1 <= p0)
        {
            return true;
        }
        const int64_t r = floorMod(p0 - bLow, ring);
        return r >= lw && r + (p1 - p0) <= ring;
    };
    const int64_t m0 = std::max(a0, bLow);
    const int64_t m1 = std::min(a1, wN);
    if (m1 > m0)
    {
        return clear(a0, m0) && clear(m1, a1);
    }
    return clear(a0, a1);
}

ShmRingMixSource::Cover ShmRingMixSource::certify(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t prevN,
                                                  int64_t ring) const noexcept
{
    Cover c;
    // 当前窗 [vf, w),且本代在途的写(写头之后最多一个写方步长)不得套圈到窗里:位置 ≥ w + 保护 − 环长。
    // 旧实现的 `w − t0 ≤ 环长` 没算在途那一段;多了这道保护,写方领先接近一个环长时也不会读到被写了一半的槽。
    int64_t rest0 = t0;
    int64_t rest1 = t1;
    int64_t restB0 = 0;
    int64_t restB1 = 0;
    if (curAnchored_)
    {
        const int64_t cLo = std::max(curVf_, w + guard - ring);
        const int64_t i0 = std::max(t0, cLo);
        const int64_t i1 = std::min(t1, w);
        if (i1 > i0)
        {
            c.curLo = i0;
            c.curLen = i1 - i0;
            rest1 = i0; // 当前窗下方那一段
            restB0 = i1; // 当前窗上方那一段
            restB1 = t1;
        }
    }
    // 新一代已发布过的最高写头:停走带静止重写会让写头退回,但退回之前写到过的位置照样换掉了同槽内容。
    const int64_t newHi = std::max(w, curHi_);
    // 新一代可能写过的跨度下界(别名判据用)。写头已确认:confirmHead 记下的界。还没确认(R1 / 暂定的头 R5):
    //   · 写头还停在上一代最后观测到的头(prevHi_)上:新一代一段都还没发布 —— 就算恰好发布到同一个值,它写的
    //     位置也都在这个头之下,与上一代窗里同位置同槽,换不掉别的位置 ⇒ 只剩在途那第一段落点不明(R1),不判;
    //   · 写头变了:可能是新一代发布了(也可能是上一代最后又多写了一笔),按「眼前这个写头就是新一代的」暂定
    //     估 —— 它若其实是上一代的旧头,这样估只会更保守;不估就会放过新一代已经换掉的槽。
    // 读方中间漏读过块时界定不了新一代写了多少,上一代窗不可用。
    const bool r1Trust = !prevBLowKnown_ && w == prevHi_;
    const bool bLowOk = prevBLowKnown_ || contiguousInGen_;
    const int64_t bLow = prevBLowKnown_ ? prevBLow_ : w - spanBound(prevN, ring) - framesSinceGen_;
    const auto prevCovers = [this, newHi, r1Trust, bLowOk, bLow, guard, ring](int64_t p0, int64_t p1) {
        if (p1 <= p0)
        {
            return true;
        }
        if (!prevOn_ || p0 < prevLo_ || p1 > prevHi_)
        {
            return false;
        }
        return r1Trust || (bLowOk && prevAliasFree(p0, p1, bLow, newHi + guard, ring));
    };
    if (!prevCovers(rest0, rest1) || !prevCovers(restB0, restB1))
    {
        return c;
    }
    c.prevA0 = rest0;
    c.prevA1 = rest1;
    c.prevB0 = restB0;
    c.prevB1 = restB1;
    c.ok = true;
    return c;
}

void ShmRingMixSource::countFailure(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t ring) noexcept
{
    // 旧实现的失准 / 饿读判别(P1-7),逐字保留:
    // 写方套圈(要读的数据已被覆盖)无条件算真失准。写头停滞 = 写方根本没在写(宿主在静音段挂起 Input /
    // bypass / 轨未激活),归 OutputSession 的 CH_SUSPENDED 管,不计失准。**本轮第一次失败不表态**:
    // 只有一个写头采样,判不出写方是在推进还是冻着,等下一块再比。本代还没在当前窗里读到过数据
    // (primed 之前)的失败一律不计 —— 空环 / 起播瞬间 / 宿主先渲染 Output / 换代交接中(ADR-002)。
    const u64 wu = static_cast<u64>(w);
    const bool lapped = w > t0 && (w - t0 > ring || w + guard - t0 > ring);
    const bool producerAdvancing = sawFail_ && wu != lastFailWriteHead_;
    lastFailWriteHead_ = wu;
    sawFail_ = true;
    if (primed_ && (lapped || producerAdvancing))
    {
        gapCount_.fetch_add(1, std::memory_order_relaxed); // 缺口→该轨该块静音+失准计数
    }
    else if (primed_)
    {
        // 写头停滞造成的失败读:不计失准,但如实留痕 —— CH_SUSPENDED 靠它区分
        // 「写方停了」与「走带停了」(后者读得到数据,一次都不会走到这儿)。
        stallFailCount_.fetch_add(1, std::memory_order_relaxed);
    }

    // [A-5] 失败成因分流(只计数)。
    if (!curAnchored_)
    {
        handoverLossCount_.fetch_add(1, std::memory_order_relaxed); // 本代还锚不上:交接中
    }
    else if (t0 >= curVf_)
    {
        if (t1 > w && !lapped && producerAdvancing)
        {
            unprimedFailCount_.fetch_add(1, std::memory_order_relaxed); // (a) 写方在推进,却还没写到块尾
        }
    }
    else if (curVf_ <= w && producerAdvancing)
    {
        stuckBelowAnchorCount_.fetch_add(1, std::memory_order_relaxed); // (b) 块在锚点之下,上一代窗也接不住
    }
    else
    {
        handoverLossCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ShmRingMixSource::read(int64_t t0, float* dst, int n, u64 readerEpoch, int64_t hostBlockEnd) noexcept
{
    // 硬约束:本方法 acquire 的绑定裸指针只在「本块」内有效 —— 底层共享内存段由 OutputSession 的
    // SegmentHandle 500ms 宽限期(kReleaseGraceMs > 单块 wall-clock)保活,不得跨块持有裸指针(S1 验收)。
    const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
    if (b == nullptr || !b->bound || dst == nullptr || n <= 0 || t0 < 0)
    {
        return false;
    }

    // 绑定指针变化(重绑 / 换环)→ 清窗、代际重来。换的是另一条环(attach / 改组)时与首次绑定同样
    // 处理(旧实现在这里把有效起点直接放在本块起点);同一条环重绑(几何改写)不当读方跳变。
    // 旧绑定由 owned_ 保活,比较 header 指针值是安全的(只比值,不解引用)。
    if (b != lastBinding_)
    {
        const bool otherRing = lastBinding_ == nullptr || lastBinding_->header != b->header;
        lastBinding_ = b;
        onRebind();
        if (otherRing)
        {
            // 另一条环 = 另一个写方:上一条环上量到的提前量与步长不属于它。
            podSeen_ = false;
            leadPrev_ = LeadStat{};
            stepMaxPrev_ = 0;
        }
    }

    const int64_t ring = static_cast<int64_t>(b->geo.ringFrames);
    const int64_t t1 = t0 + n;

    // 规则 1:读方自身跳变只看自己的时间线代号。代号没变而时间线不连续 = 中间有块没读(本轨暂时
    // 不在注入集),不是跳变;代号没变、t0 也没变 = 停走带静止重读。
    const bool jumped = !podSeen_ || readerEpoch != pod_;
    const bool reread = !jumped && t0 == lastT0_;
    const bool skipped = !jumped && !reread && t0 != end_;
    const int64_t prevN = lastN_ > 0 ? lastN_ : n;
    podSeen_ = true;
    pod_ = readerEpoch;
    lastT0_ = t0;
    end_ = t1;
    lastN_ = n;

    // 规则 2:装载顺序 epoch → 写头 → epoch。两次 epoch 不同 = 装载期间正在换代,写头属于哪一代不明。
    AudioRingHeader* const h = b->header;
    const u64 e1 = h->epoch.load(std::memory_order_acquire);
    const int64_t w = asPos(h->write_head_samples.load(std::memory_order_acquire));
    const u64 e1b = h->epoch.load(std::memory_order_acquire);

    // 段头 channels 与快照比对(geometry-25hz,统筹定案 (a))。几何改写的顺序是「写 channels → 写头归零 →
    // epoch+1 → 写方按新布局写」,而 [M] 换绑最多晚 40ms:这期间按旧快照的声道步长读新布局的数据就是撕裂。
    // 段头值**只比对、不寻址**,寻址仍只用快照(几何纪律)。channels 是 plain u32(布局冻结,不能改成
    // atomic):偏移 16、4 字节对齐(SegmentLayout.h static_assert),x86 与 ARMv8 上对齐的 32 位读写
    // 都是单次访问原子的,读到的只会是改写前或改写后的整值;volatile 防止编译器把这次读拆开或合并到别处。
    // 读到旧值:写方还没换代、还没按新布局写,本块读到的仍是旧布局数据,读后的 epoch 复核接住期间的换代;
    // 读到新值(或任何不等于快照的值):整块不读。
    const u32 hdrChannels = *static_cast<const volatile u32*>(&h->channels);
    if (hdrChannels != b->geo.channels)
    {
        handoverLossCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const bool flux = e1 != e1b;
    const u64 e = e1b;
    if (!genSeen_ || e != gen_)
    {
        newGeneration(e, w, flux, jumped, skipped, prevN, ring);
    }
    else
    {
        sameGeneration(w, flux, jumped, skipped, prevN, ring);
    }
    if (!flux && w > 0)
    {
        tryAnchor(t0, w, n, hostBlockEnd > t1 ? hostBlockEnd - t1 : 0, ring);
    }

    const int64_t guard = inflightGuard();
    const Cover c = certify(t0, t1, w, guard, prevN, ring);
    if (!c.ok)
    {
        countFailure(t0, t1, w, guard, ring);
        framesSinceGen_ += n;
        return false;
    }

    // 读样本(重叠语义:同一时间线位置后写覆盖先写 → 按地址读到的即「时间线正确者」)。
    // [J57] 按快照 channels(1|2)解码:stereo = interleaved LR(契约 v1.4)。
    const u32 mask = b->geo.ringFrames - 1;
    const u32 nch = b->geo.channels;
    for (int i = 0; i < n; ++i)
    {
        const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
        for (u32 c2 = 0; c2 < nch; ++c2)
        {
            dst[static_cast<std::size_t>(i) * nch + c2] = b->data[static_cast<std::size_t>(frame) * nch + c2];
        }
    }

    // 规则 7 撕裂复核:先 acquire 栅栏(让上面的数据读不会被挪到下面两次装载之后 —— 弱内存序平台
    // 上 seqlock 读侧的必要条件;x86 上它是空指令),再读 epoch 与写头。拷贝期间换了代,或本代在途的写
    // 套圈到了当前窗里那一段,整块弃用;上一代窗认领的段在新写头下重判一次别名。
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 e2 = h->epoch.load(std::memory_order_relaxed);
    const int64_t w2 = asPos(h->write_head_samples.load(std::memory_order_relaxed));
    bool ok = e2 == e;
    if (ok && c.curLen > 0)
    {
        ok = w2 + guard <= c.curLo + ring;
    }
    const bool prevUsed = c.prevA1 > c.prevA0 || c.prevB1 > c.prevB0;
    if (ok && prevUsed)
    {
        // 上一代窗认领的段在拷贝之后的写头 w2 下重判一次别名(写头没确认时同 certify:w2 还停在上一代最后
        // 观测到的头上就不判,变了就按 w2 暂定)。写头已确认时 curHi_ 是本代发布过的最高写头;没确认时它可能是
        // 上一代的旧头,不算。
        const bool r1Trust2 = !prevBLowKnown_ && w2 == prevHi_;
        const int64_t bLow = prevBLowKnown_ ? prevBLow_ : w2 - spanBound(prevN, ring) - framesSinceGen_;
        const int64_t hi2 = prevBLowKnown_ ? std::max(w2, curHi_) : w2;
        ok = r1Trust2 ||
             ((prevBLowKnown_ || contiguousInGen_) && prevAliasFree(c.prevA0, c.prevA1, bLow, hi2 + guard, ring) &&
              prevAliasFree(c.prevB0, c.prevB1, bLow, hi2 + guard, ring));
    }
    framesSinceGen_ += n;
    if (!ok)
    {
        if (primed_)
        {
            gapCount_.fetch_add(1, std::memory_order_relaxed);
        }
        handoverLossCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (c.curLen > 0)
    {
        primed_ = true; // 本代已在当前窗里确有可读数据,此后当前窗里的失败即真缺口
    }
    if (c.curLen == n)
    {
        // 整块都在当前窗里:量一次「写头相对读方」的提前量(同步规则 / 锁步判别的尺子)。
        const int64_t ahead = w - t0;
        const int64_t past = w - t1;
        leadCur_.hi = leadCur_.samples > 0 ? std::max(leadCur_.hi, ahead) : ahead;
        leadCur_.lo = leadCur_.samples > 0 ? std::min(leadCur_.lo, past) : past;
        ++leadCur_.samples;
    }
    sawFail_ = false; // 成功一块即清停滞账:下一次失败重新从「写头是否在动」问起
    readOkCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace scvb::output
