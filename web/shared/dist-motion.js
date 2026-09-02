// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · 声像 / 音量分布图的**帧间补间**(rAF;SL-192)
// -----------------------------------------------------------------------------
// 为什么需要它 —— Monitor 与 Output 画的是同一张分布图,数据面却是两条完全不同的路:
//
//   • Output(`tab-master.js` 的 `renderDist`)读 `scvb.params`,契约 §2.2 **25Hz**;
//   • Monitor(`web/monitor/app.js`)读 viz 共享段 —— SL-192 升频前是 **4Hz**。
//
// 两边都是「收到事件 → 重拼 innerHTML」。25Hz 下一步 40ms,看着是连续的;4Hz 下一步
// 250ms,15 根柱**同拍齐跳**,读起来就是用户说的「一秒钟刷新一两次」。CSS 里那条
// `transition: all var(--dur-1)` 救不了:整块 `innerHTML` 一换,节点是**新建**的,
// 新节点没有可过渡的旧计算值,过渡一次都不会跑 —— 两侧都如此,只是 Output 的步子小到
// 看不出来。
//
// **SL-192 后半程把数据面也提上来了**(段侧 4Hz → 30Hz,见 `docs/contract-changes/
// 20260827-viz-30hz.md`),页面实得 25Hz —— 与 Output 同档。那本文件还留着做什么?
// 两件事,都不随频率消失:
//   ① 25Hz 仍然是**离散步**,而显示器多在 60-144Hz;补间把每一步铺满整个到达周期,
//      运动才连续(Output 侧看着顺是因为步子小,不是因为它不跳);
//   ② 更要紧的是**降级路径**:写方停摆、WebView 被宿主节流、换组、旧版 4Hz Output ——
//      这些时候帧照样稀疏到几百毫秒一帧,而那正是补间与「零外推」纪律真正兑现的场合。
// 升频让本文件的日常收益变小,但它兜住的最坏情形一个都没消失。
//
// 本文件因此把分布图的写入面从「事件驱动重拼」改成「rAF 驱动补间」:
//   ① 结构(轨集 / 立体声 / lead / 高亮)变了才重拼 innerHTML;
//   ② 数值(横位 / 柱高 / 张开半宽)三条都是**连续量**,由 rAF 逐帧线性插值,
//      只给缓存好的节点写几个 CSS 变量 —— 零 DOM 重建、零 querySelector。
//
// **补间是「向最新目标推进」,不是「回放上一帧到这一帧」**。后者(经典两帧插值)会
// 凭空多押一帧 = 多 250ms 的滞后,而用户抱怨里的另一半正是「速度比那边慢」。这里的做法
// 是:新帧到达时把**当前显示值**当起点、新值当终点,在**一个到达周期**内走完 ——
// 落点恰好踩在下一帧的预期到达时刻上,平均滞后 = 半个到达周期,且**永不越过目标**。
//
// 「永不越过目标」是硬要求(反鬼影):写方停摆时帧流断掉,`p` 封顶在 1,画面就冻在最后
// 一份真数据上,与 `vizAccepts()` 的 `stale` 表现(琥珀横幅、图仍显示)一致。任何形式的
// 外推都会让停摆后的柱继续飘一段 —— 那是**凭空捏造的读数**,比不动危险得多。
//
// 空闲零 rAF(05 §6.1,与 `canvas/meter.js` / `canvas/playhead.js` 同一条纪律):
// 循环只在「有值真的在动」时跑,`p` 到 1 即自停;值没变的帧连起都不起。
//
// 复用契约(与 `trajectory-chart.js` / `distribution-chart.js` 同款):
//   • 不认 store、不认契约事件、不认 i18n —— 调用方把算好的 rows 喂进来;
//   • 纯函数部分(`frameProgress` / `lerpScalar` / `lerpDistRow` / `distShapeKey`)
//     无 DOM,node 侧可直接 import 断言;
//   • 工厂在 `container` 为 null 时退化成纯状态机(node 侧可驱动 `push`/`tick`)。
// =============================================================================

