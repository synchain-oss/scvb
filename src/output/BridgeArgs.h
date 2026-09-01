// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BridgeArgs —— 桥面 native function 参数提取/白名单助手(消息线程)。纯 JUCE 工具,可离线单测。
// 与 SegmentEditService.h 同放 src/output/(不落 scvb_core,因依赖 JUCE)。

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cstdint>
#include <map>

#include "BridgeBase.h" // strictBool 真身(两插件共用的桥面参数口径)
#include "state/StateCodec.h" // CrvsData/VersionCurve(R4 降级链的输入)

namespace scvb::output
{

// 严格布尔提取 —— 真身已上提到 scvb::bridge(plugin-common/BridgeBase.h),两侧共用一份。
// 这里转发,Output 既有调用点(unqualified strictBool)与单测一个字都不用改。
using scvb::bridge::strictBool;

// 读取参数的**工程值**(契约 §2.2 f32 工程值)。APVTS 的 getRawParameterValue 返回归一化 0..1 原子,
// 须经 convertFrom0to1 还原(PR#55 第3轮重要1;AudioParameterFloat::get() 同款)。
inline float readParamEngineering(juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
{
    if (auto* p = apvts.getParameter(id))
        return p->convertFrom0to1(p->getValue());
    return 0.0f;
}

// gesture 通道白名单(契约 §1.12):全局三件 + 当前激活版本每轨 width/freeze。
// pan/vol 走 setTrackManual(未冻结通道 = 曲线真身 + 参数面;[J85] 冻结通道 = **只**落参数面),
// 非激活版本参数不进本通道;白名单外回 badArg(不得静默忽略)。
// 「不在 gesture 白名单」≠「不写参数面」:UI 侧从不对 pan/vol 调 setParam(§1.13 防回环),但
// native 侧处理 setTrackManual 时会自己带 gesture 落一次参数(#87 裁定②)。
inline bool isGestureParam(const juce::String& id, int activeVersion)
{
    if (id == "width" || id == "ms_balance" || id == "lead_select")
        return true;
    for (int t = 1; t <= 15; ++t)
    {
        if (id == juce::String::formatted("v%d_t%02d_width", activeVersion, t) ||
            id == juce::String::formatted("v%d_t%02d_freeze", activeVersion, t))
            return true;
    }
    return false;
}

// segmentation.mode 白名单(02-dsp-spec §362,params-v0):vad_only(不做 S1)/ valley(默认)。
inline bool isSegmentationMode(const juce::String& mode)
{
    return mode == "vad_only" || mode == "valley";
}

// 「无末端」哨兵:CRVS 里 t1 = 1<<40 表示「覆盖到时间线末端」,真末端由宿主时间线提供
// (`SegmentEditService.h:89` 的 setTrackManual 常值段)。与 `kVizOpenEndedT1` 同一个数,
// #89 已在 viz 侧按「只取 t0」处理过;桥面 §2.8 的处理见 `OutputEditor::emitSegments`。
inline constexpr std::int64_t kOpenEndedT1 = static_cast<std::int64_t>(1) << 40;

// ---- R4 降级链(桥面 §2.8):无末端段上桥前的有效右端 ----
// 这三个函数是降级链的**唯一实现**,`OutputEditor::buildSegmentsPayload` 与 harness 的
// HOST R4 用例走同一份代码 —— 用例断的就是真实上桥值,revert 任何一级都会红。
//
// ① 工程级已知末端:全 15 轨该版本里所有非哨兵段的最大真末端;一个都没有(全是手动/
//    冻结轨)→ ② 已采集时间线末端。**必须是工程级、不能是本轨级**:`setTrackManual` 的
//    产物是单段全时限(`track.segments.assign(1, seg)`),按本轨算永远得 0。
inline std::int64_t knownTimelineEndSamples(const scvb::state::VersionCurve& vc, double capturedExtentS,
                                            double sampleRate)
{
    std::int64_t knownEnd = 0;
    for (const auto& track : vc.tracks)
        for (const auto& sg : track.segments)
            if (sg.t1 < kOpenEndedT1)
                knownEnd = std::max(knownEnd, sg.t1);
    if (knownEnd <= 0 && sampleRate > 0.0)
        knownEnd = static_cast<std::int64_t>(capturedExtentS * sampleRate); // ② 采集覆盖兜底
    return knownEnd;
}

// ③ 最小非零宽度(样本):一个特征 hop;sr 非法时 1。空工程下哨兵段各自退到
//    「自己的 t0 + 这个宽度」,宽度虽小但非零,UI 仍可点可切。
inline std::int64_t minOpenEndedSpanSamples(double hopSeconds, double sampleRate)
{
    return sampleRate > 0.0 ? std::max<std::int64_t>(1, static_cast<std::int64_t>(hopSeconds * sampleRate)) : 1;
}

// 有效右端:非哨兵段原样;openEnded 段 = max(①②, t0 + ③) —— **严格大于 t0**,
// 坍缩成零宽的段在波形页上点不中、切不开(v5.3 R4)。真末端由前端按 openEnded 自行延伸。
inline std::int64_t effectiveT1Samples(std::int64_t t0, std::int64_t t1, std::int64_t knownEndSamples,
                                       std::int64_t minSpanSamples)
{
    if (t1 < kOpenEndedT1)
        return t1;
    return std::max(knownEndSamples, t0 + std::max<std::int64_t>(1, minSpanSamples));
}

// 样本→秒安全换算:sampleRate<=0 返回 0.0 哨兵,绝不把 NaN/inf 进 JSON(PR#55 第6轮缺陷1)。
//
// **这里不做值域裁剪。** 曾经试过在这里把超大采样数夹成 0.0 来挡 P0-A,那是错的:
// 它把「无末端哨兵」也一并夹成 0,于是手动/冻结段的 t1S=0 < t0S —— 段在波形页上直接消失、
// 点不中、切不开。哨兵是**语义**问题,必须在**产生它的地方**按语义降级(emitSegments 把
// t1S 降级成已知时间线末端),而不是在一个通用换算函数里按数值大小一刀切。
// P0-A 的止血也不靠这条:求交 + 跨度闸 + 前端 MAX_DURATION_S 三层已经够。
inline double samplesToSeconds(std::int64_t samples, double sampleRate)
{
    return sampleRate > 0.0 ? static_cast<double>(samples) / sampleRate : 0.0;
}

// --- [SL-199] scvb.params 的隐藏期吞帧 ---------------------------------------------
//
// `emitParams` 有**两层**基线,而只有一层挡住了隐藏期丢帧:
//   ① `lastParamsJson_` —— 由 `emitIfChanged` 维护,**不可见时不推进**(那段注释已写明理由),
//      所以恢复可见后同一份 json 会自然重发;
//   ② `lastParamsValues_` —— 由 `emitParams` 自己维护,**在构建载荷时就推进了**,与这一帧
//      究竟有没有发出去无关。
// 于是:隐藏期某个 id 变了 → ② 已经等于新值 → 这一帧被 `emitEventIfBrowserIsVisible` 丢掉 →
// 下一拍 `changed == false`、`values` 为空、`any == false` **提前 return**,载荷压根不再构建,
// ① 那层保护也就无从生效。这个变化**永远不会重发**。
//
// [J85] 之后冻结维度的读回值**只**在参数面上(段表那条后路没了),所以这条洞的后果是:
// 隐藏/折叠面板期间被宿主自动化或手动写入改掉的冻结值,恢复可见后旋钮一直显示旧值,
// 直到该参数再变一次。`firstFrame_` 是 editor 生命周期级的 —— 宿主只是隐藏而不销毁 editor 时,
// `emitParams(true)` 不会重来。
//
// 修法(SL-199):**不可见→可见置一个闩锁位,强制全量,直到真的发出去才清**(scvb.segments 同款,
// 见 segmentsResendNeeded)。比「按 key
// 回滚基线」简单可靠 —— 它覆盖隐藏期被吞的**所有** id,不需要知道具体吞了哪几个;代价是每次重新打开面板多发一帧 63 个 id
// 的全量,可忽略。(Input 侧对同类问题用的是「发出去了才推进基线」,见 `InputBridgeLogic.h` 的 `advanceEmitCache` /
// `advanceConfigSeq`;两种口径都成立, 这里取前者是因为 `lastParamsValues_` 是 63 个 key 的 map,回滚要多存一份候选集。)
//
// 纯函数,可离线断言:`wasVisible` / `pendingFull` 由调用方持有(OutputEditor 成员)。
//
// **闩锁,不是一次性边沿**(#119 复审重要):边沿版把「补发」压在 `isVisible()` 刚翻真的那一拍,
// 而那一拍恰好是最不确定的时刻 —— `Component::isVisible()`(我们的判据)与 JUCE
// `emitEventIfBrowserIsVisible` 内部的判据不保证逐帧一致(WebView2 侧刚被重新显示、页面刚恢复)。
// 边沿一旦消费掉就没有第二次机会,那一帧丢了 SL-199 原样复现。
// 闩锁语义 = 「**直到真的发出去为止一直补**」,与 Input 侧 `advanceEmitCache` / `advanceConfigSeq` /
// `claimEdgeConsumed` 的幂等口径统一:没发出去基线就不动,下一拍自然重试,不依赖抓住某一拍。
//
// ⚠ 能力边界(如实记):这里的「发出去了」是 **C++ 侧观察得到的**那一层(`emitIfChanged` /
// `emitSegments` 的返回值 = 可见且确实调了 `emitEventIfBrowserIsVisible`)。JUCE 内部若在那之后
// 再丢一次,C++ 侧没有任何回执可查 —— 要闭合到「JS 真收到」需要 JS 侧 ack,那是另一条卡。
// 即便如此,闩锁仍严格优于边沿:整个不可见期与任何「这一拍没发成」的拍都会继续补。
inline void raiseResendLatch(bool visibleNow, bool& wasVisible, bool& pendingFull) noexcept
{
    if (visibleNow && !wasVisible)
    {
        pendingFull = true; // 不可见 → 可见:置位
    }
    wasVisible = visibleNow; // 记账无条件跟到当前态
}

// 这一帧的处置:`sent` = 上面那层「已下发」的观察值。只有确实发出去了才清位;
// 没发出去(不可见 / 被丢)就保持,下一拍继续补。
inline void settleResendLatch(bool sent, bool& pendingFull) noexcept
{
    if (sent)
    {
        pendingFull = false;
    }
}

// scvb.segments 的重发判定(`emitTick` 里那个 if 就是它)。
//
// 它与 params 是**同一个洞**:三个触发基线(`lastSegmentsSampleRate_` / `lastCrvsRevision_` /
// `lastStaleMask_`)在 if 体里、调 `emitSegments` **之前**就推进了,与这一帧发没发出去无关。
// 隐藏期段表变了(分析完成 / 加载工程 / stale 位翻转)→ 这一帧被丢掉 → 基线已经跟上 →
// 恢复可见后条件恒假,段表陈旧到下一次段编辑 / undo / 切版本才刷新。
// 而且它比 params **更依赖**「那一帧真的发出去了」:params 那路还有 `lastParamsJson_` 兜半层,
// segments 这路直接 `emitEventIfBrowserIsVisible`,一层保护都没有 —— 所以 `pendingFull` 必须是
// **闩锁位**(见 raiseResendLatch),补发帧自己被吞的话下一拍还补。
//
// 恢复可见时补一帧 `reason:"snapshot"` 的全量段表是**正确行为**(统筹裁定 2026-08-27):
// 重开面板本就该看到新鲜段表。web 侧 `applySegmentsEvent` 对 snapshot 是整表替换、
// `segmentsEventApplies` 的版本闸对当前激活版本恒放行 —— 中途到达的 snapshot 帧照常生效,
// 不是只有首帧才认。
inline bool segmentsResendNeeded(bool firstFrame, bool pendingFull, bool analyzed, bool sampleRateChanged,
                                 bool crvsRevisionChanged, bool staleMaskChanged) noexcept
{
    return firstFrame || pendingFull || analyzed || sampleRateChanged || crvsRevisionChanged || staleMaskChanged;
}

// scvb.params 稀疏 diff 的选择 + 基线推进(`emitParams` 的循环体就是它)。
// 返回 true = 该 id 本帧要下发;同时把基线推进到新值。
// **基线在这里无条件推进** —— 这正是上面说的洞,由 `raiseResendLatch` 的闩锁兜住;
// 抽出来是为了让「隐藏期改值 → 恢复可见 → 必达且值正确」这条链能离线断言(生产与用例同源)。
inline bool selectParamForEmit(std::map<juce::String, float>& baseline, const juce::String& id, float value,
                               bool forceFull)
{
    // ⚠ 口径说明(#119 复审):两个入参都是 float,加宽到 double 后 `approximatelyEqual<double>` 的
    // 默认容差是 double 量级(|v|≈80 时约 1.8e-14),而**相邻两个 float 在 80 附近就差 7.6e-6**
    // —— 差了八个数量级。所以这里**等价于逐位精确比较**:任意两个不同的 float 一定判「变了」。
    // 这是安全的那一侧(宁可多发一帧,绝不吞掉变化),故维持现状不动行为;要清的只是
    // 「这里有容差」这个说法 —— 它不存在。(double 加宽是 PR#55 就有的写法,非本卡引入。)
    const auto it = baseline.find(id);
    const bool changed =
        it == baseline.end() || !juce::approximatelyEqual(static_cast<double>(it->second), static_cast<double>(value));
    if (!forceFull && !changed)
        return false;
    baseline[id] = value;
    return true;
}

} // namespace scvb::output
