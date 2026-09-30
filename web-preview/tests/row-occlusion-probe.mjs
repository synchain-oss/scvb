// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SL-230 复审②「常驻入口浮层遮挡」—— 轨道行的页面级命中测量台
// =============================================================================
// **不是冒烟**:文件名不叫 `smoke-*.mjs`,gate 3e 与 CI 的 web-smoke 都按那个 glob
// 取文件,故不会被自动跑、不给谁加「浏览器 + 预览服务器」的依赖。回归门禁在
// smoke-tab2-interactions.mjs 的 (c1)-(c5c) 组(源码级接线断言,不需要浏览器)。
//
// 本文件守的是那一层**测不到**的东西:遮挡是**布局**结果 —— 谁盖住谁只有真排版算得出来,
// 源码正则、纯函数断言都看不见。判据取 `elementFromPoint`(命中测试),不看样式字符串。
//
// 病理:`.tracks-row__hint` 是 `position:absolute; top:100%`,浮在**下一行**上;行只有在
// `data-confirm="1"` 时才被 `z-index:3` 抬到兄弟之上。把这枚入口做成常驻浮条,就等于给每条
// 手动常值轨永久置上 `data-confirm` —— 于是轨 N 的浮条稳稳盖住轨 N+1,下一行点不动。
// 这正是 SL-230 要消灭的「点了没反应」,换个地方又长了一个。
//
// 改法:触发钮改成**行内**小件(落进 label 单元格),浮条只在确认那一下出。
//
// 量什么(同一棵 DOM 上三态对照,不需要来回改代码):
//   基线 —— 无浮条、行未抬升 ⇒ 轨 N+1 顶部命中 = 轨 N+1 自己;
//   改前 —— 手工把浮条 hidden 摘掉 + 给轨 N 置 data-confirm=1(复刻旧设计)⇒ 命中 = 轨 N 的浮条;
//   改后 —— 现产品态(钮在行内、data-confirm 只在确认时置 1)⇒ 命中回到轨 N+1 自己。
//
// 定谳当时的实测(轨 3 / 轨 4):
//   基线 tracks-row-4 · 改前 **tracks-row-3-restore-auto-row** · 改后 tracks-row-4。
//
// ⚠ 自检不可省:若页面上凑不出两条**相邻**的手动常值轨,「改前」那一档量不出遮挡,
// 用例会假绿。故先自己用 setTrackManual 造出轨 3/轨 4 两条,造不出就当场宣告结论作废。
//
// 用法:node web-preview/tests/row-occlusion-probe.mjs [预览服务器端口]
// 退出码:0 = 三态如期;1 = 有一态不符;2 = 环境不具备(没浏览器/没服务器)。
// ⚠ 只跑 Windows:`EXE` 候选是两条 Windows 绝对路径,Linux/macOS 上恒 2 退出。手动测量台,
// 不进 CI,故不去做跨平台发现;要在别处跑就自己把 `EXE` 指到本机浏览器上。
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const PORT_PREVIEW = Number(process.argv[2] || 8823);
const URL_ = `http://127.0.0.1:${PORT_PREVIEW}/web-preview/output.html?fixture=fifteen-tracks`;
const CH_A = 3; // 上面那条(它的浮条会往下盖)
const CH_B = 4; // 紧邻的下一条(被盖的那条)
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
        `--user-data-dir=${mkdtempSync(join(tmpdir(), "scvb-230-"))}`,
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
ws.addEventListener("message", (ev_) => {
    const m = JSON.parse(ev_.data);
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

let bad = 0;
const done = async (code) => {
    try {
        chrome.kill();
    } catch {
        /* 已退出 */
    }
    process.exit(code);
};
const check = (cond, msg) => {
    console.log(`${cond ? "  ✔" : "  ✘"} ${msg}`);
    if (!cond) bad++;
};

await cmd("Page.enable");
await cmd("Runtime.enable");
await cmd("Page.navigate", { url: URL_ });
await sleep(3000);
// 预览页把产品页装在 iframe 里;拿到那份 document 与 window(桥挂在后者上)
await ev(`(() => {
  const f = document.querySelector("iframe");
  let d = document, w = window;
  try { if (f && f.contentDocument) { d = f.contentDocument; w = f.contentWindow; } } catch { /* 同源失败就用外层 */ }
  window.__D = d; window.__W = w; return 1;
})()`);
const clickIn = (s) =>
    ev(
        `(() => { const el = window.__D.querySelector(${JSON.stringify(s)});
      if (!el) return false; el.click(); return true; })()`,
    );
await clickIn('[data-gb="tour-ask-later"]');
await sleep(600);
await clickIn('[data-tab-btn="tracks"]');
await sleep(1200);

// —— 造两条**相邻**的手动常值轨(没有它们,「改前」那一档量不出遮挡)——
// 直接打 mock 后端(`window.__SCVB_MOCK__` = shell.js 注入给 createBridge 的那个),
// 不走 UI:走 UI 得先冻结再拖旋钮,造两条轨的路径比被测目标本身还长。
await ev(`(async () => {
  const b = window.__W.__SCVB_MOCK__;
  if (!b || typeof b.setTrackManual !== "function") throw new Error("mock backend 不在 window.__SCVB_MOCK__ 上");
  await b.setTrackManual(${CH_A}, "pan", 40);
  await b.setTrackManual(${CH_B}, "pan", -40);
  return 1;
})()`);
await sleep(1200);

const hasBtn = (ch) =>
    ev(
        `(() => { const el = window.__D.querySelector('[data-gb="tracks-row-${ch}-restore-auto"]');
      return !!el && !el.hidden; })()`,
    );
const seeded = (await hasBtn(CH_A)) && (await hasBtn(CH_B));
check(
    seeded,
    `轨 ${CH_A}/${CH_B} 都成了手动常值轨(自检:不成立则本用例结论作废)`,
);
if (!seeded) {
    await done(2);
}

// 命中测试:取轨 B 行**顶边往下 3px** 的那一点,问它最上层是谁。
// 取顶边是因为浮条从上一行的 top:100% 起画,盖住的正是这条行的上沿。
//
// ⚠ 复刻旧设计的那一档**必须与测量同在一次求值里**:产品的渲染循环每帧都会把
// `data-confirm` 写回真值,隔一个 await 再量,量到的已经是被改回去的现产品态
// (第一版探针就栽在这,「改前」一档量出 tracks-row-4,看着像「本机复现不出遮挡」)。
// 同步改属性 → 同步 elementFromPoint(强制同步布局)→ 同步还原,中间不让出事件循环。
const hitOfB = (mutate) =>
    ev(`(() => {
  const row  = window.__D.querySelector('[data-gb="tracks-row-${CH_B}"]');
  const bar  = window.__D.querySelector('[data-gb="tracks-row-${CH_A}-restore-auto-row"]');
  const rowA = window.__D.querySelector('[data-gb="tracks-row-${CH_A}"]');
  if (!row || !bar || !rowA) return "(no row)";
  const wasHidden = bar.hidden, wasConfirm = rowA.getAttribute("data-confirm");
  if (${mutate ? "true" : "false"}) { bar.hidden = false; rowA.setAttribute("data-confirm", "1"); }
  const r = row.getBoundingClientRect();
  const el = window.__D.elementFromPoint(r.left + 24, r.top + 3);
  // 往上找到最近的、带 data-gb 的祖先 —— 那才是「点到了谁」的可读答案
  let n = el;
  while (n && !n.getAttribute?.("data-gb")) n = n.parentElement;
  const hit = (n && n.getAttribute("data-gb")) || (el && el.className) || "(null)";
  bar.hidden = wasHidden;
  if (wasConfirm === null) rowA.removeAttribute("data-confirm");
  else rowA.setAttribute("data-confirm", wasConfirm);
  return hit;
})()`);

const after = await hitOfB(false);
check(
    after === `tracks-row-${CH_B}`,
    `改后(触发钮进行内、data-confirm 只在确认时置 1):轨 ${CH_B} 顶部命中 = ${after}`,
);

const before = await hitOfB(true);
check(
    before === `tracks-row-${CH_A}-restore-auto-row`,
    `改前(浮条常驻 + 行抬升):轨 ${CH_B} 顶部命中 = ${before}(应为轨 ${CH_A} 的浮条 —— 量不到它就说明本机复现不出遮挡,结论作废)`,
);

const base = await hitOfB(false);
check(
    base === `tracks-row-${CH_B}`,
    `基线(浮条收回、行未抬升):轨 ${CH_B} 顶部命中 = ${base}`,
);

console.log(bad ? `\n[FAIL] ${bad} 条不符` : "\n[OK] 三态如期");
await done(bad ? 1 : 0);
