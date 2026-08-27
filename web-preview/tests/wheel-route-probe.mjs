// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SL-205 滚轮四路映射 —— 页面级测量台(无头 Chrome + CDP Input,**可信事件**)
// =============================================================================
// **这不是冒烟**:文件名不叫 `smoke-*.mjs`,gates 的 gate 3e 与 CI 的 web-smoke 都按
// 那个 glob 取文件,所以它不会被自动跑、不给谁加「浏览器 + 预览服务器」的依赖。
// 四路映射的**回归门禁**在 smoke-tab3-interactions.mjs 的 ⑪ 组(wheelRoute / wheelPx
// 纯函数 + 处理器接线的源码级断言),那一层不需要浏览器。
//
// 那本文件守什么:**「漏给浏览器默认动作」只有可信事件才量得到**。页内
// `dispatchEvent(new WheelEvent(...))` 是 isTrusted=false 的合成事件,**不触发默认动作**;
// 而本卡的病根恰恰是「没被 preventDefault 的那几路掉回了默认滚动」——
// 实测改前裸滚轮把泳道列竖着滚跑了 121px。只有 CDP 的 Input.dispatchMouseEvent 复现得出来。
//
// 定谳当时的实测(同一台无头 Chrome,改前 / 改后):
//   路               改前                                  改后
//   裸滚轮 横向平移   ✘ 漏给默认动作(lanesScrollTop 0→121)  ✔ 两向都动
//   Ctrl   横向缩放   ✔(这一路一直是好的)                   ✔ 两向都动
//   Shift  纵向平移   ✘ 毫无反应                            ✔ 两向都动
//   Alt    纵向缩放   ✘ 漏给默认动作(同上)                  ✔ 两向都动
//
// **夹取陷阱**(本探针连栽两次,写在这里免得下一个人再栽):初值 ×1.0 / 行高 34 /
// 滚动 0 都贴着某一侧的夹取边界 —— 往那一侧推读出来是「无变化」,看着像功能坏了;
// 而用「反复推同一方向」去造余量,又会冲到另一侧边界(实测顶到 ×300、行高 88),
// 照样读成假失效。故每路测前**重载页面回到确定初值**,再施一点点 prime。
//
// 用法:node web-preview/tests/wheel-route-probe.mjs <preview 端口>
// 前置:pwsh web-preview/serve.ps1 -Port <端口>
// 退出码:0 = 量完并打印;2 = 本机没有 Chrome/Edge;3 = 页面没起来。
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PORT_PREVIEW = Number(process.argv[2] || 8823);
const URL_ = `http://127.0.0.1:${PORT_PREVIEW}/web-preview/output.html?fixture=fifteen-tracks`;

const EXE = [
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
].find(existsSync);
if (!EXE) {
    console.error("no browser");
    process.exit(2);
}

const PORT = 9500 + Math.floor(Math.random() * 200);
const chrome = spawn(
    EXE,
    [
        "--headless=new",
        `--remote-debugging-port=${PORT}`,
        `--user-data-dir=${mkdtempSync(join(tmpdir(), "scvb-wheel-"))}`,
        "--window-size=1500,950",
        "--no-first-run",
        "--no-default-browser-check",
        "--force-device-scale-factor=1",
        "--hide-scrollbars",
        "about:blank",
    ],
    { stdio: "ignore" },
);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let wsUrl = null;
for (let i = 0; i < 80 && !wsUrl; i++) {
    try {
        wsUrl = (
            await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json()
        ).webSocketDebuggerUrl;
    } catch {
        await sleep(150);
    }
}
const ws = new WebSocket(wsUrl);
const pending = new Map();
let id = 0;
ws.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
        const { ok, no } = pending.get(m.id);
        pending.delete(m.id);
        m.error ? no(new Error(m.error.message)) : ok(m.result);
    }
});
await new Promise((ok) => ws.addEventListener("open", ok, { once: true }));
const root = (method, params) =>
    new Promise((ok, no) => {
        const mid = ++id;
        pending.set(mid, { ok, no });
        ws.send(JSON.stringify({ id: mid, method, params: params || {} }));
    });
const { targetId } = await root("Target.createTarget", { url: "about:blank" });
const { sessionId } = await root("Target.attachToTarget", {
    targetId,
    flatten: true,
});
const cmd = (method, params) =>
    new Promise((ok, no) => {
        const mid = ++id;
        pending.set(mid, { ok, no });
        ws.send(
            JSON.stringify({
                id: mid,
                sessionId,
                method,
                params: params || {},
            }),
        );
    });

