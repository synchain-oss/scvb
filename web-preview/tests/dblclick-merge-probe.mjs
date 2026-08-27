// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SL-207 / 复审【重要】—— 双击与拖拽两个手势的页面级测量台(CDP 可信事件)
// =============================================================================
// **不是冒烟**:文件名不叫 `smoke-*.mjs`,gate 3e 与 CI 的 web-smoke 都按那个 glob
// 取文件,故不会被自动跑、不给谁加「浏览器 + 预览服务器」的依赖。回归门禁在
// smoke-tab3-interactions.mjs 的 ⑫ 组(纯函数 + 接线断言,不需要浏览器)。
//
// 本文件守的是那一层**测不到**的东西:手势冲突只有真事件才复现得出来 ——
// 「边界微拖 ×2 被浏览器判成 dblclick」这件事,页内合成事件根本不会发生。
//
// 量什么:
//   ① 原生右键菜单抑制:非可编辑处 defaultPrevented=true;输入框上放行
//   ② 双击段间边界 = 合并左右两段;双击段内 = 在此分割(两者互为逆操作)
//   ③ 【复审重要】边界微拖 ×2 不得被判成「双击删边界」,且正常双击不被误伤
//
// 定谳当时的实测(改前 / 改后):
//   ③ 微拖两次 → 改前段数 36→**35**(边界被误删)· 改后 36→36;
//      两档都确认「其间收到 dblclick 1 次」—— 没有这个自检,本用例会假绿(见下)。
//
// ⚠ **CDP 的 clickCount 要自己递增**:它不像真浏览器那样按时间+位置自动累计,
// 第二对 press/release 必须给 clickCount=2 才会真的派发 dblclick。第一版探针两次都给 1,
// 于是根本没有 dblclick —— 摘掉守卫也照样打「✔」,一个分不出修没修的假绿。
// 现在每次都打印收到的 dblclick 次数,收不到就当场宣告结论作废。
// 判据取轨头那颗「{n} 段」读数(data-gb=wave-lane-1-covseg),不碰闭包内的段表。
//
// 自洽用例(不需要事先知道边界在哪):
//   同一个 x 上双击两次 —— 第一次落在段内 ⇒ 分割,段数 +1,且**边界正好落在这个 x**;
//   第二次同一个 x ⇒ 命中刚生成的边界 ⇒ 合并,段数 −1 退回原值。
//   两次都成立,才同时证明「两分支都在」且「边界分支排在分割前面」。
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

