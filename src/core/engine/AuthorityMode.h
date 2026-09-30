// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AuthorityMode:follow/write 仲裁状态机(03 §2.1 / §2.2 事件-转移表)。
// 三态 FOLLOW/ARMED/PRINT;每格转移按 03 §2.2 逐格落地。纯 C++17,无 JUCE。
// gesture 三段式属 Output/T17 侧:本状态机只产出「动作标志」,不触碰宿主。
namespace scvb::engine
{

enum class AuthorityMode : int
{
    Follow = 0, // 权威 = 宿主参数;零 gesture、零写入
    Armed = 1, // 权威 = 引擎曲线;零 gesture、零写入(transport 停止或区间外)
    Print = 2, // 权威 = 引擎曲线;打印器 → gesture → host
};

enum class AuthorityEvent : int
{
    EnableOutput, // 用户开 Output(output_enabled 由 false→true)
    DisableOutput, // 用户关 Output(output_enabled 由 true→false)
    TransportPlay, // transport 开始播放(本事件仅在区间内触发,见 §2.2 行注)
    TransportStop, // transport 停止
    PlayheadEnterRange, // playhead 进入已分析区间
    PlayheadLeaveRange, // playhead 离开已分析区间
    TimelineJump, // 时间线跳变(loop 回跳,epoch+1)
    HostParamChanged, // host 参数回调(仅影响 DSP 目标 / UI,见 §3.5)
    VersionSwitchRequest, // 版本切换请求
    EditorClosed, // Editor 关闭
    ReleaseResources, // releaseResources / 析构(R12 GestureGuard 兜底)
};

// 事件上下文:部分事件的结果依赖 transport 播放态与 playhead 是否在区间内。
struct AuthorityContext
{
    bool transportPlaying = false;
    bool playheadInRange = false;
};

// 单步转移结果:next = 新状态;其余为动作标志,供 Output 侧解释(写值 / gesture / 平滑换档)。
struct AuthorityStep
{
    AuthorityMode next = AuthorityMode::Follow;
    bool authorityChanged = false; // 权威在 host↔engine 之间翻转(触发 §2.4 30ms 切换平滑)
    bool beginGesture = false; // 需对活跃车道 beginChangeGesture
    bool endAllGestures = false; // 需 endAllGestures(幂等)
    bool handOver = false; // PRINT→FOLLOW 权威交接:先读 host 当前参数值再切 FOLLOW(§3.5)
    bool versionSwitchAccepted = true; // version 切换请求是否被接受(PRINT 拒绝)
    bool hostParamDrivesDsp = false; // host 参数回调是否直接更新 DSP 目标(FOLLOW 才为 true)
};

// 纯函数:给定当前态、事件与上下文,返回转移结果。覆盖 §2.2 全部 10 行 × 3 列。
AuthorityStep stepAuthority(AuthorityMode current, AuthorityEvent event, const AuthorityContext& ctx);

// 便捷谓词:ARMED/PRINT 均为「引擎权威」。
inline bool isEngineAuthority(AuthorityMode m) noexcept
{
    return m == AuthorityMode::Armed || m == AuthorityMode::Print;
}

} // namespace scvb::engine
