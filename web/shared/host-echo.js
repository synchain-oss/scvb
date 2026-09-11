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
 * 所以窗口按**走带态**分两档:停走用短的,播放中用长的(宿主两次写之间的秒级间隔
 * 照旧盖得住,SL-251 的抖不回来)。
 * 判据源 `isPlaying` 是契约 §2.6 `scvb.playhead` 的字段,页面侧由 `app.js` 的订阅存进
 * `store.playhead`;**走带态未知时取宽档**,理由见 `hostEchoUseWideWindow` 的头注。
 *
 * [SL-356] 用户实测(v5.6.7)分两档只中了一半:「停播后徽标及时熄灭没问题;但快速起停
 * 几次,播放中徽标还是会中途消失」。① 修掉了,② **没有**。
 * 上面那段 [SL-270] 追述照原样留着(它是当时的判断记录),但**别拿它当 ② 的病因**:
 *   • 「② 是 ① 的另一面 / 是上一段播放的尾巴」是当时的**诊断**,分两档之后 ② 仍在,
 *     所以它至多解释了一部分;
 *   • 「停走用短的 ⇒ ② 的残余也就不存在了」这半句已按实测**从上一段删掉**,别再照它推论。
 * ② 的真因不在窗口宽窄,而在**走带态自己零迟滞**:详见 `HOST_ECHO_TRANSPORT_HOLD_MS`。
 */
export const HOST_ECHO_RELEASE_STOPPED_MS = 900;

/**
 * [SL-394] **播放档释放窗口已删除**(原 `HOST_ECHO_RELEASE_PLAYING_MS = 2500`)。
 *
 * 用户 v5.6.12 回验 B29:自动化长时间基本无变化(比如一整句)时,徽标**中途不显示**。
 * 根因不是窗口不够宽,而是**「窗口」这条思路的前提被 native 否掉了**:
 * `OutputEditor::emitParams` 里 `if (!any && !forceFull) return true;` —— **值没变就不发帧**。
 * 于是平直段里根本没有新帧来推进 `hostEchoAt`,窗口取多宽都会到期。
 * 前三次(SL-251 闪烁 → SL-270 拆两档 → SL-356 走带去抖)都在调窗口宽窄,
 * 这是同一族的第四次,也是第一次换掉前提。
 *
 * 用户裁定(晚出,2026-09-10):「改成跟变暗那里一样的思路,播放就出现」。新口径三段:
 *   · **播放中 ∧ 本次播放里观测到过至少一次宿主写 ⇒ 常亮到停止**(不看窗口,不会中途熄);
 *   · **停止 ⇒ 沿用 `HOST_ECHO_RELEASE_STOPPED_MS`(900ms)**,≤1s 熄灭;
 *   · **未播放 ⇒ 沿用旧行为**(写后 900ms 内亮)—— 保住 05 §1.4「ARMED/PRINT 下宿主回吐」
 *     那一档:ARMED 按 03 §2.2 含停走,一律不亮会与那句冲突。
 * 判据落在 `hostEchoVisible()`。
 *
 * **这个常量整条删掉而不是留着不用**:留一个没人读的窗口常量,就是 SL-357 的
 * `slowStateEcho` 那种「谁都不看却像在起作用的开关」—— 翻它一格都不红。
 */

