// SPDX-License-Identifier: GPL-3.0-or-later
#include "AutomationPrinter.h"

#include <cmath>

namespace scvb::output
{

namespace
{
constexpr double kMsToSec = 1e-3;
} // namespace

AutomationPrinter::AutomationPrinter()
{
    m_trackEnabled.fill(true);
}

AutomationPrinter::~AutomationPrinter()
{
    // m_gestureGuard(最后成员、最先析构)兜底闭合;此处再显式幂等一次,双保险。
    endAllGestures();
}

void AutomationPrinter::setMode(scvb::engine::AuthorityMode mode)
{
    if (mode == m_mode)
        return;

    const bool leavingPrint = (m_mode == scvb::engine::AuthorityMode::Print);
    m_mode = mode;

    // §2.2 / §3.3:离开 PRINT 的所有路径(stop / 关 Output / 离开区间 / epoch 跳变)统一 endAllGestures。
    if (leavingPrint)
        endAllGestures();
}

void AutomationPrinter::setAnalyzedRange(double startSec, double endSec)
{
    m_rangeStartSec = (startSec <= endSec) ? startSec : endSec;
    m_rangeEndSec = (startSec <= endSec) ? endSec : startSec;
    m_hasRange = true;
}

bool AutomationPrinter::isLaneParam(const juce::String& paramId)
{
    // §3.5 层 1:仅 v{v}_t{tt}_{pan|vol} 是打印车道。全局三件(width/ms_balance/lead_select)与
    // 每轨 width/freeze 不落打印面(host 恒权威),宿主回吐对它们按「非车道」短路,不按模式屏蔽。
    // 严格按冻结 ParamID 形状判定(panId/volId 生成式),避免 suffix 宽松匹配误判。
    if (!paramId.startsWith("v") || !paramId.contains("_t"))
        return false;
    return paramId.endsWith("_pan") || paramId.endsWith("_vol");
}

void AutomationPrinter::setCurves(const std::array<const scvb::CurveEvaluator*, kNumTracks>& curves)
{
    m_curves = curves;
    for (int t = 0; t < kNumTracks; ++t)
    {
        m_lanes[static_cast<std::size_t>(t * 2)].curve = curves[static_cast<std::size_t>(t)];
        m_lanes[static_cast<std::size_t>(t * 2 + 1)].curve = curves[static_cast<std::size_t>(t)];
    }
}

void AutomationPrinter::bindVersion(int version, const scvb::params::ParamHandles& handles)
{
    // 版本切换:旧版本车道 gesture 全部闭合后重绑(§3.3)。
    endAllGestures();

    const int v = juce::jlimit(0, scvb::params::kNumVersions - 1, version - 1);

    for (int t = 0; t < kNumTracks; ++t)
    {
        Lane& panLane = m_lanes[static_cast<std::size_t>(t * 2)];
        Lane& volLane = m_lanes[static_cast<std::size_t>(t * 2 + 1)];

        panLane.param = handles.pan[v][t];
        panLane.freezeRaw = handles.rawFrz[v][t];
        panLane.curve = m_curves[static_cast<std::size_t>(t)];
        panLane.isPan = true;
        panLane.lastWrittenNorm = -1.0f;
        panLane.everWritten = false;
        panLane.gestureOpen = false;

        volLane.param = handles.vol[v][t];
        volLane.freezeRaw = handles.rawFrz[v][t];
        volLane.curve = m_curves[static_cast<std::size_t>(t)];
        volLane.isPan = false;
        volLane.lastWrittenNorm = -1.0f;
        volLane.everWritten = false;
        volLane.gestureOpen = false;
    }
}

void AutomationPrinter::setTrackEnabled(int track, bool enabled)
{
    if (track < 0 || track >= kNumTracks)
        return;
    m_trackEnabled[static_cast<std::size_t>(track)] = enabled;
}

void AutomationPrinter::endAllGestures()
{
    for (auto& lane : m_lanes)
    {
        if (lane.gestureOpen && lane.param != nullptr)
        {
            lane.param->endChangeGesture();
            lane.gestureOpen = false;
        }
    }
}

bool AutomationPrinter::anyGestureOpen() const
{
    return numGesturesOpen() > 0;
}

int AutomationPrinter::numGesturesOpen() const
{
    int n = 0;
    for (const auto& lane : m_lanes)
        if (lane.gestureOpen)
            ++n;
    return n;
}

void AutomationPrinter::recordHostEcho(float value) noexcept
{
    m_hostEchoCount.fetch_add(1, std::memory_order_relaxed);
    m_lastHostEchoValue.store(value, std::memory_order_relaxed);
}

void AutomationPrinter::installHostEchoShield(juce::AudioProcessorValueTreeState& apvts)
{
    for (auto* p : apvts.processor.getParameters())
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p))
            apvts.addParameterListener(rp->getParameterID(), &m_hostEchoListener);
    }
}

void AutomationPrinter::removeHostEchoShield(juce::AudioProcessorValueTreeState& apvts)
{
    for (auto* p : apvts.processor.getParameters())
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p))
            apvts.removeParameterListener(rp->getParameterID(), &m_hostEchoListener);
    }
}

