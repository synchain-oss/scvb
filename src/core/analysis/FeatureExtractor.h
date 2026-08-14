// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "dsp/KWeighting.h"

namespace scvb::analysis
{

// 特征帧:对齐 ipc-contract §3 的 FeatFrame{f32 kw_ms; f32 peak;}。
// kw_ms = 该 hop 的 K-weighted mean-square(线性能量,BS.1770 多通道求和口径)。
// peak  = 该 hop 未加权信号各通道峰值最大值。
struct FeatFrame
{
    float kw_ms = 0.0f;
    float peak = 0.0f;
};

// 10ms hop 特征提取器(02-dsp-spec §1.3)。
// 每通道持独立 K-weighting 状态;kw_ms 按 BS.1770 多通道能量求和(非下混、非平均);
// peak 取各通道最大值。输入为平面声道指针,零分配(内存仅在 prepare 时分配)。
class FeatureExtractor
{
public:
    FeatureExtractor() = default;

    // 设置采样率与通道数(v1 产品用 1|2;实现允许 ≥1)。分配预热/累加缓冲并进入首个
    // run 的预热态(见 startRun)。
    void prepare(double sampleRate, int channels);

    // run/epoch 边界(transport 跳变):丢弃未完成的部分 hop、清零全部 K-weighting 状态,
    // 并以「run 首个完整 hop 的样本」为固定预热集 —— 先整体过一遍滤波器丢弃输出,
    // 重置累加器后再正式处理同一 hop(02-dsp-spec §1.4;与宿主块长无关)。
    void startRun();

    // 推入 numSamples 帧平面声道数据(channels[c] 指向第 c 路,长度 numSamples)。
    // 完成的 hop 帧写入 out(容量 maxFrames),返回本块完成的 hop 数(可能 > maxFrames,
    // 超出部分不写;调用方应按 numSamples/hopSize+1 预留)。out 可为 nullptr(不写帧)。
    int processBlock(const float* const* channels, int numSamples, FeatFrame* out, int maxFrames);

    int hopSize() const { return hopSize_; }
    double sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

private:
    FeatFrame makeFrame() const;
    void resetAccumulator();

    double sampleRate_ = 48000.0;
    int channels_ = 1;
    int hopSize_ = 480;

    std::vector<dsp::KWeighting> kw_;
    std::vector<double> sumSq_; // 每通道当前 hop 的平方和(double 累加)
    std::vector<float> preheatBuf_; // 平面布局 [c*hopSize_ + s]
    float peak_ = 0.0f;
    int count_ = 0; // 当前 hop 已累加样本数
    int preheatFill_ = 0; // 预热缓冲已填样本数
    bool preheating_ = false;
};

} // namespace scvb::analysis
