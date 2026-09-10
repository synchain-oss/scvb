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
 * (`k = 外壳缩放 × dpr`,05 §6.1)的读方从前读的是 `state.ui.scale`;档位不再是
 * 缩放的**来源**之后,那个数字与画面上的实际倍率会在宿主改窗口时分家 —— 它们要读的
 * 一直都是「画面实际被放大了多少」,所以改读这里。
 *
 * **读方清单不写在注释里**:第一版把它写成一句「tab-wave 的 backingK 与 monitor 的
 * getUiScale」,结果漏了 `output/tab-master.js` 的两处(Tab1 轨迹图的 getUiScale 与
 * 它的倍率账),而漏掉的那两处**没有任何东西会红**。清单改由
 * `web-preview/tests/smoke-shell-fit.mjs` 的扫描判据现算 —— 那份扫描自己读文件,
 * 不走 shell 的 grep(`tab-master.js` 里有一个真的 NUL 字节,grep 会把整个文件当二进制
 * 跳过、只印一行 "Binary file matches" —— 那正是当初漏掉它的原因之一)。
 */
let current = 1;

/** 见 `current` 的注释:读方要的是实际倍率,不是档位数字。**画面缩放**用这个精值。 */
export function shellFitFactor() {
    return current;
}

/**
 * **后备存储**(`k = 外壳缩放 × dpr`,05 §6.1)专用的**粗量化**倍率。
 *
 * 画面倍率必须精(量化到 1e-4),否则就是本卡在修的那半个像素溢出。但 `k` 不必跟着
 * 一像素一跳:宿主拖着窗口边框走时,精值几乎每个像素都是新数,拿它当「后备存储要不要
 * 重建」的判据 = 每帧把 15 条泳道的 canvas 全部重新分配一次。
 *
 * 量化到 0.01:1% 的后备存储误差在屏幕上看不出来,而拖拽整程只会跨过有限几档。
 */
export function backingFitFactor() {
    return Math.round(current * 100) / 100;
}

/**
 * 把 el 钉成「永远刚好装进视口」。
 *
 * @param {object} o
 * @param {HTMLElement} o.el 设计盒外壳元素
 * @param {{w:number,h:number}} o.box 设计盒尺寸(真源 web/shared/design-box.js)
 * @param {(f:number)=>void} [o.onChange] 倍率**变了**才回调。
 *   **安装时那一次不回调**:页面通常在文档顶部就装上本模块,而回调里要摸的东西
 *   (Monitor 的轨迹图)往往是后面几十行才 `const` 出来的 —— 安装时就回调会撞 TDZ。
 *   首帧的后备存储由各画布自己 mount 时读 backingFitFactor() 得到,不需要这一次回调。
 *
 *   为什么非要有这个钩子(我一度把它删了,是错的):倍率变了之后,**没有任何一条自发
 *   路径**会去重算 `k` —— CSS zoom 改的是 used value,不改后代元素自己的 `clientWidth`,
 *   所以轨迹图 `measure()` 判不出「尺寸变了」,它的 ResizeObserver 与 observeResolution
 *   也都不会响。我当时量到「有没有这次回调都一样」,是因为量的时候 mock 正以 25Hz 推
 *   viz 帧、而每来一帧 Monitor 都会 `traj.invalidate()` 一次 —— **那是判据不可分辨,
 *   不是不需要**。真实缺口 = 帧流停顿而组仍在线的那一段里宿主改了窗口尺寸:画布会带着
 *   旧 k 一直被上采样。删除式判据 = smoke-shell-fit-page.mjs 的「开窗即 0.5 档」那一格
 *   (⑦):它读 Monitor 测试面的失效计数,拆掉本回调计数就不涨 ⇒ 必红。
 * @param {Window} [o.win] 注入用(默认 globalThis)
 * @returns {{factor:()=>number, refresh:()=>void, destroy:()=>void}}
 */
export function installShellFit(o) {
    const el = o && o.el;
    const box = (o && o.box) || {};
    const onChange =
        typeof (o && o.onChange) === "function" ? o.onChange : null;
    const win = (o && o.win) || globalThis;
    if (!el || !win) {
        return { factor: () => 1, refresh: () => {}, destroy: () => {} };
    }

    let last = 0;
    let raf = 0;
    let rafIsTimeout = false; // raf 里存的是 timeout id 还是 rAF id(destroy 按类型取消)

    function viewport() {
        const de = win.document && win.document.documentElement;
        // clientWidth 排除滚动条,是「布局视口」的准值;拿不到再退 innerWidth。
        return {
            w: num(de && de.clientWidth, 0) || num(win.innerWidth, 0),
            h: num(de && de.clientHeight, 0) || num(win.innerHeight, 0),
        };
    }

    // silent = 安装时那一次:只落样式,不回调(理由见 onChange 的参数注释)。
    function apply(silent) {
        raf = 0;
        const v = viewport();
        const f = fitFactor(v.w, v.h, box.w, box.h);
        if (f === last) return;
        last = f;
        current = f;
        // String(1) === "1":档位 1x 时读回的仍是裸 "1",不是 "1.0000"。
        el.style.zoom = String(f);
        if (onChange && silent !== true) onChange(f);
    }

    function schedule() {
        if (raf) return;
        // 包一层箭头而不是直接把 apply 交出去:rAF 会把时间戳当第一个实参塞进来,
        // 而 apply 的第一个形参是 `silent` —— 直接传就等于拿一个时间戳当布尔用。
        if (typeof win.requestAnimationFrame === "function") {
            rafIsTimeout = false;
            raf = win.requestAnimationFrame(() => apply(false));
        } else {
            // 回退到 setTimeout 时**句柄类型也变了**:destroy() 里必须按类型取消,
            // 否则 cancelAnimationFrame 取消不掉一个 timeout id —— 那次待跑的 apply()
            // 会在 destroy 之后再写一次 style.zoom,并把模块级 current 改回去。
            rafIsTimeout = true;
            raf = win.setTimeout(() => apply(false), 0);
        }
    }

    apply(true); // 首帧同步落一次:等 rAF 会先闪一帧未缩放的画面

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
        refresh: () => apply(false),
        destroy() {
            win.removeEventListener("resize", schedule);
            if (vv) vv.removeEventListener("resize", schedule);
            if (ro) ro.disconnect();
            // 按**句柄类型**取消:schedule() 在没有 rAF 的环境里回退到 setTimeout,
            // 那时 raf 里存的是 timeout id,cancelAnimationFrame 取消不掉它 ——
            // 那次待跑的 apply() 会在 destroy 之后再写一次 style.zoom、并改回 current。
            if (raf) {
                if (rafIsTimeout) win.clearTimeout(raf);
                else if (typeof win.cancelAnimationFrame === "function")
                    win.cancelAnimationFrame(raf);
            }
            raf = 0;
            // 「拆完等于没装过」:模块级 current 是给后备存储读的,留着上一个文档的
            // 倍率会让热重挂之后的第一批画布按僵值分配。
            current = 1;
            last = 0;
        },
    };
}