bool AutomationPrinter::laneFrozen(const Lane& lane) const
{
    if (lane.freezeRaw == nullptr)
        return false;

    const int frz = static_cast<int>(lane.freezeRaw->load(std::memory_order_relaxed));
    // J65:bit0=冻结 pan,bit1=冻结 vol。
    return lane.isPan ? ((frz & 1) != 0) : ((frz & 2) != 0);
}

bool AutomationPrinter::nearSegmentBoundary(const scvb::CurveEvaluator* curve, double tSec) const
{
    if (curve == nullptr)
        return false;

    // 段边界 ramp 的 2-4 个点必写(§3.2):t 落在任一非跳变边界的 ramp 窗口内。
    for (const auto& b : curve->boundaries())
    {
        if (b.isJump)
            continue; // 微小变化捷径(无 ramp),由 deadband 覆盖,不强制写
        const double lo = b.centerSec - b.tEffSec * 0.5;
        const double hi = b.centerSec + b.tEffSec * 0.5;
        if (tSec >= lo && tSec <= hi)
            return true;
    }
    return false;
}

void AutomationPrinter::tick()
{
    // —— §3.1:读 playhead 快照(50Hz 消息线程;撕裂沿用上帧,不自旋)——
    scvb::engine::PlayheadPod pod;
    if (m_shot != nullptr)
    {
        if (m_shot->read(pod))
            m_lastShot = pod;
    }
    else
    {
        m_lastShot = scvb::engine::PlayheadPod{};
    }
    const scvb::engine::PlayheadPod& shot = m_lastShot;

    // —— 非 PRINT:零写入、闭合任何遗留 gesture ——
    if (m_mode != scvb::engine::AuthorityMode::Print)
    {
        endAllGestures();
        return;
    }

    // —— 播放态防护:transport 已停但 mode 尚未切 ARMED 时不打印冻结位置(§5.4)——
    if ((shot.flags & scvb::engine::kPlayheadIsPlaying) == 0)
    {
        endAllGestures();
        return;
    }

    // —— 无有效时间线:零写入、闭合 gesture ——
    if (shot.timeSamples < 0 || shot.sampleRate <= 0.0)
    {
        endAllGestures();
        return;
    }

    const double sampleRate = shot.sampleRate;
    const double offsetSec = (static_cast<double>(m_lookaheadMs) + static_cast<double>(m_timeOffsetMs)) * kMsToSec;
    const double tSec = static_cast<double>(shot.timeSamples) / sampleRate + offsetSec;

    // —— R5 / §4.1:区间外(含未设置区间)零 gesture、零写入 ——
    if (!m_hasRange || tSec < m_rangeStartSec || tSec > m_rangeEndSec)
    {
        endAllGestures();
        return;
    }

    // —— §2.2 TimelineJump:epoch 跳变(loop 回跳)→ endAllGestures;区间内下一循环重新 begin ——
    const bool epochJump = m_hasLastEpoch && shot.epoch != m_lastEpoch;
    m_lastEpoch = shot.epoch;
    m_hasLastEpoch = true;
    if (epochJump)
        endAllGestures();

    for (int i = 0; i < kNumLanes; ++i)
    {
        Lane& lane = m_lanes[static_cast<std::size_t>(i)];
        const int track = i / 2;

        // 车道不活跃:param/curve 未接线、轨 disabled、维度被 freeze 冻结。
        const bool active = lane.param != nullptr && lane.curve != nullptr &&
                            m_trackEnabled[static_cast<std::size_t>(track)] && !laneFrozen(lane);

        if (!active)
        {
            // 冻结/disabled 动态生效:闭合该车道 gesture,停写(§1.8 / §3.2)。
            if (lane.gestureOpen)
            {
                lane.param->endChangeGesture();
                lane.gestureOpen = false;
            }
            continue;
        }

        // 活跃车道:保证 gesture 打开(幂等;解冻且在区间内 → 重新 begin,§1.8)。
        if (!lane.gestureOpen)
        {
            lane.param->beginChangeGesture();
            lane.gestureOpen = true;
        }

        // —— 真身曲线采样 → 归一化 → deadband 去重 ——
        const float v =
            lane.isPan ? static_cast<float>(lane.curve->panAt(tSec)) : static_cast<float>(lane.curve->volAt(tSec));
        const float norm = lane.param->convertTo0to1(v);
        const bool boundary = nearSegmentBoundary(lane.curve, tSec);
        const float deadband = lane.isPan ? m_deadbandPan : m_deadbandVolDb;

        // deadband 在参数值域(denorm)比较(§3.2)。
        const float denormNow = lane.param->convertFrom0to1(norm);
        const float denormLast = lane.everWritten ? lane.param->convertFrom0to1(lane.lastWrittenNorm) : 0.0f;
        const float delta = std::fabs(denormNow - denormLast);

        if (!lane.everWritten || boundary || delta > deadband)
        {
            ScopedSelfWriteFlag guard(*this); // §3.5 层 2:抑制自触发 listener
            lane.param->setValueNotifyingHost(norm);
            lane.lastWrittenNorm = norm;
            lane.everWritten = true;
        }
    }
}

} // namespace scvb::output
