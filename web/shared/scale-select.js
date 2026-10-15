// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB · 缩放档位下拉(UI-1;四处共用:Output 右下角 / Output 设置页 / Monitor 右下角 /
// Input 右下角)
// -----------------------------------------------------------------------------
// 为什么不用原生 <select>:它的展开列表是浏览器 / 系统自绘的弹层,页面 CSS 改不到 ——
// WebView2 下是一块白底系统列表,WKWebView 下是 macOS 菜单,与深色玻璃面板格格不入
// (用户 2026-10-15 报:Output 右下角已是深色浮层,另外几处一展开还是浏览器默认样式)。
// 本模块把 Output 右下角那套「触发钮 + 深色玻璃浮层」做成一件,四处共用;
// 样式在 web/shared/base.css 的 `.sc-scale*`。
//
// 分工(本模块**只管呈现与交互**,判据与状态机都留在各页 app.js):
//   • 档位表:调用方传进来的 `presets` —— 各页都先过 shell-fit.js 的 scalePresets
//     (档位真源 = design-box.js;[M09] 旧 WebKit 下只剩 1 档)。本模块不读 DESIGN。
//   • 超屏判据:调用方传 `overflows(f)`(05 §1.2:W*F > availWidth ∨ H*F > availHeight)。
//     超屏档**灰掉、不隐藏**,点了不生效;每次展开时重算一遍(窗口可能已挪到另一块屏)。
//   • 选中之后:调用 `onPick(f)` —— 10 秒防呆(previewScale / 保持 / 取消 / 超时回退)
//     仍是各页 app.js 自己那一套,本模块不碰桥。
//   • 当前档:调用方在状态回推时 `setValue(f)`;同一页两处入口(Output 右下角与设置页)
//     各自 setValue 同一个值,所以改一处另一处必然同步。
//   • 注记文案:「当前」/「超出当前屏幕」走词条 `scale.current` / `scale.overflow`,
//     节点挂 data-t,切语言时 applyI18n 会顺手刷掉;setValue 时也按 `getT()` 重写一遍。
//
// 交互(ARIA listbox 语义;触发钮 aria-haspopup="listbox" + aria-expanded):
//   • 点触发钮开 / 关;点外面关;展开时焦点落到当前档。
//   • 触发钮上 ↑ / ↓ = 展开(不直接换档:换档会立刻改窗口尺寸并起 10 秒倒计时,
//     不该由一次方向键顺手触发);Enter / Space 走按钮原生的 click,同样是展开。
//   • 浮层里 ↑ / ↓ 逐项移焦点、Home / End 到两端(超屏档也能停留,读屏能读到它的注记),
//     Enter / Space 选中、Esc 收起并把焦点还给触发钮、Tab 离开即收起。
//   • 带 Ctrl / Cmd(metaKey)/ Alt 的组合键**一律放行不吞**:那是宿主与系统的地盘
//     (见 alt-menu.js 同一条规矩;Output app.js 的 [SL-208] 注释说明了为什么这里也
//     不接 Ctrl +/-/0)。
//   • 点的正是当前档 = 只收起,不发预览(与原生 <select> 一致:同值不派发 change)。
//     旧 WebKit 锁 100% 时浮层里只剩当前那一档,点它也不会弹一个毫无意义的防呆框。
//
// 浮层定位:默认方向由 `placement` 给("up" = 页脚,向上展开;"down" = 设置页,向下展开),
// 展开时量一次可用空间 —— 到最近的裁切祖先(overflow 不是 visible 的祖先,外壳 .sc-shell
// 就是一个)与视口为止 —— 默认方向放不下且另一边更宽就翻过去,两边都放不下就压 max-height
// 改内部滚动。水平方向同理:按 `align` 贴齐触发钮一侧,越界就换另一侧。
// 包围盒都在同一个坐标系里比较(rect 对 rect),换成 CSS px 用的是**量出来**的倍率
// (触发钮的 rect 高 ÷ offsetHeight):新语义(rect 含 zoom)下它就是外壳倍率;旧语义
// ([M09],rect 不含 zoom)下外壳已锁在 1,量出来也是 1 —— 两种语义都不需要分支。
// =============================================================================

const PLACEMENTS = ["up", "down"];
const ALIGNS = ["start", "end"];
/** 浮层与触发钮之间的间距(CSS px)= base.css `.sc-scale__panel` 的 calc(100% + var(--sp-9))。 */
const PANEL_GAP_PX = 9;
/** 浮层离裁切边缘至少留这么多(CSS px),免得描边贴死在外壳圆角上。 */
const EDGE_PX = 4;
/** 两边都放不下时 max-height 的下限(CSS px):至少露出两三项,别压成一条缝。 */
const MIN_PANEL_PX = 72;

