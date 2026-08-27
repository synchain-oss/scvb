// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SL-191「柱头永远碰不到白线」—— 峰线可达性的**渲染层**测量台(无头 Chrome + CDP)
// =============================================================================
// **这不是冒烟**:文件名不叫 `smoke-*.mjs`,故 gates 的 gate 3e 与 CI 的 web-smoke
// 都不会自动跑它(两处都按 `smoke-*.mjs` 取文件)。它是一把**量尺**,留在仓里是为了让
// SL-191 的定谳可复算,也让日后接线总线电平表(.sc-meter*)的人能照同一套方法学重测
// —— 那一组眼下没有驱动,**不能照抄本次结论**(它的 fill 走 clip-path,机理与 width 不同)。
//
// 与模型层测量的分工:
//   · 模型层(advance 的 db/peakDb 数值)= smoke-tab2-interactions.mjs 里
//     「SL-191 新极大值那一帧 db==peakDb」那组,纯函数、不需要浏览器;
//   · 渲染层(真 tokens.css / base.css + 真 createMeterRenderer + 真 30Hz 事件节奏
//     + 60fps rAF,逐帧 getBoundingClientRect)= 本文件。CSS transition 造成的位移
//     滞后**只有在这一层量得到** —— 模型层怎么看都是对的,这正是本病难查的原因。
//
// 定谳当时的实测(worktree = scvb-wt-fp,device-scale=1,1 dB = 4.37px):
//   变体        阶跃滞后    渲染触线%(模型 ≈12%)   「该触线」帧的渲染Δ 均值/P90
//   current     181.8ms     4.90%                    11.74px / 29.06px
//   no-liquid   0.0ms       14.03%                   −2.35px(柱头反冲到线外)
//   no-both     0.0ms       12.95%                   0.00px / 0.00px   ← 采用
//   short-33    36.4ms      11.70%                   6.00px / 25.98px
//
// 用法:node web-preview/tests/peakline-render.mjs [worktree 根] [--variant=…]
//   --variant=current | no-peak | no-liquid | no-both | short-<ms>
//   current = 不打补丁,量真源现状;其余用 !important 就地改写 transition 作对照。
// 退出码:0 = 量完并打印;2 = 本机没有 Chrome/Edge;3/4/5 = 页面没跑起来或几何是假的。
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdtempSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// 默认取本文件所在仓库的根(web-preview/tests/ 往上两级),不写死任何人的本机路径。
const ROOT =
    process.argv[2] && !process.argv[2].startsWith("--")
        ? process.argv[2]
        : resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
// 变体:current(现状)/ no-peak / no-liquid / no-both / short-<ms>
const VARIANT = (
    process.argv.find((a) => a.startsWith("--variant=")) || "--variant=current"
).slice(10);

const MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".woff2": "font/woff2",
};

function variantCss(v) {
    if (v === "current") return "";
    if (v === "no-peak") return `.sc-tube__peak{transition:none !important}`;
    if (v === "no-liquid")
        return `.sc-tube__liquid{transition:none !important}`;
    if (v === "no-both")
        return `.sc-tube__liquid,.sc-tube__peak{transition:none !important}`;
    const m = /^short-(\d+)$/.exec(v);
    if (m)
        return `.sc-tube__liquid{transition:width ${m[1]}ms linear !important}
                .sc-tube__peak{transition:left ${m[1]}ms linear !important}`;
    throw new Error("未知变体 " + v);
}
const PATCH_CSS = variantCss(VARIANT);