const PORT = 9300 + Math.floor(Math.random() * 200);
const chrome = spawn(
    EXE,
    [
        "--headless=new",
        `--remote-debugging-port=${PORT}`,
        `--user-data-dir=${mkdtempSync(join(tmpdir(), "scvb-207-"))}`,
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
const root = (m, p) =>
    new Promise((ok, no) => {
        const mid = ++id;
        pending.set(mid, { ok, no });
        ws.send(JSON.stringify({ id: mid, method: m, params: p || {} }));
    });
const { targetId } = await root("Target.createTarget", { url: "about:blank" });
const { sessionId } = await root("Target.attachToTarget", {
    targetId,
    flatten: true,
});
const cmd = (m, p) =>
    new Promise((ok, no) => {
        const mid = ++id;
        pending.set(mid, { ok, no });
        ws.send(
            JSON.stringify({ id: mid, sessionId, method: m, params: p || {} }),
        );
    });
async function ev(expr) {
    const r = await cmd("Runtime.evaluate", {
        expression: expr,
        returnByValue: true,
        awaitPromise: true,
    });
    if (r.exceptionDetails) {
        throw new Error(
            r.exceptionDetails.exception?.description ||
                r.exceptionDetails.text,
        );
    }
    return r.result?.value;
}

await cmd("Page.enable");
await cmd("Runtime.enable");
await cmd("Page.navigate", { url: URL_ });
await sleep(3000);
await ev(`window.__D = (() => {
  const f = document.querySelector("iframe");
  try { return (f && f.contentDocument) || document; } catch { return document; }
})(), 1`);
const clickIn = (s) =>
    ev(
        `(() => { const el = window.__D.querySelector(${JSON.stringify(s)});
      if (!el) return false; el.click(); return true; })()`,
    );
await clickIn('[data-gb="tour-ask-later"]');
await sleep(600);
await clickIn('[data-tab-btn="wave"]');
await sleep(1500);

// 记录 contextmenu 是否被拦(捕获阶段挂,恒先于产品监听器跑到)
await ev(`(() => {
  window.__CM = [];
  window.__D.addEventListener("contextmenu", (e) => {
    // **必须延到下一个宏任务再读**:产品监听器挂在 document 的冒泡阶段,而本监听器
    // 在捕获阶段;微任务在同一次派发的任务尾部就被抽干,读到的仍是冒泡跑完之前的值
    // (第一版探针就栽在这,读出 false 却把产品判成没拦)。
    setTimeout(() => window.__CM.push({
      tag: (e.target && e.target.tagName) || "?", prevented: e.defaultPrevented,
    }), 0);
  }, true);
  return 1;
})()`);

async function pointOf(sel, fx = 0.5, fy = 0.5) {
    return ev(`(() => {
      const f = document.querySelector("iframe");
      const fr = f.getBoundingClientRect();
      const s = fr.width / f.contentWindow.innerWidth;
      const el = window.__D.querySelector(${JSON.stringify(sel)});
      if (!el) return null;
      const er = el.getBoundingClientRect();
      return { x: fr.left + (er.left + er.width * ${fx}) * s,
               y: fr.top + (er.top + er.height * ${fy}) * s };
    })()`);
}
async function mouse(type, pt, button, clickCount) {
    await cmd("Input.dispatchMouseEvent", {
        type,
        x: Math.round(pt.x),
        y: Math.round(pt.y),
        button,
        buttons: button === "right" ? 2 : 1,
        clickCount: clickCount || 1,
        pointerType: "mouse",
    });
}
async function rightClick(pt) {
    await mouse("mousePressed", pt, "right", 1);
    await mouse("mouseReleased", pt, "right", 1);
    await sleep(350);
}
async function dblClick(pt) {
    for (const n of [1, 2]) {
        await mouse("mousePressed", pt, "left", n);
        await mouse("mouseReleased", pt, "left", n);
    }
    await sleep(700);
}
const segCount = () =>
    ev(
        `(() => { const n = window.__D.querySelector('[data-gb="wave-lane-1-covseg"]');
      const m = n && /(\\d+)\\s*段/.exec(n.textContent || ""); return m ? +m[1] : null; })()`,
    );

const L = console.log;
L("=".repeat(80));
L("SL-207 页面级实测(CDP 可信事件)");
L("=".repeat(80));

// ---- ① 右键菜单 ----
{
    const lane = await pointOf('[data-gb="wave-lane-1"]');
    await rightClick(lane);
    const onLane = await ev("window.__CM.slice(-1)[0] || null");
    L(
        `\n① 泳道上右键:target=${onLane && onLane.tag}  defaultPrevented=${onLane && onLane.prevented}`,
    );
    L(
        `   期望 true(拦下浏览器菜单)—— ${onLane && onLane.prevented ? "✔" : "✘"}`,
    );

    // 输入框的**放行**档改用「直接向元素派发」而不是按坐标点:页内的 text input
    // 多在设置页且可能被滚动/折叠挡住,按坐标点常常落到 BODY 上(第一版探针就这么
    // 把 BODY 的命中当成了输入框的,读出「放行失败」)。这一档要验的是**目标判定**
    // 而不是坐标路由,直接派发到元素上更贴题、也不会假阴。
    const r = await ev(`(() => {
      const D = window.__D;
      const inp = D.querySelector('input[type="text"]');
      if (!inp) return null;
      const e = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
      inp.dispatchEvent(e);
      return { tag: inp.tagName, prevented: e.defaultPrevented };
    })()`);
    if (r) {
        L(
            `   输入框上右键(直接派发):target=${r.tag}  defaultPrevented=${r.prevented}`,
        );
        L(
            `   期望 false(粘贴要用原生菜单)—— ${r.prevented === false ? "✔" : "✘"}`,
        );
    } else {
        L("   (页内没找到 text input,跳过放行档)");
    }
}

// ---- ② 双击两义:同一 x 连做两次,必须互为逆操作 ----
// 不预设第一次一定落在段内:轨 1 有 36 段,边界密到每 ~2.8% 宽一条,随手取的 x
// 常常就在某条边界的 ±6px 内(第一版探针据此把「合并成功」误判成「分割失败」)。
// 正确判据 = **两次双击的方向相反、且段数回到原值**:
//   落段内  ⇒ 先 +1(分割)后 −1(合并刚生成的那条边界)
//   落边界上 ⇒ 先 −1(合并)后 +1(在同一处再分割开)
// 两种序列都同时证明「两分支都在」;而「先 −1 后 +1」这一支还额外证明了
// **边界分支排在分割前面** —— 顺序颠倒的话双击边界只会去分割,永远读不到 −1。
{
    L(`\n② 双击两义(轨 1 段数;同一 x 连做两次,必须互为逆操作)`);
    let sawSplitFirst = false;
    let sawMergeFirst = false;
    for (const fx of [0.42, 0.55, 0.63, 0.71]) {
        const pt = await pointOf('[data-gb="wave-lane-1"]', fx, 0.5);
        const n0 = await segCount();
        await dblClick(pt);
        const n1 = await segCount();
        await dblClick(pt);
        const n2 = await segCount();
        const d1 = n1 - n0;
        const kind =
            d1 === 1 ? "分割(落段内)" : d1 === -1 ? "合并(落边界上)" : "无变化";
        if (d1 === 1) sawSplitFirst = true;
        if (d1 === -1) sawMergeFirst = true;
        const inverse = n2 === n0 && Math.abs(d1) === 1;
        L(
            `   x=${fx}: ${n0} → ${n1} → ${n2}  首次=${kind}  互逆=${inverse ? "✔" : "✘"}`,
        );
    }
    L(`   观察到「双击段内 ⇒ 分割」—— ${sawSplitFirst ? "✔" : "✘ 未触到段内"}`);
    L(`   观察到「双击边界 ⇒ 合并」—— ${sawMergeFirst ? "✔" : "✘ 未触到边界"}`);
    L(`   (后者同时证明边界分支排在分割之前)`);
}

// ---- ③【复审重要】边界微拖 × 2 不得被判成「双击删边界」 ----
// 手势是「按下-微移-释放」,连着两次浏览器就发一个 dblclick。守卫的判据是
// 「刚真发过 move_boundary」,所以这里必须发**真的微拖**(不是原地点击 ——
// 那条被零位移短路挡掉、根本不打时间戳)。
{
    L(`\n③ 边界微拖 ×2(复审【重要】:不得触发合并)`);
    const X = (v) => Math.round(v);
    // clickCount 必须自己递增:CDP 不像真浏览器那样按「时间 + 位置」自动累计,
    // 第二对 press/release 给 clickCount=2 才会真的派发 dblclick。
    // **第一版探针就栽在这**:两次都给 1 ⇒ 根本没有 dblclick ⇒ 摘掉守卫也照样「✔」,
    // 一个分不出修没修的假绿。
    const microDrag = async (pt, dx, clickCount) => {
        await cmd("Input.dispatchMouseEvent", {
            type: "mousePressed",
            x: X(pt.x),
            y: X(pt.y),
            button: "left",
            buttons: 1,
            clickCount,
            pointerType: "mouse",
        });
        await cmd("Input.dispatchMouseEvent", {
            type: "mouseMoved",
            x: X(pt.x + dx),
            y: X(pt.y),
            button: "left",
            buttons: 1,
            pointerType: "mouse",
        });
        await cmd("Input.dispatchMouseEvent", {
            type: "mouseReleased",
            x: X(pt.x + dx),
            y: X(pt.y),
            button: "left",
            buttons: 0,
            clickCount,
            pointerType: "mouse",
        });
    };
    // 自检:这一段必须真的收到 dblclick,否则本用例分不出修没修
    await ev(`(() => { window.__DBL = 0;
      window.__D.addEventListener("dblclick", () => { window.__DBL++; }, true); return 1; })()`);
    const pt = await pointOf('[data-gb="wave-lane-1"]', 0.42, 0.5);
    const n0 = await segCount();
    await microDrag(pt, 2, 1);
    await sleep(120);
    await microDrag(pt, 2, 2); // 第二次微拖 —— dblclick 在这里派发
    await sleep(900);
    const n1 = await segCount();
    const dbl = await ev("window.__DBL");
    L(`   微拖两次:段数 ${n0} → ${n1}(其间收到 dblclick ${dbl} 次)`);
    if (!dbl) L("   ⚠ 没收到 dblclick —— 本用例此时分不出修没修,结论作废");
    L(
        `   期望**不变**(守卫让路,只提交 move_boundary)—— ${n0 === n1 ? "✔" : "✘ 边界被误删"}`,
    );

    // 对照:过了让路窗再双击,合并必须照常(守卫不能误伤正常手势)
    await sleep(700);
    const n2 = await segCount();
    await dblClick(pt);
    const n3 = await segCount();
    L(`   过窗后双击:段数 ${n2} → ${n3}`);
    L(`   期望 −1(正常合并没被误伤)—— ${n3 === n2 - 1 ? "✔" : "✘"}`);
}

L("\n" + "=".repeat(80));
try {
    ws.close();
} catch {
    /* 已关 */
}
chrome.kill();
process.exit(0);