import {
    distBarVars,
    distBarsHtml,
    distGeometry,
    distSpanVars,
    zeroDbLinePct,
} from "./distribution-chart.js";

/**
 * 补间时长(ms)= 页面**实际收到帧的周期**,不是段侧的发布周期。
 *
 * 链路(SL-192 升频后):发布器 30Hz(33ms) → Monitor [M] 轮询 60Hz →
 * 桥面 `emitTick` **25Hz**(WebViewHost 基准 tick,既有 50→25 减负裁定,本卡不重开)。
 * 最慢的一级就是页面实得的速率 —— **40ms / 25Hz**。拿 33ms 当时长会让补间比帧到达
 * 早结束 7ms,拿 250ms 更是慢得离谱;取 40ms 才能让落点恰好踩在下一帧的预期到达时刻上。
 *
 * 为什么是**定值**,不取实测帧间隔:新值出现时我们唯一握有的事实是「它发生在最近
 * 一个到达周期之内」,那就是唯一有依据的重建窗口。而「实测间隔」量的是另一件事 ——
 * **数据多久变一次**。引擎段内取值恒定、过渡走 ramp(`CurveEvaluator`:80ms 起,受
 * 15 pan/s、3 dB/s 限速,空隙处最长 6s),两次 ramp 之间可以好几秒不变;拿那个当时长,
 * 会把一次孤立的段边界跳变抹成几秒的爬行 —— 那是编出来的运动。
 *
 * 另一个不容易看见的好处:定值不受「谁调用了 render」影响。`scvb.groups`(1Hz)与
 * `scvb.state` 也会触发一次 render → 一次 push,若拿「上次 push 到现在」当间隔,
 * 这些与数据面无关的事件会把测量搞乱,而画面上只表现为运动时快时慢。
 */
export const DIST_SPAN_MS = 40;

function clamp(lo, hi, v) {
    return v < lo ? lo : v > hi ? hi : v;
}

/**
 * 补间进度(纯函数)。**封顶 1,不外推**(见文件头「反鬼影」)。
 * 时长非正 / 时刻非有限 ⇒ 直接给 1(当作已到位),绝不返回 NaN 让下游算出 NaN 几何。
 */
export function frameProgress(elapsedMs, spanMs) {
    if (
        !Number.isFinite(elapsedMs) ||
        !Number.isFinite(spanMs) ||
        spanMs <= 0
    ) {
        return 1;
    }
    return clamp(0, 1, elapsedMs / spanMs);
}

/**
 * 单条标量的线性插值。
 *
 * `p >= 1` 时**返回 `b` 本身**而不是 `a + (b - a) * 1` —— 浮点下后者不保证逐位等于 `b`,
 * 于是「到位了没有」这件事就没法用相等去断言,自停判据也会一直差那么一丁点。
 * 端点非有限(首次出现的轨 / 缺数据)一律取目标值:没有起点就没有可插的值。
 */
export function lerpScalar(a, b, p) {
    if (!Number.isFinite(b)) return b;
    if (!Number.isFinite(a) || p >= 1) return b;
    if (p <= 0) return a;
    return a + (b - a) * p;
}

/**
 * 一行的插值。三条连续量插值,离散面(轨号 / 立体声 / lead)**一律取目标行** ——
 * 它们是身份与标记,插值没有意义(半个 lead 帽画不出来)。离散面变了会走
 * `distShapeKey` 的重建路径,根本到不了这里。
 */
export function lerpDistRow(from, to, p) {
    const f = from || to || {};
    const t = to || {};
    return {
        ch: t.ch,
        stereo: !!t.stereo,
        lead: !!t.lead,
        pan: lerpScalar(f.pan, t.pan, p),
        volDb: lerpScalar(f.volDb, t.volDb, p),
        widthPct: lerpScalar(f.widthPct, t.widthPct, p),
    };
}

