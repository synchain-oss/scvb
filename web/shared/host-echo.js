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
 * 只是把每秒 25 次的抖降成每秒几次。SL-251 当时**一个**窗口打天下,取 2000ms:600 是
 * 为 25Hz 批次续命设的,而宿主自动化的写入按曲线事件来,间隔可以是秒级。
 *
 * [SL-270] 用户实测,一个窗口打天下两头都不对:
 *   ① **停走之后徽标还挂着近两秒** —— 走带都停了,宿主早就不写了,这两秒纯属滞留;
 *   ② **快速起停会让徽标在播放中途消失**。它就是 ① 的另一面:按停的那一刻闩锁还剩
 *      一大截,用户马上又按播放,于是这一截**残余**在新的一段播放里走完 —— 徽标在
 *      「播放中」灭掉,要等宿主下一次真写才亮回来。看着像随机掉线,其实是上一段
 *      播放的尾巴。
 * 所以窗口按**走带态**分两档:停走用短的(滞留感没了,② 的残余也就不存在了),
 * 播放中用长的(宿主两次写之间的秒级间隔照旧盖得住,SL-251 的抖不回来)。
 * 判据源 `isPlaying` 是契约 §2.6 `scvb.playhead` 的字段,页面侧由 `app.js` 的订阅存进
 * `store.playhead`;**走带态未知时取宽档**,理由见 `hostEchoUseWideWindow` 的头注。
 */
export const HOST_ECHO_RELEASE_STOPPED_MS = 900;

/**
 * **播放中**的释放窗口(ms)。见上:播放中要盖住「宿主按曲线事件写、间隔秒级」那一段,
 * 不能用停走档的 900ms —— 那会把 SL-251 修掉的抖原样放回来。
 *
 * ⚠ 900 / 2500 这两个数是**估的**,不是真机量的:「宿主两次写之间的间隔」分布本机
 * 造不出来(mock 只有慢通道)。`app.js` 里那行 `console.debug` 读数就是为收这组数留的
 * —— 用户真机若仍见眨眼,拿那几行直接把窗口调对,不用再猜。
 */
export const HOST_ECHO_RELEASE_PLAYING_MS = 2500;

/**
 * 当前该用哪一档释放窗口。单列出来是因为 `app.js` 的 console 读数也要用同一个数 ——
 * 那行读数说的是「这个间隔会让徽标灭一下再亮」,拿错档位就是在说假话。
 *
 * @param {boolean} [wide] 该不该取宽档(见 `hostEchoUseWideWindow`:播放中 **或** 走带态未知)
 */
export function hostEchoReleaseMs(wide) {
    return wide ? HOST_ECHO_RELEASE_PLAYING_MS : HOST_ECHO_RELEASE_STOPPED_MS;
}

/**
 * 宿主自动化此刻是否正在驱动车道(= 该不该给出 hostEcho 的视觉提示)。
 *
 * **只看时间戳,不看 `params.hostEcho` 那一位**——理由见 `HOST_ECHO_RELEASE_STOPPED_MS` 的头注。
 * `hostEchoAt` 由 `app.js` **只在 true 帧**推进(false 帧不重置),所以这里的语义正好是
 * 「距最后一次『宿主在写』的确认过了多久」。
 *
 * @param {{hostEchoAt?: number}} params `store.params`
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 * @param {boolean} [wide] 取宽档还是窄档;[SL-270] 由 `hostEchoUseWideWindow` 给。
 *        ⚠ **省略即窄档(停走 900ms)**:按 SL-270 之前的老习惯两参调用,拿到的不是
 *        当年那个 2000ms 单窗口,而是 900ms —— 静默变窄,正是【建议】3 要防的那一幕。
 *        生产调用方(两个 tab + app.js 读数)都传了第三个实参,smoke-tab1 ⑧(b6) 钉着。
 */
export function hostEchoOn(params, nowMs, wide) {
    const at = (params && params.hostEchoAt) || 0;
    if (!at) return false; // 从未收到过 true 帧
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    return now - at < hostEchoReleaseMs(!!wide);
}

/**
 * `store` → 该用**宽**档释放窗口吗。抽出来的第一条理由:两个 tab **都**要把这个参数
 * 传给 `hostEchoOn`,而「从 store 的哪一处取走带态」写成两份就又是 SL-251 那个病灶
 * (同一个判断各存一份)的第四次发作。
 *
 * 第二条理由是它**不等于** `store.playhead.isPlaying`([PR 178 复审【建议】3]):
 * `store.playhead` 的初值是 `null`(`app.js` 的 `scvb.playhead` 订阅到达前它一直是),
 * 直接取 `isPlaying` 会把「**不在播放**」与「**还不知道走带态**」压成同一个 `false`,
 * 于是走带态未知时释放窗口从 SL-251 的 2000ms **收窄**到 900ms —— 而 SL-251 修的那个抖
 * 恰恰是靠「窗口比宿主两次写之间的间隔宽」压住的。窗口一收窄,抖就可能回来,且回来的
 * 形态与 SL-251 逐字相同(用户视角:徽标高频眨眼),很难再归因到本卡。
 * 所以**未知并进宽档**:宁可多亮一会儿,也不要把已修的抖放回来。函数名说的就是这件事
 * ——叫 `isPlaying` 而对 `playhead == null` 返回 true 会读成 bug。
 *
 * 判据写成「**只有明确停走才取窄档**」而不是「在播放才取宽档」:两者只在
 * `playhead` 在场却没有 `isPlaying` 时不同,而那一档该算**未知**、该走宽档。
 * 该形态在本仓的契约面上不可达(native 两侧 `OutputEditor.cpp:529` /
 * `MonitorEditor.cpp:278` 都无条件写这个字段,mock 的 `makePlayhead` 也恒带),
 * 所以这不是在修一个能复现的 bug —— 是让**代码与上面那句话逐字相符**:
 * 上一版写 `!!ph.isPlaying`,`{playhead:{}}` 会悄悄落到窄档,而头注说的是「未知 ⇒ 宽」。
 * 本卡整轮都在清理「注释比代码承诺得多」,这一处便按同一条尺子改代码而不是加脚注。
 * ([PR 178 复审第四轮 pr-agent 建议;不可达性我独立核过])
 *
 * @param {{playhead?: {isPlaying?: boolean}}} store
 * @returns {boolean} 播放中 **或** 走带态未知(含 `playhead` 无 `isPlaying`)⇒ true(宽档)
 */
export function hostEchoUseWideWindow(store) {
    const ph = store && store.playhead;
    // 只有 isPlaying === false 这一种「明确停走」取窄档;缺席/未知一律宽档。
    return !ph || ph.isPlaying !== false;
}
