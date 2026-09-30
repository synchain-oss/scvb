// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FreezeBits —— 每轨 `v{v}_t{t:02d}_freeze` 原始参数值 → 冻结位([J65] int 0-3,bit0=pan / bit1=vol)
// 的**唯一**解码口径。纯 C++17,无 JUCE(ADR-011)。
//
// 为什么必须只有一份:「谁被冻结」由两条互不相干的代码路径各自判 —— 音频线程的取值仲裁
// (DspArbiter,决定读曲线还是读参数面)与消息线程的 setTrackManual / ctrl 广播 / 分析入参
// (决定写曲线还是写参数面)。两边一旦分叉,就会出现「setTrackManual 认为该维度未冻结去改曲线,
// 而 arbiter 认为已冻结去读参数面」这种错位 —— 正是 [J85] 要根治的那一类。
// 原先是两份口径:core 侧 `static_cast<int>(raw)`(截断)、output 侧 `jlimit(0,3,roundToInt(raw))`
// (四舍五入 + 钳制)。freeze 当前是 AudioParameterInt,raw 恒为精确整数 float,两者等价;但存储
// 形态一旦动一下(比如改成带插值的 float),x.5 / x.999 上就会分叉(PR #106 复审建议⑥)。
//
// 落 core 而不是 `scvb::params`(`src/output/OutputParams.h`)的理由:那个头引 juce_audio_processors,
// 而 scvb_core 必须离线可测、不得引插件宿主层(CLAUDE.md §9)—— DspArbiter 就在 core 里。
//
// 口径 = **四舍五入后钳到 [0,3]**(取 output 侧那一份)。按位与截高位会把 4 解成「两维都没冻」,
// 钳制解成「两维都冻」—— 后者是保守的那一边(宁可多冻一维播静态值,也不要让引擎去驱动一条
// 用户以为已经冻住的轨)。NaN 与负脏值(含 -Inf)没有可解的语义,回落 0 = 未冻结,与「参数未
// 接线」同款默认;+Inf **不单开分支**,走上面那条越界规则解成 3(保守多冻一维)—— 单测
// SERVICE-10 按此断言,改这里必须同改它。
//
// 同一口径在 UI 与 mock 侧各有一份实现(`web/output/tab-tracks.js` 的 `freezeBits`、
// `web-preview/mock/juce-bridge-mock.js` 的 `frzBits`),**三侧必须同改** —— 跨语言没法共用一份
// 代码,只能靠这条注释与三处交叉引用把它们钉在一起。

namespace scvb::engine
{

// raw = APVTS getRawParameterValue() 的**去归一化**工程值(freeze ∈ [0,3])。
// 分支顺序先把 NaN/±Inf/越界全挡掉,再做窄域 float→int 转换 —— 越界值直接 static_cast 是 UB。
constexpr int freezeBitsOf(float raw) noexcept
{
    if (!(raw == raw)) // NaN(constexpr 可用;std::isnan 到 C++23 才 constexpr)
        return 0;
    if (raw <= 0.0f) // 含 -Inf 与负脏值
        return 0;
    if (raw >= 3.0f) // 含 +Inf
        return 3;
    return static_cast<int>(raw + 0.5f); // 此处 raw ∈ (0,3) → 结果 ∈ [0,3],转换安全
}

// 该维度是否被冻结(isPan=true → bit0,false → bit1)。调用方拿到的都是同一份判定。
constexpr bool freezeHasDim(int bits, bool isPan) noexcept
{
    return (bits & (isPan ? 1 : 2)) != 0;
}

} // namespace scvb::engine