const PROBE = `<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="/web/shared/tokens.css">
<link rel="stylesheet" href="/web/shared/base.css">
<style>html,body{margin:0;padding:0;background:#000}
 /* display:flex 是必须的:.sc-tube 是 <span>,真页面里它是 flex 子项才被块化;
    放进普通 div 会保持 inline ⇒ width:268px 不生效 ⇒ 整根管零宽,量出来
    全是常数(第一版就栽在这)。下面还有一道非零几何守卫兜底。 */
 #body{position:absolute;left:20px;top:20px;display:flex}
 ${PATCH_CSS}</style></head><body>
<div id="body">
  <span class="sc-tube" data-gb="tracks-row-1-vol-tube">
    <span class="sc-tube__slot">
      <span class="sc-tube__liquid" style="--lv:0%"></span>
      <span class="sc-tube__peak" style="--pk:0%" data-alert="0" hidden></span>
    </span>
  </span>
</div>
<script type="module">
import { createMeterRenderer, dbToRatio }
  from "/web/output/canvas/meter.js";

const liquid = document.querySelector(".sc-tube__liquid");
const peak   = document.querySelector(".sc-tube__peak");
const store  = { playhead: { isPlaying: true, timeS: 0 } };
const m = createMeterRenderer({
  body: document.getElementById("body"),
  getStore: () => store,
  isActive: () => true,
});
m.attach();

// ---- 素材:人声样形(口径同 peakline-probe.mjs 实验 4)-----------------------
// 音节 220ms 有声 / 180ms 间隙,句内 RMS 起伏 ±4 dB;确定性伪随机便于复现。
let s = 12345;
const rnd = () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296);
const syll = [];
for (let i = 0; i < 40; i++) syll.push(-14 + (rnd() * 8 - 4));
const vocalDb = (t) => {
  const i = Math.floor(t / 400), ph = t % 400;
  return ph > 220 ? -46 : syll[i % syll.length];
};
// 阶跃素材:-6 稳态 → -40 稳态(峰线高挂)→ -6 再来一次(模型上 lv==pk)
const stepDb = (t) => (t < 600 ? -6 : t < 1400 ? -40 : -6);

const EV_MS = 1000 / 30;          // §2.5 事件 30Hz
const rows = [];
let t0 = null, lastEv = -1e9;

function makeEvent(db) {
  // peakDb 按契约照发(块真峰值 ≈ RMS+3dB),但现行 UI 不消费它 —— 照发以求真
  return { tracks: Array.from({ length: 15 }, (_, i) =>
    i === 0 ? { db, peakDb: Math.min(0, db + 3) } : { db: -60, peakDb: -60 }) };
}

function runPhase(name, sig, durMs) {
  return new Promise((done) => {
    t0 = null; lastEv = -1e9;
    const loop = (ts) => {
      if (t0 === null) t0 = ts;
      const t = ts - t0;
      if (t - lastEv >= EV_MS) { lastEv = t; m.push(makeEvent(sig(t))); }
      m.tick(ts);
      const lb = liquid.getBoundingClientRect();
      const pb = peak.getBoundingClientRect();
      const st = m.stateOf(1);
      rows.push({
        phase: name, t: Math.round(t * 10) / 10,
        db: st.db, peakDb: st.peakDb,
        modelGapDb: st.peakDb - st.db,
        // 模型上「柱头该在哪」与「白线外沿该在哪」(比例 → 同一 slot 宽)
        lvPct: dbToRatio(st.db) * 100, pkPct: dbToRatio(st.peakDb) * 100,
        // 渲染实测:液柱右缘 vs 峰线右缘(= 白线外沿,P1-8 之后两者应重合)
        liquidRight: lb.right, peakRight: pb.right,
        hidden: peak.hidden,
        renderGapPx: pb.right - lb.right,
      });
      if (t < durMs) requestAnimationFrame(loop); else done();
    };
    requestAnimationFrame(loop);
  });
}

(async () => {
  // 非零几何守卫:管子没被块化 / attach 没接上时,rect 会全是常数,
  // 统计仍会「全绿」地跑完并给出漂亮的 0.00px —— 那是假数据,必须当场判死。
  const slotW = document.querySelector(".sc-tube__slot").getBoundingClientRect().width;
  if (!(slotW > 200)) {
    window.__ERR__ = "slot 宽 " + slotW + "px —— 管子没铺开,量到的是假几何";
    window.__DONE__ = true;
    return;
  }
  await runPhase("step", stepDb, 2600);
  m.stop();
  await new Promise((r) => setTimeout(r, 100));
  await runPhase("vocal", vocalDb, 16000);
  window.__RESULT__ = JSON.stringify(rows);
  window.__DONE__ = true;
})();
</script></body></html>`;

