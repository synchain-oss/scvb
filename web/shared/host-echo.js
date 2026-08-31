// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// hostEcho 显示态 —— Tab1 与 Tab2 共用一份判据([J93] / SL-251)
// =============================================================================
// `hostEcho`(契约 §2.2)回答的是「**此刻这些车道正被宿主自动化驱动**」:native 侧由
// `AutomationPrinter::HostEchoListener` 记录,且**排除打印器自写**(`isSelfWrite()` 首行
// 短路)—— 所以它说的不是「引擎在打印」,而是「你在跟宿主抢方向盘」(v5.1 实测 P1-D)。
//
// 抽到 shared 的理由与 `readback.js` 同一条,也是同一个病灶的第三次发作:同一个判断
// 原先在 `tab-master.js` 与 `tab-tracks.js` 里**各存一份逐字相同的实现**,于是
// SL-251 的闪烁两边都有,而用户只在 Tab1 看见。两处画同一个量,判据就该是同一份代码。
// =============================================================================

/**
 * 「距最近一次 hostEcho 批次」多久算**还新鲜**(ms)。
 * 打印期 `scvb.params` 是 25Hz(40ms 周期),600ms ≈ 15 帧余量。
 */
export const HOST_ECHO_FRESH_MS = 600;

/**
 * 显示态**熄灭**的延迟(ms)。与上面的新鲜度窗口**刻意分开**两个常量。
 *
 * [SL-251] 用户实测:播放时 Tab1 的卡「开始闪烁」。页面级探针实测四张卡各 1.3 次/秒
 * 翻转,真机更快。机理是这条判据原先长这样:
 *
 *     hostEcho && (now - hostEchoAt < 600)
 *
 * 第一项是**最近一帧的原始布尔**,被每一帧 `scvb.params` 覆写 —— 一帧 false 就把视觉态
 * 瞬间打掉,零缓冲。而帧本身是「**值变了才发**」(native `if (!any && !forceFull) return
 * true;`),于是「宿主还在写、我们这一段值恰好是平的」那几百毫秒里,既没有新帧来续命、
 * 又可能来一帧带着 false 的 —— 两条路都会误熄,下一次值一变又亮回来。
 *
 * 修法是**非对称闩锁**:亮**立刻**(收到一帧 true 就亮,信息不迟到),熄**延迟**
 * (距最后一次 true 帧超过本窗口才熄)。窗口必须比「宿主两次写之间的间隔」长,否则
 * 只是把每秒 25 次的抖降成每秒几次。取 2000ms 而不是沿用 600ms:600 是为 25Hz 批次
 * 续命设的,而宿主自动化的写入是**按曲线事件**来的,间隔可以是秒级。
 */
export const HOST_ECHO_RELEASE_MS = 2000;

/**
 * 宿主自动化此刻是否正在驱动车道(= 该不该给出 hostEcho 的视觉提示)。
 *
 * **只看时间戳,不看 `params.hostEcho` 那一位**——理由见 `HOST_ECHO_RELEASE_MS` 的头注。
 * `hostEchoAt` 由 `app.js` **只在 true 帧**推进(false 帧不重置),所以这里的语义正好是
 * 「距最后一次『宿主在写』的确认过了多久」。
 *
 * @param {{hostEchoAt?: number}} params `store.params`
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 */
export function hostEchoOn(params, nowMs) {
    const at = (params && params.hostEchoAt) || 0;
    if (!at) return false; // 从未收到过 true 帧
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    return now - at < HOST_ECHO_RELEASE_MS;
}
