// SPDX-License-Identifier: GPL-3.0-or-later
#include "VizPlane.h"

#include <cstring>

namespace scvb
{

namespace
{
// v1 仅 Windows,OS 前缀固定 "Local\"(与 CtrlPlane.cpp 的 ctrlFullName 同构)。
std::wstring vizFullName(u32 group)
{
    return L"Local\\" + segmentLogicalName(group, SegmentKind::kViz);
}
} // namespace

VizPlane::VizPlane(ISegmentBackend& backend, u32 group) : backend_(backend), group_(group) {}

VizPlane::~VizPlane()
{
    releaseHandle();
}

void VizPlane::releaseHandle()
{
    base_ = nullptr;
    if (view_.base != nullptr)
    {
        // 立即解映射(仅消息线程):viz 无音频线程访问,不需要租约握手与宽限期。
        backend_.unmap(view_);
    }
}

void VizPlane::release()
{
    releaseHandle();
}

InitResult VizPlane::open()
{
    releaseHandle();
    readOnly_ = false;

    SegmentView view;
    const auto r = backend_.createOrOpen(vizFullName(group_), kVizSegmentSize, view);
    if (r != InitResult::kOk)
    {
        return r;
    }

    auto* h = static_cast<VizHeader*>(view.base);
    // 几何字段在 magic 发布前写定(与 AudioRingHeader 的 ring_frames 同款口径)。
    const auto initData = [h]() {
        h->column_count = kVizColumns;
        h->track_count = kMaxChannels;
        h->pan_scale = static_cast<u32>(kVizPanScale);
    };
    // owner 角色:覆盖式重初始化分支从帧区起点清零(header 的几何字段随后由 initData 重写)。
    const auto ir = backend_.initHeader(view, &h->magic, &h->abi, &h->generation, kVizFrameOffset, initData,
                                        /*allowOverwrite=*/true);
    if (ir != InitResult::kOk)
    {
        backend_.unmap(view);
        return ir;
    }
    view_ = std::move(view);
    base_ = static_cast<unsigned char*>(view_.base);
    // I2:**不在这里绑 owner 线程**。open() 由 prepareToPlay 触发,而 JUCE/VST3 不保证
    // prepareToPlay 与 timerCallback 在同一条线程上 —— 绑错的后果是此后每一次 publish() 都被
    // 护栏挡掉,viz 段静默保持全零而没有任何报错。改为**首次 publish() 时绑定**(见 publish)。
    return InitResult::kOk;
}

InitResult VizPlane::attachReadOnly()
{
    releaseHandle();
    readOnly_ = true;

    SegmentView view;
    // 只读权限打开已存在段(契约 §2.4 最小权限口径);段不存在 → kFailed → Monitor 空态。
    const auto r = backend_.openExistingReadOnly(vizFullName(group_), view);
    if (r != InitResult::kOk)
    {
        return r;
    }

    const auto* h = static_cast<const VizHeader*>(view.base);
    // 非破坏性校验:只读触碰 + 单次 acquire 读 magic/abi。绝不 memset、绝不写头部。
    const auto ir = backend_.checkHeaderReadOnly(view, h->magic, h->abi);
    if (ir != InitResult::kOk)
    {
        backend_.unmap(view);
        return ir;
    }
    view_ = std::move(view);
    base_ = static_cast<unsigned char*>(view_.base);

    if (!geometryMatches())
    {
        // 同 abi 下的几何漂移(理论不应发生):按拒连处理,不半兼容。
        releaseHandle();
        return InitResult::kAbiMismatch;
    }
    return InitResult::kOk;
}

void VizPlane::setGroupWriter(u32 newGroup)
{
    releaseHandle();
    readOnly_ = false;
    group_ = newGroup;
}

InitResult VizPlane::changeGroup(u32 newGroup)
{
    const bool wasReadOnly = readOnly_;
    releaseHandle();
    group_ = newGroup;
    return wasReadOnly ? attachReadOnly() : open();
}

void VizPlane::setGroupReadOnly(u32 newGroup)
{
    releaseHandle();
    readOnly_ = true; // 一旦声明只读角色就不再回退 —— 之后 publish() 恒 no-op
    group_ = newGroup;
}

VizHeader* VizPlane::header() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizHeader*>(base_ + kVizHeaderOffset);
}
VizFrame* VizPlane::frame() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizFrame*>(base_ + kVizFrameOffset);
}
VizTrackColors* VizPlane::colors() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizTrackColors*>(base_ + kVizColorsOffset);
}
VizCoverage* VizPlane::coverageBits() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizCoverage*>(base_ + kVizCoverageOffset);
}
VizLanes* VizPlane::lanes() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizLanes*>(base_ + kVizLanesOffset);
}
VizTrackState* VizPlane::trackState() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizTrackState*>(base_ + kVizTrackStateOffset);
}
VizTrackLabels* VizPlane::labels() const
{
    return base_ == nullptr ? nullptr : reinterpret_cast<VizTrackLabels*>(base_ + kVizLabelsOffset);
}

