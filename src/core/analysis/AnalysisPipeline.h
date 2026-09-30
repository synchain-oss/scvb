// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AnalysisPipeline —— 把已有的分析零件串成一条可跑的链(02 §2→§6 的编排层)。
//
// 零件本身早就齐了、也各有单测:EnergyVad(§2)、splitValleys(§3.2)、buildGlobalIntervals
// (§3.4)、assignInterval + solveBalanceWithFallback(§5/§6)。缺的一直是**把它们串起来的人** ——
// `handleAnalyze` 是 T29 占位:回一个 {ok:true, affected:{0,0,0}} 却从不置 analysis_run.running,
// 而 web 在受理回执后要等 running 翻真才把状态交回 state 驱动,于是「分析中」永久挂着,
// 永不出结果(v4 实测 P0-1)。本文件补上这条编排。
//
// 纯 C++17、JUCE-free、无 I/O、无全局状态:入参是特征快照 + 配置,出参是每轨段表。
// 可离线单测,也可在任意后台线程跑(实际调用方就是把它放在工作线程上,绝不占消息线程 ——
// 契约 §1.6「长耗时分析绝不阻塞消息线程」)。
//
// 取消与进度经回调注入:调用方给 shouldCancel(每段/每区间边界处轮询),给 onProgress(0..1)。
// 取消时立即返回**部分结果**并置 cancelled=true;调用方按契约丢弃即可。

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "analysis/AutoAssign.h"
#include "analysis/EnergyVad.h"
#include "analysis/LeadTimeline.h"
#include "analysis/Segmentation.h"

namespace scvb::analysis
{

inline constexpr int kPipelineTracks = 15;

// 每轨配置(取自 Output 的 runtime state;与 AutoAssign::TrackMeta 的配置面一一对位)。
struct PipelineTrackConfig
{
    bool enabled = true; // §1.15;false = 整轨不参与分析
    double priority = 5.0; // 0..10
    int pairId = 0; // 0 = 无配对
    bool leadLock = false; // 分析期主唱锁:恒居中、不占槽(全程;逐区间的 lead_select 另见 leadRuns)
    bool leadVolExempt = false; // 透传
    int freeze = 0; // bit0 = pan 冻结,bit1 = vol 冻结
    bool participateInAutoPan = true; // [J83] 未显式设置一律 true
    SourceChannels source = SourceChannels::Mono;
    double currentPan = 0.0; // 现值(manual / nonpart 轨保持它)
};

// 一次分析作业的全部输入配置。
struct PipelineConfig
{
    double sampleRate = 48000.0;
    int hopMs = 10; // feat 段几何常量
    std::int64_t rangeStartSample = 0; // 分析范围(半开)
    std::int64_t rangeEndSample = 0;
    VadParams vad;
    SegmentationParams segmentation;
    AutoAssignConfig assign;
    BalanceConfig balance;
    std::array<PipelineTrackConfig, kPipelineTracks> tracks{};

    // [SL-216 / J136] `lead_select` 在时间线上的记录(样本域,按 t0 升序、互不重叠;Output 在走带
    // 播放时逐块记下)。**只取 `automated` 的记录**(宿主写的),见下面 `leadFallback`。
    // [SL-570 / J167] 用法:求有效主唱 E(t)(`effectiveLeadPieces`),在 E 变值处(吸附后,`snapLeadSwitches`)
    // 把全局区间切开;子区间的主唱 n ∈ 1..15 且轨 n 在其中活跃 ⇒ 这一轨在**这个子区间**按主唱锁处理(并入集合 C:
    // 恒居中、不占槽,02 §5.2),其余声部据此排槽、平衡也把它当作居中的那一轨来算。
    std::vector<LeadRun> leadRuns;

