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
// 只给一头的场景(理论上可能,契约没禁)在**指名了轨**时按「给了的照用、没给的取 all 档
// 同侧端点」处理;两头都给 → 逐字照用,行为与修复前完全一致(Tab3 工具条那条链路一字未变)。
//
// `tracksMask` 参与判定的理由(PR #112 评审【重要】):0 在 processor 侧是「**不限轨**」
// (startAnalysis 的 `tracksMask != 0 && …`)。「既不指名轨、又不给范围」的对象形 scope
// 修复前被 [0,0] 挡下,若也跟着走 "all" 推导就成了「全 15 轨 × 全时间线」—— 配上
// `clearManual:true` 那是不可撤销的 origin 全清(§1.6「撤销:否」)。本 PR 只该放开
// 「指名了轨、没给范围」这一种形状;要全轨全时间线请显式用 `"all"`,那条路有 UI 二次确认。
// 精确口径:`tracksMask == 0` 时**只有「两头都给」能透传**,其余形状(全缺 / 只给一头)一律判空。
// 逐字保留「两头都给」是因为 `{tracksMask:0, startS, endS}` = 不限轨 + 显式范围,修复前就成立
// (`tab-master.js` 的 `analyzeScope()` 在全部轨被禁用 + 显式范围档时正好产出它)。
// 只给一头也挡掉是安全方向:`{tracksMask:0, startS:5}` 否则就成了「全轨、5s 到时间线末端」的
// origin 全清,与本守卫的用意正相反;现无调用方产这个形状。
//
// 一个**有意**的取舍(PR #112 评审建议②):`daw_loop` / `manual` 档下缺省范围跟的是
// `global.range`,于是 Tab2 的单轨重新识别只重算 range **内**的区间 —— 解冻提示要清的那条
// 「全时限 user_edited 常值段」会被部分清除,range 外仍留着旧手动值。这与 Tab3「分析」同口径:
// 用户显式设了范围就尊重它,不替他扩大写入面。要改成「单轨重新识别恒取全时间线」是产品语义
// 决策(需用户拍板),不是这里顺手改的事。
inline AnalyzeRange analyzeScopeRange(unsigned int tracksMask, bool hasStartS, double startS, bool hasEndS, double endS,
                                      int rangeMode, double rangeStartS, double rangeEndS, double capturedExtentS)
{
    if (hasStartS && hasEndS)
    {
        AnalyzeRange r;
        r.startS = startS;
        r.endS = endS;
        return r;
    }

    if (tracksMask == 0)
    {
        return AnalyzeRange{}; // 空范围 → §1.6 拒绝态 {ok:false, affected:{0,0,0}}
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
