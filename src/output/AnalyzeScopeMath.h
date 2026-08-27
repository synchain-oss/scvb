// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// analyze/previewAnalyze 的「全部」范围推导(§1.5/§1.6 的 "all" 分支)。
//
// 单拎成纯函数的理由:它是 v5.1 P1-F 的**唯一**修复点,而它原先埋在
// OutputEditor::parseAnalyzeScope(私有成员,构造要 WebView)里 —— 免 DAW harness 够不着,
// 于是那一轮的回归用例只能绕开它直接调 startAnalysis,把修复改回去用例照样绿(评审 I1)。
// 纯函数没有 JUCE 依赖,scvb_tests 可以直接断言。

namespace scvb::output
{

struct AnalyzeRange
{
    double startS = 0.0;
    double endS = 0.0;
    bool valid() const { return endS > startS; }
};

// rangeMode:0 = follow(无显式范围),非 0 = daw_loop / manual(有显式范围)。
// capturedExtentS:全轨 coverage 的最大终点(秒),没采过回 0。
//
// follow 档取**整条已采集时间线**,与播放头无关。原实现取 [0, 当前播放头]:
// Cubase「播完回开头」会把播放头送回 0 → endS≈0 → 范围恒空 → 分析永远受理不了,
// 按钮亮着点了没反应。一帧都没采到时回空范围(由 §1.6 拒绝态 + UI 空态提示作答),
// **不拿播放头兜底** —— 那会把「没采集」误报成「采到了播放头这里」。
inline AnalyzeRange analyzeAllRange(int rangeMode, double rangeStartS, double rangeEndS, double capturedExtentS)
{
    AnalyzeRange r;
    if (rangeMode != 0 && rangeEndS > rangeStartS)
    {
        r.startS = rangeStartS;
        r.endS = rangeEndS;
        return r;
    }
    r.startS = 0.0;
    r.endS = capturedExtentS > 0.0 ? capturedExtentS : 0.0;
    return r;
}

// 对象形 scope `{tracksMask, startS?, endS?}` 的范围推导(§1.6:后两个字段是**可选**的)。
//
// 缺省口径 = 与 `"all"` 同一条推导,**不是 0.0**。原实现两个字段都 `getProperty(..., 0.0)`
// 兜底,于是「只给 tracksMask」的调用者拿到 [0,0] —— `startAnalysis` 在 `!(endS > startS)`
// 处直接回 `{ok:false, affected:{0,0,0}}`,一段都不重算。Tab2 解冻提示条的「重新识别(含手动段)」
// 正是这个形状(`analyze({tracksMask}, {clearManual:true})`),所以点了没有任何反应(v5.4 实测 SL-190)。
// mock 桥对缺省 startS/endS 取的是 ±∞(整条时间线),web smoke 因此一直是绿的 —— 两侧口径分叉,
// 冒烟测不到真桥。本函数把真桥这一侧对齐成「缺省 = 未指定 = 走 `"all"` 的那条推导」。
//
// 只给一头的场景(理论上可能,契约没禁)按「给了的照用、没给的取 all 档同侧端点」处理;
// 两头都给 → 逐字照用,行为与修复前完全一致(Tab3 工具条那条链路一字未变)。
inline AnalyzeRange analyzeScopeRange(bool hasStartS, double startS, bool hasEndS, double endS, int rangeMode,
                                      double rangeStartS, double rangeEndS, double capturedExtentS)
{
    if (hasStartS && hasEndS)
    {
        AnalyzeRange r;
        r.startS = startS;
        r.endS = endS;
        return r;
    }

    AnalyzeRange r = analyzeAllRange(rangeMode, rangeStartS, rangeEndS, capturedExtentS);
    if (hasStartS)
    {
        r.startS = startS;
    }
    if (hasEndS)
    {
        r.endS = endS;
    }
    return r;
}

} // namespace scvb::output