/**
 * 行集的**结构指纹** —— 决定 ① 拼串产物长什么样,② 逐轨下标还对不对得上。
 *
 * 轨集变了(某轨上线/下线)之后,`rows[i]` 与上一帧的 `rows[i]` 就不是同一条轨了;
 * 此时若照下标插值,会画出「A 轨的柱滑向 B 轨的位置」这种**完全虚构**的运动。
 * 故指纹一变就直接落到目标值(见 `push` 的重建分支),不插。
 */
export function distShapeKey(rows) {
    return (rows || [])
        .map(
            (r) =>
                `${r && r.ch}/${r && r.stereo ? 1 : 0}${r && r.lead ? 1 : 0}`,
        )
        .join(",");
}

/** 两个行集的**数值面**是否逐字相同(相同 ⇒ 没有可补间的运动,不起帧)。 */
export function sameDistValues(a, b) {
    const x = a || [];
    const y = b || [];
    if (x.length !== y.length) return false;
    for (let i = 0; i < x.length; i++) {
        if (
            x[i].pan !== y[i].pan ||
            x[i].volDb !== y[i].volDb ||
            x[i].widthPct !== y[i].widthPct
        ) {
            return false;
        }
    }
    return true;
}

/**
 * 行是否可用 —— **必须与 `distBarsHtml` 的过滤谓词逐字同义**。
 *
 * 拼串那边对 `ch` 非有限的行是 `continue`(不产出节点),而 `paint()` 是按**下标**把
 * `shown[i]` 写进 `bars[i]` 的。两边谓词一旦不一致,坏行会让节点与行**整体错位一格**,
 * 于是每根柱都写上邻轨的几何 —— 而 `setVars` 的 null 保护会让它一声不吭。
 * Monitor 现路的 `colorChOf()` 恒出有限整数,这是潜雷不是现雷;在入口滤掉最省事。
 */
function usableRow(r) {
    return !!r && Number.isFinite(Number(r.ch));
}

function copyRow(r) {
    return {
        ch: r.ch,
        stereo: !!r.stereo,
        lead: !!r.lead,
        pan: r.pan,
        volDb: r.volDb,
        widthPct: r.widthPct,
    };
}

/**
 * 分布图补间渲染器工厂。
 *
 * @param {object} opts
 * @param {Element} [opts.container] 柱体容器(`.dist-bars`);省略 ⇒ 纯状态机(node 侧)
 * @param {() => boolean} [opts.isVisible] 本图当前可见吗;false 的帧**不写 DOM**
 * @param {() => number} [opts.getGlobalWidthPct] 全局「最大角度」(几何要用;
 *        Monitor 的 viz 段不带这个值,缺省 100 = 不缩放,与 `distGeometry` 同口径)
 * @returns {{push:Function, tick:Function, reset:Function, stop:Function,
 *            destroy:Function, rows:Function, targets:Function, diag:Function}}
 */
