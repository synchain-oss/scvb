// SPDX-License-Identifier: GPL-3.0-or-later
// check-changelog-drafts.mjs —— 「合了就得把预写条目搬进正文」的机检(零依赖,Node >= 18,ESM)。
//
// [SL-295] 病灶:`CHANGELOG.md` 末尾那个 HTML 注释块放的是**待合并 PR 的预写条目**,规矩是
//   「各 PR 合并时把自己那条搬到上面对应小节」。这条规矩**只写在注释里,没有任何执行者** ——
//   于是它就是不被执行:清点时块里有 **24 条**预写条目对应的卡**早已合进 `feature/v1`**
//   (最早的一条随 `11fb039` / #107 上线),而正文里一个字都没有。没有任何门禁看得见:
//   注释块渲染不出来,发版清单第 1 步「每条带 PR 号」查的是**正文**,漏搬的条目不在它眼里。
//
// 判据:注释块里任何 `pending #SL<号>`,只要那张卡的号出现在 **base 分支的提交标题**里,
//   就是**漏搬**,门禁红。
//
// ⚠ 卡号必须按**压缩写法展开**再比。本仓的合并标题会把同批卡压成一条,例如 `d8ef5b9`:
//     fix(ui): [R3] SL-204/205/207 + 增补 SL-211/213/214 + J88 改名;… (#123)
//   朴素模式 `SL-(\d+)` 只吃得到每组的**第一个**号(204 与 211),205/207/213/214 全漏 ——
//   而这四张卡的条目正是本次清出来的漏搬条目里的四条。所以生产判据一律走
//   `SL-(\d+(?:\/\d+)*)` 再按 `/` 展开;`--self-test` 拿 `d8ef5b9` 的**真标题**当夹具做删除式:
//   换回朴素模式必须**漏掉**那条泄漏(判据没牙就红),展开式必须**抓到**。
//
// ⚠ 边界(照实说,别让人以为它更严):
//   · 它查得出「**合了却没搬**」,查不出「**搬了但内容不对**」。后者只有人逐条比对合并提交
//     才验得出来 —— SL-295 那 24 条就是这么核的(对照表在 PR 描述里)。
//   · 已上线集合取自 **base 分支**(默认 `origin/feature/v1`),**不含当前 PR 自己的提交** ——
//     否则一个 PR 给自己的卡写预写条目,就会被自己判成漏搬。代价是「本 PR 合并时没搬」这件事
//     要等**下一个** PR 的 CI 才照得出来;要更早照出来得在合并那一刻跑,本仓没有那个钩子。
//   · **浅克隆下 fail-closed 退 1**,不静默放行:`git log` 在 `fetch-depth: 1` 的 checkout 上
//     只看得见一条提交 ⇒ 已上线集合近乎为空 ⇒ 门禁永远绿。这正是本仓「SKIP 吞掉判据」那一族
//     的形态,所以宁可红着叫人去加 `fetch-depth: 0`。
//   · 目前**只认 `pending #SL<号>` 这一种形态**。块里还有 `pending #<PR号>` 与 `pending #J<号>`
//     两种写法**不在覆盖面内**,每次运行都会把它们的条数打出来 —— 别把「通过」读成「块里干净」。
//   · 它**不校验搬过去的位置对不对**(小节挑错了它看不见),也不看正文里 `(#TBD)` 这类占位。
//
// 用法:
//     node scripts/check-changelog-drafts.mjs [--help] [--self-test] [--base <ref>]
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const CHANGELOG = path.join(ROOT, "CHANGELOG.md");
const DEFAULT_BASE = "origin/feature/v1";
// 删除式夹具:压缩写法的**真标题**,不手抄(手抄的夹具会跟着真源漂,漂完还一直绿)。
const FIXTURE_SHA = "d8ef5b9";
const FIXTURE_EXPECT = ["204", "205", "207", "211", "213", "214"];

// ---- 卡号抽取:生产用展开式,朴素式只作自测的反例 -------------------------
const EXPANDED_RE = /SL-(\d+(?:\/\d+)*)/g;
const NAIVE_RE = /SL-(\d+)/g;
const cardsExpanded = (t) => {
    const out = [];
    for (const m of t.matchAll(EXPANDED_RE))
        for (const n of m[1].split("/")) out.push(n);
    return out;
};
const cardsNaive = (t) => [...t.matchAll(NAIVE_RE)].map((m) => m[1]);

// ---- 草稿块与 pending 标记 -----------------------------------------------
// 标记有两种收尾:`(pending #SL204)` 与 `(pending #SL191;起跳的柔化随后由 …)`。
// 所以**不要**把右括号写进模式 —— 写进去就会漏掉后一种(它正是本次 24 条里的一条)。
const PENDING_SL_RE = /pending #SL(\d+)/g;
const PENDING_OTHER_RE = /pending #(J\d+|\d+)/g;
const BLOCK_RE = /<!--\s*={5,}[\s\S]*?={5,}\s*-->/g;

