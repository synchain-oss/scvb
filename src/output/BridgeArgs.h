// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BridgeArgs —— 桥面 native function 参数提取/白名单助手(消息线程)。纯 JUCE 工具,可离线单测。
// 与 SegmentEditService.h 同放 src/output/(不落 scvb_core,因依赖 JUCE)。

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cstdint>
#include <map>

#include "state/StateCodec.h" // CrvsData/VersionCurve(R4 降级链的输入)

namespace scvb::output
{

// 严格布尔提取(PR#55 缺陷3):仅接受 bool / int / int64 数值;isBool → 真值,isInt/isInt64 → !=0。
// 其它(字符串/对象/void)返回 false,调用方回 badArg。JUCE 8.0.8 的 var::operator bool() 实为
// type->toBool(value)(juce_Variant.cpp:567;bool→boolToBool L246、int→intToBool L149、void→defaultToBool
// L100),对 bool 返回真值、对 void 返回 false;此处仍显式分型,避免字符串 "false" 被 stringToBool
// 误判为非空即 true。
inline bool strictBool(const juce::var& v, bool& out)
{
    if (v.isBool())
    {
        out = static_cast<bool>(v);
        return true;
    }
    if (v.isInt() || v.isInt64())
    {
        out = static_cast<juce::int64>(v) != 0;
        return true;
    }
    return false;
}

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
// 修法(SL-199):**不可见→可见的边沿强制一次全量**。比「按 key 回滚基线」简单可靠 ——
// 它覆盖隐藏期被吞的**所有** id,不需要知道具体吞了哪几个;代价是每次重新打开面板多发一帧
// 63 个 id 的全量,可忽略。(Input 侧对同类问题用的是「发出去了才推进基线」,见
// `InputBridgeLogic.h` 的 `advanceEmitCache` / `advanceConfigSeq`;两种口径都成立,
// 这里取前者是因为 `lastParamsValues_` 是 63 个 key 的 map,回滚要多存一份候选集。)
//
// 纯函数,可离线断言:`wasVisible` 由调用方持有(OutputEditor 成员),本函数负责记账。
inline bool paramsForceFull(bool firstFrame, bool visibleNow, bool& wasVisible) noexcept
{
    const bool becameVisible = visibleNow && !wasVisible;
    wasVisible = visibleNow;
    return firstFrame || becameVisible;
}

// scvb.params 稀疏 diff 的选择 + 基线推进(`emitParams` 的循环体就是它)。
// 返回 true = 该 id 本帧要下发;同时把基线推进到新值。
// **基线在这里无条件推进** —— 这正是上面说的洞,由 `paramsForceFull` 的边沿兜住;
// 抽出来是为了让「隐藏期改值 → 恢复可见 → 必达且值正确」这条链能离线断言(生产与用例同源)。
inline bool selectParamForEmit(std::map<juce::String, float>& baseline, const juce::String& id, float value,
                               bool forceFull)
{
    const auto it = baseline.find(id);
    const bool changed =
        it == baseline.end() || !juce::approximatelyEqual(static_cast<double>(it->second), static_cast<double>(value));
    if (!forceFull && !changed)
        return false;
    baseline[id] = value;
    return true;
}

} // namespace scvb::output