async function evalIn(expr) {
    const r = await cmd("Runtime.evaluate", {
        expression: expr,
        returnByValue: true,
        awaitPromise: true,
    });
    if (r.exceptionDetails) {
        throw new Error(
            "页内求值抛错:" +
                (r.exceptionDetails.exception?.description ||
                    r.exceptionDetails.text),
        );
    }
    return r.result?.value;
}

await cmd("Page.enable");
await cmd("Runtime.enable");
await cmd("Page.navigate", { url: URL_ });
await sleep(3000);

// 之后一切选择器走 __D(iframe 内的真源文档),与 shot.mjs 同口径
await evalIn(`window.__D = (() => {
  const f = document.querySelector("iframe");
  try { return (f && f.contentDocument) || document; } catch { return document; }
})(), 1`);

const clickIn = (sel) =>
    evalIn(
        `(() => { const el = window.__D.querySelector(${JSON.stringify(sel)});
      if (!el) return false; el.click(); return true; })()`,
    );

await clickIn('[data-gb="tour-ask-later"]');
await sleep(600);
await clickIn('[data-tab-btn="wave"]');
await sleep(1200);

// iframe 内坐标 → 顶层视口坐标(壳页对 iframe 可能有缩放,按渲染宽/内部宽求比例)
async function pointOf(sel, fx = 0.5, fy = 0.5) {
    return evalIn(`(() => {
      const f = document.querySelector("iframe");
      const fr = f.getBoundingClientRect();
      const s  = fr.width / f.contentWindow.innerWidth;
      const el = window.__D.querySelector(${JSON.stringify(sel)});
      if (!el) return null;
      const er = el.getBoundingClientRect();
      return { x: fr.left + (er.left + er.width * ${fx}) * s,
               y: fr.top  + (er.top  + er.height * ${fy}) * s };
    })()`);
}

// 观测面:全部取可见 DOM,不碰闭包内的 timeline
async function snap() {
    return evalIn(`(() => {
      const D = window.__D;
      const lanes = D.querySelector('[data-gb="wave-lanes"]');
      const panel = D.querySelector('[data-gb="tab-wave"]');
      const win   = D.querySelector('[data-gb="wave-window"]');
      const hz    = D.querySelector('[data-gb="wave-hzoom-value"]');
      const vz    = D.querySelector('[data-gb="wave-vzoom-value"]');
      const thumb = D.querySelector('[data-gb="wave-hzoom-thumb"]');
      const tick  = D.querySelector('.wave-ruler__tick');
      return {
        hzoom: hz ? hz.textContent.trim() : null,
        vzoom: vz ? vz.textContent.trim() : null,
        laneH: lanes ? lanes.style.getPropertyValue("--lane-h") : null,
        lanesScrollTop: lanes ? lanes.scrollTop : null,
        panelScrollTop: panel ? panel.scrollTop : null,
        winScrollTop:   win ? win.scrollTop : null,
        docScrollTop:   D.scrollingElement ? D.scrollingElement.scrollTop : null,
        thumbLeft: thumb ? thumb.style.left : null,
        firstTick: tick ? tick.textContent.trim() : null,
      };
    })()`);
}

const MOD = { none: 0, alt: 1, ctrl: 2, shift: 8 };
async function wheel(pt, mod, dy, times = 3) {
    for (let i = 0; i < times; i++) {
        await cmd("Input.dispatchMouseEvent", {
            type: "mouseWheel",
            x: Math.round(pt.x),
            y: Math.round(pt.y),
            deltaX: 0,
            deltaY: dy,
            modifiers: MOD[mod] || 0,
            pointerType: "mouse",
        });
        await sleep(120);
    }
    await sleep(400);
}

const L = console.log;
const diff = (a, b) => {
    const out = [];
    for (const k of Object.keys(a)) {
        if (JSON.stringify(a[k]) !== JSON.stringify(b[k])) {
            out.push(`${k}: ${JSON.stringify(a[k])} → ${JSON.stringify(b[k])}`);
        }
    }
    return out.length ? out.join(" | ") : "(无变化)";
};

const pt = await pointOf('[data-gb="wave-window"]');
if (!pt) {
    console.error("找不到泳道窗");
    process.exit(3);
}
L("=".repeat(84));
L("SL-205 滚轮四路映射 —— 页面级实测(CDP 可信事件,会触发默认动作)");
L(`泳道窗中心(顶层视口坐标)= (${pt.x.toFixed(0)}, ${pt.y.toFixed(0)})`);
L("=".repeat(84));