export function createDistMotion(opts) {
    const o = opts || {};
    const container = o.container || null;
    const isVisible =
        typeof o.isVisible === "function" ? o.isVisible : () => true;
    const getGlobalWidthPct =
        typeof o.getGlobalWidthPct === "function"
            ? o.getGlobalWidthPct
            : () => 100;

    // [SL-280] 把 0 dB 基准线的高度喂给 CSS。**挂在这里而不是各页自己写**:两页都只经
    // 本工厂拿到 container(Output 的 wireChart、Monitor 的 app.js),写成两份就又是
    // 「同一个数各存一份」——本仓刚为这类漂移连收过几轮。
    // 线要落在「0 dB 那根柱的顶边」上,而柱顶由 `barHeightPct` 决定,故两者同源
    // (`zeroDbLinePct()` 就是 `barHeightPct(0)`)。CSS 侧**不给缺省**:喂不到就是
    // 声明失效,由页面级用例抓,而不是画在一个看起来合理却错的位置上。
    const plot =
        container && container.closest && container.closest(".dist-plot");
    if (plot && plot.style) {
        plot.style.setProperty("--zero-h", zeroDbLinePct() + "%");
    }

    // `null` 而不是 `""`:空行集的指纹也是 `""`,用它当「还没有过任何一帧」会让
    // 「reset 之后收到的第一帧」被误判成「结构没变」,于是从上一组的值插过去。
    let shapeKey = null;
    let hi = 0;
    let from = []; // 本次补间的起点(= 新帧到达那一刻的显示值)
    let target = []; // 最近一帧(补间终点)
    let shown = []; // 当前显示值
    let t0 = 0; // 本次补间的起始时刻
    let bars = [];
    let spans = [];
    let raf = 0;
    // 循环是否在跑。**不能用 `raf !== 0` 代替**:`loop()` 一进回调就把 `raf` 置 0,
    // 于是「正在补间」的那一刻从外面读到的是 false —— 诊断面骗人,排查时会看反。
    // (PR #115 审查提的第二条。)
    let running = false;
    // 隐藏期跳过的那些 paint 记成欠账:`tick()` 在不可见时照常推进 `shown` 却不写 DOM,
    // 若之后来的 push 数值与 target 相同,`sameDistValues` 会早退、不重画 —— DOM 就停在
    // 隐藏前那一帧上。Monitor 现路走 `reset()` 碰不到(不可见即清空、回来必重建),
    // 但这是工厂的通用契约,别把正确性押在某一个调用方的接线上。
    let paintOwed = false;
    // 上一次画进 DOM 的全局「最大角度」。
    //
    // 它**进几何但不进补间**:`distGeometry` 拿它缩放横位与张开半宽,可它是**直接操纵**
    // (Output 的「最大角度」滑杆),要的是跟手,不是平滑 —— 给手势加 40ms 惯性是坏体验。
    // 但它必须能**触发重画**:只拧滑杆时每轨的 pan/vol/width 一个都没变,`sameDistValues`
    // 会早退 ⇒ 柱子纹丝不动,而 v5 P2-10 用户裁定要的正是「拧滑杆时分布图实时收拢/张开」。
    // 故单独记一份,变了就立刻补一帧(不重启补间窗口)。
    // Monitor 侧的 getter 恒回 100,这条永远不触发。
    let lastPaintedWidth = null;
    let destroyed = false;
    // 只读诊断(页面级冒烟据此判「渲染循环是 rAF 驱动还是事件驱动」——
    // 事件驱动的话 `frames` 恒为 0,这是两者最干脆的分界)。
    let frames = 0;
    let pushes = 0;

    function nowMs() {
        return typeof performance !== "undefined" && performance.now
            ? performance.now()
            : Date.now();
    }

    /**
     * 结构变了才走:重拼 innerHTML 并重新缓存节点。
     *
     * `width` 由调用方传,理由同 `paint()`:一拍里的重拼与随后的写变量必须用同一个数。
     */
    function rebuild(width) {
        if (!container) return;
        const w = Number.isFinite(width) ? width : getGlobalWidthPct();
        const html = distBarsHtml(shown, hi, w);
        if (container.innerHTML !== html) container.innerHTML = html;
        bars = [];
        spans = [];
        if (typeof container.querySelectorAll !== "function") return;
        // 按**文档顺序**取,不按 `[data-ch]` 选 —— `colorChOf()` 允许两轨映到同一个
        // 色号,那时按属性选会拿到同一个节点两次。拼串里柱与横线各自按行序 push,
        // 故文档顺序就是行序(横线只覆盖 stereo 那些行,下标另算)。
        bars = Array.from(container.querySelectorAll(".dist-bar"));
        spans = Array.from(container.querySelectorAll(".dist-span"));
    }

    function setVars(node, vars) {
        if (!node || !node.style) return;
        for (const k of Object.keys(vars)) node.style.setProperty(k, vars[k]);
    }

    /**
     * 逐帧写入面:只写 CSS 变量,不碰节点结构、不做任何布局读。
     *
     * `width` 可由调用方传:`push()` 一拍里可能既重拼又画(甚至画两次),各自取一次
     * getter 的话,滑杆正在拖时这一拍内部就能取到两个不同的值 —— 拼串那一版用旧的、
     * 写变量那一版用新的。传参把**整拍**钉成一个数。`tick()` 不传:它就是新的一帧,
     * 本来就该重新取。
     */
    function paint(width) {
        if (!container) return;
        // 一帧只取一次:既省掉每行一次的 getter 调用(Output 满配下 ~900 次/秒),
        // 更要紧的是**保证同一帧里每根柱用的是逐字节同一个 width** ——
        // 逐行取的话,滑杆正在拖动时它可能在一帧中途变掉,画出半帧新半帧旧的几何。
        const globalWidth = Number.isFinite(width)
            ? width
            : getGlobalWidthPct();
        lastPaintedWidth = globalWidth;
        let si = 0;
        for (let i = 0; i < shown.length; i++) {
            const r = shown[i];
            const geo = distGeometry(r.pan, r.volDb, r.widthPct, globalWidth);
            setVars(bars[i], distBarVars(geo));
            if (r.stereo) {
                setVars(spans[si], distSpanVars(geo));
                si++;
            }
        }
    }

    function start() {
        if (raf || destroyed) return;
        if (typeof requestAnimationFrame !== "function") return;
        const loop = (ts) => {
            raf = 0;
            // 空闲零 rAF:`tick` 报「还没到位」才续帧;到位那一帧写完就让出主线程,
            // 下一次起帧由 `push()` 里「值真的变了」那一支负责。
            if (tick(ts)) raf = requestAnimationFrame(loop);
            else running = false;
        };
        running = true;
        raf = requestAnimationFrame(loop);
    }

    function stop() {
        if (raf && typeof cancelAnimationFrame === "function") {
            cancelAnimationFrame(raf);
        }
        raf = 0;
        running = false;
    }

    /**
     * 单帧推进(导出出去,便于 node 侧不起 rAF 也能驱动)。
     * @returns {boolean} 还没到位 ⇒ 循环续帧;到位 ⇒ 自停。
     */
    function tick(ts) {
        frames++;
        const p = frameProgress(
            (Number.isFinite(ts) ? ts : nowMs()) - t0,
            DIST_SPAN_MS,
        );
        for (let i = 0; i < target.length; i++) {
            shown[i] = lerpDistRow(from[i], target[i], p);
        }
        if (isVisible()) {
            paint();
            paintOwed = false;
        } else {
            paintOwed = true;
        }
        return p < 1;
    }

    return {
        /**
         * 新的一帧目标行集。
         *
         * @param {object[]} rows 已算好的行(`vizDistRows()` / `renderDist` 的产物)
         * @param {number} [highlightCh] 图例 hover 联动的轨号(0 = 无)
         * @param {number} [atMs] 到达时刻(省略 = 现在;node 侧断言用它喂确定的时间)
         */
        push(rows, highlightCh, atMs) {
            if (destroyed) return;
            if (!isVisible()) {
                // **不可见就一步都不走**(05 §6.1 空闲零 rAF)。Output 侧这条是实打实的:
                // Tab1 不在前台、或图表切到轨迹档时,`scvb.params` 仍以 25Hz 推着 render,
                // 照常补间等于对着没人看的画面烧一条 60fps 循环。
                //
                // 顺带**不跨隐藏边界插值**:把结构指纹清成 null,回到可见的第一帧必走下面的
                // 重建分支(隐藏期结构可能已经变过),且从当前真值直接落位 ——
                // 隐藏那段运动没人看见,插出来只会是一段鬼影。
                stop();
                shapeKey = null;
                from = [];
                target = [];
                shown = [];
                // 隐藏期欠的那笔重绘账**就此作废**:回到可见必走下面的重建分支
                // (它自己会 rebuild + paint),再留着只会让 push 多画一次同样的东西。
                paintOwed = false;
                return;
            }

            const list = (Array.isArray(rows) ? rows : [])
                .filter(usableRow)
                .map(copyRow);
            const t = Number.isFinite(atMs) ? atMs : nowMs();
            const key = distShapeKey(list);
            const nextHi = Number(highlightCh) || 0;
            // **一拍一个 width**:下面最多会重拼一次、写变量两次,各自取 getter 的话
            // 滑杆拖动中这一拍内部就能取到不同的值(拼串旧、写变量新)。取一次贯穿到底。
            const width = getGlobalWidthPct();

            if (key !== shapeKey) {
                // 轨集 / 立体声 / lead 变了 ⇒ 逐轨下标不再对齐:**直接落到目标值**,
                // 不插(理由见 distShapeKey 的头注)。换组后的第一帧也走这一支 ——
                // `reset()` 把指纹清成 null,于是必然不匹配。
                shapeKey = key;
                hi = nextHi;
                target = list;
                from = list.map(copyRow);
                shown = list.map(copyRow);
                t0 = t;
                stop();
                rebuild(width);
                if (isVisible()) paint(width);
                return;
            }
            if (nextHi !== hi) {
                // 高亮只改 `data-hi`,不动下标对齐 —— 重拼一次(用**当前显示值**拼,
                // 免得 hover 的瞬间把补间中的柱弹回目标位),补间照旧往下走。
                hi = nextHi;
                rebuild(width);
            }
            if (width !== lastPaintedWidth) {
                // 只拧了滑杆:每轨三条值一个没变,下面的 `sameDistValues` 会早退 ——
                // 不在这里补一帧,柱子就不跟手了(v5 P2-10)。**不重启补间窗口**:
                // 直接操纵要的是当场跟手,给它加惯性是坏体验。
                paint(width);
            }
            if (paintOwed && isVisible()) {
                // 还了隐藏期欠下的那一笔,再走下面的「值没变就早退」——顺序不能倒。
                paint(width);
                paintOwed = false;
            }
            if (sameDistValues(target, list)) {
                // 值没变 ⇒ 起帧也画不出任何变化(与 meter.js 的 `settled && same` 同款)。
                // 注意这里**什么都不记** —— 补间时长是定值,不依赖任何实测,
                // 于是 `scvb.groups`(1Hz)这类与数据面无关的 render 不会干扰到运动。
                return;
            }
            from = shown.map(copyRow);
            target = list;
            t0 = t;
            pushes++;
            start();
        },

        tick,
        stop,

        /**
         * 清零(换组 / 组不在线 / 拒连)。
         *
         * 必须连指纹一起清:留着指纹会让下一组的第一帧在轨集恰好相同时被当成
         * 「结构没变」,于是柱从**上一组**的位置滑到新组的位置 —— 一段完全虚构的运动。
         */
        reset() {
            stop();
            shapeKey = null;
            hi = 0;
            from = [];
            target = [];
            shown = [];
            bars = [];
            spans = [];
            if (container && container.innerHTML !== "") {
                container.innerHTML = "";
            }
        },

        /** 不再使用(窗口收起)。此后一切写入口早退,与 trajectory-chart 的 destroy 同款。 */
        destroy() {
            destroyed = true;
            stop();
            // 放掉引用:`app.js` 的 pagehide 注释说的是「不拆就连着数据面与节点数组一起
            // 钉在内存里」——只停 rAF 不清引用的话那句话不成立。
            from = [];
            target = [];
            shown = [];
            bars = [];
            spans = [];
        },

        /** 当前显示值(插值后)。 */
        rows: () => shown.map(copyRow),
        /** 最近一帧的目标值。 */
        targets: () => target.map(copyRow),
        /** 只读诊断面(页面级冒烟用;不暴露任何写入口)。 */
        diag: () => ({
            frames,
            pushes,
            animating: running,
            spanMs: DIST_SPAN_MS,
            shown: shown.map(copyRow),
            target: target.map(copyRow),
        }),
    };
}