const server = createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p === "/probe.html") {
        res.writeHead(200, { "Content-Type": MIME[".html"] }).end(PROBE);
        return;
    }
    const abs = resolve(join(ROOT, p));
    if (
        !abs.startsWith(resolve(ROOT)) ||
        !existsSync(abs) ||
        !statSync(abs).isFile()
    ) {
        res.writeHead(404).end("no");
        return;
    }
    res.writeHead(200, {
        "Content-Type":
            MIME[extname(abs).toLowerCase()] || "application/octet-stream",
        "Cache-Control": "no-store",
    }).end(readFileSync(abs));
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const BASE = `http://127.0.0.1:${server.address().port}`;

const EXE = [
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
].find(existsSync);
if (!EXE) {
    console.error("no browser");
    process.exit(2);
}

const PORT = 9700 + Math.floor(Math.random() * 200);
const chrome = spawn(
    EXE,
    [
        "--headless=new",
        `--remote-debugging-port=${PORT}`,
        `--user-data-dir=${mkdtempSync(join(tmpdir(), "scvb-pkr-"))}`,
        "--window-size=900,300",
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
const { targetId } = await send0("Target.createTarget", { url: "about:blank" });
const { sessionId } = await send0("Target.attachToTarget", {
    targetId,
    flatten: true,
});
function send0(method, params) {
    return new Promise((ok, no) => {
        const mid = ++id;
        pending.set(mid, { ok, no });
        ws.send(JSON.stringify({ id: mid, method, params: params || {} }));
    });
}
function cmd(method, params) {
    return new Promise((ok, no) => {
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
}

await cmd("Page.enable");
await cmd("Runtime.enable");
await cmd("Page.navigate", { url: `${BASE}/probe.html` });

for (let i = 0; i < 200; i++) {
    await sleep(300);
    const r = await cmd("Runtime.evaluate", {
        expression: "!!window.__DONE__",
        returnByValue: true,
    });
    if (r.result.value) break;
}
const err = await cmd("Runtime.evaluate", {
    expression: "window.__ERR__ || ''",
    returnByValue: true,
});
if (err.result.value) {
    console.error("几何守卫判死:" + err.result.value);
    process.exit(4);
}
const got = await cmd("Runtime.evaluate", {
    expression: "window.__RESULT__",
    returnByValue: true,
});
if (!got.result.value) {
    console.error("页面没跑完(rAF 没起或脚本抛错)");
    process.exit(3);
}
const rows = JSON.parse(got.result.value);
{
    const spread = new Set(rows.map((r) => Math.round(r.liquidRight))).size;
    if (spread < 5) {
        console.error(
            `液柱右缘只有 ${spread} 个不同取值 —— 几何是死的,数据不可信`,
        );
        process.exit(5);
    }
}

// ---------------------------------------------------------------- 统计与打印
const fmt = (x, n = 2) => (Math.round(x * 10 ** n) / 10 ** n).toFixed(n);
const L = console.log;
const SLOT_W = 268 - 2 - 4;

L("=".repeat(86));
L(`SL-191 峰线可达性 —— 真实渲染几何实测  变体=${VARIANT}`);
L(`worktree=${ROOT}`);
L(`帧数 ${rows.length};1 dB = ${fmt(SLOT_W / 60)} px`);
L("=".repeat(86));

// ---- 阶跃相:看单个瞬态里两者怎么走 ----
const step = rows.filter((r) => r.phase === "step");
L("\n【阶跃相】-6 稳态 → -40 稳态(白线高挂)→ t=1400ms 起再来 -6");
L("  模型上 t>=1400 的第一帧就有 lv==pk;问渲染要多久才真的重合。");
L("    t(ms)   模型Δ(dB)   液柱right   峰线right   渲染Δ(px)");
for (const ms of [
    500, 1390, 1400, 1420, 1450, 1500, 1550, 1600, 1700, 1800, 2000, 2400,
]) {
    const r = step.find((x) => x.t >= ms);
    if (!r) continue;
    L(
        `  ${String(ms).padStart(6)}   ${fmt(r.modelGapDb).padStart(8)}   ` +
            `${fmt(r.liquidRight).padStart(9)}   ${fmt(r.peakRight).padStart(9)}   ${fmt(r.renderGapPx).padStart(8)}`,
    );
}
{
    const after = step.filter((r) => r.t >= 1400);
    const touch = after.find((r) => r.renderGapPx < 0.5);
    const m0 = after.find((r) => r.modelGapDb < 1e-9);
    L(`  模型首次 Δ==0:t=${m0 ? fmt(m0.t, 1) : "—"} ms`);
    L(
        `  渲染首次重合(<0.5px):t=${touch ? fmt(touch.t, 1) : "从未"} ms` +
            (touch && m0 ? `  → 滞后 ${fmt(touch.t - m0.t, 1)} ms` : ""),
    );
}

// ---- 人声相:模型说「该触线」的帧,渲染上到底触没触 ----
const voc = rows.filter((r) => r.phase === "vocal" && r.t > 3000 && !r.hidden);
const modelTouch = voc.filter((r) => r.modelGapDb < 1e-9);
const renderTouch = voc.filter((r) => r.renderGapPx < 0.5);
const pct = (a, p) =>
    a.length ? [...a].sort((x, y) => x - y)[Math.floor(a.length * p)] : NaN;
const mean = (a) => (a.length ? a.reduce((x, y) => x + y, 0) / a.length : NaN);

L("\n【人声相】音节 220ms 有声 / 180ms 间隙(掐掉前 3s 起振)");
L(`  可用帧 ${voc.length}`);
L(
    `  模型上 Δ==0(柱头该触线)的帧:${modelTouch.length} (${fmt((modelTouch.length / voc.length) * 100)}%)`,
);
L(
    `  渲染上真的触线(<0.5px)的帧:${renderTouch.length} (${fmt((renderTouch.length / voc.length) * 100)}%)`,
);
if (modelTouch.length) {
    const g = modelTouch.map((r) => r.renderGapPx);
    L(`  ↳ 在「模型说该触线」的那些帧上,渲染 Δ:`);
    L(
        `      均值 ${fmt(mean(g))} px  中位 ${fmt(pct(g, 0.5))} px  ` +
            `P90 ${fmt(pct(g, 0.9))} px  最小 ${fmt(Math.min(...g))} px  最大 ${fmt(Math.max(...g))} px`,
    );
}
{
    const g = voc.map((r) => r.renderGapPx);
    L(
        `  全体帧渲染 Δ:均值 ${fmt(mean(g))} px  中位 ${fmt(pct(g, 0.5))} px  最小 ${fmt(Math.min(...g))} px`,
    );
}

// ---- 平滑度:逐帧位移。去掉 transition 会不会把动画变成一跳一跳? ----
// 分上行/下行两侧看:上行按口径②本就是「瞬时跟随」,一跳是设计;
// 下行由 JS 的 fast-follow 逐帧推,才是「该平滑」的那一侧。
{
    const up = [],
        down = [];
    for (let i = 1; i < voc.length; i++) {
        const d = voc[i].liquidRight - voc[i - 1].liquidRight;
        if (d > 0) up.push(d);
        else if (d < 0) down.push(-d);
    }
    L(`\n  【平滑度】液柱逐帧位移(px/帧)`);
    L(
        `    上行(口径②瞬时跟随,一跳是设计):帧数 ${up.length}  中位 ${fmt(pct(up, 0.5))}  P90 ${fmt(pct(up, 0.9))}  最大 ${fmt(Math.max(...up))}`,
    );
    L(
        `    下行(JS fast-follow 逐帧推,该平滑的一侧):帧数 ${down.length}  中位 ${fmt(pct(down, 0.5))}  P90 ${fmt(pct(down, 0.9))}  最大 ${fmt(Math.max(...down))}`,
    );
}

L("\n" + "=".repeat(86));
try {
    ws.close();
} catch {
    /* 已关 */
}
chrome.kill();
server.close();
process.exit(0);
