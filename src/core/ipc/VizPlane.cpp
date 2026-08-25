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
    base_ = nullptr;
    handle_.release(steadyNowMs());
    // pendingReleases_ 的 SegmentHandle 元素随 vector 析构各自 release(进程退出兜底)。
}

void VizPlane::releaseHandle()
{
    base_ = nullptr;
    if (handle_.valid() && !handle_.release(steadyNowMs()))
    {
        // 租约在途/宽限期未满:压入待回收列表,由 [M] reapPendingReleases() 回收(防泄漏)。
        pendingReleases_.push_back(std::move(handle_));
    }
}

void VizPlane::release()
{
    releaseHandle();
}

void VizPlane::reapPendingReleases(u64 nowMs)
{
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();)
    {
        if (it->release(nowMs))
        {
            it = pendingReleases_.erase(it);
        }
        else
        {
            ++it;
        }
    }
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
    handle_ = SegmentHandle(std::move(view), &backend_);
    base_ = static_cast<unsigned char*>(handle_.base());
    ownerThread_ = std::this_thread::get_id(); // 之后只认这个线程写(RT 零写入护栏)
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
    handle_ = SegmentHandle(std::move(view), &backend_);
    base_ = static_cast<unsigned char*>(handle_.base());

    if (!geometryMatches())
    {
        // 同 abi 下的几何漂移(理论不应发生):按拒连处理,不半兼容。
        releaseHandle();
        return InitResult::kAbiMismatch;
    }
    return InitResult::kOk;
}

InitResult VizPlane::changeGroup(u32 newGroup)
{
    const bool wasReadOnly = readOnly_;
    releaseHandle();
    group_ = newGroup;
    return wasReadOnly ? attachReadOnly() : open();
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
    if (std::this_thread::get_id() != ownerThread_)
    {
        // 非 owner 线程(典型误接线 = 从 processBlock 调进来):一个字节都不写,只计数。
        foreignThreadWrites_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* f = frame();

    // seqlock 写:seq 先 +1 变奇(进入临界区)→ 写载荷 → 再 +1 变偶(发布完成)。
    f->seq.fetch_add(1, std::memory_order_release);

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

    if (writeLanes)
    {
        auto* c = colors();
        auto* cov = coverageBits();
        auto* ln = lanes();
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

    for (int attempt = 0; attempt < kVizReadRetries; ++attempt)
    {
        const u32 before = f->seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
        {
            continue; // 写方在临界区内
        }

        out.publishMs = f->publish_ms.load(std::memory_order_relaxed);
        out.windowStartSamples = f->window_start_samples.load(std::memory_order_relaxed);
        out.windowSpanSamples = f->window_span_samples.load(std::memory_order_relaxed);
        out.playheadSamples = f->playhead_samples.load(std::memory_order_relaxed);
        out.loopStartSamples = f->loop_start_samples.load(std::memory_order_relaxed);
        out.loopEndSamples = f->loop_end_samples.load(std::memory_order_relaxed);
        out.sampleRate = f->sample_rate.load(std::memory_order_relaxed);
        out.versionActive = f->version_active.load(std::memory_order_relaxed);
        out.playheadFlags = f->playhead_flags.load(std::memory_order_relaxed);
        out.playheadEpoch = f->playhead_epoch.load(std::memory_order_relaxed);
        out.onlineMask = f->track_online_mask.load(std::memory_order_relaxed);
        out.coveredMask = f->track_covered_mask.load(std::memory_order_relaxed);
        out.stereoMask = f->track_stereo_mask.load(std::memory_order_relaxed);
        out.laneRevision = f->lane_revision.load(std::memory_order_relaxed);

        for (u32 t = 0; t < kMaxChannels; ++t)
        {
            out.trackColor[t] = c->index[t].load(std::memory_order_relaxed);
            for (u32 w = 0; w < kVizCoverageWords; ++w)
            {
                out.coverage[t][w] = cov->bits[t][w].load(std::memory_order_relaxed);
            }
            for (u32 i = 0; i < kVizColumns; ++i)
            {
                out.pan[t][i] = ln->pan[t][i].load(std::memory_order_relaxed);
            }
        }

        // seqlock 读边界:载荷读取不得越过第二次 seq 读。
        std::atomic_thread_fence(std::memory_order_acquire);
        if (f->seq.load(std::memory_order_relaxed) == before)
        {
            return true;
        }
    }
    return false; // 连续撕裂:沿用上帧
}

} // namespace scvb