function draftBlock(md) {
    const hits = md.match(BLOCK_RE) || [];
    if (hits.length !== 1)
        throw new Error(
            "CHANGELOG.md 里的预写条目注释块命中 " +
                hits.length +
                " 个(应当恰好 1 个)—— 块的首尾标记被改过?判据无从下手,故 fail-closed",
        );
    return hits[0];
}

const pendingSl = (block) =>
    [...block.matchAll(PENDING_SL_RE)].map((m) => m[1]);

// ---- git ------------------------------------------------------------------
function git(args, { allowFail = false } = {}) {
    const r = spawnSync("git", args, {
        cwd: ROOT,
        encoding: "utf8",
        maxBuffer: 64 * 1024 * 1024,
    });
    if (r.error) throw new Error("跑不动 git:" + r.error.message);
    if (r.status !== 0) {
        if (allowFail) return null;
        throw new Error(
            "git " +
                args.join(" ") +
                " 退 " +
                r.status +
                ":" +
                (r.stderr || "").trim(),
        );
    }
    return r.stdout;
}

function shippedTitles(base) {
    if ((git(["rev-parse", "--is-shallow-repository"]) || "").trim() === "true")
        throw new Error(
            "仓库是浅克隆 —— `git log` 看不见历史,已上线集合会近乎为空、门禁会假绿。" +
                "CI 上给这一步的 checkout 加 `fetch-depth: 0`(见 .github/workflows/format.yml 的 docs-truth)",
        );
    if (
        git(["rev-parse", "--verify", "--quiet", base + "^{commit}"], {
            allowFail: true,
        }) === null
    )
        throw new Error(
            "解析不出 base ref `" +
                base +
                "` —— 本地跑请先 `git fetch origin`;CI 上请确认 checkout 拉到了远端分支",
        );
    return git(["log", "--format=%h\t%s", base])
        .split("\n")
        .filter(Boolean)
        .map((l) => {
            const i = l.indexOf("\t");
            return { sha: l.slice(0, i), title: l.slice(i + 1) };
        });
}

// ---- 核心判定(纯函数,自测直接喂它)--------------------------------------
function leaks(block, titles, extract = cardsExpanded) {
    const shipped = new Map(); // 卡号 -> 最新一条带它的提交
    for (const t of titles)
        for (const c of extract(t.title))
            if (!shipped.has(c)) shipped.set(c, t);
    const out = [];
    for (const card of new Set(pendingSl(block))) {
        const hit = shipped.get(card);
        if (hit) out.push({ card, sha: hit.sha, title: hit.title });
    }
    return out.sort((a, b) => Number(a.card) - Number(b.card));
}

// ---- CLI ------------------------------------------------------------------
let selfTest = false;
let base = process.env.SCVB_CHANGELOG_BASE || DEFAULT_BASE;
for (let i = 2; i < process.argv.length; i++) {
    const a = process.argv[i];
    if (a === "--help" || a === "-h") {
        console.log(
            [
                "check-changelog-drafts.mjs —— 预写条目漏搬门禁",
                "  --self-test    自检:用 " +
                    FIXTURE_SHA +
                    " 的真标题做删除式(朴素模式必须漏、展开式必须抓到)",
                "  --base <ref>   已上线集合取自哪个 ref(默认 " +
                    DEFAULT_BASE +
                    ";亦可用环境变量 SCVB_CHANGELOG_BASE)",
                "退出码:0 = 没有漏搬 / 1 = 有漏搬,或判据无从下手(浅克隆、base 解析不出、注释块找不到)",
            ].join("\n"),
        );
        process.exit(0);
    } else if (a === "--self-test") {
        selfTest = true;
    } else if (a === "--base") {
        base = process.argv[++i];
        if (!base) {
            console.error("--base 后面要跟一个 ref");
            process.exit(1);
        }
    } else {
        console.error("未知参数:" + a);
        process.exit(1);
    }
}