/**
 * 走带态**去抖**窗口(ms):`isPlaying` 掉到 false 之后要**连续**停走这么久,释放窗口
 * 才真的收窄到停走档。
 *
 * [SL-356] 用户实测(v5.6.7):「停播后徽标及时熄灭没问题;但快速起停几次,播放中徽标
 * 还是会中途消失」。SL-270 只给了**熄侧**迟滞(`hostEchoAt` 那一头:亮立刻、熄延迟),
 * **走带态这一头一点迟滞都没有** —— `hostEchoUseWideWindow` 上一版是「当前这一帧
 * `isPlaying`」的纯函数,一帧 false 就把释放窗口从 2500 瞬间收到 900。于是只要距最后
 * 一次宿主写入已经过了 900ms(播放中这完全正常 —— 窗口取 2500 就是为了盖住「宿主按
 * 曲线事件写、间隔秒级」那一段),那一帧 false 到达的**当拍**徽标就熄。
 *
 * 「一帧 false」有两条都走得到的来路,两条都不代表走带真的停了:
 *   ① 用户真的快速起停 —— 停的那几十/几百毫秒里 `isPlaying` 本来就是 false;
 *   ② 宿主某个块给不出 playhead —— `ScvbOutputAudioProcessor::processBlock` 里
 *      `playing` 初值就是 `false`,只在 `getPosition()` 有值时才被覆写
 *      (`src/output/OutputProcessor.cpp` 的 `if (p.hasValue())` 那段),
 *      取不到位置的那一块照样会发一帧 `isPlaying:false`。
 *
 * 修法 = 给走带态本身补一层迟滞:**只有连续停走超过本窗口才收窄**。短促的 false 帧
 * 于是既不缩窗口、也不熄徽标,而「真停」那一半一个字节没动。
 *
 * ⚠ 本常量必须**小于** `HOST_ECHO_RELEASE_STOPPED_MS` —— 这是「停播后仍及时熄灭」
 * (用户已确认这一半是对的,不许修坏)的算术保证:真停在 T、最后一帧宿主写入在
 * `at ≤ T`,熄灭时刻 = `max(T + 本窗口, at + 900)` ≤ `T + 900`。
 * 判据 = `web-preview/tests/smoke-tab1-interactions.mjs` ⑧(a15)。
 */
export const HOST_ECHO_TRANSPORT_HOLD_MS = 500;

/**
 * 当前该用哪一档释放窗口。单列出来是因为 `app.js` 的 console 读数也要用同一个数 ——
 * 那行读数说的是「这个间隔会让徽标灭一下再亮」,拿错档位就是在说假话。
 *
 * @param {boolean} [wide] 该不该取宽档(见 `hostEchoUseWideWindow`:播放中 **或** 走带态未知)
 */
export function hostEchoReleaseMs(wide) {
    // [SL-394] 播放档窗口已删,两档合一:释放窗口**只剩** 900ms 这一档。
    // 形参 `wide` 保留但不再影响结果 —— 播放中该不该亮由 `hostEchoVisible()` 的
    // **播放期闩锁**回答,不再由窗口宽窄回答。保留形参是为了让 `app.js` 那行读数
    // 与调用点不必同一推里一起改形状;它是**有意的恒等**,不是漏改。
    void wide;
    return HOST_ECHO_RELEASE_STOPPED_MS;
}

/**
 * [SL-394 复审] 走带态**未知**吗 —— 「未知」的唯一定义,两个消费者共用。
 *
 * 未知 = 还没有一帧 `scvb.playhead` 到过页面(`store.playhead` 为 null),**或者**
 * 到了却没带 `isPlaying`(契约面上不可达 —— native 两侧与 mock 都无条件写该字段 ——
 * 但它是「未知」不是「停走」,判据要与这句话逐字相符)。
 *
 * 抽出来的理由:`hostEchoUseWideWindow` 与 `hostEchoVisible` 的 ⓪ 分支**都**要问这件事,
 * 而两处各写一遍就是这一族缺陷五次复发的形状(同一个判断各存一份)。
 */
export function transportUnknown(store) {
    const ph = store && store.playhead;
    return !ph || typeof ph.isPlaying !== "boolean";
}

