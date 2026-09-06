// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 三张 UI 卡的**页面级**冒烟(SL-272 / SL-275 / SL-276 / SL-273)
// =============================================================================
// 为什么必须页面级:这一批改的全是**几何与时序**——
//   • SL-272 「已接管…」要单行:折不折行只有排完版才知道,源码正则读不出来;
//   • SL-275 标题区留白:多出来的是 <p> 的 UA 默认 margin,它在 CSS 文件里根本没有
//     对应的一行可以 grep,只能量 getBoundingClientRect;
//   • SL-276 弹窗:「弹出 → 点重新分析 → 真的跑了一次 analyze」是一条跨模块的接线,
//     纯函数断言只能证明「判据会算」,证明不了「算出来的东西真的开了框、真的发了桥调用」。
// 本仓已经栽过三次「三层机检全绿、窗口是白的」,所以这三条一律真跑一次页面。
//
// 跑什么:
//   A. `input.html?scenario=connected`(已接管态)× 三语:
//      A1 pillSub 恰好一行(offsetHeight ≤ 一个 line-height 的 1.4 倍);
//      A2 不被截断(scrollWidth ≤ clientWidth ——「反话护栏」:光靠 A1 的话,
//         把文字裁掉一半也是一行);
//      A3 承载它的 .ipt-header__conn 真的横跨整个 header(≥ header 宽 − 4px)——
//         这一条是 SL-272 的**根因面**:旧布局把它关在品牌块右侧 200px 的栏里。
//         删掉 `grid-column: 1 / -1` 即红;
//      A4 语言胶囊在**右上角**(右边缘齐 header 右缘、顶边齐 header 顶缘,且高于连接行);
//      A5 「?」重看入口在**右下角**(落在 footer 里、且是 footer 最右的一件);
//      A6 header / 连接行 / footer 各自零横向溢出(check-design-box 的运行期对偶);
//      A7 [复审] 副文案的 title 与正文**逐字一致**:nowrap + ellipsis 那道护栏真生效时,
//         被截掉的半句只剩 title 这一条通路(悬停 / 读屏)。三语各验一次 —— 语言一切,
//         renderPill() 会把两者一起重写,验的正是「两者没有漂开」;
//         删掉 app.js renderPill() 里那行 setAttribute("title", …) 即红。
//   B. `output.html` Tab3 建议表视图:
//      B1 视图内每个 <p> 的上下 margin 都是 0(删掉 `.suggest-view p{margin:0}` 即红);
//      B2 标题区高度 = 标题 + gap + 说明句(±2px),不含任何隐形外边距;
//      B3 标题卡整卡 ≤ 70px(改前 108px,改后实测 61px);
//   C. `output.html` Tab4 「分析口径已更改」弹窗(SL-276):
//      C1 初始不弹;切响度档 ⇒ 弹出,正文/两枚钮的文案取自字典(不是 key 字面量);
//      C2 「稍后」关框,而琥珀 badge **仍在**(它是常驻状态位,不随关框消失);
//      C3 换到另一个脏值 ⇒ 再弹一次(按值记,不是一次性开关);
//      C4 「重新分析」= 关框 + 真的跑完一次 analyze —— 判据是**琥珀 badge 自己灭掉**
//         (基线由 scvb.segments 的 analyze 帧同步),而不是「按钮被点到了」。
//         把 tab-settings.js 里的 `call("analyze", "all")` 删掉,本条即红;
//      C4b [SL-276 二轮复审] analyze 在途期间主钮置灰(防连点打第二发),跑完必须解开:
//         删掉 doReanalyzeFromAsk 的 finally 即红(钮永久停在 disabled,其余条目照样绿);
//      C4c [SL-276 三轮复审] 锁**本身**有没有牙:给 mock 的 analyze 套「慢回执 + 计数」
//         垫片,同一同步回合里连点两下主钮 ⇒ 只准打出一发。C4b 断的是「跑完解得开」,
//         两道锁一起删掉它照样绿,所以必须另立本条。两道锁(reanalyzeInFlight 早退 /
//         btn.disabled)各自独立挡得住第二下,故本条是**两道都拆掉才红**。
//         同批断言在途期间主钮挂着 `data-disabled="1"` —— 本仓禁用视觉走这个属性钩子,
//         光设 `.disabled` 一个像素都不会变(没有对应的 `:disabled` 规则);
//      C4d [SL-276 四轮复审] 在途 + 键盘的**组合**面:主钮置灰时从「稍后」正向 Tab 不许
//         出框。判据是 `defaultPrevented`(这次 Tab 有没有被框吃掉),**不是**「焦点还在
//         框里」—— 合成 KeyboardEvent 不触发原生走焦,后者修好前后都成立,是条无牙断言。
//         把 Tab 圈闭正向分支的 `here === focusable(last, first)` 改回 `here === last`
//         即红。同一处已连出三轮、每轮都是上一轮补丁的副作用,所以判据钉在组合上;
//      C5 Esc 关框;
//      C6 三语各弹一次,正文非空且不等于 key;
//      C8 [SL-276 复审] **弹窗只由用户点击驱动,不由派生的 stale 位驱动**:
//         `?scenario=loudness-nondefault` 的工程存的是 rms(不是出厂默认档),于是
//         一进 Tab4 stale 就为真 —— 琥珀 badge **该亮**(它是纯派生的常驻状态位),
//         而弹窗**不该弹**(用户一个字都没改)。这是三条误报路径里最容易复现的一条,
//         另两条(只读观察态 / 切版本)同一道闸一并挡住。
//         [SL-354] 那道闸从布尔位换成了「用户刚写成功的字段与值」,判据形态没变,
//         但**删除式要拆两道**(复审 r1 加的 `pendingStale` 早退在没有待观察的写时
//         回落成 false,顺带也把这一条兜住了):
//           ① 把「没有待观察的写就只留 badge、不弹框」那句早退删掉,**并把下游三处
//              读 `pending.field` / `pending.value` 的地方一并改成 null-safe** —— 不改的话
//              删完只会抛 TypeError,那是崩溃不是「闸没了」,框反而弹不出来、本条照绿;
//           ② 同时删掉 `if (!pendingStale) return;`。
//         实测(6021122):只做 ① ⇒ 全套照绿;①+② ⇒ `C8s 但弹窗**没有**弹` 当场红
//         (连带 C10e2 / C10g 也红,那是超集)。
//         ⚠ 早前只做 ① 的一轮里还见过 C8r 那一段(SL-348 范围档)**抖动地**变红:
//         闸没了以后模态框会在别的段里乱弹,扰动那边的点击与焦点。连带红不是本条的
//         判据面,别据它读结论。
//         改完档之后照样弹(C1 覆盖),所以这道闸没有把功能一起关掉。
//      C9 [SL-276 二轮复审] 那道闸是**一次性**的:用户改档弹过、点「稍后」关掉之后,
//         再来一次**非用户驱动**的口径变化(这里直接调 mock 的 setAnalysisConfig,
//         绕开 UI 写入路径 —— 与只读观察态收 scvb.state / 切版本走的是同一条「值从
//         后端来」的路)不得再弹。C8 管的是「从没被置位过」,C9 管的是「置位过、已经
//         用掉了」。
//         [SL-354] **这一条现在由两道锁各自独立挡住,拆一道不红、两道都拆才红** ——
//         与 C4c 同形。原来那道「开框时就地清掉开闸位」没了(开闸位还要继续当
//         「这一帧新不新」的尺子,见 C10a),接替它的是:
//           ① `reanalyzeAskedFor` 记的「字段=值」token —— 后端换到另一个值时 token 没变,
//              于是不重开;
//           ② 「待观察的写还没在 state 里露面就什么都不做」那句早退 —— 后端把值换成了
//              别的,与本位记的那次写对不上,这一帧直接不作数。
//         实测:单删 ① 或单删 ② 本条都照绿(另一道兜住);**两条一起删** ⇒
//         `C9 非用户驱动的换档**不再弹**` 当场红。同一注入会连带打红 C10a/b/c 与
//         C8r 那一段(后者抖动),那些是超集不是本条的判据面。
//         末尾再由用户真改一次档确认框照样弹。
//      C7 [SL-273] 换档影响面这句话在**两处**都写着,且逐字同一句:设置页响度卡第二行
//         与弹窗第二段共用词条 set.reanalyze.scopeNote。断言取两处的 textContent 做
//         全等比较 —— 拿掉任一处的 data-t(或把它换成另一条词条)即红,三语各验一次;
//         [SL-354 复审第 1 轮] 这条耦合现在是**按触发字段**的:框里那一段改中央槽时换成
//         `set.centerSlot.scopeNote`。本条走的全是响度那条路(C1 起就一直改响度档),
//         所以断言形态不变;中央槽那一侧由 C10c2 断,两格合起来才是完整口径;
//         同批一条排版断言:弹窗正文段的上下 margin 为 0(两段间距只由 .sc-modal__note
//         的 margin-top 决定),删掉 `.sc-modal--reanalyze .sc-modal__body{margin:0}` 即红;
//      C10 [SL-354] 用户 v5.6.7 真机四条 + 一道兜底闸。前四条**必须跑在异步回声场景上**
//         (`scenario=slow-state-echo`):默认 mock 同步 emit `scvb.state`,写回执到达时
//         store 已是新值,①② 那两条链在它上面根本不存在 —— C10a 因此先量一次「点击 →
//         徽标亮」的页内耗时,把「这个场景真的还有牙齿」也钉住。逐格与删除式见那一段的
//         行内注释:a 第一下就弹 / b 过期全量帧不关框 / c 中央槽同样弹 /
//         c2 框内说明段说的是被改的那一项(复审第 1 轮)/ d 说明段字号走 --fs-110 /
//         e 改回基线三件事 / f 缺 applied 的全量帧不许清闸关框 /
//         g 两项都脏时把其中一项改回基线不该再弹(复审第 1 轮)/
//         h 非 UI 改回基线后重选同一个值仍要弹(复审第 2 轮,接在 C9 那段后面跑)。
//
// 章节在下面的执行顺序是 A → E → B → C(E 紧跟 A,因为两段用的是同一张 Input 页)。
//
// **本段量的不是 spotlight 本身**:亮区是画在 canvas 上的,取不到 DOM 矩形。
// 量的是它的输入(锚点的 bounding rect)与输出(说明框的落位),这两样才是挪位会改的东西:
//   E. `input.html?scenario=input-first-run` 的 mini tour 末步([J80] 第 ⑤ 步):
//      「?」按 SL-272② 从 header 右上挪到了卡片右下 —— 它同时是 tour 的 spotlight 锚点,
//      挪动锚点会改说明框的落位方向(placeCallout 的 fitsBelow 从真变假)。所以这条不是
//      「顺手多测一点」,而是本卡改动的直接受害面:
//      E1 走到 5/5,且「?」确实落在卡片下半部(挪位本身);
//      E2 说明框整体仍落在卡内(改前它在锚点下方,锚点到了底边就只能翻到上方);
//      E3 说明框与「?」不相交(翻错方向的典型症状是盖住自己要讲的东西)。
//   D. 上面每一段跑完都要零未捕获异常、零 console.error。
//
// 用法:node web-preview/tests/smoke-ui-layout-page.mjs [仓库根绝对路径]
//   --chrome=<路径>  显式指定浏览器
// 退出码:0 = 全绿;1 = 有断言失败;**2 = 环境里没有 Chrome/Edge**(口径同
//   smoke-output-stale-page.mjs 与 CLAUDE.md §6:可选依赖缺席不判红,但也绝不算通过);
//   **3 = 浏览器在,但这一次没起来 / 没连上**([SL-297],见 `browserFailed()`)——
//   同样不判红,但在 gates 汇总里打 `[FLAKY-SKIP]`,免得「没跑成」被读成「跑过了」。
//
// CDP 那 30 行与 smoke-output-stale-page.mjs 同源(node 内置 fetch + WebSocket,
// 零依赖 —— 仓库红线是不引 puppeteer)。同样不抽公共模块:那份断的是 Output 的提示面,
// 本份断的是三处几何,合并只会让两边被对方的参数面绑住。
// =============================================================================

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import {
    existsSync,
    mkdtempSync,
    readFileSync,
    rmSync,
    statSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT =
    process.argv[2] && !process.argv[2].startsWith("--")
        ? process.argv[2]
        : resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

const argv = new Map(
    process.argv
        .slice(2)
        .filter((a) => a.startsWith("--"))
        .map((a) => {
            const i = a.indexOf("=");
            return i < 0 ? [a.slice(2), "1"] : [a.slice(2, i), a.slice(i + 1)];
        }),
);

const CHROME_CANDIDATES = [
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    `${process.env.LOCALAPPDATA || ""}\\Google\\Chrome\\Application\\chrome.exe`,
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
];

const LANGS = ["zh", "en", "fr"];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let fail = 0;
const log = (s) => console.log(s);
function check(cond, msg) {
    if (cond) return true;
    fail++;
    console.log(`  [FAIL] ${msg}`);
    return false;
}
function le(got, want, msg) {
    return check(
        typeof got === "number" && got <= want,
        `${msg}(实得 ${got},上限 ${want})`,
    );
}
function ge(got, want, msg) {
    return check(
        typeof got === "number" && got >= want,
        `${msg}(实得 ${got},下限 ${want})`,
    );
}

// ---------------------------------------------------------------- 静态服务
const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".woff2": "font/woff2",
};
const server = createServer((req, res) => {
    let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
    if (p.endsWith("/")) p += "index.html";
    // 无头浏览器会自己发一条 /favicon.ico;仓里没这个文件,404 会以
    // console.error 的形式进错误桶,把「零 console.error」那条断言淹死。
    // 回 204 而不是把它从错误桶里过滤掉:过滤器会顺手放过真的资源 404。
    if (p === "/favicon.ico") {
        res.writeHead(204).end();
        return;
    }
    const abs = resolve(join(ROOT, p));
    if (!abs.startsWith(resolve(ROOT))) {
        res.writeHead(403).end("nope");
        return;
    }
    if (!existsSync(abs) || !statSync(abs).isFile()) {
        res.writeHead(404).end("not found");
        return;
    }
    res.writeHead(200, {
        "Content-Type":
            MIME[extname(abs).toLowerCase()] || "application/octet-stream",
        "Cache-Control": "no-store",
    });
    res.end(readFileSync(abs));
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}`;

// ---------------------------------------------------------------- CDP 小客户端
function cdpConnect(wsUrl) {
    const ws = new WebSocket(wsUrl);
    const pending = new Map();
    const listeners = [];
    let id = 0;
    ws.addEventListener("message", (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id && pending.has(msg.id)) {
            const { resolve: ok, reject: no } = pending.get(msg.id);
            pending.delete(msg.id);
            msg.error ? no(new Error(msg.error.message)) : ok(msg.result);
        } else if (msg.method) {
            for (const fn of listeners) fn(msg);
        }
    });
    const ready = new Promise((ok, no) => {
        ws.addEventListener("open", ok, { once: true });
        ws.addEventListener("error", () => no(new Error("CDP 连接失败")), {
            once: true,
        });
    });
    // ⚠ CDP 截止时间**按调用点取,不取一个文件级常数**([SL-287],本机实测逼出来的)。
    //
    // 两类调用的合法时长根本不同,一个常数满足不了:
    //   · `waitFor` 内部的 evaluate —— 上界必须**小于该次 waitFor 自己的预算**,
    //     否则一次超时就吃穿整个预算,把「丢一次响应下一轮补上」变成硬红。
    //   · 一次性的直接 evaluate —— 里面可能是**故意跑很久**的在页探针。
    //     实测 smoke-output-dist-page 的 `measureOff` 合法跑满 12s,而同一文件最紧的
    //     `waitFor` 预算也是 12s:两个约束互相矛盾,任何单一常数都满足不了。
    //
    // 所以:`waitFor` 内部的 evaluate 传**本次还剩多少预算**(见下面 waitFor —— 第一版写的是
    // 「预算的一半」,被复审指出那会把耗时落在 (ms/2, ms) 的**合法**调用从过变成必红,
    // 等于新增一类红;按剩余预算取则不改变任何原本能过的行为)。其余调用用这个宽的默认值 ——
    // 它只负责兜住**真挂死**,不负责区分快慢。
    const CDP_DEFAULT_TIMEOUT_MS = 20000;
    return {
        ready,
        on: (fn) => listeners.push(fn),
        send(method, params, timeoutMs) {
            const mid = ++id;
            const budget = timeoutMs || CDP_DEFAULT_TIMEOUT_MS;
            return new Promise((ok, no) => {
                // 每条 CDP 调用都必须有截止时间:原版把 resolve 塞进 `pending` 就返回,
                // 响应不来就**永远不 resolve**。SL-274 在同源的 seg-diff-fold 上实测挂过
                // 75 分钟零输出(Chrome 与 node 都还活着)。
                // ⚠ 因果限定在**当时**:那次还赶上 [SL-277] 拆锁**之前**的形态 ——
                // 整条 gates 被外部目录锁包着,所以一套挂死会把整批 agent 一起堵住。
                // [SL-301] 起 3e 也持 `Local\SCVB-ipc-tests` 了(它的 Chrome 负载会把同机
                // 别人的 gate 6 拖红)⇒ **一套挂死会堵住全场**,不再只停死本轮。
                // 这正是本文件那条 CDP 截止时间与 gates 3e 的 300s/套上界现在更要紧的原因。
                // 那仍然是一整轮,所以超时照加;但别照着旧说法去推断锁的作用域。
                // CI 上则是一路烧到 job 超时才红。
                //
                // 超时**抛错而不重试**:响应不来说明页面或渲染器已经不对了,
                // 重试只会把一个确定的红拖成一个更慢的红。错误里带 method 与 id,
                // 红出来直接指到是哪一条卡住。
                const timer = setTimeout(() => {
                    pending.delete(mid);
                    no(
                        new Error(
                            `CDP 调用超时 ${budget}ms:${method}(id=${mid})—— ` +
                                "响应没回来。多半是这一步之前的导航把渲染器换掉了;" +
                                "**不要**改成重试或调大超时,那只是把红拖慢",
                        ),
                    );
                }, budget);
                pending.set(mid, {
                    resolve: (v) => {
                        clearTimeout(timer);
                        ok(v);
                    },
                    reject: (e) => {
                        clearTimeout(timer);
                        no(e);
                    },
                });
                ws.send(
                    JSON.stringify({ id: mid, method, params: params || {} }),
                );
            });
        },
        close: () => ws.close(),
    };
}

function noBrowser(msg) {
    console.error(
        `❌ ${msg}\n` +
            "   页面级冒烟无法运行(退出码 2)。这**不是**通过:装一个 Chrome/Edge," +
            "或用 --chrome=<路径> 指定。",
    );
    try {
        server.close();
    } catch {}
    process.exit(2);
}

// [SL-297] **浏览器在,但没起来 / 没连上** —— 与 `noBrowser()` 分开走 **退出码 3**。
// 为什么必须分开:gates 3e 与 CI 都把 **2 读成「本机没有浏览器」** 并按可选依赖记 SKIP、
// 照算 PASS。而「装着 Chrome、这一次没连上」是**一次失败的运行**,不是缺依赖 ——
// 压成同一个码之后,一台装着 Chrome 的机器上一次瞬时超时就会让整套判据**无声消失**,
// 汇总行还写着全 PASS(SL-293 实测撞到三次,每次掉的套件还不一样)。
// **仍然不判红**(理由见调用点):判红会把每个 PR 卡在与改动无关的环境抖动上。
// 3 的语义就是「这一轮没跑成,而且不是因为没装浏览器」——由 gates 打成 [FLAKY-SKIP]。
function browserFailed(msg) {
    console.error(
        `❌ ${msg}\n` +
            "   页面级冒烟**没跑成**(退出码 3):浏览器是在的,但这一次没起来 / 没连上。" +
            "这**不是**通过,也**不是**「本机没装浏览器」——重跑一次通常就好;" +
            "连续复现请查 CDP 端口占用、机器负载或 Chrome 版本。",
    );
    try {
        server.close();
    } catch {}
    process.exit(3);
}

function chromePath() {
    if (argv.has("chrome")) {
        const p = argv.get("chrome");
        if (!existsSync(p)) noBrowser(`--chrome 指定的路径不存在:${p}`);
        return p;
    }
    for (const p of CHROME_CANDIDATES) if (existsSync(p)) return p;
    noBrowser("本机找不到 Chrome/Edge");
    return null;
}

const exe = chromePath();
const CDP_PORT = Number(
    argv.get("cdp") || 9800 + Math.floor(Math.random() * 400),
);
const userDataDir = mkdtempSync(join(tmpdir(), "scvb-ui-layout-"));
const chrome = spawn(
    exe,
    [
        "--headless=new",
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userDataDir}`,
        "--window-size=1400,1000",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-timer-throttling",
        "--disable-backgrounding-occluded-windows",
        "--disable-renderer-backgrounding",
        "--force-device-scale-factor=1",
        ...(process.env.CI ? ["--no-sandbox"] : []),
        "about:blank",
    ],
    { stdio: "ignore" },
);