// ---- 自测 -----------------------------------------------------------------
if (selfTest) {
    const bad = [];
    const check = (ok, msg) => {
        if (!ok) bad.push(msg);
    };
    let fixtureTitle = null;
    try {
        fixtureTitle = git(["log", "-1", "--format=%s", FIXTURE_SHA]).trim();
    } catch (e) {
        bad.push(
            "取不到夹具提交 " + FIXTURE_SHA + " 的标题(浅克隆?):" + e.message,
        );
    }
    if (fixtureTitle) {
        const exp = cardsExpanded(fixtureTitle);
        const nai = cardsNaive(fixtureTitle);
        for (const c of FIXTURE_EXPECT)
            check(
                exp.includes(c),
                "展开式漏掉了 SL-" + c + " —— 夹具标题:" + fixtureTitle,
            );
        const missed = FIXTURE_EXPECT.filter((c) => !nai.includes(c));
        check(
            missed.length > 0,
            "朴素模式把 " +
                FIXTURE_EXPECT.join("/") +
                " 全抓到了 —— 夹具失去牙齿(标题被改成非压缩写法?),删除式不再证明展开是必需的",
        );

        if (missed.length) {
            // 删除式:同一份真标题 + 一条泄漏,展开式必须报、朴素式必须漏。
            // 标记拼装而不写成整串:本脚本将来若扩成全仓扫描,写死的整串会让它入库即自炸。
            const mark = (n) => "(pend" + "ing #SL" + n + ")";
            const block =
                "<!-- =====\n新增\n- 夹具条目" +
                mark(missed[0]) +
                "\n===== -->";
            const titles = [{ sha: FIXTURE_SHA, title: fixtureTitle }];
            check(
                leaks(block, titles, cardsExpanded).some(
                    (l) => l.card === missed[0],
                ),
                "展开式没抓到 SL-" + missed[0] + " 的泄漏 —— 判据本身失效",
            );
            check(
                leaks(block, titles, cardsNaive).length === 0,
                "把展开换回朴素模式之后**照样抓到**了 —— 删除式没有区分度,证明不了展开是必需的",
            );
            // 上面两条验的是**两个抽取函数**,不验**生产走的是哪一个**。少了这一条,
            // 把 `leaks` 的默认参数改成 `cardsNaive`(正是本卡要防的退化)自测照样绿。
            check(
                leaks(block, titles).some((l) => l.card === missed[0]),
                "生产默认的抽取器抓不到压缩写法里的 SL-" +
                    missed[0] +
                    " —— `leaks` 的默认参数被换成朴素模式了?判据没牙",
            );
            // 未上线的卡不得误报(#83 那种「PR 关掉了、从没进主线」的条目必须留得住)。
            const safe =
                "<!-- =====\n修复\n- 没上线的条目" +
                mark("9999") +
                "\n===== -->";
            check(leaks(safe, titles).length === 0, "对未上线的卡号误报");
            // 分号收尾那种标记必须照样认(漏了它就漏掉 SL-191 那一条)。
            const semi =
                "<!-- =====\n修复\n- 分号收尾(pend" +
                "ing #SL" +
                missed[0] +
                ";随后由别的卡补上)\n===== -->";
            check(
                leaks(semi, titles).length === 1,
                "分号收尾的 pending 标记没被认出来 —— 模式把右括号写死了?",
            );
        }
    }
    // 块首尾被改坏时必须 fail-closed,而不是「找不到块 ⇒ 没有泄漏 ⇒ 绿」。
    let threw = false;
    try {
        draftBlock("# Changelog\n\n没有注释块\n");
    } catch {
        threw = true;
    }
    check(
        threw,
        "注释块缺失时没有 fail-closed —— 块被删掉/首尾标记被改就等于门禁静默消失",
    );

    if (bad.length) {
        console.error(
            "check-changelog-drafts --self-test:失败 " + bad.length + " 项:",
        );
        for (const b of bad) console.error("  [FAIL] " + b);
        process.exit(1);
    }
    console.log(
        "check-changelog-drafts --self-test:全部通过(夹具 = " +
            FIXTURE_SHA +
            " 的真标题)",
    );
    process.exit(0);
}

// ---- 实跑 -----------------------------------------------------------------
try {
    const md = fs.readFileSync(CHANGELOG, "utf8");
    const block = draftBlock(md);
    const titles = shippedTitles(base);
    const found = leaks(block, titles);
    const slCount = new Set(pendingSl(block)).size;
    const otherCount = [...block.matchAll(PENDING_OTHER_RE)].length;

    if (found.length) {
        console.error(
            "check-changelog-drafts 失败(" +
                found.length +
                " 张卡的预写条目漏搬):",
        );
        for (const f of found)
            console.error(
                "  [FAIL] SL-" +
                    f.card +
                    " 已随 " +
                    f.sha +
                    " 上线,预写条目却还留在注释块里 —— " +
                    f.title,
            );
        console.error(
            "  搬法见 CHANGELOG.md 注释块开头那段「搬运规矩」:换成本 PR 号 → 追加到正文同名小节 → 块里整条删掉。",
        );
        process.exit(1);
    }
    console.log(
        "check-changelog-drafts 通过:注释块里没有已上线的 SL 卡预写条目(块里 " +
            slCount +
            " 张 SL 卡待合并;已上线集合取自 " +
            base +
            " 的 " +
            titles.length +
            " 条提交标题)。块里另有 " +
            otherCount +
            " 处 `pending #<PR号>` / `pending #J<号>` 形态**不在本检查覆盖面内**。",
    );
    process.exit(0);
} catch (e) {
    console.error("check-changelog-drafts 失败:" + e.message);
    process.exit(1);
}
