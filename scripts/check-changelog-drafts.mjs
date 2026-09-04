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
//   · **「卡号出现在合并标题里」≠「这张卡上线了」**:`32e52a3` 的标题里那句
//     `SL-189 权威链定谳(接线完好)` 说的是「查完了,接线没问题,不改代码」——
//     SL-189 至今没有任何交付,可它照样进已上线集合。这类假红由注释块里的「门禁放行」
//     解除(必须写理由,每次运行连理由一起打出来)—— 见下面 ALLOW_HEAD 那一段。
//   · 已上线集合取自 **base 分支**,**不含当前 PR 自己的提交** —— 否则一个 PR 给自己的卡写
//     预写条目,就会被自己判成漏搬。代价是「本 PR 合并时没搬」要等**下一次** run 才照得出来
//     (`push` 到 `dev` / `feature/**` 也跑这个 job,所以合并后那一次 push 就会照出来)。
//     base 由 CI 透传 `SCVB_CHANGELOG_BASE=origin/<base_ref 或 ref_name>`;脚本里的默认值
//     `origin/feature/v1` 只作本地兜底 —— 把分支名写死在判据里的话,等 v1 收口、该分支被删,
//     所有 run 会一起硬红在「解析不出 base ref」上(#197 复审【建议】①)。
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
// 写满 40 位而不写缩写:`git log -1 <缩写>` 在缩写歧义时会直接失败,而那条红的原因与被守护的
// 判据毫无关系,排查成本全落在下一个人身上(#197 复审【建议】②)。
const FIXTURE_SHA = "d8ef5b95ffefd7130dc56588603cc72b91ae8b3d";
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

// ---- 放行口(#197 复审【重要】)-------------------------------------------
// 「卡号出现在合并标题里」≠「这张卡上线了」:`32e52a3` 的标题里那句
// `SL-189 权威链定谳(接线完好)` 说的是「查完了不用改」,SL-189 至今没有任何交付,
// 可它照样会被收进已上线集合。等 SL-189 真要落地时,它的 PR 一在块里写预写条目就会当场红,
// 而 CI 那两步是裸调用、改不动判据来源 —— 那是一条**解除不掉的假红**。所以给一个口子,
// 但**必须写理由、且每次运行都把放行连理由打出来**:静默豁免正是本卡自己反对的那类形态。
//
// 写法(在注释块里,`门禁放行` 那一段之下,一张卡一行):
//     - SL-189 —— 只在 32e52a3 的标题里被「定谳(接线完好)」提到,这张卡本身没有交付
// 失效方向都是**红**,不是放行:段落写在空行之后 ⇒ 解析不到 ⇒ 那张卡照样判漏搬;
// 理由留空 ⇒ 判负;放行的卡在块里已经没有预写条目(条目搬走了、放行忘了删)⇒ 判负。
const ALLOW_HEAD = "门禁放行";
const ALLOW_LINE_RE = /^-\s*SL-(\d+)\s*——\s*(.*)$/;

function allowList(block) {
    const lines = block.split("\n");
    const head = lines.findIndex((l) => l.startsWith(ALLOW_HEAD));
    const out = new Map();
    if (head < 0) return out;
    const noReason = [];
    for (let j = head + 1; j < lines.length; j++) {
        if (!lines[j].trim()) break; // 空行收尾
        const m = ALLOW_LINE_RE.exec(lines[j]);
        if (!m) continue; // 说明行 / 占位行
        if (!m[2].trim()) noReason.push(m[1]);
        else out.set(m[1], m[2].trim());
    }
    if (noReason.length)
        throw new Error(
            "「" +
                ALLOW_HEAD +
                "」里这几张卡没写理由:SL-" +
                noReason.join(" / SL-") +
                " —— 放行必须写理由,不写就是静默豁免",
        );
    return out;
}

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
    const allow = allowList(block);
    const pending = new Set(pendingSl(block));
    const dead = [...allow.keys()].filter((c) => !pending.has(c));
    if (dead.length)
        throw new Error(
            "「" +
                ALLOW_HEAD +
                "」里这几张卡在块里已经没有预写条目:SL-" +
                dead.join(" / SL-") +
                " —— 条目搬走了就把放行一起删掉,别留着一个没人用的豁免口",
        );
    const shipped = new Map(); // 卡号 -> 最新一条带它的提交
    for (const t of titles)
        for (const c of extract(t.title))
            if (!shipped.has(c)) shipped.set(c, t);
    const out = [];
    for (const card of pending) {
        if (allow.has(card)) continue;
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

            // ---- 放行口(#197 复审【重要】)------------------------------
            // 反例取夹具标题里**不属于交付集**的那个号(`SL-206 定谳报统筹` 的 206):
            // 这里验的是「标题提到 ⇒ 照样进已上线集合」这个机制,与 206 后来有没有真上线无关
            // (它后来确实随 `ae43d06` / #151 上线了,所以正文里的例子改用至今没交付的 SL-189)。
            const falsePositive = cardsExpanded(fixtureTitle).find(
                (c) => !FIXTURE_EXPECT.includes(c),
            );
            check(
                Boolean(falsePositive),
                "夹具标题里已经没有「提到但没上线」的卡号了 —— 放行口那一组用例失去了真反例",
            );
            if (falsePositive) {
                const fpBlock = (allowTail) =>
                    "<!-- =====\n修复\n- 条目" +
                    mark(falsePositive) +
                    "\n\n" +
                    ALLOW_HEAD +
                    "(卡号 —— 理由)\n" +
                    allowTail +
                    "\n===== -->";
                // ① 没有放行 ⇒ 照样判漏搬(这条先坐实假红真的存在,否则下面两条无从谈起)。
                check(
                    leaks(fpBlock("- (无)"), titles).length === 1,
                    "没写放行时 SL-" +
                        falsePositive +
                        " 没被判成漏搬 —— 那这一组用例守的不是真反例",
                );
                // ② 写了理由 ⇒ 放行。
                check(
                    leaks(
                        fpBlock("- SL-" + falsePositive + " —— 标题里提到而已"),
                        titles,
                    ).length === 0,
                    "写了理由的放行没生效 —— 假红解除不掉",
                );
                // ③ 理由留空 ⇒ 判负(不能变成「写个横杠就豁免」)。
                let emptyThrew = false;
                try {
                    leaks(fpBlock("- SL-" + falsePositive + " —— "), titles);
                } catch {
                    emptyThrew = true;
                }
                check(emptyThrew, "理由留空的放行被放过了 —— 静默豁免口");
                // ④ 放行的卡在块里已经没有预写条目 ⇒ 判负(别留没人用的豁免)。
                let deadThrew = false;
                try {
                    leaks(
                        "<!-- =====\n" +
                            ALLOW_HEAD +
                            "(卡号 —— 理由)\n- SL-" +
                            falsePositive +
                            " —— 条目早搬走了\n===== -->",
                        titles,
                    );
                } catch {
                    deadThrew = true;
                }
                check(
                    deadThrew,
                    "没有对应预写条目的放行被留着了 —— 豁免口会烂在这里",
                );
            }
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

    // 放行**永远打出来**(红绿都打):豁免不显形就等于没有豁免纪律。
    for (const [card, why] of allowList(block))
        console.log("  [ALLOW] SL-" + card + " 放行 —— " + why);

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