// ⚠ 收尾必须走**所有**退出路径,不只是跑完那一条([SL-287])。
// 本文件原有的 try/finally 只包住**主断言体**,而 Chrome 是在进入那个 try **之前**就 spawn 的 ——
// 那段窗口里抛错(CDP 连接、Page.enable、首次导航,恰好是上面新加的超时最可能开火的地方)
// 就会把 headless Chrome 留在机器上。
// 而且信号完全没人接:SL-287 同时给 gates 3e 加了整套超时,超时会向本进程发信号。
//
// ⚠ **本机(Windows)实测的边界,别把这段的作用说大**:
//   · 浏览器进程:node 一死,Windows 会把 spawn 出来的 Chrome 一起收掉 —— 实测原版在
//     SIGTERM 下也能从 10 个进程回到 0。所以在 Windows 上这段对**进程**是双保险,不是唯一解。
//     它真正吃劲的地方是 **Linux**:`web-smoke` 跑在 ubuntu-latest,而 POSIX 下父进程退出
//     **不会**自动收掉 spawn 的子进程 —— 而本文件的 try/finally 只包住主断言体,
//     spawn 到进 try 之间那段窗口(CDP 连接、Page.enable、首次导航)在 Linux 上没人收。
//   · 临时目录:`chrome.kill()` 之后文件句柄未必立刻释放,紧跟的 `rmSync` 在 Windows 上
//     **会失败**,留下一个空壳目录 —— 实测本 PR 版本与原版在注入失败时**同样各留 1 个**。
//     这一点不吹:本机 temp 下现有 981 个 `scvb-*` 残留目录,这段收不干净它们。
//     它保证的是「每条退出路径都**尝试过**收尾」,以及在 Linux 上真的收得掉。
let tornDown = false;
function teardown() {
    if (tornDown) return;
    tornDown = true;
    try {
        cdp?.close();
    } catch {}
    try {
        chrome?.kill();
    } catch {}
    try {
        server.close();
    } catch {}
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
}
// `exit` 处理器只能同步收尾(Node 规范),所以这里不等句柄、不重试 rmSync ——
// 留一个空壳目录是可接受的残渣(系统会清),而**跑着的 headless Chrome 不是**。
process.on("exit", teardown);
for (const sig of ["SIGINT", "SIGTERM"]) {
    process.on(sig, () => {
        teardown();
        process.exit(130);
    });
}
// 未捕获异常 / 未处理拒绝:先打印再收尾,否则 Chrome 会跟着一起漏。
// 新加的 CDP 超时是**定时器里 reject**,那条 promise 当时若没人 await,
// 就会以 unhandledRejection 形式到这里 —— 这一支不是摆设。
for (const ev of ["uncaughtException", "unhandledRejection"]) {
    process.on(ev, (e) => {
        console.error(`  [FATAL] ${ev}:`, e && e.message ? e.message : e);
        teardown();
        process.exit(1);
    });
}

chrome.on("error", (e) => browserFailed(`浏览器启动失败:${e.message}`));

let cdp = null;
let bucket = { label: "启动", errors: [], exceptions: [] };
const newBucket = (label) => {
    bucket = { label, errors: [], exceptions: [] };
};

async function evaluate(expression, timeoutMs) {
    const r = await cdp.send(
        "Runtime.evaluate",
        {
            expression,
            returnByValue: true,
            awaitPromise: true,
        },
        timeoutMs,
    );
    if (r.exceptionDetails) {
        throw new Error(
            "页内求值抛错:" +
                (r.exceptionDetails.exception?.description ||
                    r.exceptionDetails.text),
        );
    }
    return r.result?.value;
}

async function waitFor(expr, ms = 15000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        let v = null;
        try {
            // 这次 evaluate 的上界 = **本次 waitFor 还剩多少预算**(留 250ms 收尾),
            // 不是「预算的一半」。第一版写成 ms/2,被复审指出**把语义改窄了**:
            // 一次耗时落在 (ms/2, ms) 区间的**合法**调用,改动前能过、改动后必红 ——
            // 而 monitor 这一套的实测最慢单次是 3020ms、上界只有 5000ms,余量 1.65 倍,
            // 在与别的 job 抢 CPU 的 ubuntu runner 上抖一下就会把慢但合法判成红。
            // (PR 里那次「未复现的 monitor exit=1」很可能就是这个,首要假设。)
            // 按剩余预算取则**不改变任何原本能过的行为**:慢调用可以用掉几乎整个预算,
            // 真挂死仍会在预算到点前被砍断,由下面的 while 条件收尾。
            v = await evaluate(
                expr,
                Math.max(1000, ms - (Date.now() - t0) - 250),
            );
        } catch {
            v = null;
        }
        if (v) return true;
        await sleep(120);
    }
    return false;
}

// 真源文档在 iframe 里(壳页只有工具条);一切选择器走它。
const IN = (js) => `(() => {
    const f = document.querySelector("iframe");
    const w = f && f.contentWindow;
    const d = f && f.contentDocument;
    if (!w || !d) return null;
    const q = (s) => d.querySelector(s);
    const gb = (n) => q('[data-gb="' + n + '"]');
    const vis = (el) => !!el && !el.hidden;
    const R = (el) => { const r = el.getBoundingClientRect(); return {
        x: Math.round(r.left), y: Math.round(r.top),
        r: Math.round(r.right), b: Math.round(r.bottom),
        w: Math.round(r.width), h: Math.round(r.height) }; };
    ${js}
})()`;

async function click(gbOrSel) {
    const sel = gbOrSel.startsWith("[") ? gbOrSel : `[data-gb="${gbOrSel}"]`;
    return await evaluate(
        IN(
            `const n = q(${JSON.stringify(sel)}); if (!n) return false; n.click(); return true;`,
        ),
    );
}

function assertClean(label) {
    check(
        bucket.exceptions.length === 0,
        `${label}:零未捕获异常(实得 ${bucket.exceptions.length} 条:${bucket.exceptions.slice(0, 2).join(" | ")})`,
    );
    check(
        bucket.errors.length === 0,
        `${label}:零 console.error(实得 ${bucket.errors.length} 条:${bucket.errors.slice(0, 2).join(" | ")})`,
    );
}

// =============================================================================
// 探针
// =============================================================================

// ---- A. Input header 几何(SL-272)
const INPUT_READY = IN(`
    const n = gb("input.header.pillSub");
    return !!(n && n.textContent && n.textContent.length > 0);
`);

const INPUT_PROBE = IN(`
    const sub = gb("input.header.pillSub");
    const conn = gb("input.header.conn");
    const lang = gb("input.header.lang");
    const help = gb("input.header.help");
    const badge = gb("input.header.groupBadge");
    const header = gb("input.header");
    const footer = gb("input.footer");
    const connrow = q(".ipt-header__connrow");
    if (!sub || !conn || !lang || !help || !badge || !header || !footer || !connrow) return null;
    const lh = parseFloat(w.getComputedStyle(sub).lineHeight) || 0;
    const fr = R(footer);
    return {
        text: sub.textContent.trim(),
        subTitle: sub.getAttribute("title"),
        subH: sub.offsetHeight,
        lineH: lh,
        subScrollW: sub.scrollWidth,
        subClientW: sub.clientWidth,
        connW: conn.clientWidth,
        headerW: header.clientWidth,
        lang: R(lang),
        help: R(help),
        badge: R(badge),
        header: R(header),
        conn: R(conn),
        footer: fr,
        helpInFooter: footer.contains(help),
        headerScrollW: header.scrollWidth,
        headerClientW: header.clientWidth,
        connrowScrollW: connrow.scrollWidth,
        connrowClientW: connrow.clientWidth,
        footerScrollW: footer.scrollWidth,
        footerClientW: footer.clientWidth,
        docScrollW: d.documentElement.scrollWidth,
        docClientW: d.documentElement.clientWidth,
    };
`);