u32 VizPlane::generation() const
{
    const auto* h = header();
    return h == nullptr ? 0 : h->generation.load(std::memory_order_acquire);
}

bool VizPlane::geometryMatches() const
{
    const auto* h = header();
    if (h == nullptr)
    {
        return false;
    }
    return h->column_count == kVizColumns && h->track_count == kMaxChannels &&
           h->pan_scale == static_cast<u32>(kVizPanScale);
}

void VizPlane::publish(const VizSnapshot& s, bool writeLanes)
{
    if (base_ == nullptr || readOnly_)
    {
        return; // 只读 attach 状态下绝不写(防误写;Monitor 侧的零写入铁律由此在类型外再兜一层)
    }
    if (ownerThread_ == std::thread::id{})
    {
        // 首次发布即认定 owner 线程(生产路径 = Output 的 [M] 定时器)。绑在这里而不是 open(),
        // 见 open() 里 I2 的说明。
        ownerThread_ = std::this_thread::get_id();
    }
    if (std::this_thread::get_id() != ownerThread_)
    {
        // 非 owner 线程(典型误接线 = 从 processBlock 调进来):一个字节都不写,只计数。
        foreignThreadWrites_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* f = frame();

    // seqlock 写:seq 先 +1 变奇(进入临界区)→ 写载荷 → 再 +1 变偶(发布完成)。
    // I1:第一次递增用 **relaxed + release 栅栏**,不能用 release。
    // release 是「之前的写不越到之后」的单向栅栏 —— 它挡不住**后续**的 relaxed 载荷写被提升到
    // seq 变奇之前,而那恰恰是读方要靠奇数看见的「正在写」窗口。栅栏才是挡后续写的那一侧。
    f->seq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    f->publish_ms.store(s.publishMs, std::memory_order_relaxed);
    f->window_start_samples.store(s.windowStartSamples, std::memory_order_relaxed);
    f->window_span_samples.store(s.windowSpanSamples, std::memory_order_relaxed);
    f->playhead_samples.store(s.playheadSamples, std::memory_order_relaxed);
    f->loop_start_samples.store(s.loopStartSamples, std::memory_order_relaxed);
    f->loop_end_samples.store(s.loopEndSamples, std::memory_order_relaxed);
    f->sample_rate.store(s.sampleRate, std::memory_order_relaxed);
    f->version_active.store(s.versionActive, std::memory_order_relaxed);
    f->playhead_flags.store(s.playheadFlags, std::memory_order_relaxed);
    f->playhead_epoch.store(s.playheadEpoch, std::memory_order_relaxed);
    f->track_online_mask.store(s.onlineMask, std::memory_order_relaxed);
    f->track_covered_mask.store(s.coveredMask, std::memory_order_relaxed);
    f->track_stereo_mask.store(s.stereoMask, std::memory_order_relaxed);
    f->lane_revision.store(s.laneRevision, std::memory_order_relaxed);
    f->track_lead_mask.store(s.leadMask, std::memory_order_relaxed);

    // 每轨当前值(分布图数据面):随每帧刷新 —— 它们是「此刻」,不受 writeLanes 分频影响。
    auto* ts = trackState();
    for (u32 t = 0; t < kMaxChannels; ++t)
    {
        ts->panNow[t].store(s.panNow[t], std::memory_order_relaxed);
        ts->volDb[t].store(s.volDb[t], std::memory_order_relaxed);
        ts->widthPct[t].store(s.widthPct[t], std::memory_order_relaxed);
    }

    if (writeLanes)
    {
        auto* c = colors();
        auto* cov = coverageBits();
        auto* ln = lanes();
        auto* lb = labels();
        for (u32 t = 0; t < kMaxChannels; ++t)
        {
            c->index[t].store(s.trackColor[t], std::memory_order_relaxed);
            for (u32 w = 0; w < kVizCoverageWords; ++w)
            {
                cov->bits[t][w].store(s.coverage[t][w], std::memory_order_relaxed);
            }
            for (u32 i = 0; i < kVizColumns; ++i)
            {
                ln->pan[t][i].store(s.pan[t][i], std::memory_order_relaxed);
            }

            // 轨名:UTF-8 边界安全截断 → 打包进 u32 字(主机序),尾部 NUL 补齐。
            const std::string& name = s.label[t];
            const std::size_t n = vizUtf8TruncateLen(name, kVizLabelBytes - 1);
            unsigned char bytes[kVizLabelBytes] = {};
            for (std::size_t i = 0; i < n; ++i)
            {
                bytes[i] = static_cast<unsigned char>(name[i]);
            }
            for (u32 w = 0; w < kVizLabelWords; ++w)
            {
                u32 word = 0;
                std::memcpy(&word, bytes + w * 4, 4);
                lb->utf8[t][w].store(word, std::memory_order_relaxed);
            }
        }
    }

    f->seq.fetch_add(1, std::memory_order_release);
}

bool VizPlane::read(VizSnapshot& out) const
{
    if (base_ == nullptr)
    {
        return false;
    }
    const auto* f = frame();
    const auto* c = colors();
    const auto* cov = coverageBits();
    const auto* ln = lanes();
    const auto* ts = trackState();
    const auto* lb = labels();

    // I3:先读进 scratch_,整帧一致才拷给 out —— 撕裂时 out 一个字节都不动,
    // 「沿用上帧」因此在调用方真的做得到(早期版本直接读进 out,撕裂即把上一帧覆盖成拼接)。
    VizSnapshot& out2 = scratch_;
    for (int attempt = 0; attempt < kVizReadRetries; ++attempt)
    {
        const u32 before = f->seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
        {
            continue; // 写方在临界区内
        }

        out2.publishMs = f->publish_ms.load(std::memory_order_relaxed);
        out2.windowStartSamples = f->window_start_samples.load(std::memory_order_relaxed);
        out2.windowSpanSamples = f->window_span_samples.load(std::memory_order_relaxed);
        out2.playheadSamples = f->playhead_samples.load(std::memory_order_relaxed);
        out2.loopStartSamples = f->loop_start_samples.load(std::memory_order_relaxed);
        out2.loopEndSamples = f->loop_end_samples.load(std::memory_order_relaxed);
        out2.sampleRate = f->sample_rate.load(std::memory_order_relaxed);
        out2.versionActive = f->version_active.load(std::memory_order_relaxed);
        out2.playheadFlags = f->playhead_flags.load(std::memory_order_relaxed);
        out2.playheadEpoch = f->playhead_epoch.load(std::memory_order_relaxed);
        out2.onlineMask = f->track_online_mask.load(std::memory_order_relaxed);
        out2.coveredMask = f->track_covered_mask.load(std::memory_order_relaxed);
        out2.stereoMask = f->track_stereo_mask.load(std::memory_order_relaxed);
        out2.laneRevision = f->lane_revision.load(std::memory_order_relaxed);
        out2.leadMask = f->track_lead_mask.load(std::memory_order_relaxed);
        out2.seq = before;
        out2.generation = header()->generation.load(std::memory_order_acquire); // 与 generation() 同口径

        for (u32 t = 0; t < kMaxChannels; ++t)
        {
            out2.trackColor[t] = c->index[t].load(std::memory_order_relaxed);
            out2.panNow[t] = ts->panNow[t].load(std::memory_order_relaxed);
            out2.volDb[t] = ts->volDb[t].load(std::memory_order_relaxed);
            out2.widthPct[t] = ts->widthPct[t].load(std::memory_order_relaxed);
            for (u32 w = 0; w < kVizCoverageWords; ++w)
            {
                out2.coverage[t][w] = cov->bits[t][w].load(std::memory_order_relaxed);
            }
            for (u32 i = 0; i < kVizColumns; ++i)
            {
                out2.pan[t][i] = ln->pan[t][i].load(std::memory_order_relaxed);
            }

            // 轨名:u32 字 → 字节 → 到第一个 NUL 为止(段内恒 NUL 补齐,不会漏尾)。
            unsigned char bytes[kVizLabelBytes] = {};
            for (u32 w = 0; w < kVizLabelWords; ++w)
            {
                const u32 word = lb->utf8[t][w].load(std::memory_order_relaxed);
                std::memcpy(bytes + w * 4, &word, 4);
            }
            std::size_t n = 0;
            while (n < kVizLabelBytes && bytes[n] != 0)
            {
                ++n;
            }
            out2.label[t].assign(reinterpret_cast<const char*>(bytes), n);
        }

        // seqlock 读边界:载荷读取不得越过第二次 seq 读。
        std::atomic_thread_fence(std::memory_order_acquire);
        if (f->seq.load(std::memory_order_relaxed) == before)
        {
            out = out2; // 整帧一致:此刻才交给调用方
            return true;
        }
    }
    return false; // 连续撕裂:沿用上帧
}

} // namespace scvb
