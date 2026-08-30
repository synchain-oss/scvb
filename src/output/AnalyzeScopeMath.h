// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// analyze 的**范围 → hop 窗**量化(§1.6:范围是半开区间 [startS, endS))。
//
// 单拎成纯函数的理由同上:它决定的不只是「分析哪几个 hop」,还决定 finishAnalysis 里
// 「哪些既有段算落在范围内、要被本次产出取代」—— applyAnalysisSegments 的 outsideRange
// 判据读的正是 `firstHop * hopSamples`。两件事共用一个数,量化方向错一格就是**范围外的段
// 被静默删掉**,而这一格在 DAW 里够不着(段边界要非 hop 对齐才现形)。
//
// 口径 = **向内取整**(firstHop 上取、lastHop 下取):取「完全被 [startS, endS) 包住」的
// 那个最大 hop 窗。修复前 firstHop 走的是**截断**(向 0 取整),于是范围起点只要不是 hop
// 的整数倍,`rangeStartSample` 就落到 startS **之前**最多一个 hop(10ms)处 —— 紧挨在
// 范围左边、`t1 == startS` 的那一段于是满足 `t1 > rangeStartSample`,被判成「与范围相交」
// 而从段表里删掉;可本次分析的产出只覆盖 [rangeStart, rangeEnd),补不回它的段身
// ⇒ **一整段(可能几十秒)凭空消失**。10ms 的重叠毁掉一个任意长的段,这是 SL-242 的
// 一条独立成因(另一条在 web 侧:段级「恢复自动」发的是轨级 scope)。
//
// 谁碰得到:段边界非 hop 对齐 = 用户编辑过的边界(split / move_boundary,§5.4)。
// 那两个 op 的后置是 `locked=true`,锁定段本就免疫,所以现网多数情形被锁挡住了;
// 一旦用户把相邻两段都解了锁再对右边那段做「恢复自动」(clearManual 放开 origin 那层),
// 左邻段就直接没了 —— 用户看到的正是「整个大片段被合并」。
//
// 代价:范围两端各有不足一个 hop 的边角不进本次分析。hop 是分析的时间分辨率下限
// (kFeatHopMs=10),半个 hop 本来就没有可分析的特征值,这是**量化本身**的代价,不是丢数据。
// 反过来,窄于一个 hop 的范围会退成空窗 → §1.6 拒绝态 `{ok:false, affected:{0,0,0}}`;
// 段短于 10ms 时段级「恢复自动」于是不受理(分段最小时长 J23 默认 400ms,够不着)。
//
// `kHopEps`:秒值是「样本 ÷ 采样率」算出来的,再除以 hopS 会带浮点残差。1e-6 hop
// = 10ns @10ms hop,比一个采样(48k 下 ~20.8µs)小三个量级 —— 只吃残差,吃不到真数据。
// 不带它的话「恰好 hop 对齐」的范围会因为 399.999999997 被上取到 400、下取到 399,
// 平白丢掉首尾各一个 hop。
struct AnalyzeHopWindow
{
    std::uint64_t firstHop = 0;
    std::uint64_t lastHop = 0; // 半开:窗 = [firstHop, lastHop)
    bool valid() const { return lastHop > firstHop; }
};

inline AnalyzeHopWindow analyzeHopWindow(double startS, double endS, double hopS)
{
    AnalyzeHopWindow w;
    if (!(hopS > 0.0) || !(endS > startS))
    {
        return w; // 空窗 → 调用方回 §1.6 拒绝态
    }
    constexpr double kHopEps = 1e-6;
    // 上限夹取(#161 复审【建议】⑦):`NaN` 已被上面的 `!(endS > startS)` 挡住,但
    // `+Inf` 不会 —— `lastFloor` 会是 inf,而 `double → uint64` 在值域外是 **UB**。
    // 桥面的 `givenNumber()` 只判「是不是数」不判有限性,`{startS:0, endS:Infinity}`
    // 透得进来(截断版有同样的洞,非本卡引入)。9e15 hop @10ms ≈ 285 万年,任何真
    // 时间线都够不着,夹在这里既堵 UB 又不改变任何可达输入的行为。
    constexpr double kMaxHop = 9.0e15;
    const double firstCeil = std::ceil(std::max(0.0, startS) / hopS - kHopEps);
    const double lastFloor = std::min(kMaxHop, std::floor(std::max(0.0, endS) / hopS + kHopEps));
    if (!(firstCeil <= kMaxHop) || !(lastFloor > firstCeil))
    {
        return w; // 范围窄于一个 hop(或起点本身已越界):没有完整 hop 可分析
    }
    w.firstHop = static_cast<std::uint64_t>(firstCeil < 0.0 ? 0.0 : firstCeil);
    w.lastHop = static_cast<std::uint64_t>(lastFloor < 0.0 ? 0.0 : lastFloor);
    return w;
}

} // namespace scvb::output