// ---- E. mini tour 末步的「?」自指(SL-272② 挪了 spotlight 锚点)
const TOUR_PROBE = IN(`
    const help = q('[data-tour="help"]');
    const callout = q(".tour-callout");
    const shell = d.getElementById("ipt-shell");
    const overlay = q("[data-tour-overlay]");
    if (!help || !callout || !shell || !overlay || overlay.hidden) return null;
    // spotlight 是画在 canvas 上的,取不到 DOM 矩形;改用 tour-in 自己算亮区那条路
    // (spotRectOf 的输入 = 锚点的 bounding rect),故这里直接量锚点与说明框。
    return {
        step: (q(".tour-callout__step") || {}).textContent || "",
        help: R(help),
        callout: R(callout),
        shell: R(shell),
    };
`);

// ---- B. 建议表留白(SL-275)
const SUGGEST_PROBE = IN(`
    const view = q(".suggest-view");
    const head = q(".suggest-head");
    const titles = q(".suggest-head__titles");
    const title = q(".suggest-title");
    const disc = q(".suggest-disclaimer");
    if (!view || !head || !titles || !title || !disc) return null;
    const margins = [];
    for (const p of view.querySelectorAll("p")) {
        const cs = w.getComputedStyle(p);
        margins.push({
            gb: p.getAttribute("data-gb") || p.className,
            mt: cs.marginTop,
            mb: cs.marginBottom,
        });
    }
    const gap = parseFloat(w.getComputedStyle(titles).rowGap) || 0;
    return {
        open: !view.hidden,
        headH: head.offsetHeight,
        titlesH: titles.offsetHeight,
        titleH: title.offsetHeight,
        discH: disc.offsetHeight,
        gap: gap,
        margins: margins,
    };
`);

// ---- C. 重分析弹窗(SL-276)
const ASK_PROBE = IN(`
    const ask = gb("reanalyze-ask");
    const body = gb("reanalyze-ask-body");
    const later = gb("reanalyze-ask-later");
    const primary = gb("reanalyze-ask-primary");
    const badge = gb("settings-loudnessmode-stale");
    // [SL-273] 同一条词条的两个渲染点:弹窗第二段 / 设置页响度卡第二行。
    const askNote = gb("reanalyze-ask-scopenote");
    const setNote = gb("settings-loudnessmode-scopenote");
    const panel = gb("reanalyze-ask-panel");
    if (!ask || !body || !later || !primary) return null;
    const bs = w.getComputedStyle(body);
    return {
        open: vis(ask),
        body: body.textContent.trim(),
        later: later.textContent.trim(),
        primary: primary.textContent.trim(),
        badgeShown: vis(badge),
        askNote: askNote ? askNote.textContent.trim() : null,
        setNote: setNote ? setNote.textContent.trim() : null,
        noteInPanel: !!(panel && askNote && panel.contains(askNote)),
        // [SL-279 复审第 6 轮] 范围档专用提示:范围档下点主钮不会灭徽标,得把这句摆出来。
        // [SL-348 复审第 1 轮] 本格问的是「范围提示这条词条在不在」,所以读的是装它的那个
        // span,不是外层段落 —— 段落里现在还有 live region 那半,读段落的 textContent 拿到
        // 的是两句拼串。今天四处引用都是真值判断,拼串不出错;但探针名与注释都说它是范围
        // 提示,下一个人照 assertScopeNote 加一条**全等**断言就会莫名其妙红,而且红的原因
        // 与他改的东西无关。可见性仍看外层段落(hidden 挂在它身上)。
        //
        // ⚠ 本段在模板字符串里:**不要写反引号**,会当场把 IN() 的模板截断(初稿栽过)。
        rangeNote: (() => {
            const p = gb("reanalyze-ask-rangenote");
            const hint = gb("reanalyze-ask-rangehint");
            return p && vis(p) && hint ? hint.textContent.trim() : null;
        })(),
        // [SL-279 复审第 7 轮] 读屏用户拿到的那一半:describedby 有没有把范围提示带上。
        describedBy: panel ? panel.getAttribute("aria-describedby") : null,
        // [SL-348 复审第 1 轮] live region 挂在**哪个节点**上。
        // 说清这一格钉的是什么:它钉**结构**,不钉 AT 行为 —— 双读与 aria-atomic 整段重读
        // 都发生在读屏软件里,无头 Chrome 的 DOM 看不见。能被机器看见的只有「role=status
        // 在谁身上」,而那正是两种行为的唯一分叉点,所以这一格是这条论证唯一拿得到的判据。
        statusOnDone: (() => {
            const n = gb("reanalyze-ask-rangedone");
            return !!n && n.getAttribute("role") === "status";
        })(),
        statusOnNote: (() => {
            const n = gb("reanalyze-ask-rangenote");
            return !!n && n.getAttribute("role") === "status";
        })(),
        // [SL-279 复审第 8 轮] live region 里那半动态文本:范围档下点主钮之后要有话可念。
        rangeDone: (() => {
            const n = gb("reanalyze-ask-rangedone");
            return n ? n.textContent.trim() : null;
        })(),
        // [SL-348 复审第 2 轮] **不 trim 的那一份**:分隔交给 CSS ::before 之后,词条里不该
        // 再留前导空格,而 trim 过的探针看不见它 —— 三语任一处把空格加回来都不会红。
        rangeDoneRaw: (() => {
            const n = gb("reanalyze-ask-rangedone");
            return n ? n.textContent : null;
        })(),
        // [SL-348 复审第 3 轮] 分隔那条 ::before 到底生没生效。初稿写成 #id 选择器,
        // 而那个 span 没有 id —— 匹配零元素、**静默失效**,没有任何东西会红,而分隔
        // 就退回只靠两个 span 之间的行间空白(下一次格式化就吃掉)。读 computed content。
        rangeDoneBefore: (() => {
            const n = gb("reanalyze-ask-rangedone");
            if (!n) return null;
            return w.getComputedStyle(n, "::before").content;
        })(),
        // [SL-348 复审第 4 轮] applyI18n 结尾会写 documentElement.lang —— 「语言真的切过去了」
        // 的第二条独立证据,与「两条译文不相等」互不依赖。
        docLang: d.documentElement ? d.documentElement.lang : null,
        bodyMt: bs.marginTop,
        bodyMb: bs.marginBottom,
    };
`);

// [SL-273] 定谳原文要求「设置页响度口径项说明**与**弹窗正文」都写明换档影响面。
// 实现上两处共用同一条词条(set.reanalyze.scopeNote),所以这里断言的是**全等**而不是
// 「各自非空」—— 把任一处的 data-t 拿掉、或改指另一条词条,这条立刻红。
function assertScopeNote(probe, lang) {
    if (
        !check(
            typeof probe.askNote === "string" && probe.askNote.length > 0,
            `${lang}:弹窗写明了换档影响面(实得 ${JSON.stringify(probe.askNote)})`,
        )
    )
        return;
    if (
        !check(
            typeof probe.setNote === "string" && probe.setNote.length > 0,
            `${lang}:设置页响度卡写明了换档影响面(实得 ${JSON.stringify(probe.setNote)})`,
        )
    )
        return;
    check(
        !probe.askNote.startsWith("set."),
        `${lang}:影响面这段取自字典而不是 key 字面量(实得 ${JSON.stringify(probe.askNote)})`,
    );
    check(
        probe.askNote === probe.setNote,
        `${lang}:两处逐字同一句(弹窗 ${JSON.stringify(probe.askNote)} / 设置页 ${JSON.stringify(probe.setNote)})`,
    );
}

const askOpen = IN(`const n = gb("reanalyze-ask"); return !!n && !n.hidden;`);
const askClosed = IN(`const n = gb("reanalyze-ask"); return !!n && n.hidden;`);
const badgeGone = IN(
    `const n = gb("settings-loudnessmode-stale"); return !!n && n.hidden;`,
);

// [SL-354] 中央槽策略也要能点 —— 本卡把弹窗铺到它身上。
async function setCenterSlot(value) {
    return await evaluate(
        IN(`const btn = q('[data-gb="settings-centerslot-seg"] [data-value="${value}"]');
            if (!btn) return false; btn.click(); return true;`),
    );
}

async function setLoudness(value) {
    return await evaluate(
        IN(`const btn = q('[data-gb="settings-loudnessmode-seg"] [data-value="${value}"]');
            if (!btn) return false; btn.click(); return true;`),
    );
}

async function pressEscape() {
    await evaluate(
        IN(`d.dispatchEvent(new w.KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
            return true;`),
    );
}

// 语言胶囊切语言(Input 与 Output 的锚点名不同族,各给一个)。
async function switchLangInput(code) {
    return await click(`input.header.lang.${code}`);
}
async function switchLangOutput(code) {
    return await click(`header-lang-${code}`);
}

// 首启遮挡(引导页 / tour 询问框)——都点掉,别让它们盖住待测面。
async function dismissOverlays() {
    await evaluate(
        IN(`for (const n of ["guide-overlay-start", "tour-ask-later"]) {
                const b = gb(n);
                if (b && b.offsetParent !== null) b.click();
            }
            return true;`),
    );
    await sleep(250);
    await evaluate(
        IN(`const b = gb("tour-ask-later");
            if (b && b.offsetParent !== null) b.click();
            return true;`),
    );
    await sleep(250);
}