const base = await snap();
L(`\n初始:${JSON.stringify(base)}`);

// **夹取陷阱**(第一版探针连栽两次,记在这里):初始 ×1.0 是最小缩放、行高 34、
// 滚动位置 0 —— 这些都贴着某一侧的夹取边界,往那一侧再推读出来就是「无变化」,
// 看着像功能坏了,其实是探针没给余量。而如果用「反复推同一方向」来造余量,又会
// 冲到另一侧的夹取边界(实测把缩放顶到 ×300、行高顶到 88),照样读成假失效。
// 正解:**每路测前重载页面回到确定初值**,再施一点点固定 prime 让两向都有余量。
async function gotoWave() {
    await cmd("Page.navigate", { url: URL_ });
    await sleep(3000);
    await evalIn(`window.__D = (() => {
      const f = document.querySelector("iframe");
      try { return (f && f.contentDocument) || document; } catch { return document; }
    })(), 1`);
    await clickIn('[data-gb="tour-ask-later"]');
    await sleep(500);
    await clickIn('[data-tab-btn="wave"]');
    await sleep(1200);
}
async function prime() {
    await wheel(pt, "ctrl", -120, 2); // 放大到 ~×3:横向平移与缩小都有余量
    await wheel(pt, "alt", -120, 1); // 行高 34→38:纵向缩放两向都有余量
    await wheel(pt, "shift", 120, 1); // 往下滚一点:纵向平移两向都有余量
}

for (const [tag, mod, expect, keys] of [
    ["③ 裸滚轮 → 横向平移", "none", "视口左右移", ["firstTick", "thumbLeft"]],
    ["① Ctrl+滚轮 → 横向缩放", "ctrl", "hzoom 读数变", ["hzoom"]],
    ["② Shift+滚轮 → 纵向平移", "shift", "泳道列竖着滚", ["lanesScrollTop"]],
    ["④ Alt+滚轮 → 纵向缩放", "alt", "行高变", ["laneH"]],
]) {
    L(`\n${tag}(期望:${expect})`);
    for (const [dirTag, dy] of [
        ["正向(下滚 +)", 120],
        ["反向(上滚 −)", -120],
    ]) {
        await gotoWave();
        await prime();
        const a = await snap();
        await wheel(pt, mod, dy);
        const b = await snap();
        const moved = keys.some(
            (k) => JSON.stringify(a[k]) !== JSON.stringify(b[k]),
        );
        L(`   ${dirTag}:${moved ? "✔ 生效" : "✘ 无反应"}  ${diff(a, b)}`);
    }
}
// 组合切换也从干净态起,免得继承上面测出来的极值
await gotoWave();
await prime();

// ---- 复现用户报的组合:Alt 用过之后 Ctrl 还灵不灵 ----
L("\n" + "-".repeat(84));
L("组合复现:Alt(纵缩放)之后再 Ctrl(横缩放)——用户报 Ctrl 会失效");
{
    const a0 = await snap();
    await wheel(pt, "ctrl", -120, 2);
    const a1 = await snap();
    L(`  (a) 先单独 Ctrl:${diff(a0, a1)}`);

    await wheel(pt, "alt", 120, 3);
    const a2 = await snap();
    L(`  (b) 再 Alt:      ${diff(a1, a2)}`);

    await wheel(pt, "ctrl", -120, 2);
    const a3 = await snap();
    L(`  (c) 又 Ctrl:     ${diff(a2, a3)}`);
    L(`  → Ctrl 在 Alt 之后${a3.hzoom !== a2.hzoom ? "仍然有效" : "**失效**"}`);

    // 另一条更可能的因果:裸滚轮把泳道列**竖着滚跑了**(实测确实会),
    // 视图突变之后用户再 Ctrl,观感上就像「缩放坏了」。分开验一下 Ctrl 本身。
    await wheel(pt, "none", 240, 3);
    const a4 = await snap();
    L(`  (d) 裸滚轮滚动后:${diff(a3, a4)}`);
    await wheel(pt, "ctrl", -120, 2);
    const a5 = await snap();
    L(`  (e) 再 Ctrl:     ${diff(a4, a5)}`);
    L(
        `  → Ctrl 在裸滚轮之后${a5.hzoom !== a4.hzoom ? "仍然有效" : "**失效**"}`,
    );
}

L("\n" + "=".repeat(84));
try {
    ws.close();
} catch {
    /* 已关 */
}
chrome.kill();
process.exit(0);
