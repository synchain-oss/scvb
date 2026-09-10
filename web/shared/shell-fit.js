// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// shell-fit.js —— 设计盒「装进窗口」的唯一缩放来源(SL-380)
// -----------------------------------------------------------------------------
// 机制 9 原本的两半是「固定设计盒 + CSS zoom」与「宿主 setSize(设计盒 × 档位)」。
// 三个页面里只有 Input / Monitor 写过前一半,Output **一行都没有** —— 于是 Output
// 改档位时窗口按 F 变了、页面恒停在 1180×780:F<1 画面超出窗口,F>1 窗口空一圈而
// 画面不长。用户 v5.6.11 实测 B13 报的就是这两条。
//
// 本模块把「页面缩放多少」从**档位数字**改成**实际视口**:
//
//     zoom = min(视口宽 / 设计宽, 视口高 / 设计高)
//
// 档位那条路一点没变(仍是 setUiScale → 原生 setSize(W×F, H×F) → 10 秒防呆),只是
// 页面不再自己去读那个数字:窗口成了 W×F 之后,上式**自然**算出 F。这样一来
//   ① 档位生效(用户的两条抱怨同时消失);
//   ② 宿主自作主张改窗口尺寸(部分 DAW 会绕过 setResizable(false,false))也不再超框;
// 两件事共用同一条公式,不需要第二个开关。
//
// 取 min 而不是各轴独立拉伸:设计盒是定死宽高比的,分轴拉伸会变形。短边贴边、长边
// 留黑边(body 已经 flex 居中),这是「刚好装进去」的唯一解。
//
// ⚠ 只写 `el.style.zoom`,不碰 transform:命中测试(web/shared/hit.js)与后备存储
// (web/output/canvas/hidpi.js)全套按 CSS zoom 的语义写的,换成 transform 会让
// getBoundingClientRect 与布局坐标脱钩。
// =============================================================================

/** 有限数才用,否则回落 d(视口在首帧/隐藏时会读到 0 或 NaN)。 */
function num(v, d) {
    return typeof v === "number" && Number.isFinite(v) ? v : d;
}

/**
 * 「装进窗口」的缩放系数 —— 纯函数,node 可直接断言。
 *
 * 量化方向是**向下**取到 1e-4:四舍五入会让 `设计宽 × f` 比视口大出半个像素,而
 * 「大出半个像素」正是本卡要根治的那个溢出。向下取最多少半个像素,不会溢出。
 *
 * 视口或设计盒任一维不是正有限数 → 回落 1(首帧 / 页面隐藏时不许把画面缩成 0)。
 *
 * @param {number} vw 视口宽(CSS px)
 * @param {number} vh 视口高(CSS px)
 * @param {number} boxW 设计盒宽
 * @param {number} boxH 设计盒高
 * @returns {number} 缩放系数,恒 > 0
 */
export function fitFactor(vw, vh, boxW, boxH) {
    const w = num(vw, 0);
    const h = num(vh, 0);
    const bw = num(boxW, 0);
    const bh = num(boxH, 0);
    if (!(w > 0) || !(h > 0) || !(bw > 0) || !(bh > 0)) return 1;
    const f = Math.min(w / bw, h / bh);
    if (!(f > 0) || !Number.isFinite(f)) return 1;
    return Math.floor(f * 1e4) / 1e4;
}

/**
 * 当前页面实际生效的外壳缩放。
 *
 * 为什么是模块级单例:一个文档里只有一个设计盒外壳(Output/Monitor 的 `#card`、
 * Input 的 `#ipt-shell`),这是页面结构本身的不变量。后备存储倍率
 * (`k = 外壳缩放 × dpr`,05 §6.1)的几个读方(output/tab-wave.js 的 backingK、
 * monitor/app.js 给轨迹图的 getUiScale)从前读的是 `state.ui.scale`;档位不再是
 * 缩放的**来源**之后,那个数字与画面上的实际倍率会在宿主改窗口时分家 —— 它们要读的
 * 一直都是「画面实际被放大了多少」,所以改读这里。
 */
let current = 1;

/** 见 `current` 的注释:读方要的是实际倍率,不是档位数字。 */
export function shellFitFactor() {
    return current;
}

/**
 * 把 el 钉成「永远刚好装进视口」。
 *
 * @param {object} o
 * @param {HTMLElement} o.el 设计盒外壳元素
 * @param {{w:number,h:number}} o.box 设计盒尺寸(真源 web/shared/design-box.js)
 * @param {Window} [o.win] 注入用(默认 globalThis)
 * @returns {{factor:()=>number, refresh:()=>void, destroy:()=>void}}
 *
 * 这里**没有** onChange 之类的「倍率变了通知我」钩子,别急着补:三处消费方都不需要它。
 * Output 的泳道画布由 25Hz 的 render 顺带比对 shellFitFactor() 重建;Monitor 的轨迹图
 * 自己就跟得上(实测过,见 monitor/app.js 安装点的注释);Input 没有画布。而钩子是有
 * 代价的 —— 页面在文档顶部就装本模块,回调里要摸的东西往往是后面几十行才 `const`
 * 出来的,安装时那一次回调会直接撞 TDZ。
 */
export function installShellFit(o) {
    const el = o && o.el;
    const box = (o && o.box) || {};
    const win = (o && o.win) || globalThis;
    if (!el || !win) {
        return { factor: () => 1, refresh: () => {}, destroy: () => {} };
    }

    let last = 0;
    let raf = 0;

    function viewport() {
        const de = win.document && win.document.documentElement;
        // clientWidth 排除滚动条,是「布局视口」的准值;拿不到再退 innerWidth。
        return {
            w: num(de && de.clientWidth, 0) || num(win.innerWidth, 0),
            h: num(de && de.clientHeight, 0) || num(win.innerHeight, 0),
        };
    }

    function apply() {
        raf = 0;
        const v = viewport();
        const f = fitFactor(v.w, v.h, box.w, box.h);
        if (f === last) return;
        last = f;
        current = f;
        // String(1) === "1":档位 1x 时读回的仍是裸 "1",不是 "1.0000"。
        el.style.zoom = String(f);
    }

    function schedule() {
        if (raf) return;
        // 包一层箭头而不是直接把 apply 交出去:rAF 会把时间戳当第一个实参塞进来,
        // 现在 apply 不吃参数所以无害,但哪天它吃了参数,这里就会静默传错。
        raf =
            typeof win.requestAnimationFrame === "function"
                ? win.requestAnimationFrame(() => apply())
                : win.setTimeout(() => apply(), 0);
    }

    apply(); // 首帧同步落一次:等 rAF 会先闪一帧未缩放的画面

    win.addEventListener("resize", schedule);
    const vv = win.visualViewport || null;
    if (vv) vv.addEventListener("resize", schedule);

    // resize 事件在 WebView2 里并非每次窗口尺寸变化都到(宿主 setBounds 走的是原生
    // 路径),ResizeObserver 观察 documentElement 是那条兜底 —— 两条都挂,只多算一次
    // apply(倍率没变就直接返回,不会来回写样式)。
    let ro = null;
    if (typeof win.ResizeObserver === "function" && win.document) {
        ro = new win.ResizeObserver(schedule);
        ro.observe(win.document.documentElement);
    }

    return {
        factor: () => last,
        refresh: apply,
        destroy() {
            win.removeEventListener("resize", schedule);
            if (vv) vv.removeEventListener("resize", schedule);
            if (ro) ro.disconnect();
            if (raf && typeof win.cancelAnimationFrame === "function") {
                win.cancelAnimationFrame(raf);
            }
            raf = 0;
        },
    };
}
