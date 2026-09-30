// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// [SL-357] 冒烟共用助手:**等状态回声到位**。
//
// 为什么要它:SL-357 把 preview mock 的状态回声改成了**默认异步**(与真桥同形 ——
// 写的回执先到、`scvb.state` 后到一拍 250ms)。于是所有「写完立刻读 store」的断言
// 都会读到**旧值**。那不是产品缺陷,是**判据本来就靠同步语义撑着** —— 真桥上它们
// 从来就不成立,只是同步 mock 替它们兜住了。
//
// ⚠ **不要用 `caps.syncStateEcho` 把这类用例弄绿**。那等于把这一套退回「preview 看不见
//   真桥时序」的老状态 —— 正是本卡要根除的东西。正确改法是在这里等那一帧。
//
// ⚠ **也不要写死 `await sleep(300)`**:那是把判据钉在 mock 当前的延迟常数上,
//   常数一改(或机器一慢)就变成偶发红。这里轮询到条件成立为止,并给一个明确的上界。
// =============================================================================

/** 轮询直到 `read()` 满足 `ok()`,或超时抛错(错误里带最后一次读到的值,便于定位)。 */
export async function awaitState(
    read,
    ok,
    label,
    timeoutMs = 3000,
    stepMs = 25,
) {
    const t0 = Date.now();
    let last;
    for (;;) {
        last = read();
        if (ok(last)) return last;
        if (Date.now() - t0 > timeoutMs) {
            throw new Error(
                `[awaitState] 等 ${label} 超时(${timeoutMs}ms);最后读到:` +
                    JSON.stringify(last),
            );
        }
        await new Promise((r) => setTimeout(r, stepMs));
    }
}

/** 便捷式:等 `seen` 里某个事件的最新载荷满足条件。 */
export function awaitSeen(seen, event, ok, label, timeoutMs) {
    return awaitState(
        () => seen.get(event + ":last"),
        (v) => v != null && ok(v),
        label || event,
        timeoutMs,
    );
}
