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
    bool leadLock = false; // 分析期主唱锁:恒居中、不占槽
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
};

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
    int intervals = 0; // 全局区间数(§3.4)
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
};

using PipelineProgressFn = std::function<void(float)>; // 0..1
using PipelineCancelFn = std::function<bool()>; // true = 请中止

// 全链:每轨 VAD → 超长段谷切分 → 全局区间 → 逐区间指派 + 平衡 → 回写每轨段(pan/volDb)。
// features[t] 与 cfg.tracks[t] 同序(下标 = ch-1)。
PipelineResult runAnalysisPipeline(const std::array<PipelineTrackFeatures, kPipelineTracks>& features,
                                   const PipelineConfig& cfg, const PipelineProgressFn& onProgress = {},
                                   const PipelineCancelFn& shouldCancel = {});

} // namespace scvb::analysis