// =============================================================================
// 主流程
// =============================================================================
try {
    // ---- 连上无头浏览器
    let targets = null;
    for (let i = 0; i < 60 && !targets; i++) {
        try {
            const r = await fetch(`http://127.0.0.1:${CDP_PORT}/json/list`);
            const list = await r.json();
            targets = list.find((t) => t.type === "page") ? list : null;
        } catch {
            await sleep(200);
        }
    }
    if (!targets) browserFailed("CDP 端口没起来");
    const page = targets.find((t) => t.type === "page");
    cdp = cdpConnect(page.webSocketDebuggerUrl);
    await cdp.ready;
    await cdp.send("Runtime.enable");
    await cdp.send("Log.enable");
    await cdp.send("Page.enable");
    cdp.on((msg) => {
        if (msg.method === "Runtime.exceptionThrown") {
            const d = msg.params.exceptionDetails;
            bucket.exceptions.push(
                d.exception?.description || d.text || "(无描述)",
            );
        } else if (msg.method === "Runtime.consoleAPICalled") {
            if (msg.params.type !== "error") return;
            bucket.errors.push(
                (msg.params.args || [])
                    .map((a) => a.value ?? a.description ?? "")
                    .join(" "),
            );
        } else if (msg.method === "Log.entryAdded") {
            if (msg.params.entry.level === "error")
                bucket.errors.push(msg.params.entry.text);
        }
    });

    // =========================================================== A. SL-272
    log("A. SL-272 Input header 布局(三语)");
    for (const lang of LANGS) {
        newBucket(`input/${lang}`);
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/input.html?scenario=connected`,
        });
        check(await waitFor(INPUT_READY), `${lang}:Input 页装载并吃到首帧`);
        check(await switchLangInput(lang), `${lang}:语言胶囊可点`);
        await sleep(400);
        const p = await evaluate(INPUT_PROBE);
        if (!check(p, `${lang}:探针取到全部锚点`)) continue;

        // A1 单行 —— line-height 的 1.4 倍是「一行 + 一点点行盒余量」的上限,
        // 两行必然超过它(两行 = 2.0 倍)。
        le(p.subH, Math.ceil(p.lineH * 1.4), `${lang}:pillSub 恰好一行`);
        // A2 不截断(反话护栏:裁掉一半字也是一行)
        le(
            p.subScrollW,
            p.subClientW,
            `${lang}:pillSub 未被截断(scrollWidth ≤ clientWidth)`,
        );
        check(p.text.length > 0, `${lang}:pillSub 有文案`);
        // A3 根因面:连接块横跨整个 header
        ge(p.connW, p.headerW - 4, `${lang}:连接块横跨整个 header`);
        // A4 语言胶囊在右上角
        le(
            Math.abs(p.lang.r - p.header.r),
            2,
            `${lang}:语言胶囊右缘齐 header 右缘`,
        );
        le(
            Math.abs(p.lang.y - p.header.y),
            2,
            `${lang}:语言胶囊顶边齐 header 顶缘`,
        );
        check(
            p.lang.b <= p.conn.y,
            `${lang}:语言胶囊在连接行之上(实得 lang.bottom=${p.lang.b} / conn.top=${p.conn.y})`,
        );
        // A4b 组 badge 落在第二行行首(左贴齐,不再挤在右上角)
        check(
            p.badge.x < p.lang.x && p.badge.y > p.lang.y,
            `${lang}:组 badge 在第二行行首(实得 badge=${p.badge.x},${p.badge.y} / lang=${p.lang.x},${p.lang.y})`,
        );
        // A5 「?」在右下角
        check(p.helpInFooter, `${lang}:「?」落在 footer 里`);
        le(
            Math.abs(p.help.r - p.footer.r),
            2,
            `${lang}:「?」是 footer 最右的一件`,
        );
        check(
            p.help.y > p.conn.b,
            `${lang}:「?」在 header 连接行之下(右下角,不再是右上角)`,
        );
        // A6 header 三个横向容器零溢出(check-design-box 的运行期对偶)。
        //    量的是 header / 连接行 / footer 自身,**不是** #ipt-shell —— shell 的
        //    scrollWidth 恒为 578(三枚装饰性 .sc-halo 是 absolute 且刻意画在盒外,
        //    改前改后都一样),拿它当判据只会得到一条永远红的断言。
        le(p.headerScrollW, p.headerClientW, `${lang}:header 零横向溢出`);
        le(p.connrowScrollW, p.connrowClientW, `${lang}:连接行零横向溢出`);
        le(p.footerScrollW, p.footerClientW, `${lang}:footer 零横向溢出`);
        le(p.docScrollW, p.docClientW, `${lang}:文档零横向滚动`);
        // A7 截断护栏的第二条通路:title 必须与正文逐字一致(切语言后也不许漂开)。
        check(
            typeof p.subTitle === "string" && p.subTitle.trim() === p.text,
            `${lang}:pillSub 的 title 与正文逐字一致(title=${JSON.stringify(p.subTitle)} / 正文=${JSON.stringify(p.text)})`,
        );
        assertClean(`input/${lang}`);
    }

    // =========================================================== E. SL-272②
    // 「?」既是重看入口、也是 mini tour 末步的 spotlight 锚点。它从 header 右上挪到
    // 卡片右下之后,placeCallout 的落位分支从 fitsBelow 翻到 fitsAbove —— 这一段就是
    // 在真页面上确认「翻对了方向、没盖住自己要讲的按钮、也没掉出卡外」。
    log("E. SL-272② 「?」挪位后 mini tour 末步仍指得准");
    newBucket("input-tour");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/input.html?scenario=input-first-run`,
    });
    check(
        await waitFor(
            IN(`const n = gb("lang-start"); return !!n && !n.hidden;`),
        ),
        "首启语言卡弹出",
    );
    check(
        await click('[data-lang-pick="zh"]'),
        "选中文 ⇒ 语言卡关闭、mini tour 起",
    );
    check(
        await waitFor(
            IN(`const n = q("[data-tour-overlay]"); return !!n && !n.hidden;`),
        ),
        "mini tour 起来了",
    );
    // 5 步基线:点 4 次「下一步」到末步(末步的按钮变成「完成」,不再点)。
    for (let i = 0; i < 4; i++) {
        check(
            await click('[data-tour-btn="next"]'),
            `mini tour 第 ${i + 1} 次「下一步」可点`,
        );
        await sleep(200);
    }
    const tp = await evaluate(TOUR_PROBE);
    if (check(tp, "tour 探针取到锚点")) {
        check(
            tp.step.trim() === "5/5",
            `走到末步(实得 ${JSON.stringify(tp.step)})`,
        );
        // E2 说明框整体落在卡内
        check(
            tp.callout.x >= tp.shell.x &&
                tp.callout.r <= tp.shell.r &&
                tp.callout.y >= tp.shell.y &&
                tp.callout.b <= tp.shell.b,
            `E2 说明框仍在卡内(callout=${tp.callout.x},${tp.callout.y},${tp.callout.r},${tp.callout.b} / shell=${tp.shell.x},${tp.shell.y},${tp.shell.r},${tp.shell.b})`,
        );
        // E3 说明框不压住它要讲的那枚「?」
        const overlaps =
            tp.callout.x < tp.help.r &&
            tp.callout.r > tp.help.x &&
            tp.callout.y < tp.help.b &&
            tp.callout.b > tp.help.y;
        check(
            !overlaps,
            `E3 说明框没盖住「?」(help=${tp.help.x},${tp.help.y},${tp.help.r},${tp.help.b})`,
        );
        // E1 「?」确实在卡片右下(挪位本身),且末步讲的就是它
        check(
            tp.help.b > tp.shell.y + (tp.shell.b - tp.shell.y) / 2,
            `E1 「?」落在卡片下半部(help.bottom=${tp.help.b} / shell 中线=${Math.round(tp.shell.y + (tp.shell.b - tp.shell.y) / 2)})`,
        );
    }
    assertClean("input-tour");

    // =========================================================== B. SL-275
    log("B. SL-275 建议表标题区留白");
    newBucket("suggest");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(IN(`const n = gb("tabnav-wave"); return !!n;`)),
        "Output 页装载",
    );
    await dismissOverlays();
    await click("tabnav-wave");
    await sleep(400);
    check(await click("wave-btn-suggest"), "打开建议表视图");
    await sleep(600);
    const b = await evaluate(SUGGEST_PROBE);
    if (check(b && b.open, "建议表视图已打开")) {
        // B1 每个 <p> 的上下 margin 归零(删掉 `.suggest-view p{margin:0}` 即红)
        const bad = b.margins.filter((m) => m.mt !== "0px" || m.mb !== "0px");
        check(
            bad.length === 0,
            `建议表内 <p> 上下 margin 全为 0(实得 ${bad.length} 个不为 0:${bad
                .slice(0, 3)
                .map((m) => `${m.gb} ${m.mt}/${m.mb}`)
                .join(" | ")})`,
        );
        check(
            b.margins.length >= 4,
            `探针真的量到了 <p>(实得 ${b.margins.length} 个)`,
        );
        // B2 标题区高度 = 标题 + gap + 说明句(±2px):没有隐形外边距
        le(
            Math.abs(b.titlesH - (b.titleH + b.gap + b.discH)),
            2,
            "标题区高度 = 标题 + gap + 说明句",
        );
        // B3 整卡高度回到与仓内其他卡片同档(改前实测 108px)
        le(b.headH, 70, "标题卡高度收紧(改前 108px,改后 61px)");
    }
    assertClean("suggest");

    // =========================================================== C. SL-276
    log("C. SL-276 「分析口径已更改」弹窗");
    newBucket("reanalyze-ask");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "Output 页装载(Tab4 锚点在)",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(400);

    // C1 初始不弹(这一档的工程口径 = 出厂默认,stale 本来就为假)
    check(await evaluate(askClosed), "初始不弹");
    check(await setLoudness("rms"), "切响度档 rms 可点");
    check(await waitFor(askOpen, 4000), "C1 切响度档 ⇒ 弹窗弹出");
    let c = await evaluate(ASK_PROBE);
    if (check(c, "弹窗探针取到锚点")) {
        check(
            c.body.length > 0 && !c.body.startsWith("set."),
            `C1 正文取自字典(实得 ${JSON.stringify(c.body)})`,
        );
        check(
            c.primary.length > 0 && !c.primary.startsWith("set."),
            `C1 「重新分析」钮取自字典(实得 ${JSON.stringify(c.primary)})`,
        );
        check(
            c.later.length > 0 && !c.later.startsWith("set."),
            `C1 「稍后」钮取自字典(实得 ${JSON.stringify(c.later)})`,
        );
        // C7 [SL-273] 换档影响面这句在**两处**都写着,且逐字同一句。
        assertScopeNote(c, "zh");
        // C7b 弹窗正文段不吃 <p> 的 UA 默认 margin(两段间距只由 .sc-modal__note 定)。
        check(
            c.bodyMt === "0px" && c.bodyMb === "0px",
            `C7 弹窗正文段上下 margin 归零(实得 ${c.bodyMt}/${c.bodyMb})`,
        );
        check(c.noteInPanel, "C7 影响面这段落在弹窗面板里(不是被遮罩截在框外)");
    }

    // C2 「稍后」关框,但琥珀 badge 仍在
    check(await click("reanalyze-ask-later"), "「稍后」可点");
    check(await waitFor(askClosed, 3000), "C2 「稍后」关框");
    c = await evaluate(ASK_PROBE);
    check(c && c.badgeShown, "C2 关框后琥珀 badge 仍在(常驻状态位)");

    // C3 换到另一个脏值 ⇒ 再弹
    check(await setLoudness("peak_dbfs"), "切到 peak_dbfs 可点");
    check(await waitFor(askOpen, 4000), "C3 换脏值 ⇒ 再弹一次");

    // C4 「重新分析」= 关框 + 真的跑完一次 analyze(判据 = badge 自己灭)
    check(await click("reanalyze-ask-primary"), "「重新分析」可点");
    check(await waitFor(askClosed, 3000), "C4 点「重新分析」后关框");
    check(
        await waitFor(badgeGone, 8000),
        "C4 analyze 真的跑完(琥珀 badge 由 segments 帧自己灭)",
    );
    // C4b [SL-276 二轮复审] 主钮在 analyze 在途期间会被置灰(防连点打出第二发),
    // 这一条钉的是**它一定解得开**:doReanalyzeFromAsk 的 finally 一旦丢了,钮就永久
    // 停在 disabled 上,而框已经关掉、badge 也灭了,上面几条照样全绿 —— 看不出来。
    check(
        await evaluate(
            IN(`const b = gb("reanalyze-ask-primary");
                return !!b && b.disabled === false
                    && !b.hasAttribute("data-disabled");`),
        ),
        "C4b analyze 跑完后主钮解锁(in-flight 置灰不会把钮永久锁死)",
    );

    // C4c [SL-276 三轮复审] 钉**锁本身**。C4b 只断言「跑完解得开」,把两道锁一起删掉它
    // 照样全绿 —— 所以另立一条:给 mock 的 analyze 套一层「慢回执 + 计数」垫片,在同一个
    // 同步回合里连点两下主钮,断言只打出**一发**。
    // 两道锁(reanalyzeInFlight 早退 / btn.disabled)各自都能独立挡住第二下,所以本条是
    // 「两道都拆掉才红」;单拆一道仍绿是设计如此,不是判据没牙。
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m) return false;
                const orig = Object.getPrototypeOf(m).analyze;
                if (typeof orig !== "function") return false;
                w.__uir7Calls = 0;
                m.analyze = function (scope) {
                    w.__uir7Calls++;
                    return new Promise((res) => {
                        w.setTimeout(() => res(orig.call(m, scope)), 1500);
                    });
                };
                return true;`),
        ),
        "C4c 慢回执 analyze 计数垫片装上",
    );
    check(await setLoudness("rms"), "C4c 再改档以重新开框");
    check(await waitFor(askOpen, 4000), "C4c 框已开");
    // 同一回合里连点两下:第一下同步置起两道锁,第二下必须打不出第二发。
    check(
        await evaluate(
            IN(`const b = gb("reanalyze-ask-primary");
                if (!b) return false;
                b.click();
                b.click();
                return true;`),
        ),
        "C4c 连点两下已发出",
    );
    check(
        await evaluate(
            IN(`const b = gb("reanalyze-ask-primary");
                return !!b && b.getAttribute("data-disabled") === "1";`),
        ),
        "C4c 在途期间主钮挂上仓内禁用口径 data-disabled(光设 .disabled 在本仓不可见)",
    );
    // C4d [SL-276 四轮复审] 在途 + 键盘:主钮置灰时,从「稍后」**正向** Tab 不许出框。
    //
    // 判据取 `defaultPrevented` 而**不是**「焦点还在框里」:合成 KeyboardEvent 不会触发
    // 浏览器的原生 Tab 走焦,所以「焦点没动」在修好前后都成立 —— 那样写是条无牙断言。
    // 真正区分两者的是**这次 Tab 有没有被框吃掉**:修好后正向分支命中 ⇒ preventDefault();
    // 没修则三条分支全不命中 ⇒ 事件放行 ⇒ 真实浏览器就把焦点交给「稍后」后面那个可聚焦
    // 元素(主钮此刻 disabled、被跳过,于是落到遮罩背后的响度胶囊 / 诊断区)。
    check(
        await evaluate(
            IN(`const later = gb("reanalyze-ask-later");
                const b = gb("reanalyze-ask-primary");
                if (!later || !b || b.disabled !== true) return null;
                later.focus();
                if (d.activeElement !== later) return null;
                const ev = new w.KeyboardEvent("keydown", {
                    key: "Tab", bubbles: true, cancelable: true,
                });
                d.dispatchEvent(ev);
                return ev.defaultPrevented;`),
        ),
        "C4d 在途置灰时从「稍后」正向 Tab 被弹窗吃掉(圈闭没有开口,焦点逃不到遮罩背后)",
    );
    check(
        await waitFor(
            IN(`const b = gb("reanalyze-ask-primary");
                return !!b && b.disabled === false;`),
            8000,
        ),
        "C4c 慢回执落地后解锁",
    );
    const uir7Calls = await evaluate(IN(`return w.__uir7Calls;`));
    check(uir7Calls === 1, `C4c 连点两下只打出一发 analyze(实得 ${uir7Calls})`);
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m) return false;
                delete m.analyze;
                return typeof m.analyze === "function";`),
        ),
        "C4c 垫片已摘(analyze 回到原型上的真实现)",
    );

    // C8 [SL-276 复审] stale 一上来就为真的工程:badge 亮、框不弹。
    // 与 C1 的分工:C1 是「stale 为假 ⇒ 不弹」(弱),C8 是「stale 为真但不是用户改的
    // ⇒ 仍不弹」(强)。没有 C8,把弹窗退回纯派生触发时冒烟依然全绿。
    newBucket("stale-on-load");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=loudness-nondefault`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C8 非默认口径工程装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);
    const sol = await evaluate(ASK_PROBE);
    if (check(sol, "C8 探针取到锚点")) {
        // [SL-279] **这一条反转了**。本档的语义是「工程存 rms 且**按 rms 分析过**」——
        // 从前 UI 拿 mount 快照当基线,加载完就误报「需重新分析」,那时这里断言 badge 亮。
        // 基线换成 state 的 `analysis.applied.*` 之后,这一档**不该再亮** ——
        // 它正是 SL-279 要修的那条误报的可达用例:谁把基线改回本地快照,这一条当场红。
        check(
            !sol.badgeShown,
            "C8 琥珀 badge **不亮**(存的档就是上次分析用的档,不是「需重新分析」)",
        );
        check(!sol.open, "C8 弹窗也没有弹 —— 用户什么都没改,不该被模态框打断");
    }
    // 同一张页上再确认这道闸没有把功能一起关掉:用户真去改档,照样弹。
    check(await setLoudness("peak_dbfs"), "C8 在这张页上改档可点");
    check(await waitFor(askOpen, 4000), "C8 用户真改档 ⇒ 照样弹");

    // C8s [SL-279] **真的**在加载时 stale 的那一档:工程存 rms、上次分析用的是 kw_integrated
    // (用户改了档没重分析就存盘)。badge **该亮**,而弹窗**仍不该弹**(不是用户此刻改的)。
    // C8 与 C8s 是一对:少了 C8s,把 stale 判据改成「恒假」也能全绿;少了 C8,
    // 把基线改回 mount 本地快照也能全绿。两档方向相反,各钉一半。
    // [复审第 1 轮] **先收 C8 这一档的桶再开新桶** —— `newBucket` 是覆盖式的,
    // 不收就把 `loudness-nondefault` 那一整页(导航 → 切 Tab4 → 探针 → 改档弹框)攒下的
    // errors/exceptions 整个丢掉,再没有任何一处断言它们为空。
    // 这在本卡上格外要紧:上一轮的病灶正是 `store is not defined`,就是靠这类断言照出来的。
    assertClean("stale-on-load");
    newBucket("stale-on-load-real");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=loudness-stale-on-load`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C8s 「改了档没重分析」的工程装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);
    const solReal = await evaluate(ASK_PROBE);
    if (check(solReal, "C8s 探针取到锚点")) {
        check(
            solReal.badgeShown,
            "C8s 琥珀 badge 亮着(当前档 ≠ 上次分析所用档)",
        );
        check(
            !solReal.open,
            "C8s 但弹窗**没有**弹 —— stale 为真也不该由派生位驱动模态框",
        );
    }
    assertClean("stale-on-load-real"); // 标签对上桶名(复审第 1 轮:原来两处同名)

    // C8r [SL-279 复审第 6 轮] **范围档下点「重新分析」不是死路。**
    //
    // 统筹裁 B(范围档下不前移基线)之后冒出来的形态:范围档下 `analyze("all")` 只重算
    // `global.range`,契约 §1.21 规定不前移 `applied.*` ⇒ 徽标不灭。而后端回的是 `ok:true`,
    // 原实现照 `ok` 关框 —— 框关了、徽标还挂着、没有任何别的反馈,逐字就是 `doReanalyzeFromAsk`
    // 头注为拒绝态写的那句要避免的东西。
    //
    // 为什么必须页面级:这条链的三段(回执判据 / 关不关框 / 提示显不显)分别在 JS、DOM
    // 和 i18n 三处,源码级断言逐条都能绿而链子仍然断。
    newBucket("range-manual-reanalyze");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=range-manual`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C8r 范围档工程装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);

    // ① 用户真改一次档 ⇒ 弹框(与 C9 同一条路,这里只借它把框打开)。
    check(await setLoudness("peak_dbfs"), "C8r 范围档下改档可点");
    check(await waitFor(askOpen, 4000), "C8r 改档 ⇒ 弹框");
    const rmOpen = await evaluate(ASK_PROBE);
    if (check(rmOpen, "C8r 探针取到锚点(点主钮前)")) {
        // ← 去掉 index.html 那段 rangenote,或去掉 tab-settings 里的开合,这一格红。
        check(
            !!rmOpen.rangeNote,
            "C8r 框一开就带范围提示(范围档下这枚钮达不成用户要的结果)",
        );
        // [复审第 7 轮] 读屏那一半:describedby 必须把范围提示带上,否则 AT 用户拿到的
        // 仍是「框不关、什么也没说」—— 视觉修好了、读屏没修,是这一族的经典漏法。
        check(
            (rmOpen.describedBy || "").includes("reanalyze-ask-rangenote"),
            "C8r aria-describedby 带上了范围提示(读屏念得到)",
        );
        // [SL-348 复审第 1 轮] live region 必须挂在**播报句那个 span** 上,不能挂在外层段落:
        //   ① 外层段落同时是 aria-describedby 的目标,开框时由 hidden 变可见会被念一遍、
        //      description 又念一遍 ⇒ 双读;挂在 span 上,开框那一刻它是空的,不产生播报;
        //   ② role=status 隐含 aria-atomic=true,挂在段落上会把 70 多字的范围提示连同新增
        //      那句整段重读,而真正变的只有后半句。
        // ← 把 role="status" 挪回外层段落,这一格红。
        check(
            rmOpen.statusOnDone && !rmOpen.statusOnNote,
            "C8r live region 挂在播报句那个 span 上,不在外层段落上",
        );
    }

    // ② 点主钮 ⇒ 后端受理(ok:true),但基线不前移 ⇒ **框仍开、提示仍在、徽标仍亮**。
    check(await click("reanalyze-ask-primary"), "C8r 主钮可点");

    // [复审第 7 轮] **先证「这一轮真的跑过分析」,再谈「跑了却没前移」。**
    //
    // 为什么必须有这一格:`doReanalyzeFromAsk` 的拒绝态(`ok:false` / `observer` / call 抛了)
    // 走的**也是**「不关框 + requestRender」,而 rangeNote 在开框那一下就显出来了、badge 本来
    // 就亮 —— 下面那三格在「受理了但判据挡住前移」与「压根没跑起来」两种情形下取值完全相同。
    // 那样「mock 的 analyze 根本没起来」就会冒充「跑了、判据挡住了」,和 host 侧用
    // `takeAnalysisDone()` 堵掉的是同一个形态(commit `16261a0` 的 message 里写过这条规矩,
    // web 侧上一轮漏了)。
    //
    // 正信号取 `analysis_run.progress`:装载时该字段**根本不存在**(mock-data 的初值是
    // `{running:false}`),只有跑完一轮流水线才被写成 1;被拒时它一动不动。
    // [复审第 8 轮] **轮询,不用固定 sleep。** 原来写的是 `sleep(1200)`,而 mock 的收尾是裸
    // `later(800, …)` —— 余量只有 400ms,而 gate 3e 会同时起 6 个无头 Chrome。一旦滑过去,
    // `progress` 仍是 undefined,下面那格会打印「分析真的跑完了一轮 失败」——**与它要证伪的
    // 东西完全同形**,读日志的人会去查一个不存在的回归。假红伪装成真红比单纯 flaky 贵得多。
    // 轮到就走、超时才判负,快路径还更快(同一段 ③ 对照组用的就是这个写法)。
    const RUN_DONE = IN(`const m = w.__SCVB_MOCK__;
        if (!m || typeof m.requestInitialState !== "function") return false;
        return Promise.resolve(m.requestInitialState()).then(
            (st) => (st.analysis_run || {}).progress === 1 && !(st.analysis_run || {}).running,
        );`);
    check(
        await waitFor(RUN_DONE, 6000),
        "C8r 分析在 6s 内跑完一轮(轮询,不靠固定等待)",
    );

    const rmRun = await evaluate(
        IN(`const m = w.__SCVB_MOCK__;
            if (!m || typeof m.requestInitialState !== "function") return null;
            // IN() 包出来的不是 async 函数,所以返回 promise 让 Runtime.evaluate
            // 的 awaitPromise 去解(:423),不要在这里写 await。
            return Promise.resolve(m.requestInitialState()).then((st) => ({
                progress: (st.analysis_run || {}).progress,
                running: !!(st.analysis_run || {}).running,
                applied: ((st.analysis || {}).applied || {}).loudness_mode,
                current: (st.analysis || {}).loudness_mode,
            }));`),
    );
    if (check(rmRun, "C8r 取到 mock 快照")) {
        // ← 让 mock 的 analyze 回 {ok:false}:**本格与上面那格轮询会一起红**(轮询先走满
        //   6s 超时,再红这一格),而「框仍开 / 提示仍在 / 徽标仍亮」三格照样绿 —— 那三格
        //   分不开「受理了但判据挡住」与「压根被拒」,这两格才是拆开它们的那把刀。
        //   [SL-348 复审第 1 轮] 换轮询之后本格已被上面那格**严格蕴含**;留着是因为它是
        //   文档化的锚点(点名 `progress` 这个正信号取得到、且顺带钉住 applied/current 的
        //   取值),单独删掉不会让任何形态漏网。
        check(
            rmRun.progress === 1 && !rmRun.running,
            "C8r 分析**真的跑完了一轮**(受理了,不是被拒)",
        );
        // 跑完了却没前移 —— 这才是判据挡住的证据,不是「没跑所以没变」。
        check(
            rmRun.applied === "kw_integrated" && rmRun.current === "peak_dbfs",
            "C8r 跑完仍 stale:applied 停在 kw_integrated、当前是 peak_dbfs",
        );
    }

    const rmAfter = await evaluate(ASK_PROBE);
    if (check(rmAfter, "C8r 探针取到锚点(点主钮后)")) {
        // ← 把 doReanalyzeFromAsk 里那段 `if (rangeLimited()) { … return; }` 删掉,这一格红。
        check(
            rmAfter.open,
            "C8r 点完主钮**框仍开** —— 受理成功 ≠ 达成了用户点它的目的",
        );
        check(rmAfter.rangeNote, "C8r 范围提示仍摆在眼前");
        // [SL-348 复审第 1 轮] 两个探针必须**互不包含**:段落里现在有两个 span,若 rangeNote
        // 读的是整段 textContent,它就会把播报句一起吞进来 —— 那时探针名与它实际测的东西
        // 对不上,下一个人加一条全等断言会红在无关的地方。
        // ← 把 rangeNote 探针改回读整段,这一格红。
        check(
            !!rmAfter.rangeDone &&
                !(rmAfter.rangeNote || "").includes(rmAfter.rangeDone),
            "C8r rangeNote 探针只读静态那半(不含播报句)",
        );
        // [SL-348 复审第 2 轮] 词条里**不留前导/尾随空白**:分隔由 CSS ::before 给。
        // 读的是没 trim 过的那一份,否则探针自己把要查的东西擦掉了。
        // ← zh(本格)/ en / fr(下面 noPad 那两格)任一处把前导空格加回来,**对应那一格**红。
        //   本格只读得到 zh:rmAfter 是在 zh 下取的。
        check(
            typeof rmAfter.rangeDoneRaw === "string" &&
                rmAfter.rangeDoneRaw !== "" &&
                rmAfter.rangeDoneRaw === rmAfter.rangeDoneRaw.trim(),
            "C8r 播报句词条本身不带前导/尾随空白(分隔交给样式)",
        );
        // ← 把那条规则的选择器改回 `#id`(或删掉),这一格红:分隔真的没了,而上面那格
        //   仍然绿 —— 两格合起来才说得出「空白从词条里挪到了样式上」,少任何一格都是
        //   「拆了旧的没装新的」也能全绿。
        // 规则不匹配时 Chrome 的 computed content 是 "none" / "normal",两者都不含空格,
        // 所以 `.includes(" ")` 一条就够 —— 原来那个 `!== "none"` 合取项**恒真**,留着只会让
        // 人以为它在守什么(复审第 4 轮)。
        check(
            typeof rmAfter.rangeDoneBefore === "string" &&
                rmAfter.rangeDoneBefore.includes(" "),
            "C8r 分隔那条 ::before 真的生效了(选择器没写空)",
        );
        // [复审第 8 轮] 读屏那一侧的反馈:点之前这段是空的(开框时清掉),受理回来写入一句
        // 真话 ⇒ live region 有变化可念。少了这一格,「aria 加上了但点下去零反馈」照样全绿。
        check(
            !!rmAfter.rangeDone &&
                rmAfter.rangeDone !== (rmOpen || {}).rangeDone,
            "C8r 点主钮后 live region 里多出一句可播报的话(点之前是空的)",
        );
        check(
            rmAfter.badgeShown,
            "C8r 琥珀 badge **仍亮**(范围外的段还是旧口径,这是真话)",
        );
    }

    // ②c [SL-348 复审第 1 轮] **框开着切语言,两半必须一起换。**
    //
    //     播报句是由 JS 写进去的,`applyI18n` 只认 `data-t` —— 若把**文本**存起来,切语言时
    //     静态那半换成新语言、这半停在旧语言,同一个段落里前半英文后半中文。所以实现存的是
    //     **key**,每次 render 按当前字典重填(与 tab-wave 的 renderReidentifyBody 同一族)。
    //     ← 把 renderRangeDone() 从 syncReanalyzeRangeNote() 里摘掉(= 退回「只在写入那一刻
    //       填一次」),这一格红。
    //
    //     判据写成「两半是不是同一种文字」而不是比对具体字串:比字串就得把三语原文抄进用例,
    //     词条一改就红在无关的地方(本仓 assertScopeNote 那条教训)。
    check(await switchLangOutput("en"), "C8r 切到 en 可点(框开着)");
    await sleep(400);
    const rmEn = await evaluate(ASK_PROBE);
    if (check(rmEn, "C8r 切语言后探针取到锚点")) {
        const CJK = /[一-鿿]/;
        check(
            !!rmEn.rangeDone,
            "C8r 切语言后播报句仍在(不是被 applyI18n 抹掉)",
        );
        // [SL-348 复审第 2 轮] 前半必须先断**非空** —— `!CJK.test(null || "")` 恒真,
        // 「静态那半在 en 下整段不显」这个形态本来会空跑成绿。纯否定断言在空值上自动成真,
        // 是本卡反复栽的同一族;每写一条 `!x` 就机械地问一句「x 为空时这句是不是自动真」。
        check(
            !!rmEn.rangeNote &&
                !CJK.test(rmEn.rangeNote) &&
                !CJK.test(rmEn.rangeDone || ""),
            "C8r 切到 en 之后**两半都不含中文**(不会前半英文后半中文)",
        );
    }
    // [SL-348 复审第 3 轮] **前导空白那一格要覆盖三语,不能只钉 zh。**
    //   上一版只在 zh 下断过一次:en / fr 的词条串首把空格加回来,本卡新增的任何一格都不红
    //   ——「断言只覆盖三分之一的数据面」,与本卡记账里那一族(否定断言在空值上恒真)同源,
    //   都是「写下了可验证的行为,给出的判据却只钉住其中一部分」。这里每切一次语言就断一次
    //   **未 trim** 的那份。fr 也走一遍:这个桶原本 zh → en → zh,fr 一次都没出现过。
    const noPad = (probe, lang) =>
        check(
            typeof probe.rangeDoneRaw === "string" &&
                probe.rangeDoneRaw !== "" &&
                probe.rangeDoneRaw === probe.rangeDoneRaw.trim(),
            `C8r ${lang} 的播报句词条也不带前导/尾随空白`,
        );
    if (rmEn) noPad(rmEn, "en");
    check(await switchLangOutput("fr"), "C8r 切到 fr 可点(框仍开着)");
    await sleep(400);
    const rmFr = await evaluate(ASK_PROBE);
    if (check(rmFr, "C8r fr 下探针取到锚点")) {
        check(!!rmFr.rangeDone, "C8r fr 下播报句仍在");
        noPad(rmFr, "fr");
        // [SL-348 复审第 4 轮] **这一趟必须证得出它取的是 fr。**
        //   en 那侧有 `!CJK.test(...)` 兜着(zh→en 换了字符集);fr 与 en 同为拉丁字母,
        //   `noPad` 与 `!!rangeDone` 在「fr 字典整块缺失 / 回退到 en」时**全绿** ——
        //   那样这一趟只是多跑了一遍 en 的形态。而 `switchLangOutput("fr")` 那格只保证
        //   胶囊**可点**(click 里 n.click(),节点在就回 true),不保证语言真切过去。
        //   ← 让 fr 字典缺这条 / 回退到 en,下面第一格红。
        //   断「与 en 那条不相等」而不是比对译文:不用把任何一句 fr 原文抄进用例,
        //   词条改写也不会红在无关的地方。
        check(
            !!(rmEn && rmEn.rangeDone) && rmFr.rangeDone !== rmEn.rangeDone,
            "C8r fr 的播报句与 en 的**不相等**(真的换到了 fr,不是回退)",
        );
        check(
            rmFr.docLang === "fr",
            "C8r documentElement.lang 也切到了 fr(applyI18n 结尾写的那一处)",
        );
    }

    check(await switchLangOutput("zh"), "C8r 切回 zh(不影响后面的桶)");
    await sleep(400);

    // ②b [复审第 8 轮] **重开框时那句播报必须已经清掉。**
    //     不清的话,用户下次在范围档下开框,框里一上来就写着「已按当前范围重新分析」——
    //     这一次他什么都还没点。那是一句**当下为假**的话,而且 live region 只在文本变化时
    //     播报,不清还会让第二次点击变成零变化、读屏什么也不念(回到本轮要修的原点)。
    //     ← 删掉 openReanalyzeAsk 里那句 setRangeDoneText(""),只红这一格。
    check(await click("reanalyze-ask-later"), "C8r 「稍后」可点(收框以便重开)");
    check(await waitFor(askClosed, 3000), "C8r 框已关");
    check(await setLoudness("rms"), "C8r 再改一次档(重新置起开闸位)");
    check(await waitFor(askOpen, 4000), "C8r 框重开");
    const rmReopen = await evaluate(ASK_PROBE);
    if (check(rmReopen, "C8r 重开后探针取到锚点")) {
        check(
            rmReopen.rangeDone === "",
            "C8r 重开时上一轮的播报文本已清空(这一次用户还没点任何东西)",
        );
        check(!!rmReopen.rangeNote, "C8r 重开后范围提示仍在(档位没变)");
    }

    // ②d [SL-348 复审第 1 轮] **框开着切出范围档再切回来,上一轮那句不得复活。**
    //
    //     `syncReanalyzeRangeNote()` 只开合外层段落,里面那句原来不动 —— 切回范围档时
    //     上一轮的播报句会随段落重新显出来,而它描述的那次分析早已不是「当前范围」。
    //     所以实现在 `limited` 为假时**连 key 一起清掉**。
    //     ← 把那句 `if (!limited) …= null` 删掉,这一格红。
    //
    //     经桥直接改档位(不是点 UI):Tab3 的范围控件不在设置页上,而这条链要的正是
    //     「框开着、用户在别处改了档位」这个形态。
    // ★ 先把 key 重新置上:上一格(②b)重开框时已经把它清掉了,不重置的话本格
    //   变成「本来就是空的 ⇒ 恒绿」,钉不住任何东西(初稿正是这么滑过去的)。
    check(
        await click("reanalyze-ask-primary"),
        "C8r 再点一次主钮(重新置上播报句)",
    );
    check(await waitFor(RUN_DONE, 6000), "C8r 第二轮分析也跑完了");
    const rmAgain = await evaluate(ASK_PROBE);
    check(!!(rmAgain && rmAgain.rangeDone), "C8r 前置:播报句确实被重新置上了");
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m || typeof m.setRange !== "function") return false;
                return Promise.resolve(m.setRange("follow", 0, 0)).then(() => true);`),
        ),
        "C8r 框开着切到 follow 档",
    );
    await sleep(400);
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                return Promise.resolve(m.setRange("manual", 5, 9)).then(() => true);`),
        ),
        "C8r 再切回 manual 档",
    );
    await sleep(400);
    const rmBack = await evaluate(ASK_PROBE);
    if (check(rmBack, "C8r 档位来回切之后探针取到锚点")) {
        check(!!rmBack.rangeNote, "C8r 切回范围档后范围提示回来了");
        check(
            rmBack.rangeDone === "",
            "C8r 但上一轮的播报句**没有**跟着复活(它描述的已不是当前范围)",
        );
    }

    // ③ 对照组:follow 档下同一枚主钮**必须**关框 —— 少了这一格,把「恒不关框」写死也全绿。
    assertClean("range-manual-reanalyze");
    newBucket("range-follow-reanalyze");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=loudness-stale-on-load`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C8r 对照组(follow 档)装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);
    check(await setLoudness("peak_dbfs"), "C8r 对照组改档可点");
    check(await waitFor(askOpen, 4000), "C8r 对照组 ⇒ 弹框");
    const flOpen = await evaluate(ASK_PROBE);
    if (check(flOpen, "C8r 对照组探针取到锚点")) {
        check(
            !flOpen.rangeNote,
            "C8r follow 档**不**显示范围提示(它只在范围档下才是真话)",
        );
        // [复审第 8 轮] **显隐断了不等于读屏断了。** AccName/Description 计算对
        // aria-describedby **直接引用**的节点是「即使 hidden 也纳入」的 —— 把两个 id
        // 静态并进 index.html、再删掉 tab-settings 里那段 setAttribute,上面那格照样绿
        // (那个 <p> 确实 hidden),而 follow 档的读屏用户会被念到范围提示这句假话。
        // 本仓两处注释花了整段论证这一点,却一直没有机器守着它 —— 这一格就是。
        check(
            !(flOpen.describedBy || "").includes("reanalyze-ask-rangenote"),
            "C8r follow 档的 aria-describedby **不**带范围提示(hidden 也会被读屏念到)",
        );
    }
    check(await click("reanalyze-ask-primary"), "C8r 对照组主钮可点");
    check(
        await waitFor(askClosed, 4000),
        "C8r follow 档下点完主钮**框关掉** —— 这一下真的达成了用户的目的",
    );
    assertClean("range-follow-reanalyze");

    // C10 [SL-354] 用户 v5.6.7 真机四条 + 一道兜底闸,分三张页跑:
    //     · C10a/b/e:`scenario=slow-state-echo`(写的回执先到、状态帧后到一拍,中间还夹
    //       一帧内容早于写、送达晚于写的全量快照)。默认 mock 是同步 emit 的,①② 在它上面
    //       **一条都复现不出来** —— 那正是 preview 三个月没照出这些缺陷的原因;
    //     · C10c / C10c2 / C10g:同一场景**另开一张干净页**(理由见 C10c 那处);
    //     · C10f + C10d:`scenario=applied-echo-drop`(写落地后补一帧缺 `analysis.applied`
    //       的全量快照)—— 与时序无关的另一条路,故不与上面几格共页;字号那一格搭在这里,
    //       理由是它只需要「框开着」,不该被 ①② 的守卫带红(见那处)。
    newBucket("sl354-real-cadence");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=slow-state-echo`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C10 真机时序场景装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);

    // C10a ①「第一下只出横幅、第二次切换才出弹窗」。
    //   根因:开闸位原来是布尔,点档回执到达时 state 还没回来 ⇒ 派生 stale 为假 ⇒
    //   `!stale` 分支把刚置起的闸当场清掉。**这一格断的是「第一下就弹」**。
    //   ← 把 syncStale 里那句「待观察的写还没露面 ⇒ 什么都不做」删掉,这一格红。
    // ★ 先钉住**这个复现场景真的还有牙齿**:量「点下去 → 状态帧真正到达 UI」的耗时。
    //   真机是 4Hz,一帧 250ms,①② 两条缺陷都活在这个差里。没有这一格的话,谁把 mock 的
    //   回声延时「优化」回 0,下面几格照样全绿 —— **缺陷压根没被造出来,判据看着接住了
    //   其实什么也没接**。本卡实测:那一刀当场空了,才补的这一格。
    //   量的是**页内**的时间,不是 node 侧的 —— node 侧还夹着 CDP 往返,分辨率不够。
    //   徽标是纯派生的(state 到了才亮),所以它变亮的那一刻就是状态帧到达 UI 的那一刻。
    //   mock 的快照是**同步**更新的、延后的只是事件,所以不能拿 requestInitialState() 判
    //   (初稿这么写,当场读到新值 —— 读错了对象)。
    //   ← 把 `later(250, …)` 改回 `later(0, …)`,这一格红。
    const c10lag = await evaluate(
        IN(`const btn = q('[data-gb="settings-loudnessmode-seg"] [data-value="rms"]');
            if (!btn) return null;
            const t0 = w.performance.now();
            btn.click();
            return new Promise((resolve) => {
                const poll = () => {
                    const b = gb("settings-loudnessmode-stale");
                    if (b && vis(b)) return resolve({ ms: w.performance.now() - t0 });
                    if (w.performance.now() - t0 > 3000) return resolve({ ms: -1 });
                    w.requestAnimationFrame(poll);
                };
                poll();
            });`),
    );
    check(!!c10lag, "C10a 第一次改响度档可点");
    check(
        !!c10lag && c10lag.ms >= 100,
        `C10a 状态帧确实滞后于写回执(徽标 ${c10lag && Math.round(c10lag.ms)}ms 后才亮;同步 mock 下是一帧内)`,
    );
    check(
        await waitFor(askOpen, 4000),
        "C10a **第一次**改档就弹窗(不是只出琥珀徽标、要等第二次)",
    );

    // C10b ②「弹窗出来一下就闪现消失了」。
    //   扳机是上面那一帧过期全量快照(旧 current + 旧 applied ⇒ 两者相等 ⇒ stale 假)。
    //   等它跑完再看框还在不在:旧实现会在那一帧上关框且此后不再弹。
    //   ← 同上那句守卫删掉,这一格也红(两格一起红是设计内:同一个根因)。
    await sleep(1200);
    check(
        await evaluate(askOpen),
        "C10b 过期全量帧过去之后**框还在**(不被一帧旧快照关掉)",
    );
    const c10 = await evaluate(ASK_PROBE);
    if (check(c10, "C10 探针取到锚点")) {
        check(c10.badgeShown, "C10b 琥珀徽标也还在");
    }

    // C10e 「用户自己改回去了」那条路 —— ①② 的修法把「写还没回来」和「改回原值」两个
    //   长得一样的形态分开了,这一格钉的是**分对了的那一半**:改回基线 ⇒ 框该关、闸该清。
    //   三步各钉一件事。删除式**按 6021122 上的实测写**(复审 r1/r2 两轮各加了一道闸,
    //   把原来「单拆即红」的两格兜成了「两道都拆才红」—— 旧说法已作废,别照旧文推):
    //     e1 框关     ← 拆掉 `!stale` 那支里的 closeReanalyzeAsk() ⇒ e1 红
    //                   (e2 断的是同一个「框是关着的」DOM 状态,会**连带**红);
    //     e2 不误弹   ← **两道**:`!stale` 里清 `askPending` 那一行 / r1 加的
    //                   `pendingStale` 早退。单拆任一道都照绿(实测),**两道一起拆**
    //                   ⇒ e2(与 C10g)红。语义上前者管「记录该作废了」、后者管
    //                   「弹的是刚改走的那一项」,两条独立成立,故都留。
    //     e3 能再弹   ← **两道**:wireSeg 里「一次新的用户写就清 `reanalyzeAskedFor`」
    //                   / `!stale` 那支里同名的那一行。单拆任一道都照绿(实测),
    //                   **两道一起拆** ⇒ e3(与 C10h)红。下面特意改回 **rms** 这个
    //                   C10a 用过的同一个值,token 逐字相同,只有被清掉才可能再弹。
    //                   ⚠ 单拆 wireSeg 那一道时红的是 **C10h**(不是 e3)—— e3 走的是
    //                   「先经 `!stale` 支清过」的路,C10h 走的是「那支根本没跑到」的路,
    //                   两格覆盖同一条闸的两个入口。
    check(await setLoudness("kw_integrated"), "C10e 改回基线可点");
    check(
        await waitFor(askClosed, 4000),
        "C10e1 改回基线 ⇒ 框关(当前 == 基线,这一帧是真读数不是回落值)",
    );
    check(await waitFor(badgeGone, 4000), "C10e1 琥珀徽标同时灭掉");
    // e2:非 UI 路径改**另一项**(中央槽),绕开 wireSeg ⇒ 不该重新置闸。
    // 走 loudness 那一项验不到本条:那一项的当前值恰好等于旧记录里的值,会被
    // 「写还没露面」那把尺子先挡下,红不了。
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m || typeof m.setAnalysisConfig !== "function") return false;
                const r = m.setAnalysisConfig({ center_slot_policy: "lead_exclusive" });
                return !!r && r.ok !== false;`),
        ),
        "C10e2 mock 侧改中央槽被受理(非 UI 写入路径)",
    );
    check(
        await waitFor(
            IN(`const b = q('[data-gb="settings-centerslot-seg"] [data-value="lead_exclusive"]');
                return !!b && b.getAttribute("aria-pressed") === "true";`),
            5000,
        ),
        "C10e2 新口径经 scvb.state 落到 UI(证明这一轮 syncStale 真跑过)",
    );
    await sleep(600); // 让 slow-state-echo 那三帧(250/300/350ms)全部走完
    check(
        await evaluate(askClosed),
        "C10e2 后端改的另一项**不弹框**(待观察记录已作废 + 弹的是刚改走的那一项,两道各自兜得住)",
    );
    // e3:用户真改一次,而且改回 C10a 用过的同一个值 —— token 逐字相同。
    check(await setLoudness("rms"), "C10e3 用户再改走可点");
    check(
        await waitFor(askOpen, 4000),
        "C10e3 改回基线之后再改走 ⇒ **照样弹**(哪怕是刚弹过的同一个值)",
    );
    check(await click("reanalyze-ask-later"), "C10e 「稍后」收框");
    check(await waitFor(askClosed, 3000), "C10e 框已关");
    assertClean("sl354-real-cadence");

    // C10c ③「B3(中央槽策略)完全没有弹出弹窗,应该和前面一样」。
    //   原来 wireSeg 只给 loudness_mode 置闸、syncStale 只读 loudnessStale —— 那是 SL-276
    //   按当时的用户口径**有意**做的,本卡按用户新口径翻面。
    //
    //   ⚠ **必须开一张干净的页**:接在上面那几格后面的话,响度档已经是 rms 而基线还是
    //   kw_integrated ⇒ `loudnessStale` 恒真 ⇒ 就算把判据改回「只读响度」这一格照样绿
    //   (实测过:那一刀当场空了)。一格判据的有效性取决于它前面几格留下的状态 ——
    //   本卡第二次栽在这上面,所以这里单开一页、只动中央槽这一项。
    //   ← 把开闸点改回只认 loudness_mode,或把判据改回只读 loudnessStale,这一格红。
    //   本格跑在同一个异步回声场景上,所以 ① 那道守卫被拆掉时它**也会**红(第一下就
    //   弹不出来)——那是超集,不是本格的判据失效;本格自己的两条删除式是上面两条。
    newBucket("sl354-centerslot");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=slow-state-echo`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-centerslot-seg"); return !!n;`),
        ),
        "C10c 干净页装载(响度档未动 ⇒ loudnessStale 为假)",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);
    const c10cPre = await evaluate(ASK_PROBE);
    if (check(c10cPre, "C10c 前置探针")) {
        check(
            !c10cPre.badgeShown && !c10cPre.open,
            "C10c 前置:响度那半不 stale、框也没开(否则本格会被撑成恒真)",
        );
    }
    check(await setCenterSlot("lead_exclusive"), "C10c 改中央槽策略可点");
    check(
        await waitFor(askOpen, 4000),
        "C10c 改中央槽策略**同样弹窗**(与响度档一视同仁)",
    );

    // C10c2 [SL-354 复审第 1 轮] 框里的说明段**说的是被改的那一项**。
    //   弹窗铺到中央槽之后,这一段原本恒是响度口径专用的那条词条 —— 只改中央槽的用户
    //   看到的框在解释另一件事;而这个 `<p>` 同时是本框 aria-describedby 的目标,读屏
    //   用户听到的描述同样错。判据取**三处 textContent 全等/不等**,不钉词条 key 字面:
    //     · 框内说明段 == 中央槽卡第二行(逐字);
    //     · 且 != 响度卡第二行(否则「两条词条恰好一样」也能蒙混过关)。
    //   ← 把 syncReanalyzeScopeNote() 里按字段取 key 那一句改回恒取响度那条,本格红。
    const c10c2 = await evaluate(
        IN(`const a = gb("reanalyze-ask-scopenote");
            const c = gb("settings-centerslot-scopenote");
            const l = gb("settings-loudnessmode-scopenote");
            if (!a || !c || !l) return null;
            return {
                ask: a.textContent.trim(),
                center: c.textContent.trim(),
                loud: l.textContent.trim(),
            };`),
    );
    if (check(c10c2, "C10c2 取到三处说明段")) {
        check(
            c10c2.center.length > 0 && c10c2.loud.length > 0,
            "C10c2 两条对照词条都非空(空串会把下面两格撑成恒真/恒假)",
        );
        check(
            c10c2.center !== c10c2.loud,
            "C10c2 两条词条本来就不一样(不然下面那格分不出取的是哪条)",
        );
        check(
            c10c2.ask === c10c2.center,
            `C10c2 框内说明段 = 中央槽卡那条(实得 ${JSON.stringify(c10c2.ask.slice(0, 24))})`,
        );
    }

    // C10g [SL-354 复审第 1 轮] **两项都脏时,把其中一项改回基线不该再弹一次框。**
    //   `stale` 是两项取或,而 token 只按待观察的那一项算 —— 改回基线时走不进 `!stale`
    //   那支(另一项还脏),token 却换了 ⇒ 框又弹、焦点被抢到主钮上。用户刚**撤销**了
    //   自己的一个改动却收到一个 alertdialog,与 SL-276 的口径对不上。
    //   ← 把 syncStale 里「待观察的那一项自己得是脏的」那句早退删掉,本格红。
    check(await click("reanalyze-ask-later"), "C10g 先收掉中央槽那一框");
    check(await waitFor(askClosed, 3000), "C10g 框已关");
    check(await setLoudness("rms"), "C10g 再把响度也改走(两项同时脏)");
    check(await waitFor(askOpen, 4000), "C10g 响度那一下照常弹");
    check(await click("reanalyze-ask-later"), "C10g 再收掉");
    check(
        await waitFor(askClosed, 3000),
        "C10g 框已关(两项都脏,两枚徽标都亮着)",
    );
    check(await setLoudness("kw_integrated"), "C10g 把响度改回基线可点");
    check(
        await waitFor(badgeGone, 4000),
        "C10g 响度那枚徽标灭(改回基线生效了)",
    );
    const c10g = await evaluate(
        IN(`const ask = gb("reanalyze-ask");
            const cb = gb("settings-centerslot-stale");
            if (!ask || !cb) return null;
            return { open: vis(ask), centerBadge: vis(cb) };`),
    );
    if (check(c10g, "C10g 探针取到锚点")) {
        check(
            c10g.centerBadge,
            "C10g 正证据:中央槽那枚徽标**还亮着** —— 确实处在「另一项仍脏」这个形态里",
        );
        check(
            !c10g.open,
            "C10g 撤销自己的一个改动**不弹框**(弹的判据是「刚改走的那一项自己脏」)",
        );
    }
    assertClean("sl354-centerslot");

    // C10f 兜底闸:**回落值只许渲染,不许做破坏性判断**。
    //   `appliedAnalysisConfigOf` 在 state 缺 `analysis.applied` 时回落到当前值(SL-279 的
    //   设计,为的是旧插件下徽标不误亮),回落之后派生的 stale 恒假 —— 与「基线真的等于
    //   当前值」逐字节相同。拿它去清闸 + 关框,就是用户报的 ② 的**缺字段变体**。
    //   夹具 = `scenario=applied-echo-drop`:写走同步路径(框正常弹),300ms 后补一帧
    //   **只摘掉 applied 这一支**的全量快照;全量帧在 UI 侧是整体替换,于是 store 里的
    //   applied 被抹掉。与 slow-state-echo 分成两个场景,免得两条路互相顶替。
    //
    //   ★ 这一格自带**正证据**:徽标是纯派生的,回落之后它会当场灭掉 —— 徽标灭 = 那一帧
    //   确实到了、也确实被渲染了。所以「框还在」不可能是「什么都没发生」冒充的。
    //   ← 把 syncStale 里 `!stale` 那支开头的「没带 applied ⇒ 什么都不做」删掉,
    //     「框还在」那一格红,而「徽标灭了」那一格照绿(它验的是回落仍在渲染面生效)。
    //
    //   真桥今天恒发这两个字段,所以本格守的是类别不是当前可达路径(见 tab-settings.js
    //   里 hasAppliedAnalysisConfig 的头注)。
    newBucket("sl354-applied-drop");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?scenario=applied-echo-drop`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C10f 缺字段场景装载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(600);
    check(await setLoudness("rms"), "C10f 改响度档可点");
    check(await waitFor(askOpen, 4000), "C10f 改档 ⇒ 弹窗(同步路径,先立起来)");
    const c10fPre = await evaluate(ASK_PROBE);
    if (check(c10fPre, "C10f 前置探针")) {
        check(
            c10fPre.badgeShown,
            "C10f 前置:缺字段那一帧还没到,徽标是亮的(下一格的对照点)",
        );
    }

    // C10d ④「这些字有点小」。钉的是**用了哪条刻度变量**,不是像素数:写死数值等于把
    //   设计值抄成第二份。
    //   ⚠ 搭在**这张页**上而不是 slow-state-echo 那张:那张页上「框开着」本身是 ①② 的
    //   修法挣来的,拆掉那道守卫会把本格一起带红,红的原因就跟字号无关了。这张页走同步
    //   路径,弹框与 ①② 的守卫无关。
    //   ⚠ 「框开着」用**盒高**判,不用 `vis(说明段)`:`vis` 只看元素自己的 hidden 属性,
    //   而 hidden 挂在外层遮罩上 —— 拿它判这一段等于写了条恒真断言(初稿如此,D1 注入
    //   时框根本没开、这一格照绿才发现)。display:none 的元素 getComputedStyle 仍解析得出
    //   font-size,所以不加这一格的话,量到的可能压根不是用户眼前那一段。
    //   ← 把 index.html 里 `.sc-modal--reanalyze` 那条 font-size 规则删掉(退回全局的
    //     `.sc-modal__note`,即 --fs-95),这一格红。
    const c10d = await evaluate(
        IN(`const p = gb("reanalyze-ask-scopenote");
            if (!p) return null;
            const root = d.documentElement;
            const token = w.getComputedStyle(root).getPropertyValue("--fs-110").trim();
            return {
                got: w.getComputedStyle(p).fontSize,
                token,
                boxH: Math.round(p.getBoundingClientRect().height),
            };`),
    );
    if (check(c10d, "C10d 取到弹窗说明段与刻度变量")) {
        check(
            c10d.boxH > 0,
            `C10d 量的是框开着时真上屏的那一段(盒高 ${c10d && c10d.boxH}px > 0)`,
        );
        check(
            !!c10d.token && c10d.got === c10d.token,
            `C10d 弹窗说明段用的是 --fs-110 这条刻度(实得 ${c10d && c10d.got},刻度 ${c10d && c10d.token})`,
        );
    }
    await sleep(1200); // 等那一帧缺 applied 的全量快照(300ms)送达并渲染完
    const c10f = await evaluate(ASK_PROBE);
    if (check(c10f, "C10f 探针取到锚点")) {
        check(
            !c10f.badgeShown,
            "C10f 正证据:缺 applied 的全量帧确实到了 —— 回落成「基线 = 当前值」,徽标当场灭",
        );
        check(
            c10f.open,
            "C10f **框还在** —— 回落算出来的 stale 假不作数,不许拿它清闸关框",
        );
    }
    assertClean("sl354-applied-drop");

    // C9 [SL-276 二轮复审] 开闸位是**一次性**的:弹过就不许再自己弹一次。
    // C8 管「从没被置位过」,C9 管「置位过、已经用掉了」—— 后者是 C8 的改法留下的口子:
    // 本位原来只有 stale 归假才灭,于是「改档 → 弹 → 稍后」之后它仍为真,再来一次
    // 非用户驱动的换档照样能把框推到眼前。
    // [SL-354] 一次性的**承担者换了**:开闸位从布尔换成「刚写成功的字段与值」,开框时
    // 不再就地清掉它(它还要继续当「这一帧新不新」的尺子,见 C10a),改由
    // `reanalyzeAskedFor` 记下的那个 token,**外加**「待观察的写还没露面就什么都不做」
    // 那句早退 —— 两道各自独立挡得住,所以本条的删除式是**两道一起拆**(单拆任一道都
    // 照绿,实测过)。逐条见文件头 C9 那段。
    newBucket("reanalyze-ask-oneshot");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C9 页面重载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(400);

    // ① 用户真改一次档 ⇒ 置位 + 弹框(此后 reanalyzeAskedFor = "loudness_mode=peak_dbfs";
    //    [SL-354] token 带字段名,不再是裸值)。
    check(await setLoudness("peak_dbfs"), "C9 用户改档 peak_dbfs 可点");
    check(await waitFor(askOpen, 4000), "C9 用户改档 ⇒ 弹框");
    check(await click("reanalyze-ask-later"), "C9 「稍后」可点");
    check(await waitFor(askClosed, 3000), "C9 「稍后」关框");

    // ② 非用户驱动的口径变化:直接调 mock 后端的 setAnalysisConfig,**绕开 UI 的写入
    //    路径**(wireSeg 那条),所以开闸位不会被重新置起。这与只读观察态收
    //    scvb.state、切版本、快照恢复是同一条「新值从后端来」的路。换到 rms —— 与上一步
    //    记下的 peak_dbfs 是不同值,于是 reanalyzeAskedFor !== mode 成立,**唯一**还能挡住
    //    这一框的就是被清掉的那一位。
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m || typeof m.setAnalysisConfig !== "function") return false;
                const r = m.setAnalysisConfig({ loudness_mode: "rms" });
                return !!r && r.ok !== false;`),
        ),
        "C9 mock 侧改档被受理(非 UI 写入路径)",
    );
    check(
        await waitFor(
            IN(`const b = q('[data-gb="settings-loudnessmode-seg"] [data-value="rms"]');
                return !!b && b.getAttribute("aria-pressed") === "true";`),
            5000,
        ),
        "C9 新口径经 scvb.state 落到 UI(证明这一轮 syncStale 真跑过,不是没触发)",
    );
    const oneShot = await evaluate(ASK_PROBE);
    if (check(oneShot, "C9 探针取到锚点")) {
        check(
            !oneShot.open,
            "C9 非用户驱动的换档**不再弹** —— 一次置位只换一次弹框,用完就清",
        );
        check(oneShot.badgeShown, "C9 琥珀 badge 仍在(纯派生语义不变)");
    }

    // ③ 这道「一次性」没有把功能一起关掉:用户再真改一次档,照样弹。
    //    先回基线再改走 —— 直接点 peak_dbfs 的话会撞上 wireSeg 的「点击已选中档不
    //    重复写」,验不到本条想验的东西。
    //    [SL-354 复审第 2 轮] 这里「照样弹」的承担者是 **wireSeg 里那次新用户写清掉
    //    reanalyzeAskedFor**;`!stale` 那支里同名的那一行是冗余的(见 C10e3 注释)。
    check(await setLoudness("kw_integrated"), "C9 回基线可点");
    check(await waitFor(badgeGone, 4000), "C9 回基线 ⇒ stale 归假(badge 灭)");
    check(await evaluate(askClosed), "C9 回基线不弹框");
    check(await setLoudness("rms"), "C9 用户再改走可点");
    check(await waitFor(askOpen, 4000), "C9 用户再改走 ⇒ 照样弹");

    // C10h [SL-354 复审第 2 轮] **非 UI 路径把口径改回基线之后,用户重选同一个值仍要弹。**
    //   复审给的可复现序列,逐字照做(接着 C9 的状态往下走,框正开着、记着
    //   `loudness_mode=rms` 这个 token):
    //     ① 「稍后」收框;
    //     ② **非 UI 路径**把响度改回基线 —— 那一帧 `当前值 != 刚写的值`,被「这一帧不
    //        作数」的尺子早退挡住,`!stale` 那支一次都跑不到 ⇒ 旧 token 留在位上;
    //     ③ 用户**再点 rms**(与 ① 之前那次逐字同一个值)⇒ 必须弹。
    //   不修的话这一格红在 ③:token 逐字相同 ⇒ 只亮徽标不弹框 —— 那正是用户报的 ①
    //   换了个入口,而旧的布尔实现在这条序列上是会弹的(本卡引入的静默行为变化)。
    //   ② 里断「徽标灭了」是**正证据**:证明那一帧真的到了、也真的渲染了,于是 ③ 的红
    //   不可能是「什么都没发生」冒充的。
    //   ← 拆掉 wireSeg 里「一次新的用户写就清 reanalyzeAskedFor」那一行,本格 ③ 红。
    check(await click("reanalyze-ask-later"), "C10h ① 「稍后」收框");
    check(await waitFor(askClosed, 3000), "C10h ① 框已关");
    check(
        await evaluate(
            IN(`const m = w.__SCVB_MOCK__;
                if (!m || typeof m.setAnalysisConfig !== "function") return false;
                const r = m.setAnalysisConfig({ loudness_mode: "kw_integrated" });
                return !!r && r.ok !== false;`),
        ),
        "C10h ② mock 侧改回基线被受理(非 UI 写入路径)",
    );
    check(
        await waitFor(badgeGone, 5000),
        "C10h ② 正证据:徽标灭了 —— 那一帧确实到了 UI(当前值已回到基线)",
    );
    check(await evaluate(askClosed), "C10h ② 这一下不该把框弹出来");
    check(await setLoudness("rms"), "C10h ③ 用户再点同一个值可点");
    check(
        await waitFor(askOpen, 4000),
        "C10h ③ 重选同一个值**照样弹**(一次新的用户写就重新开闸)",
    );
    assertClean("reanalyze-ask-oneshot");

    // C5 Esc 关框
    newBucket("reanalyze-ask-2");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(
            IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
        ),
        "C5 页面重载",
    );
    await dismissOverlays();
    await click("tabnav-settings");
    await sleep(400);
    check(await setLoudness("rms"), "再切一次 rms 可点");
    check(
        await waitFor(askOpen, 4000),
        "C5 改走 ⇒ 弹出(为 Esc 备一个开着的框)",
    );
    await pressEscape();
    check(await waitFor(askClosed, 3000), "C5 Esc 关框");
    assertClean("reanalyze-ask-2");

    // C6 三语各弹一次:正文非空且不是 key 字面量
    for (const lang of LANGS) {
        newBucket(`ask/${lang}`);
        await cdp.send("Page.navigate", {
            url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
        });
        check(
            await waitFor(
                IN(`const n = gb("settings-loudnessmode-seg"); return !!n;`),
            ),
            `${lang}:Output 页装载`,
        );
        await dismissOverlays();
        check(await switchLangOutput(lang), `${lang}:语言胶囊可点`);
        await sleep(300);
        await click("tabnav-settings");
        await sleep(300);
        check(await setLoudness("rms"), `${lang}:切响度档可点`);
        check(await waitFor(askOpen, 4000), `${lang}:弹窗弹出`);
        const t = await evaluate(ASK_PROBE);
        if (check(t, `${lang}:探针取到锚点`)) {
            check(
                t.body.length > 0 && !t.body.startsWith("set."),
                `${lang}:正文已翻(实得 ${JSON.stringify(t.body)})`,
            );
            check(
                t.primary.length > 0 && !t.primary.startsWith("set."),
                `${lang}:主行动钮已翻(实得 ${JSON.stringify(t.primary)})`,
            );
            check(
                t.later.length > 0 && !t.later.startsWith("set."),
                `${lang}:「稍后」已翻(实得 ${JSON.stringify(t.later)})`,
            );
            assertScopeNote(t, lang);
        }
        assertClean(`ask/${lang}`);
    }

    // =========================================================== F. SL-374
    // 「基于角度的音量调整 ±12 dB」曲线窗被压扁 / 上排控件卡太高(用户 v5.6.8 实测)。
    //
    // ★ 为什么是页面级:这一条全是**网格轨道之间怎么分高度**——
    //   `.master-grid` 是 `auto auto 1fr` 三行,第三行(曲线卡)吃剩余,于是**任何长在
    //   上面的东西**都直接从曲线窗身上扣。谁长了多少、扣掉之后还剩几个像素,只有排完版
    //   才知道,CSS 文件里没有一行可以 grep。
    //
    // ★ 三个「档」不是窗口高度,是**上面吃掉多少**——这一点与卡面的字面写法有出入,
    //   照实说明:Output 的设计盒**固定 1180x780**(web/shared/design-box.js),窗口
    //   变大变小走的是 CSS zoom 整体缩放,版式一个像素都不重排。所以真正会变的两件事是:
    //     ① 顶部横幅出不出来(本机实测:一条横幅吃掉 38.7px);
    //     ② RANGE 卡在哪一档(「手动」档的面板比「全曲」档高 27px)。
    //   T4 那一档另外把设计盒本身压到 580px,用来验**地板真的接得住**——它是纯合成条件,
    //   产品里到不了,注释里就写明它是压力档,别读成「窗口 580 时的真实版式」。
    //
    // ★ 本节只量版式,**不走交互路径**:RANGE 档位靠直接写 `data-range` 属性切
    //   (那正是本页 CSS 读的状态钩子,文件头「状态钩子一览」里逐字写着),
    //   横幅靠直接切 `hidden`。点真按钮会连带触发 SL-354 的「口径已改」弹窗,
    //   把版式量成弹窗盖住的样子;档位切换的**真行为**归上面 C 节与 range-manual-* 那几节。
    //
    // ★ 删除式(本机实测,实得清单在 PR 描述里):
    //   · E1 `.master-grid` 的 `grid-template-rows` 退回 `auto auto 1fr`
    //        ⇒ F1@T2 / F1@T3 / F1@T4 红(曲线窗 86 → 74 / 26 / 0)+ F4 红
    //          (T4 曲线卡 140 → 2);F1@T1、F2、F3 绿。
    //          第一版这里写的是「只有 F1@T4 红」—— 假句,复审逐条对过数才发现:
    //          地板一拆,凡是**自然版式已经跌破 80px 下限**的档位都跟着红,
    //          而 F4 本来就是同一条地板的另一半判据,不可能不红。
    //   · E2 删掉 `.master-grid > .sc-card[data-gb=…]` 那条内距/行距覆盖
    //        ⇒ F2 红(第 4 张 LEAD SELECT 的自然高度 116 → 131;另三张本来就在上限内,
    //          这与「行高由最高那张定」是同一件事)+ F3 两档都红(126 → 138 / 153 → 165)。
    //          F3 跟着红不是漏网:被删的那条覆盖**本来就同时挂着 RANGE 卡**。
    //   · E3 删掉 RANGE 卡的四条内距/行距/行高覆盖
    //        ⇒ 只有 F3(手动档)红(自然高度 153 → 166;E2 那条覆盖还在,所以退不到
    //          完全未改的 177.59)
    //     ⚠ E3 这一格**第一版一条都没红**,是补出来的:当时 F3 量的是「网格给了
    //       RANGE 多高」,而 E1 加的地板一咬住就把 RANGE 压到 141px —— 正好落在
    //       上限 160 之内。判据被自己 PR 里另一条判据兜绿了(「加一条判据可能让
    //       另一条判据失去它的删除式」)。改法:F2 / F3 改量**自然高度**
    //       (探针里临时把网格改成 align-items:start),把「轨道给多少」这一维拿掉。
    // =========================================================================
    log("F. SL-374 整体调整页版式:曲线窗地板 + 上排控件卡改矮");
    newBucket("sl374-layout");
    await cdp.send("Page.navigate", {
        url: `${base}/web-preview/output.html?fixture=fifteen-tracks`,
    });
    check(
        await waitFor(IN(`const n = gb("master-grid"); return !!n;`)),
        "Output 页装载(整体调整页)",
    );
    await dismissOverlays();
    await sleep(300);
    const LAYOUT_PROBE = IN(`
        const grid = gb("master-grid");
        const card = q("#card");
        const range = gb("master-range");
        const banner = gb("banner-staleCapture");
        const curve = gb("master-pancurve");
        const plot = q(".curve-plot");
        const row1 = ["master-group-selector", "master-width", "master-msbalance", "master-leadselect"];
        if (!grid || !card || !range || !banner || !curve || !plot) return null;
        const cardH0 = card.style.height;
        const range0 = range.getAttribute("data-range") || "follow";
        const bannerHidden0 = banner.hidden;
        const floorRaw = String(w.getComputedStyle(grid).getPropertyValue("--master-curve-min-h") || "").trim();
        const TIERS = [
            { name: "T1 设计盒原高 / 无横幅 / RANGE 全曲", h: 0, range: "follow", bannerHidden: true },
            { name: "T2 设计盒原高 / 无横幅 / RANGE 手动", h: 0, range: "manual", bannerHidden: true },
            { name: "T3 设计盒原高 / 有横幅 / RANGE 手动", h: 0, range: "manual", bannerHidden: false },
            { name: "T4 设计盒压到 580 / 有横幅 / RANGE 手动", h: 580, range: "manual", bannerHidden: false },
        ];
        const out = { floorRaw, tiers: [] };
        for (const t of TIERS) {
            card.style.height = t.h > 0 ? t.h + "px" : cardH0;
            range.setAttribute("data-range", t.range);
            banner.hidden = t.bannerHidden;
            void grid.offsetHeight;
            out.tiers.push({
                name: t.name,
                gridH: R(grid).h,
                curveCard: R(curve).h,
                curvePlot: plot.clientHeight,
                rangeH: R(range).h,
                row1: row1.map((n) => (gb(n) ? R(gb(n)).h : -1)),
            });
        }
        // ---- 另量一遍**自然高度**(卡自己的内容高,不受网格拉伸/压缩影响)----
        // 为什么必须单独量:F2 / F3 问的是「这张卡自己有没有变矮」,而网格里量到的
        // 是**轨道给了它多高**。地板一咬住,RANGE 就被压到上限之内 —— 本机实测:
        // 把 RANGE 的四条内距/行距/行高覆盖删掉(删除式 E3),自然高度从 153 退回 166,
        // 而 T2 那一档量到的仍是 141(地板把它压进去了)⇒ 那一格一条都不红。
        // 临时把网格改成 align-items:start,让每张卡按自己的内容取高,
        // 把「轨道给多少」这一维拿掉。(本段在页内探针的模板串里,不能出现反引号。)
        card.style.height = cardH0;
        banner.hidden = true;
        grid.style.alignItems = "start";
        const nat = {};
        for (const mode of ["follow", "manual"]) {
            range.setAttribute("data-range", mode);
            void grid.offsetHeight;
            nat[mode] = {
                rangeH: R(range).h,
                row1: row1.map((n) => (gb(n) ? R(gb(n)).h : -1)),
            };
        }
        grid.style.alignItems = "";
        out.natural = nat;
        card.style.height = cardH0;
        range.setAttribute("data-range", range0);
        banner.hidden = bannerHidden0;
        void grid.offsetHeight;
        return out;
    `);
    const lay = await evaluate(LAYOUT_PROBE);
    if (
        check(
            !!lay && Array.isArray(lay.tiers) && lay.tiers.length === 4,
            "SL-374 版式探针取到四档读数",
        )
    ) {
        for (const t of lay.tiers) {
            log(
                `  ${t.name}:grid ${t.gridH} / 曲线卡 ${t.curveCard} / 曲线窗 ${t.curvePlot} / RANGE ${t.rangeH} / 上排 ${JSON.stringify(t.row1)}`,
            );
        }
        // ---- F1 ★ 四档下曲线窗都不许被压扁 -----------------------------------
        // 下限 80px。这条是用户那句「压缩到基本什么都看不见」的判据,取值来路:
        //   本夹具(fifteen-tracks,与用户截图同量级的紧)改前实测 全曲档 70 /
        //   手动档 **32**;改后 T1 101 / T2 86 / T3 86 / T4 86(T2 起由地板兜)。
        //   80 卡在「改后的最小值 86」与「改前的全曲档 70」之间 —— 两侧都留了余量,
        //   而任何一次退回旧版式都会掉到 70 以下。
        for (const t of lay.tiers) {
            ge(t.curvePlot, 80, `F1 ★ ${t.name}:曲线窗高度`);
        }
        // ---- F2 / F3 量的是**自然高度**,不是轨道给了多高 ---------------------
        // (理由与实测见探针里那段注释:地板一咬住,轨道量到的数会把「卡没变矮」兜绿。)
        if (
            check(
                !!lay.natural && !!lay.natural.follow && !!lay.natural.manual,
                "SL-374 版式探针取到自然高度读数",
            )
        ) {
            log(
                `  自然高度:RANGE 全曲 ${lay.natural.follow.rangeH} / 手动 ${lay.natural.manual.rangeH} / 上排 ${JSON.stringify(lay.natural.follow.row1)}`,
            );
            // F2 ★ 上排四张控件卡都要矮下来。
            // 上限 120px:改前行高由最高的 LEAD SELECT 顶到 134px(GROUP 80 /
            // WIDTH 97 / MS BALANCE 97 三张本来就构不成上限),改后 116px。
            for (let i = 0; i < lay.natural.follow.row1.length; i++) {
                le(
                    lay.natural.follow.row1[i],
                    120,
                    `F2 ★ 上排第 ${i + 1} 张控件卡的自然高度`,
                );
            }
            // F3 ★ RANGE 卡限高:「全曲」档改前 139.5 → 改后 125.5;
            //     「手动」档改前 177.59 → 改后 152.69。
            le(lay.natural.follow.rangeH, 135, "F3 ★ RANGE 卡(全曲档)自然高度");
            le(lay.natural.manual.rangeH, 160, "F3 ★ RANGE 卡(手动档)自然高度");
        }
        // ---- F4 ★ 地板真的接得住(压力档)-------------------------------------
        // 读的是 `.master-grid` 上那个自定义属性本身,不手抄数字:真源只有 CSS 一处。
        // 先断它解得出、且不是个形同虚设的小数 —— 属性被改成 0 时上面 F1 会跟着塌,
        // 但报错会指向「曲线窗太矮」而不是「地板没了」,这一格负责把真因指出来。
        const floorPx = parseFloat(lay.floorRaw);
        check(
            Number.isFinite(floorPx) && floorPx >= 130,
            `F4 ★ --master-curve-min-h 解得出且 >= 130px(实得 ${JSON.stringify(lay.floorRaw)})`,
        );
        if (Number.isFinite(floorPx)) {
            ge(
                lay.tiers[3].curveCard,
                Math.round(floorPx) - 1,
                "F4 ★ T4 压力档:曲线卡不低于地板 —— 空间不够时先压上排控件卡,不压图",
            );
        }
    }
    assertClean("sl374-layout");
} catch (e) {
    fail++;
    console.log(`  [FAIL] 冒烟自身抛错:${e && e.stack ? e.stack : e}`);
} finally {
    try {
        if (cdp) cdp.close();
    } catch {}
    try {
        chrome.kill();
    } catch {}
    try {
        server.close();
    } catch {}
    try {
        rmSync(userDataDir, { recursive: true, force: true });
    } catch {}
}

if (fail === 0) {
    console.log(
        "✅ smoke-ui-layout-page:SL-272 / SL-275 / SL-276 / SL-273 / SL-374 全绿",
    );
    process.exit(0);
}
console.log(`❌ smoke-ui-layout-page:${fail} 条断言失败`);
process.exit(1);