/**
 * [SL-394] 徽标 / 灰显此刻该不该显示 —— **两个 tab 唯一的消费入口**。
 *
 * 三段口径(用户 2026-09-10 裁定,理由见 `HOST_ECHO_RELEASE_PLAYING_MS` 那段的墓志铭):
 *   ① **本次播放里观测到过宿主写 ⇒ 常亮到停止**。判据是 `hostEchoAt >= playbackStartedAt`,
 *      **没有窗口** —— 平直段里 native 不发帧也不会熄,这正是 B29 要修的那一幕。
 *   ② 播放中但**本次播放还没观测到写** ⇒ 回落 900ms 窗口。
 *      不直接返回 false 是为了「不闪」:宿主在 ARMED 里写了一笔、用户随即按播放,
 *      直接 false 会让徽标在起播那一刻先灭一下、等宿主下一次写再亮。
 *   ③ 停走 ⇒ 900ms 窗口(用户已确认「停播后及时熄灭」这一半是对的,一个字节没动)。
 *
 * ⚠ **粒度限制,别把注释写得比代码强**:契约 §2.2 的 `hostEcho` 是**整批一个布尔**,
 * 不是逐参数的。所以「没观测到宿主写的**参数**不亮」在本实现里只能做到**整批**——
 * 从未收到过 true 帧(`at === 0`)则全灭,一旦收到则该批次覆盖的卡一起亮。
 * 要做到逐参数,得先给 §2.2 加逐 id 的回吐位,那是契约变更,不在本卡。
 *
 * 「在不在本次播放里」用 `hostEchoUseWideWindow()`(= 播放中 / 走带态未知 / 刚停走还没过
 * 去抖窗)而**不是**裸的 `playhead.isPlaying`:裸帧判定会让播放中随便一帧 `isPlaying:false`
 * (快速起停途中,或宿主某个块给不出位置)当场结束「本次播放」,徽标当拍熄 ——
 * 那正是 SL-356 修掉的那一幕换个形态复活。两处共用同一条判据,不另立第二份。
 *
 * @param {{params?:{hostEchoAt?:number}, playhead?:object, playingAt?:number,
 *          playbackStartedAt?:number}} store
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 */
export function hostEchoVisible(store, nowMs) {
    const at = (store && store.params && store.params.hostEchoAt) || 0;
    if (!at) return false; // 从未收到过「宿主在写」的确认 ⇒ 不亮
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    // ⓪ **一帧 `scvb.playhead` 都还没到**:走带态未知,按 [SL-270 / PR 178] 那条
    // 「未知**不当**停走处理」办 —— 收到过宿主写就亮。此前这一档靠「未知取宽档
    // (2500ms)」实现,而宽档已随本卡删掉,所以要在这里显式留一条,否则未知态会
    // 静默退到 900ms,正是 PR 178【建议】3 提防的那一幕(窗口一收窄,SL-251 的抖
    // 就可能以逐字相同的形态回来,且很难再归因到本卡)。
    // 边界照实说:`playhead` 恒不到达时(宿主不给时间线),一次宿主写会让徽标**一直**亮着
    // —— 旧实现是亮 2500ms。这一档在契约面上近乎不可达(noTimeline 时输出直通、
    // 打印头不写,`hostEcho` 不会为真),但它**不是**「不可能」,别把注释写成保证。
    // 实际时长由第一帧 30Hz `scvb.playhead` 终结(约 33ms)。
    if (transportUnknown(store)) return true;
    const start = (store && store.playbackStartedAt) || 0;
    // ① 本次播放里写过 ⇒ 常亮(无窗口)。
    //
    // ⚠ 门限是 `start - HOST_ECHO_RELEASE_STOPPED_MS` 而**不是**裸的 `start`:
    // `scvb.params` 与 `scvb.playhead` 是**两条独立的事件流**,谁先到达没有保证。
    // 用户按下播放的那一刻宿主就开始写,而本段播放的起点要等第一帧 `playhead` 才落下来
    // —— 裸 `start` 会把「早到几十毫秒的那一笔写」判成「上一段播放的事」,于是
    // **每次起播都先灭一下**,直到宿主下一笔写才亮。用户裁定里「不闪」那一条当场破。
    //
    // 宽限量**取的就是停走档那个常量**(不是走带去抖那一档,也不是新造的数)。
    // 两个量回答的是**不同的问题**:`HOST_ECHO_TRANSPORT_HOLD_MS` 回答「走带抖动该吸收
    // 多久」,而这里问的是「一笔写多久之内还算数」—— 后者只有一把尺子,就是停走档。
    // ⚠ [SL-394 复审②] 本段原先写着「复用 `HOST_ECHO_TRANSPORT_HOLD_MS`(走带去抖那一档)」,
    // 那是取齐之前的旧文,与下一行代码**直接打对台**,而且与 (a23) 那一格的注释互相矛盾。
    // 取齐的理由不只是「同一把尺子好看」:宽限 500 而回落窗口 900 会留下 [500, 900) 这条
    // 400ms 带 —— 停走态写一笔、0.5–0.9s 后按下播放,那一笔既出了宽限(不算本次播放里
    // 写过)、又还在窗口内(按播放前的判据本该还亮),于是起播**当拍**熄一下。
    //
    // 别把这条宽限读成「窗口回来了」:它只挪动**这一段播放的左边界**,
    // 不给徽标任何到期时间。
    const inThisPlayback = at >= start - HOST_ECHO_RELEASE_STOPPED_MS;
    if (start && inThisPlayback && hostEchoUseWideWindow(store, now)) {
        return true;
    }
    // ②③ 其余一律走停走档窗口 —— **委托给 `hostEchoOn()`**,不在这里再写一遍
    // `now - at < 窗口`。两个理由:
    //   · 窗口判据只该有一份(这一族缺陷五次复发的根就是「同一个判断各存一份」);
    //   · 否则 `hostEchoOn` 在生产侧就**零消费者**了 —— 一个只有用例在调的导出函数,
    //     与本卡刚删掉的那个没人读的窗口常量是同一种死代码,而 (a1..a7)(a12) 那几格
    //     会退化成「测一个没人用的零件」。
    return hostEchoOn(store && store.params, now, false);
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
 * @param {boolean} [wide] **[SL-394 起已失效,保留形参只为不逼调用点同一推里改形状]**
 *        `hostEchoReleaseMs()` 现在恒等返回 900(播放档窗口已删,播放期该不该亮改由
 *        `hostEchoVisible()` 的**闩锁**回答),所以传不传、传真传假都不改变结果。
 *        ⚠ 这里原先写着「**省略即窄档**:拿到的不是当年那个 2000ms 单窗口,而是 900ms ——
 *        静默变窄,正是【建议】3 要防的那一幕 / 生产调用方(两个 tab + app.js 读数)都传了
 *        第三个实参,smoke-tab1 ⑧(b6) 钉着」。**[SL-394 复测:两半现在都不成立]**
 *        —— 分档没了,「省略就变窄」不可能发生;而 b6 早就不钉「第三个实参传没传」,它现在的
 *        断言是「两个 tab 的源码里不得出现 `hostEchoOn(`」(那一段自己写了这次改口),
 *        而全仓**唯一**残留的 `hostEchoOn(` 调用就在本文件下面,传的正是 `false`。
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
 * 判据写成「**不是明确停走就取宽档**」而不是「在播放才取宽档」:两者只在
 * `playhead` 在场却没有 `isPlaying` 时不同,而那一档该算**未知**、该走宽档。
 * 该形态在本仓的契约面上不可达(native 两侧 `OutputEditor.cpp:529` /
 * `MonitorEditor.cpp:278` 都无条件写这个字段,mock 的 `makePlayhead` 也恒带),
 * 所以这不是在修一个能复现的 bug —— 是让**代码与上面那句话逐字相符**:
 * 上一版写 `!!ph.isPlaying`,`{playhead:{}}` 会悄悄落到窄档,而头注说的是「未知 ⇒ 宽」。
 * 本卡整轮都在清理「注释比代码承诺得多」,这一处便按同一条尺子改代码而不是加脚注。
 * ([PR 178 复审第四轮 pr-agent 建议;不可达性我独立核过])
 *
 * [SL-356] 「明确停走」还**不够**收窄:一帧 false 就收窄的话,播放中随便一帧
 * `isPlaying:false`(快速起停途中、或宿主某个块给不出位置)都会把徽标当拍打掉。
 * 所以窄档还要再过一道**去抖**:连续停走超过 `HOST_ECHO_TRANSPORT_HOLD_MS` 才算数。
 * 记账那一头是 `transportPlayingAt()`(由 `app.js` 的 `scvb.playhead` 订阅逐帧调用,
 * 存进 `store.playingAt`)—— 这里只读它,不自己攒状态。
 *
 * `store.playingAt` 为 0(= 本会话从未观测到「非停走」的一帧)时**直接取窄档**:
 * 那不是「不知道」,而是「从页面装上起就一直停着」,窄档正是对的那一档。
 * (与上面那条「未知 ⇒ 宽档」不冲突:走带态未知在第一条分支就返回了,根本走不到这里。)
 *
 * @param {{playhead?: {isPlaying?: boolean}, playingAt?: number}} store
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 * @returns {boolean} 播放中 / 走带态未知(含 `playhead` 无 `isPlaying`)/ 刚停走还没过
 *          去抖窗口 ⇒ true(宽档)
 */
export function hostEchoUseWideWindow(store, nowMs) {
    const ph = store && store.playhead;
    // 缺席/未知一律宽档;只有 isPlaying === false 这一种「明确停走」才有资格进窄档。
    // [SL-394 复审] 「未知」这半条抽成 `transportUnknown()`,与 `hostEchoVisible` 的 ⓪
    // 分支**共用同一份**;两处各写一遍正是这一族缺陷反复复发的形状。
    if (transportUnknown(store) || ph.isPlaying !== false) return true;
    // [SL-356] 有资格 ≠ 就收窄:还要连续停走满去抖窗口。
    const at = (store && store.playingAt) || 0;
    if (!at) return false; // 从未观测到非停走的一帧 ⇒ 一直停着 ⇒ 窄档
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    return now - at < HOST_ECHO_TRANSPORT_HOLD_MS;
}

/**
 * 走带态去抖的**记账**:收到一帧 §2.6 `scvb.playhead` 之后,「最近一次**不是明确停走**
 * 的时刻」该记成多少。
 *
 * 抽成纯函数而不是在 `app.js` 里写一行三目,是为了给「怎么记」留一个能被用例直接钉住的
 * 落点:记反了(停走帧也刷新)走带态就**永远进不了窄档**,停播之后徽标不再按时熄灭
 * (smoke-tab1 ⑧(a19) 注入实测 **[900, -1, -1]ms** —— 后两格压根没熄)。
 * ⚠ **[SL-394 复测:这一条的读数与形态都翻了向。]** 旧记录写「2520 / 2220 / 1620ms、
 * 是『熄得晚』不是『永不熄』」—— 那是**播放档窗口还在**的年代量的:`hostEchoTimerWide`
 * 那一拍(at+2550)会兜住它。窗口随 SL-394 删除之后没有那一拍了,退化形态就是**永不熄**;
 * 唯一还绿的那一格是恰好卡在 `<= 900` 等号上的第一格。
 *
 * 「不是明确停走」与 `hostEchoUseWideWindow` 第一条分支**同一条判据**(播放中或走带态
 * 未知),两处必须同口径:记账那头把「未知」算成停走的话,一次 `playhead` 缺字段就会
 * 冻住时间戳,去抖窗口当场到期。
 *
 * @param {number} prevAt 上一次的记账值(`store.playingAt`)
 * @param {{isPlaying?: boolean}|null} playhead 本帧 §2.6 载荷
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 * @returns {number} 播放中 **或** 走带态未知 ⇒ 本帧时刻;明确停走 ⇒ 原样沿用 `prevAt`
 */
export function transportPlayingAt(prevAt, playhead, nowMs) {
    if (playhead && playhead.isPlaying === false) return prevAt || 0;
    return Number.isFinite(nowMs) ? nowMs : Date.now();
}

/**
 * [SL-394] **本次播放的起点**该记成多少 —— 与 `transportPlayingAt()` 成对的记账纯函数。
 *
 * 「本次播放」的边界必须与 `hostEchoUseWideWindow()` **同一条判据**(去抖后的走带态),
 * 否则两头各说各的:那一头认为还在播放、这一头已经开了新一段,`at >= start` 就会
 * 在一次无害的 `isPlaying:false` 帧之后变成 false,徽标当拍熄 —— SL-356 那一幕复活。
 *
 * 判据(三条,按顺序):
 *   · 本帧**明确停走** ⇒ 原样沿用 `prevStartedAt`。**不清零**:清了的话「停走 → 立刻
 *     再播」这条路上 `start` 会短暂为 0,`hostEchoVisible` 的 ① 分支进不去,徽标闪一下;
 *     而停走期间 ① 分支本来就被 `hostEchoUseWideWindow` 挡着,留着旧值无害。
 *   · 本帧不是明确停走,且**上一次非停走已经隔了超过去抖窗**(或本会话从未有过)
 *     ⇒ 这是**新的一段播放** ⇒ 记本帧时刻。
 *   · 否则 ⇒ 同一段播放的延续 ⇒ 原样沿用。
 *
 * ⚠ **必须在 `transportPlayingAt()` 覆写 `store.playingAt` 之前调用**,或者像 `app.js`
 * 那样显式把**旧的** `playingAt` 传进来:先覆写再算的话,`prevPlayingAt` 永远是本帧时刻,
 * 「隔了超过去抖窗」恒不成立,于是**永远开不出新的一段播放** —— `start` 停在第一次播放,
 * 此后每一段播放都会拿上一段的写当成「本次播放写过」。这与 `app.js` 里
 * `prevHostEchoAt` / `wasStopped` 是同一个坑(整体覆写之后再比,永远比不出边沿)。
 *
 * @param {number} prevStartedAt 上一次的记账值(`store.playbackStartedAt`)
 * @param {number} prevPlayingAt **覆写前**的 `store.playingAt`
 * @param {{isPlaying?: boolean}|null} playhead 本帧 §2.6 载荷
 * @param {number} [nowMs] 注入时钟(用例用;省略取 `Date.now()`)
 * @returns {number} 新的一段播放 ⇒ 本帧时刻;否则原样沿用 `prevStartedAt`
 */
export function playbackStartedAt(
    prevStartedAt,
    prevPlayingAt,
    prevPlayhead,
    playhead,
    nowMs,
) {
    if (playhead && playhead.isPlaying === false) return prevStartedAt || 0;
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    // 本会话还没有过任何一段 ⇒ 这一帧就是第一段的起点。
    if (!prevStartedAt) return now;
    // [SL-394 复审 —— 行为错] **新的一段只能由「明确停走 → 播放」这个转换开启,
    // 绝不能由「两帧之间隔了多久」推断。**
    //
    // 上一版写的是「距上次非停走 ≥ 去抖窗 ⇒ 新段」,那等于把**时间空洞**当成了一段播放
    // 的结束。而 `scvb.playhead` 断流的路子有好几条,每一条都与「走带停了」无关:
    //   · 面板切走 / 不可见 —— `emitEventIfBrowserIsVisible` 整帧丢弃;
    //   · 载荷逐帧逐字相同(宿主给 `isPlaying` 却不给 `timeInSamples`,`timeS` 恒 0.0)
    //     —— native 的 `emitIfChanged` 与页面侧的 `samePlayhead` 两道去重都判「没变」;
    //   · 消息线程被别的活儿堵住,几帧挤在一起晚到。
    // 断流之后那一帧 `isPlaying:true` 一到,旧写法就开出一段**新**播放,起点跑到宿主
    // 那次写之后 ⇒ `at >= start` 变假 ⇒ **闩锁当场掉**,徽标在播放中途熄灭 ——
    // 正是本卡要治的那一幕,只是触发条件从「自动化平直」换成了「帧断流」。
    //
    // 所以断流期间**保持闩锁**:上一次观测到的不是明确停走,就还是同一段。
    // ⚠ [SL-394 已知行为后果 —— 复核点名,不放宽] 这条承诺比它字面上窄一格:**帧断流**
    // 才走这里,而「面板隐藏期间停止再播放」根本收不到那几帧停走(不可见的帧被整帧丢弃),
    // 于是 `prevPlayhead` 停在「还在播」,上一段的闩锁被带进下一段 —— 徽标/灰显会亮到下一次
    // 明确停走。J93a 同向(宁可多亮),用户裁定接受,**不改行为**;写在这里免得下一个人拿
    // 这条注释当保证去推。
    const wasStopped = !!prevPlayhead && prevPlayhead.isPlaying === false;
    if (!wasStopped) return prevStartedAt;
    // 确实是从「明确停走」回来的:再要求那次停走**满去抖窗**才算新段 ——
    // 否则播放中途一帧假停走(SL-356 的那一幕)会被当成一次真的停→播。
    const prev = prevPlayingAt || 0;
    if (!prev || now - prev >= HOST_ECHO_TRANSPORT_HOLD_MS) return now;
    return prevStartedAt;
}