function pctOf(f) {
    return Math.round(Number(f) * 100);
}

function hasOwn(o, k) {
    return !!o && Object.prototype.hasOwnProperty.call(o, k);
}

function isModified(e) {
    return !!(e.ctrlKey || e.metaKey || e.altKey);
}

/**
 * 当前缩放倍率(元素所在坐标系 → 视口)= rect 高 ÷ offsetHeight。
 * 量不出来(未布局、0 高)就按 1 处理 —— 失效方向是「按未缩放算空间」,最多让浮层多压一点。
 */
export function measuredZoom(el) {
    if (!el || typeof el.getBoundingClientRect !== "function") return 1;
    const h = el.offsetHeight;
    const r = el.getBoundingClientRect();
    if (!(h > 0) || !(r && r.height > 0)) return 1;
    const k = r.height / h;
    return Number.isFinite(k) && k > 0 ? k : 1;
}

/**
 * 纯几何(node 可直接断言):给出触发钮与裁切框(同一坐标系的 top/bottom)、浮层自然高度
 * 与倍率,回答「往哪边开、要不要压高度」。
 *   • 默认方向放得下 → 就它;
 *   • 放不下、另一边放得下 → 翻过去;
 *   • 两边都放不下 → 取更宽的一边,maxHeight = 那一边的空间(不低于 MIN_PANEL_PX)。
 * @returns {{ placement: "up"|"down", maxHeight: number|null }} maxHeight 为 CSS px;null = 不压
 */
export function choosePlacement(prefer, trig, clip, naturalPx, zoom) {
    const k = Number.isFinite(zoom) && zoom > 0 ? zoom : 1;
    const first = PLACEMENTS.indexOf(prefer) >= 0 ? prefer : "up";
    const other = first === "up" ? "down" : "up";
    const room = {
        up: (trig.top - clip.top) / k - PANEL_GAP_PX - EDGE_PX,
        down: (clip.bottom - trig.bottom) / k - PANEL_GAP_PX - EDGE_PX,
    };
    const need = Number.isFinite(naturalPx) ? naturalPx : 0;
    if (need <= room[first]) return { placement: first, maxHeight: null };
    if (need <= room[other]) return { placement: other, maxHeight: null };
    const side = room[other] > room[first] ? other : first;
    return {
        placement: side,
        maxHeight: Math.max(MIN_PANEL_PX, Math.floor(room[side])),
    };
}

/** 从 el 往上找所有会裁切的祖先(overflow 不是 visible),与视口求交。坐标 = 视口坐标。 */
function clipBox(el) {
    const win = (el.ownerDocument && el.ownerDocument.defaultView) || window;
    let box = {
        top: 0,
        bottom: win.innerHeight,
        left: 0,
        right: win.innerWidth,
    };
    for (let n = el.parentElement; n; n = n.parentElement) {
        const cs = win.getComputedStyle(n);
        const clips =
            /(hidden|auto|scroll|clip)/.test(cs.overflowY || "") ||
            /(hidden|auto|scroll|clip)/.test(cs.overflowX || "");
        if (!clips) continue;
        const r = n.getBoundingClientRect();
        box = {
            top: Math.max(box.top, r.top),
            bottom: Math.min(box.bottom, r.bottom),
            left: Math.max(box.left, r.left),
            right: Math.min(box.right, r.right),
        };
    }
    return box;
}

/**
 * 装一个缩放下拉。HTML 侧要先有四件(各页 index.html 里写死,data-gb 锚点由各页定):
 *   wrap(.sc-scale,data-open="0")> trigger(button.sc-scale__trigger)> label(span)
 *   wrap > panel(.sc-scale__panel,role="listbox")
 *
 * @param {object} o
 * @param {HTMLElement} o.wrap      根节点:data-open / data-placement / data-align 写在它身上
 * @param {HTMLElement} o.trigger   触发钮
 * @param {HTMLElement} [o.label]   触发钮上显示当前档的那段文字
 * @param {HTMLElement} o.panel     浮层(档位项由本模块生成)
 * @param {number[]} o.presets      档位表(调用方已过 scalePresets)
 * @param {(f:number)=>boolean} [o.overflows]  超屏判据
 * @param {(pct:number)=>string} [o.optGb]     档位项的 data-gb(给冒烟钉锚点)
 * @param {()=>object} [o.getT]     当前语言词典
 * @param {(f:number)=>void} o.onPick  用户选中了一档(不是超屏档、也不是当前档)
 * @param {"up"|"down"} [o.placement]  默认展开方向
 * @param {"start"|"end"} [o.align]    默认水平贴齐(start = 左缘对齐触发钮,end = 右缘)
 * @param {number} [o.value]        初值(缺省 1)
 */
