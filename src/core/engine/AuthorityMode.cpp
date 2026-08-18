// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/AuthorityMode.h"

namespace scvb::engine
{

AuthorityStep stepAuthority(AuthorityMode current, AuthorityEvent event, const AuthorityContext& ctx)
{
    AuthorityStep r;
    r.next = current;

    switch (event)
    {
    case AuthorityEvent::EnableOutput:
        // FOLLOW → ARMED(playing 且在区间则直接 → PRINT);DSP 目标切引擎值,走切换平滑。
        if (current == AuthorityMode::Follow)
        {
            r.authorityChanged = true;
            if (ctx.transportPlaying && ctx.playheadInRange)
            {
                r.next = AuthorityMode::Print;
                r.beginGesture = true;
            }
            else
            {
                r.next = AuthorityMode::Armed;
            }
        }
        // ARMED / PRINT 下 EnableOutput 为 no-op(已开)。
        break;

    case AuthorityEvent::DisableOutput:
        if (current == AuthorityMode::Armed)
        {
            r.next = AuthorityMode::Follow;
            r.authorityChanged = true;
        }
        else if (current == AuthorityMode::Print)
        {
            // 先 endAllGestures → HANDING_OVER 标志 → 下一次 50Hz timerCallback 读 host 值后 → FOLLOW。
            r.next = AuthorityMode::Follow;
            r.authorityChanged = true;
            r.endAllGestures = true;
            r.handOver = true;
        }
        // FOLLOW 下 DisableOutput 为 no-op(已关)。
        break;

    case AuthorityEvent::TransportPlay:
        // FOLLOW:无(DSP 继续读 host 参数);ARMED:区间内 → PRINT + beginGesture;PRINT:no-op。
        if (current == AuthorityMode::Armed && ctx.playheadInRange)
        {
            r.next = AuthorityMode::Print;
            r.beginGesture = true;
        }
        break;

    case AuthorityEvent::TransportStop:
        // PRINT:endAllGestures → ARMED;FOLLOW/ARMED:无。
        if (current == AuthorityMode::Print)
        {
            r.next = AuthorityMode::Armed;
            r.endAllGestures = true;
        }
        break;

    case AuthorityEvent::PlayheadEnterRange:
        // ARMED 且 playing → PRINT;其余无。
        if (current == AuthorityMode::Armed && ctx.transportPlaying)
        {
            r.next = AuthorityMode::Print;
            r.beginGesture = true;
        }
        break;

    case AuthorityEvent::PlayheadLeaveRange:
        // PRINT:离开 → endAllGestures → ARMED(播放继续);其余无。
        if (current == AuthorityMode::Print)
        {
            r.next = AuthorityMode::Armed;
            r.endAllGestures = true;
        }
        break;

    case AuthorityEvent::TimelineJump:
        // PRINT:endAllGestures → 若新位置仍在区间内立即重新 begin(= 新 pass);其余无。
        if (current == AuthorityMode::Print)
        {
            r.endAllGestures = true;
            if (ctx.playheadInRange)
                r.beginGesture = true;
            // 状态仍为 PRINT。
        }
        break;

    case AuthorityEvent::HostParamChanged:
        // 不改变状态;仅决定 host 参数是否直接驱动 DSP 目标(FOLLOW 才为 true,§2.2 / §3.5)。
        r.hostParamDrivesDsp = (current == AuthorityMode::Follow);
        break;

    case AuthorityEvent::VersionSwitchRequest:
        // FOLLOW/ARMED 允许;PRINT 拒绝(UI 灰掉,§5.3)。
        r.versionSwitchAccepted = (current != AuthorityMode::Print);
        break;

    case AuthorityEvent::EditorClosed:
        // 三态皆无状态变化;PRINT 继续打印(Timer 挂 Processor,§3.2)。
        break;

    case AuthorityEvent::ReleaseResources:
        // PRINT:GestureGuard 兜底 endAllGestures(R12);状态保持不变(析构路径无后续)。
        if (current == AuthorityMode::Print)
            r.endAllGestures = true;
        break;
    }

    return r;
}

} // namespace scvb::engine