    // [SL-545 / J143 + J143b] 点分析那一刻的 `lead_select`(0..15;0 = 无主唱)。自动化证据按**写入来源**判,
    // 不按记录里有几个不同的值判(J143b 取代 J143a):只有宿主写进来的值(`LeadRun::automated`)算自动化,
    // 插件界面上改的、撤销重做写回的、载入工程恢复的都不算 —— 用户在插件里拧旋钮再试听一段,试听那段
    // 记下的值不会让记录「看起来像自动化」。
    //   · 计算窗里一条宿主记录都没有 ⇒ **整窗**取它(记录不看);
    //   · 有 ⇒ 宿主记录盖到的地方按记录,盖不到的地方取它(宿主记录优先)。
    // 两支是同一个表达式:`effectiveLeadPieces(automatedLeadRuns(leadRuns), 窗, leadFallback)` —— 一条都没有时
    // 整窗落到回落值。[SL-570 / J167] E 全程一个值 ⇒ 没有切换点 ⇒ 不切区间,与 J167 之前逐位相同;
    // 为 0 且没有宿主记录 ⇒ 与 SL-216 之前逐位相同。
    int leadFallback = 0;
};

// [SL-570 / J167] 按主唱切开后的一个子区间:全局区间 `interval`(下标)里的 [t0, t1),主唱为 `lead`。
// 子区间只切时间,活跃轨集合沿用它所在的那个全局区间(每条活跃轨都盖满整个全局区间,切开后仍盖满)。
struct LeadSubInterval
{
    std::size_t interval = 0;
    std::int64_t t0 = 0;
    std::int64_t t1 = 0;
    int lead = 0;
};

// [SL-570 / J167] 用吸附后的有效主唱分段 `pieces`(首尾相接,盖满 [intervals.front().t0, intervals.back().t1))
// 把每个全局区间切成子区间,按时间顺序返回。某个区间里没有切换点 ⇒ 它原样成为一个子区间(t0/t1 不变)。
// 片段盖不到的那几截(调用方违约)按 `fallback`,到下一个片段起点为止。线性:区间与片段各扫一遍。
std::vector<LeadSubInterval> splitIntervalsAtLeadSwitches(const std::vector<GlobalInterval>& intervals,
                                                          const std::vector<LeadPiece>& pieces, int fallback);

// K 加权均方(线性能量)→ LUFS(BS.1770 的 −0.691 偏置;静音回 −120 替身)。
// §2.8 `loudnessLufs` 与 `AnalysisSegment.loudnessLufs` 的**唯一换算口径**:
// 流水线产段时用它,桥面 emit 时按 FEAT 重算也用它([SL-257] 真值化)。
double lufsFromMeanKw(double meanKwLinear);

// [J146] 谷切分的 ℓ 包络:逐 hop `frameLoudnessDb(kw)`(ℓ 的唯一口径,见 EnergyVad.h)。out 先清空。
void envelopeDbInto(const float* kwMs, std::size_t n, std::vector<float>& out);

// [J146] S1 单轨的后一半:VAD 段里超过 `seg.maxSegmentS` 的按谷切分(02 §3.2),短段原样保留;
// 返回 hop 域段序列(绝对时间线)。**分析流水线与拖动档预览(契约 §1.18)共用这一份**。
// `kwMs[0..n)` 对应绝对 hop `firstHop..firstHop+n`;`envDb` 为空时按需由 `envelopeDbInto` 现建
// (流水线的惰性口径),非空则视为已建好(预览传缓存)。任一段找不到自然切点时置 *noNaturalCut。
std::vector<VadSegment> splitLongVadSegments(const std::vector<VadSegment>& vadSegs, std::int64_t firstHop,
                                             const SegmentationParams& seg, double hopSec, const float* kwMs,
                                             std::size_t n, std::vector<float>& envDb, bool* noNaturalCut);

// 每轨的特征切片(调用方从 FrameStore 里按范围抠出来的快照:kw 线性能量 + 峰值)。
// 长度必须一致 = 范围内 hop 数;未覆盖的 hop 由调用方填静音(kw=0)。
// covered 标记哪些 hop 真有采集数据 —— 全 false 的轨直接跳过(不产生段)。
struct PipelineTrackFeatures
{
    std::vector<float> kwMs;
    std::vector<float> peak;
    std::vector<std::uint8_t> covered;
    bool anyCovered = false;
};

struct PipelineResult
{
    std::array<std::vector<AnalysisSegment>, kPipelineTracks> segments{};
    int intervals = 0; // 全局区间数(§3.4);[SL-570] 按主唱切开的子区间(`LeadSubInterval`)不计入
    int tracksTouched = 0; // 产出了段的轨数
    bool cancelled = false;
    std::vector<std::string> warnings; // VAD 守卫 / 宽度不足 / 平衡回退

    // [SL-206] 逐 hop 的 VAD **后验** p[k] ∈ [0,1](§2.4 的截断后验),下标 0 = firstHop。
    //
    // 为什么要带出来:`EnergyVad` 早就支持 `posteriorOut`,`FrameStore` 早就有 `setVadP`,
    // `waveformOf` 早就按 `vadP(h) > 127` 给瓦片算 `vad` 列,泳道也早就画绿线 —— 唯独**中间
    // 这一段没人接**:管线调 `runEnergyVad` 时第五参传的是 `nullptr`,后验算完就地扔掉,
    // 于是 `vadP` 全仓**没有生产者**,真机上恒 0、绿线一次都没画出来过。
    // (web-preview 的 mock 自己算了一份,所以 preview 里一直看得见 —— 这条正是「mock 盖住真机」
    //  的第三次,用例必须与 native 口径对拍。)
    //
    // 只对**本次真参与分析**的轨填(未启用 / 无覆盖的轨留空 vector),与 `segments` 同口径。
    std::array<std::vector<float>, kPipelineTracks> vadPosterior{};
    std::int64_t firstHop = 0; // vadPosterior[t][0] 对应的**绝对** hop 序号(写回 FrameStore 要它)

    // [SL-284] 本次分析里**最坏**的平衡回退级(§6.4 回退链,取值 1..4);没有任何区间跑过
    // 平衡时留 **0**(空段表/全部区间被跳过 —— 0 不是合法级,读侧据此区分「没跑过」与「跑了且收敛」)。
    //
    // 为什么要带出来:`solveBalanceWithFallback` 早就逐级填 `BalanceResult::fallbackLevel`,
    // 但那个值在下面的区间循环里**用完就扔** —— 于是「首趟 solveBalance 到底收敛没有」
    // 在管线之外**没有任何观察点**。与 `vadPosterior` 是同一形态:两头都有,中间没人接。
    //
    // ⚠ **为什么取 max 而不是单值/末值**:`solveBalanceWithFallback` 是**逐区间**调用的,
    // 每个区间各有自己的级。「首趟收敛」唯一有意义的表述是**没有任何区间掉出 level 1**,
    // 即 `maxFallbackLevel == 1`;取末值会被最后一个收敛的区间盖掉中间掉过级的事实。
    int maxFallbackLevel = 0;
};

using PipelineProgressFn = std::function<void(float)>; // 0..1
using PipelineCancelFn = std::function<bool()>; // true = 请中止

// 全链:每轨 VAD → 超长段谷切分 → 全局区间 → 逐区间指派 + 平衡 → 回写每轨段(pan/volDb)。
// features[t] 与 cfg.tracks[t] 同序(下标 = ch-1)。
PipelineResult runAnalysisPipeline(const std::array<PipelineTrackFeatures, kPipelineTracks>& features,
                                   const PipelineConfig& cfg, const PipelineProgressFn& onProgress = {},
                                   const PipelineCancelFn& shouldCancel = {});

} // namespace scvb::analysis