export function createScaleSelect(o) {
    const opts = o || {};
    const wrap = opts.wrap;
    const trigger = opts.trigger;
    const panel = opts.panel;
    const label = opts.label || null;
    if (!wrap || !trigger || !panel) return null;
    const doc = wrap.ownerDocument || document;
    const presets = Array.isArray(opts.presets) ? opts.presets.slice() : [1];
    const overflows =
        typeof opts.overflows === "function" ? opts.overflows : () => false;
    const getT = typeof opts.getT === "function" ? opts.getT : () => ({});
    const onPick = typeof opts.onPick === "function" ? opts.onPick : () => {};
    const optGb = typeof opts.optGb === "function" ? opts.optGb : null;
    const prefer =
        PLACEMENTS.indexOf(opts.placement) >= 0 ? opts.placement : "up";
    const align = ALIGNS.indexOf(opts.align) >= 0 ? opts.align : "end";
    let value = Number.isFinite(opts.value) ? opts.value : 1;

    wrap.setAttribute("data-open", "0");
    wrap.setAttribute("data-placement", prefer);
    wrap.setAttribute("data-align", align);
    trigger.setAttribute("aria-haspopup", "listbox");
    trigger.setAttribute("aria-expanded", "false");
    panel.setAttribute("role", "listbox");

    // ---- 档位项 ----
    const items = presets.map((f) => {
        const b = doc.createElement("button");
        b.type = "button";
        b.className = "sc-scale__opt";
        b.setAttribute("role", "option");
        b.setAttribute("tabindex", "-1");
        b.setAttribute("aria-selected", "false");
        b.setAttribute("data-scale", String(f));
        b.setAttribute("data-current", "0");
        b.setAttribute("data-over", "0");
        if (optGb) b.setAttribute("data-gb", optGb(pctOf(f)));
        const num = doc.createElement("span");
        num.textContent = pctOf(f) + "%";
        const note = doc.createElement("span");
        note.className = "sc-scale__note";
        b.append(num, note);
        return { f, el: b, note };
    });
    panel.replaceChildren(...items.map((it) => it.el));

    function refreshOver() {
        for (const it of items) {
            const over = !!overflows(it.f);
            it.el.setAttribute("data-over", over ? "1" : "0");
            if (over) it.el.setAttribute("aria-disabled", "true");
            else it.el.removeAttribute("aria-disabled");
        }
    }

    function render() {
        const t = getT() || {};
        if (label) label.textContent = pctOf(value) + "%";
        for (const it of items) {
            const cur = it.f === value;
            const over = it.el.getAttribute("data-over") === "1";
            it.el.setAttribute("data-current", cur ? "1" : "0");
            it.el.setAttribute("aria-selected", cur ? "true" : "false");
            // 超屏优先于「当前」:当前档若也超屏(宿主按存档开了一个放不下的窗),要说的是后者
            const key = over ? "scale.overflow" : cur ? "scale.current" : "";
            if (key) {
                it.note.setAttribute("data-t", key);
                it.note.textContent = hasOwn(t, key) ? t[key] : "";
            } else {
                it.note.removeAttribute("data-t");
                it.note.textContent = "";
            }
        }
    }

    function isOpen() {
        return wrap.getAttribute("data-open") === "1";
    }

    function place() {
        wrap.setAttribute("data-placement", prefer);
        wrap.setAttribute("data-align", align);
        panel.style.maxHeight = "";
        const k = measuredZoom(trigger);
        const clip = clipBox(wrap);
        const decision = choosePlacement(
            prefer,
            trigger.getBoundingClientRect(),
            clip,
            panel.scrollHeight,
            k,
        );
        wrap.setAttribute("data-placement", decision.placement);
        if (decision.maxHeight !== null) {
            panel.style.maxHeight = decision.maxHeight + "px";
        }
        // 水平:贴齐的那一侧越出裁切框就换另一侧(两侧都越界时保持默认,不来回翻)
        const pr = panel.getBoundingClientRect();
        if (align === "end" && pr.left < clip.left) {
            wrap.setAttribute("data-align", "start");
        } else if (align === "start" && pr.right > clip.right) {
            wrap.setAttribute("data-align", "end");
        }
    }

    function ensureVisible(el) {
        if (!el) return;
        const top = el.offsetTop;
        const bottom = top + el.offsetHeight;
        if (top < panel.scrollTop) panel.scrollTop = top;
        else if (bottom > panel.scrollTop + panel.clientHeight) {
            panel.scrollTop = bottom - panel.clientHeight;
        }
    }

    function focusItem(i) {
        const it = items[i];
        if (!it) return;
        it.el.focus({ preventScroll: true });
        ensureVisible(it.el);
    }

    function currentIndex() {
        for (let i = 0; i < items.length; i++) {
            if (items[i].f === value) return i;
        }
        return 0;
    }

    function setOpen(open, focusTarget) {
        const was = isOpen();
        if (open) {
            refreshOver();
            render();
        }
        wrap.setAttribute("data-open", open ? "1" : "0");
        trigger.setAttribute("aria-expanded", open ? "true" : "false");
        if (open) {
            place();
            focusItem(currentIndex());
        } else if (was && focusTarget === "trigger") {
            trigger.focus({ preventScroll: true });
        }
    }

    function pick(it) {
        setOpen(false, "trigger");
        // 超屏档位灰掉但不隐藏(05 §1.2)—— 选它不生效
        if (it.el.getAttribute("data-over") === "1") return;
        // 选的就是当前档:只收起(原生 <select> 同值不派发 change)
        if (it.f === value) return;
        onPick(it.f);
    }

    // 收起时焦点还给触发钮(展开时焦点在浮层里,浮层一收它就无处可落)
    trigger.addEventListener("click", () => setOpen(!isOpen(), "trigger"));
    trigger.addEventListener("keydown", (e) => {
        if (isModified(e)) return;
        if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
        e.preventDefault();
        if (!isOpen()) setOpen(true);
        else focusItem(currentIndex());
    });

    panel.addEventListener("click", (e) => {
        const btn =
            e.target instanceof Element
                ? e.target.closest("[data-scale]")
                : null;
        if (!btn) return;
        const it = items.find((x) => x.el === btn);
        if (it) pick(it);
    });

    panel.addEventListener("keydown", (e) => {
        if (isModified(e)) return;
        const at = items.findIndex((x) => x.el === doc.activeElement);
        const last = items.length - 1;
        let next = -1;
        if (e.key === "ArrowDown")
            next = at < 0 ? currentIndex() : Math.min(last, at + 1);
        else if (e.key === "ArrowUp")
            next = at < 0 ? currentIndex() : Math.max(0, at - 1);
        else if (e.key === "Home") next = 0;
        else if (e.key === "End") next = last;
        if (next >= 0) {
            e.preventDefault();
            focusItem(next);
            return;
        }
        if (e.key === "Enter" || e.key === " ") {
            // 自己接,不等按钮原生的 click:Space 的原生激活落在 keyup 上,那时焦点已经
            // 回到触发钮,会把刚收起的浮层又点开
            e.preventDefault();
            if (at >= 0) pick(items[at]);
            return;
        }
        if (e.key === "Escape") {
            e.preventDefault();
            setOpen(false, "trigger");
        }
        // Tab:不拦,焦点自然离开,下面的 focusout 收起浮层
    });

    // 下拉里按下鼠标**不挪焦点**(菜单类控件的通行做法)。不拦的话 WKWebView 上会出事:
    // WebKit 的按钮不接受鼠标焦点,按下浮层里的档位项时焦点会从当前档掉到 body,下面的
    // focusout 判「焦点离开了」先把浮层收起,随后那一下 click 就落空了;按触发钮去收起时
    // 同理会先收起、再被 click 重新打开。click 本身不受影响(只拦 mousedown 的默认动作)。
    wrap.addEventListener("mousedown", (e) => e.preventDefault());

    // 焦点离开下拉(Tab 走了 / 别处抢了焦点)⇒ 收起。判据看**落定之后**的 activeElement,
    // 不看 relatedTarget:Tab 出了 iframe / WebView 的最后一个控件时 relatedTarget 是 null,
    // 而那正是「离开」。窗口整体失焦时文档里的 activeElement 不变 ⇒ 不收(切回来浮层还在)。
    wrap.addEventListener("focusout", () => {
        setTimeout(() => {
            if (isOpen() && !wrap.contains(doc.activeElement)) setOpen(false);
        }, 0);
    });
    doc.addEventListener("pointerdown", (e) => {
        if (!isOpen()) return;
        if (e.target instanceof Node && wrap.contains(e.target)) return;
        setOpen(false);
    });

    refreshOver();
    render();

    return {
        /** 状态回推:当前档。 */
        setValue(f) {
            const v = Number(f);
            if (!Number.isFinite(v) || v <= 0) return;
            value = v;
            render();
        },
        value: () => value,
        /** 切语言后重写注记(data-t 已能被 applyI18n 刷到;这里是兜底)。 */
        render,
        setOpen: (open) => setOpen(!!open),
        isOpen,
    };
}
