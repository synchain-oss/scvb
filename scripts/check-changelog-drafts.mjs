// SPDX-License-Identifier: GPL-3.0-or-later
// check-changelog-drafts.mjs —— 「合了就得把预写条目搬进正文」的机检(零依赖,Node >= 18,ESM)。
//
// [SL-295] 病灶:`CHANGELOG.md` 末尾那个 HTML 注释块放的是**待合并 PR 的预写条目**,规矩是
//   「各 PR 合并时把自己那条搬到上面对应小节」。这条规矩**只写在注释里,没有任何执行者** ——
//   于是它就是不被执行:清点时块里 **37 条**预写条目对应的东西**早已合进 `feature/v1`**
//   (最早的一条随 `11fb039` / #107 上线),而正文里一个字都没有。没有任何门禁看得见:
//   注释块渲染不出来,发版清单第 1 步「每条带 PR 号」查的是**正文**,漏搬的条目不在它眼里。
//
// 判据:注释块里任何 `pending #<号>`,只要那个号出现在 **base 分支的提交标题**里,就是漏搬。
//   号有三种形态,各有各的抽法(见 FORMS):
//     · `#SL204` —— 卡号,标题里写作 `SL-204`;
//     · `#83`    —— PR 号,标题里写作**尾部**的 `(#83)`(或老式 `Merge pull request #83 from`);
//     · `#J87`   —— 裁定号,标题里写作 `J87`。
//
// ⚠ 卡号必须按**压缩写法展开**再比。本仓的合并标题会把同批卡压成一条,例如 `d8ef5b9`:
//     fix(ui): [R3] SL-204/205/207 + 增补 SL-211/213/214 + J88 改名;… (#123)
//   朴素模式 `SL-(\d+)` 只吃得到每组的**第一个**号(204 与 211),205/207/213/214 全漏 ——
//   而这四张卡的条目正是本次清出来的漏搬条目里的四条。所以生产判据一律走
//   `SL-(\d+(?:\/\d+)*)` 再按 `/` 展开;`--self-test` 拿 `d8ef5b9` 的**真标题**当夹具做删除式:
//   换回朴素模式必须**漏掉**那条泄漏(判据没牙就红),展开式必须**抓到**。
//
// ⚠ PR 号只认**落地位**,不认标题里的引用。`0077606` 的标题是「补 #124 评审的 5 处… (#131)」——
//   `#124` 是**引用**,`(#131)` 才是这条提交的落地 PR。若按裸 `#\d+` 抓,任何一句「见 #83」
//   都会把一个从未合并的 PR 判成已上线。所以只认尾部 `(#N)` 与老式 `Merge pull request #N from`。
//
// ⚠ 边界(照实说,别让人以为它更严):
//   · 它查得出「**合了却没搬**」,查不出「**搬了但内容不对**」。后者只有人逐条比对合并提交
//     才验得出来 —— SL-295 那 37 条就是这么核的(对照表在 PR 描述里)。
//   · **「号出现在合并标题里」≠「它上线了」**:`32e52a3` 的标题里那句
//     `SL-189 权威链定谳(接线完好)` 说的是「查完了不用改」—— SL-189 至今没有任何交付,
//     可它照样进已上线集合。这类假红由注释块里的「门禁放行」解除(必须写理由,每次运行连
//     理由一起打出来)—— 见下面 ALLOW_HEAD 那一段。
//   · 已上线集合取自 **base 分支**,**不含当前 PR 自己的提交** —— 否则一个 PR 给自己的号写
//     预写条目,就会被自己判成漏搬。代价是「本 PR 合并时没搬」要等**下一次** run 才照得出来
//     (`push` 到 `dev` / `feature/**` 也跑这个 job,所以合并后那一次 push 就会照出来)。
//     base 由 CI 透传 `SCVB_CHANGELOG_BASE=origin/<base_ref 或 ref_name>`;脚本里的默认值
//     `origin/feature/v1` 只作本地兜底 —— 把分支名写死在判据里的话,等 v1 收口、该分支被删,
//     所有 run 会一起硬红在「解析不出 base ref」上(#197 复审【建议】①)。
//   · **本地跑时 base 可能是陈旧的** remote-tracking ref(谁也不保证跑 gates 之前 fetch 过),
//     陈旧 ⇒ 已上线集合变小 ⇒ **静默变绿**。在 gates 里联网去挡不合适,所以改为**显形**:
//     **红绿都打**一行 `[BASE] <base>@<sha> (<date>) —— N 条提交标题(落地位 K 个,最大 #M);
//     块里 T 个待合并的号`。[SL-319] 起中间那段是**落地位基线**:下面那道 fail-closed 只挡
//     「集合为空」,挡不住「集合被缩小」,而缩小的失效形态一样(maxLanded 变小 ⇒ 正文几乎
//     每个号都走 WARN)。基线绿时也报,下次不对劲才有对照
//     (#197 复审 gates【建议】①、第 6/7 轮【建议】)。判红时它回答「这次拿哪天的 base 比的」,
//     绿时它回答「块里是不是近乎空转」。它与 [ALLOW] 都排在 `leaks()` **之前** —— leaks 会为
//     放行写法不对 / 理由留空 / 放行无对应条目三种情形抛错,排在它后面就不是「红绿都打」了。
//     **挂方括号标记而不是挂散文**:gates 3i 按标记回显它,挂在成功消息的措辞上的话,一次
//     很自然的文案编辑就会让那条回显静默失效(#197 复审第 5 轮【建议】)。
//   · **浅克隆下 fail-closed 退 1**,不静默放行:`git log` 在 `fetch-depth: 1` 的 checkout 上
//     只看得见一条提交 ⇒ 已上线集合近乎为空 ⇒ 门禁永远绿。这正是本仓「SKIP 吞掉判据」那一族
//     的形态,所以宁可红着叫人去加 `fetch-depth: 0`。
//   · PR 号这一支**依赖合并方式**:squash(尾部 `(#N)`)与 merge commit 认得出,
//     **Rebase and merge 认不出** —— 那种合并既不产生 merge commit、也不往 subject 里加
//     `(#N)`,那个号从此不出现在主线任何一条标题里,对应条目会**永远判绿**(不是假红,
//     是这道门禁最反对的那种「永远绿」)。卡号 / 裁定号两支不受影响 —— 它们写在标题正文里,
//     与合并方式无关。这条不改判据(该禁掉的是 rebase 合并,不是脚本能管的事)。
//   · 它**不校验搬过去的位置对不对**(小节挑错了它看不见)。
//   · [SL-316] 起**正文也在判定面内**:每个 `(#N)` 必须在主线落过地、`(#TBD)` 即红 ——
//     与上面那半共用同一条 `landedPrs`(取号见 BODY_PAREN_RE 那一段)。它判不出「这个号是不是
//     **这条**改动的落地位」,那仍要人逐条比对。
//     判定面覆盖**整份文件**(块被原地抹白,不是切掉)—— 块之后的小节也在内。
//   · 正文侧**只判括号里的号**:裸的「见 #83」不进判定面(躲开「#1770 标准」这类非 PR 引用);
//     而 `#TBD` 则**不要求括号** —— 两条口径不对称,是有意的,
//     别从 `#TBD` 那条推断裸号也在判定面内。
//   · 正文侧的假红走注释块里的「正文放行」(与门禁放行同一套解析、同样理由必填)。
//   · **[SL-319] 已知的洞:大于「当前最大落地号」的号只 WARN,不判红。** 于是一个**打错的
//     未来号**(把 `(#166)` 敲成 `(#1660)`)会被放过,而 `#106 < #111` 那种关掉没合的号
//     照旧判红 —— 牙齿只在 ≤ max 那一侧。
//     **保留 WARN 而不改判红,理由是这一档没有可靠的判据**:合并前按规矩③ 写自己的号、
//     以及规矩③ 明确要求写出来的同批交叉引用(「随后由 #123 补上」),在 CI 与本地都
//     **还没落地**,判红会让「按规矩写」变成必红。想收紧只有两条路,都更坏:
//       · 卡一个「离 max 多远算离谱」的窗口 ⇒ 又一个拍脑袋的常数,而本仓的判例是别写死数;
//       · 只在没有 selfPr 时才 WARN ⇒ 「本地绿、推上去才红」的分叉,format.yml 那步明禁。
//     所以这一档**明写成边界而不是假装守住了**:真要挡打错的号,靠的是 PR 里人眼看那行
//     `[WARN]`(CI 上还带 `file=,line=` 锚到 CHANGELOG 那一行),不是靠这道门禁。
//   · 块尾「尚未开 PR 的在途卡」那几行没有 `pending #…` 标记,**不在判定面内**。
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
// 「从未合并」的真样本:PR #83 已 CLOSED,主线没有任何 subject 带 `(#83)`。它必须恒绿。
const FIXTURE_UNMERGED_PR = "83";

// ---- 三种号的抽取 ---------------------------------------------------------
const cardsExpanded = (t) => {
    const out = [];
    for (const m of t.matchAll(/SL-(\d+(?:\/\d+)*)/g))
        for (const n of m[1].split("/")) out.push(n);
    return out;
};
// 朴素式,只作自测的反例:它吃不到压缩写法里除第一个以外的号。
const cardsNaive = (t) => [...t.matchAll(/SL-(\d+)/g)].map((m) => m[1]);
// PR 号只认落地位:尾部 `(#N)`(squash 合并)或老式 `Merge pull request #N from`。
const prsLanded = (t) => {
    const out = [];
    const tail = /\(#(\d+)\)\s*$/.exec(t);
    if (tail) out.push(tail[1]);
    const merge = /^Merge pull request #(\d+) from/.exec(t);
    if (merge) out.push(merge[1]);
    return out;
};
// 裸 `#N`,只作自测的反例:它会把标题里的**引用**也当成落地位。
const prsAnywhere = (t) => [...t.matchAll(/#(\d+)/g)].map((m) => m[1]);
const rulings = (t) => [...t.matchAll(/J(\d+)/g)].map((m) => m[1]);

const FORMS = {
    SL: { label: "卡号", extract: cardsExpanded },
    PR: { label: "PR 号", extract: prsLanded },
    J: { label: "裁定号", extract: rulings },
};
// `pending #SL204` / `pending #83` / `pending #J87` —— SL/J 两支必须排在纯数字之前。
const PENDING_RE = /pending #(SL\d+|J\d+|\d+)/g;
function classify(token) {
    if (token.startsWith("SL")) return { form: "SL", num: token.slice(2) };
    if (token.startsWith("J")) return { form: "J", num: token.slice(1) };
    return { form: "PR", num: token };
}

// ---- 草稿块 ---------------------------------------------------------------
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

// 标记有两种收尾:`(pending #SL204)` 与 `(pending #SL191;起跳的柔化随后由 …)`。
// 所以**不要**把右括号写进模式 —— 写进去就会漏掉后一种(它正是本次 37 条里的一条)。
const pendingTokens = (block) =>
    [...block.matchAll(PENDING_RE)].map((m) => m[1]);

// ---- 正文侧([SL-316])----------------------------------------------------
// 上面那半守的是「合了却没搬」;这半守的是**搬上来之后号对不对**。两个方向同一个真源
// (base 分支的落地位集合),所以复用同一条 `prsLanded`,不另写第二份引擎。
//
// 病灶(SL-316 清点):正文里 **4 条**挂着 `(#TBD)` —— 发版清单第 1 步「每条带 PR 号」
// 当场卡住,而它们其实都早已落地(3 条随 `e83d138`/#164、1 条随 `b91cc11`/#166);
// 另 **2 条**写着 `(#106)`,而 **PR #106 从未合并** —— 那件事实际落在 `6af6653`/#111
// (它的标题就写着「接替 #106」)。也就是本卡这一族的**反方向**:块里那半是「合了没搬」,
// 正文这半是「搬了但号是错的」,而此前**只有人眼在看**。
//
// ⚠ 边界:它只判「这个号在主线落过地」,判不出「这个号是不是**这条**改动的落地位」——
// 后者仍要人逐条比对(SL-316 那 6 条就是这么核的:先找哪次提交把这条**加进 CHANGELOG**,
// 再回去核提交内容对不对)。
// `#TBD` **不要求带括号**:第一版写的是逐字 `(#TBD)`,于是 `(#TBD;等合并后补)` 或
// 裸的 `— #TBD` 就是零命中 —— 绕过成本一个分号(#203 复审【重要】)。
const BODY_TBD_RE = /#TBD/g;
// 正文里的 PR 引用:**凡是括号里出现 `#\d+` 就算一个引用**。
// 第一版要求号串之后紧接 `)`,于是这五种库里现成的写法整组静默逃逸:
//   `(认领 / 接管 / 心跳 / 代际,#38)`、`(逐声道残差…,#4)`、`(段陈旧保活…,#66)`、
//   `(#116;起跳的柔化随后由 #123 补上,见「变更」)`、`([SL-239] v5.6.2 实测,#146 之后)`
// —— 「只扫我以为它会出现的形态」又一次。实测:放宽后收到 96 个不同号,**零误报**
// (全部在落地位集合里),而放宽前有 5 个括号里的号根本没进判定面。
// [SL-326] **正文侧号形态的唯一真源**。此前它有两份写法:`bodyPrRefs` 里的 `#(\d+)`
// 与正文放行收窄用的 `^\d+$` —— 正是本卡 §3 从 `maxLanded` 那里拿掉的形态,而我一边治它
// 一边在旁边又造了一个。两侧都从这里派生之后,「放宽了取号却忘了放宽收窄 ⇒ 放行静默拒」
// 这条路结构上不存在,不必再靠一格夹具替它站岗(#210 复审第 1/2 轮【建议】)。
const BODY_REF_NUM_RE = /\d+/;
// [SL-327] 粗筛也从 `BODY_REF_NUM_RE` 派生。它是取号的**前置粗筛**(先挑出括号组,再在组内
// 取号),所以它自己也是一份号形态 —— 真源放宽而它没跟上的话,`(见 #SL189)` 这样的括号
// **根本不进候选**,组内那一轮再宽也没机会跑 ⇒ 引用静默不进判定面。它比收窄那处更钝:
// 不报错、不判红,只是少收几个号(#210 复审第 3 轮【建议】)。
// 非捕获分组不能省:真源一旦带 `|`,`#\d+|SL\d+` 会在这里错位结合。
// 两处构造都走 helper,且都带**默认参数** —— 自测因此能喂一个「放宽了的」真源,
// 把「真源带 `|` 时分组对不对」当场量出来,而不必真去改那个常量(#210 第 3 轮【建议】)。
const bodyRefAnchored = (src = BODY_REF_NUM_RE.source) =>
    new RegExp("^(?:" + src + ")$");
const bodyParenRe = (src = BODY_REF_NUM_RE.source) =>
    new RegExp("\\([^)]*#(?:" + src + ")[^)]*\\)", "g");
const BODY_PAREN_RE = bodyParenRe();

function bodyPrRefs(body) {
    const out = [];
    for (const g of body.matchAll(BODY_PAREN_RE))
        for (const m of g[0].matchAll(
            new RegExp("#(" + BODY_REF_NUM_RE.source + ")", "g"),
        ))
            out.push({ num: m[1], at: g.index });
    return out;
}

// 报错要能一眼走到现场:正文里有 96 个号,只说「引用了 (#106)」而不说哪一行,
// 同一个错号出现两次就会打出两条逐字相同的 [FAIL](#203 复审【建议】)。
const lineOf = (text, at) => text.slice(0, at).split("\n").length;

// 落地位集合:与 `leaks()` 内部的 `shipped.PR` 是同一件东西,抽出来给两边用,
// 少一处「将来只改一边」的机会(#203 复审【建议】)。
const landedPrs = (titles) => {
    const s = new Set();
    for (const t of titles) for (const n of FORMS.PR.extract(t.title)) s.add(n);
    return s;
};

// [SL-326] 「最大落地号」的推导**只此一份**。此前 `judgeBody` 与 `[BASE]` 各写了一遍同一个
// 式子:集合是共享的、推导不是,哪天判据那半改了口径(排除某类号、或把 0 兜底改掉),
// `[BASE]` 会继续按老式子报一个**判据并不使用**的数 —— 而那一行的全部价值就是「给人一个
// 可对照的基线」,基线与判据不同源比没有基线更坏:它会让人确信自己核过了(#208 第 4 轮
// 【建议】)。这与本卡从 `allowSampleFor` 那里拿掉的是同一个形态,只是从「样例」挪到「基线」。
const maxLandedOf = (landed) => Math.max(0, ...[...landed].map(Number));

// ---- 正文判定层(纯函数,[SL-319])------------------------------------------
// 抽成纯函数的理由是**可测**,不是好看:在此之前这一层直接长在 run() 里,输入要靠
// 真 CHANGELOG + 真 git 历史凑,于是「今天量不到的退化」就没有夹具 —— #203 复审注入
// 四种退化(取号退回严格括号组 / `#TBD` 退回要求带括号 / 放行清单不生效 / 落地位集合空),
// **自测与实跑全绿**。收成入参之后,前三种退化在自测里各有一格。
// ⚠ **第四种(落地位集合空)不是被这次抽取夹住的;恰恰相反,「收成入参」正是造出那个
// 盲区的原因** —— 本函数每一格都自带 `landed`,压根问不到「landed 是不是真抽出来了」。
// 夹它的是自测里钉 `landedPrs` 的第 ⑧ 格,以及 run() 里那道「集合为空即 fail-closed」。
// 别顺着「输入全是普通值」推出「这一族退化都在本函数的格子里」—— 上一轮复审踩的正是这个。
//
// 输入:body(把注释块抹白之后的正文)、landed(落地位号集合)、selfPr(本 PR 号,可为空)、
//       bodyAllow(正文放行清单 Map<号, 理由>)
// 输出:{ fails: string[], warns: [{ line, text }] } —— **只算不打**:`console.log` /
//       `::warning::` / 退出码都留在 run() 里。否则自测要去截 stdout 才能断言,
//       那又是一层「量不到」。
function judgeBody(body, landed, selfPr, bodyAllow) {
    const fails = [];
    const warns = [];
    const maxLanded = maxLandedOf(landed);
    for (const m of body.matchAll(BODY_TBD_RE))
        fails.push(
            "CHANGELOG.md:" +
                lineOf(body, m.index) +
                " 还挂着 `#TBD` —— 发版清单第 1 步「每条带 PR 号」会当场卡住;" +
                "回溯到把这条**加进 CHANGELOG** 的那次提交、拿它的落地位补号" +
                "(查不到就删条目或标「未落地」)",
        );
    for (const r of bodyPrRefs(body)) {
        if (landed.has(r.num) || r.num === selfPr || bodyAllow.has(r.num))
            continue;
        const line = lineOf(body, r.at);
        const where = "CHANGELOG.md:" + line;
        // 大于最大落地号 ⇒ 只 WARN。两边同判(不看 selfPr 在不在):同批交叉引用
        // (规矩③ 要求写出来的「随后由 #123 补上」)在 CI 与本地都还没落地,只在没有
        // selfPr 时才 WARN 就成了「本地绿、推上去才红」——正是 format.yml 那步明禁的分叉。
        if (Number(r.num) > maxLanded) {
            warns.push({
                line,
                text:
                    where +
                    " 的 `#" +
                    r.num +
                    "` 还没落地,但它大于当前最大落地号 #" +
                    maxLanded +
                    " —— 按「多半是尚未合并的号(本 PR 自己的 / 同批交叉引用的)」放过;" +
                    "这一档 CI 与本地同判,都不判红(这个洞与它为什么留着,见头注 §边界)",
            });
            continue;
        }
        fails.push(
            where +
                " 引用了 `#" +
                r.num +
                "`,但主线没有任何提交以它落地(既不是尾部 `(#" +
                r.num +
                ")`,也不是 `Merge pull request #" +
                r.num +
                " from`)—— 多半是引了一个**关掉没合**的 PR;" +
                "去找真正落地的那个号(例:#106 从未合并,那件事落在 `6af6653` / #111,标题写着「接替 #106」);" +
                "确认这个号是对的、机检判错了,就去 CHANGELOG 注释块的「" +
                BODY_ALLOW_HEAD +
                "」段写一行 `" +
                allowSampleFor(r.num) +
                "` 放行,别改条目躲开",
        );
    }
    // [SL-319] 过期的正文放行 ⇒ 判负,与 `leaks()` 里 `dead` 那段**同一条纪律**:
    // 别留着一个没人用的豁免口。两种过期形态都收:那个号在正文里已经没有引用了(条目被删),
    // 或者它后来真落地了(放行从此多余,却还在替一个不需要豁免的号挡着)。
    // 门禁那半一直有这一环,正文这半此前没有 —— 两边不对称,而失效方向正是本文件反复在防的
    // 「静默豁免」(复审【建议】;趁清单还空的时候补最便宜)。
    const refNums = new Set(bodyPrRefs(body).map((r) => r.num));
    const deadBody = [...bodyAllow.keys()].filter(
        (n) => !refNums.has(n) || landed.has(n),
    );
    if (deadBody.length)
        fails.push(
            "「" +
                BODY_ALLOW_HEAD +
                "」里这几个号已经不需要放行了:#" +
                deadBody.join(" / #") +
                " —— 要么正文里已经没有引用它(条目改了或删了),要么它后来真落地了;" +
                "把这几行删掉,别留着没人用的豁免口",
        );
    return { fails, warns };
}

// ---- 放行口(#197 复审【重要】)-------------------------------------------
// 「号出现在合并标题里」≠「它上线了」:`32e52a3` 的标题里那句 `SL-189 权威链定谳(接线完好)`
// 说的是「查完了不用改」,SL-189 至今没有任何交付,可它照样会被收进已上线集合。等它真要落地
// 时,它的 PR 一在块里写预写条目就会当场红,而 CI 那两步是裸调用、改不动判据来源 —— 那是一条
// **解除不掉的假红**。所以给一个口子,但**必须写理由、且每次运行都把放行连理由打出来**:
// 静默豁免正是本卡自己反对的那类形态。
//
// 写法(在注释块里 `门禁放行` 那一段之下,一个号一行;号写成与 `pending #…` 里一模一样):
//     - #SL189 —— 只在 32e52a3 的标题里被「定谳(接线完好)」提到,这张卡本身没有交付
// 失效方向都是**红**,不是放行:段落写在空行之后 ⇒ 解析不到 ⇒ 那个号照样判漏搬;理由留空
// ⇒ 判负;写法不对(破折号写错 / 照抄标题形态 `- SL-189 ——` / 多个连字符 `- #SL-189 ——`)
// ⇒ 判负并给出样例行,不再静默当说明行忽略;
// 放行的号在块里已经没有预写条目(条目搬走了、放行忘了删)⇒ 判负。
// 成功消息提成常量,好让 `--self-test` 能断它 —— **本文件的散文里提到 ALLOW / BASE / WARN
// 这三个信号时一律不带方括号**([SL-318] 起是三个;此前只说 ALLOW / BASE,而本文件的正文判定
// 那半也会打 WARN 行):gates 3i 对成功输出**按标记回显 ALLOW 与 BASE、按行首标记数 WARN 与
// ALLOW**(口径在 gates.ps1 gate 3i 开头一处定义,别在这里抄它的字面量 —— 抄下来就会漂),
// 带了括号这句成功消息就会被当成一条真信号 —— 回显多一行,落到计数那一档则**0 条也数出 1**。
// 这条不变量已经踩过两次(#197 第 5 轮的 BASE、`379de23` 的 ALLOW),两次都只有散文守着;
// 现在有两个执行者:本文件 `--self-test` 里那条断言(#201 复审【重要】),以及
// `check-gates-visibility` 对 gate 3i 这一圈脚本的跨文件守卫([SL-315] 立、[SL-318] 扩到三标记)。
const OK_MSG =
    "check-changelog-drafts 通过:判定面内没有漏搬的预写条目" +
    "(块里有几个号见上面那行 BASE 戳,放行的号见 ALLOW 行;" +
    "SL 卡号 / PR 号 / J 裁定号三种形态全在判定面内)。";

const ALLOW_HEAD = "门禁放行";
const ALLOW_LINE_RE = /^-\s*#(SL\d+|J\d+|\d+)\s*——\s*(.*)$/;
// like-模式要比 ALLOW_LINE_RE **宽**:它的活儿是「看着像放行行就别静默忽略」。
// 只认 `#SL189` 那一种形态的话,`- SL-189 —— 理由`(照抄上面说明文字里的标题形态)与
// `- #SL-189 —— 理由`(手滑多个连字符)两种都会掉回静默忽略 —— 与它要堵的洞同构
// (#197 复审第 3 轮【建议】)。扫描面已被限死在「门禁放行 标题行 → 下一个空行」之间,
// 放宽不会误伤正文条目。
const ALLOW_LOOKS_LIKE_RE = /^-\s*#?\s*(?:SL-?\d+|J-?\d+|\d+)/;
const ALLOW_SAMPLE =
    "- #SL189 —— 只在 32e52a3 的标题里被顺带提到,这张卡本身没有交付";
// [SL-319] 放行行的形态**只此一份**:判红话术里那句「照着写一行」原先是手拼的第二份真源
// (逐字复刻了 `ALLOW_LINE_RE` 要求的那个「 —— 」)。破折号形态哪天改了,门禁那半跟着
// `ALLOW_SAMPLE` 走、正文这半会继续教人写旧写法 —— 而正文这条话术恰恰是给「第一次找放行口」
// 的人看的,教错的代价更大(复审【建议】)。自测里有一格断言它自己过得了 `ALLOW_LINE_RE`。
const ALLOW_REASON_PLACEHOLDER = "<理由>";
const allowSampleFor = (num) => "- #" + num + " —— " + ALLOW_REASON_PLACEHOLDER;

// [SL-316] 正文侧也要一个放行口:块那半有「门禁放行」,正文那半此前一个都没有,于是两种
// **改不动的红**没有出路 —— ① 那个号永远不会落地(rebase 合并不产生 `(#N)`,或维护者手改掉了
// squash 标题里的号);② 历史条目引的号来自别的仓 / 别的时期。而唯一的「出路」会变成改数据
// 迁就门禁、甚至删条目 —— 那正是本脚本自己明禁的动作(规矩⑤)。
// 复用**同一套**解析:同一个函数、同一种行形态、同样理由必填、同样每次运行打出来。
const BODY_ALLOW_HEAD = "正文放行";

function allowList(block, headText = ALLOW_HEAD) {
    const lines = block.split("\n");
    const head = lines.findIndex((l) => l.startsWith(headText));
    const out = new Map();
    if (head < 0) return out;
    const noReason = [];
    const malformed = [];
    const wrongForm = [];
    for (let j = head + 1; j < lines.length; j++) {
        if (!lines[j].trim()) break; // 空行收尾
        const m = ALLOW_LINE_RE.exec(lines[j]);
        if (!m) {
            // 长得像放行行却不合模式 ⇒ 判负。静默当说明行忽略的话,写的人只会看到
            // 「我明明写了放行,还是红」,而输出里既没说放行没生效、也没给正确写法。
            if (ALLOW_LOOKS_LIKE_RE.test(lines[j]))
                malformed.push(lines[j].trim());
            continue;
        }
        // [SL-326] **正文放行只吃纯数字 PR 号**。`ALLOW_LINE_RE` 是两段共用的,也收
        // `SL<号>` / `J<号>`;而正文侧的引用只可能是纯数字(`bodyPrRefs` 出自 `/#(\d+)/`),
        // 于是在这一段写 `- #SL189 —— 真理由` 会**静默不生效**,而且紧接着撞 `deadBody`
        // 判负、话术还说成「正文里没有引用它 / 它已经落地」——**两条都不是真因**。
        // 这条路比「照抄报错样例」更好走:CHANGELOG 里「正文放行」自己写着「写法与门禁放行
        // 相同」,而上面那段的说明与样例全是 `#SL189`,隔几行照抄最自然(#208 第 4 轮【建议】)。
        // 所以在这里**当场判负并说清真因**,不留到下游变成一句指错方向的红。
        if (headText === BODY_ALLOW_HEAD && !bodyRefAnchored().test(m[1])) {
            wrongForm.push(m[1]);
            continue;
        }
        // [SL-319] 占位符 `<理由>` **不算理由**:它 `.trim()` 非空,于是照抄样例行
        // 粘上去就是一条合法放行 —— 写的人不必编理由,而「不写理由就是静默豁免」
        // 正是这道口子要堵的。样例行的受众就是照抄的人,样例本身不能成为绕过。
        if (!m[2].trim() || m[2].trim() === ALLOW_REASON_PLACEHOLDER)
            noReason.push(m[1]);
        else out.set(m[1], m[2].trim());
    }
    // [SL-327] 三类问题**一次报完**,不再先到先抛。与本文件 `bodyBad` / `leaks()` 那处
    // 同一条纪律(那段注释写着「在这里直接 exit 会…让人跑两轮」):一段里同时写了形态不对的号
    // 和写坏的破折号时,先到先抛会让作者改完一种、重跑才第一次看见下一种,连撞几次红。
    // 样例只在末尾给一次,并按**段**选(正文段给纯数字形态,块那半给 SL 形态)。
    const problems = [];
    if (wrongForm.length)
        problems.push(
            "· 这几个号不是**纯数字 PR 号**:#" +
                wrongForm.join(" / #") +
                " —— SL 卡号 / J 裁定号是**块里那半**(「" +
                ALLOW_HEAD +
                "」)的形态;正文里的引用只可能是 PR 号。写在这里放行不掉;" +
                "这道拦截若不在,它还会接着被当成「已经不需要的放行」判负",
        );
    if (malformed.length)
        problems.push(
            "· 这几行像放行行、写法却不对(一行要写成 `- #<号> —— <理由>`,号与 `pending #…` 里同形):\n      " +
                malformed.join("\n      "),
        );
    if (noReason.length)
        problems.push(
            "· 这几个号没写理由:#" +
                noReason.join(" / #") +
                " —— 放行必须写理由,不写就是静默豁免;样例里的占位符 `" +
                ALLOW_REASON_PLACEHOLDER +
                "` **不算理由**,要换成真的",
        );
    if (problems.length)
        throw new Error(
            "「" +
                headText +
                "」有 " +
                problems.length +
                " 类问题:\n    " +
                problems.join("\n    ") +
                "\n  正确写法:" +
                // 样例按**段**选:正文放行下给 SL 形态的话,照抄的人会拿到 `#SL189` ——
                // `ALLOW_LINE_RE` 收得下,但正文侧只认纯数字,于是他会**再撞一次红**
                // (而且是上面第一类),第二次红还指错方向([SL-319] / [SL-326] 判例)。
                (headText === BODY_ALLOW_HEAD
                    ? allowSampleFor("111")
                    : ALLOW_SAMPLE),
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

// base 的 tip 是哪一天的 —— 陈旧的 remote-tracking ref 会让门禁静默变绿,只能靠显形。
const baseStamp = (b) => git(["log", "-1", "--format=%h (%cs)", b]).trim();

// ---- 核心判定(纯函数,自测直接喂它)--------------------------------------
function leaks(block, titles, forms = FORMS) {
    const allow = allowList(block);
    const tokens = new Set(pendingTokens(block));
    const dead = [...allow.keys()].filter((t) => !tokens.has(t));
    if (dead.length)
        throw new Error(
            "「" +
                ALLOW_HEAD +
                "」里这几个号在块里已经没有预写条目:#" +
                dead.join(" / #") +
                " —— 条目搬走了就把放行一起删掉,别留着一个没人用的豁免口",
        );
    const shipped = { SL: new Map(), PR: new Map(), J: new Map() };
    for (const t of titles)
        for (const form of Object.keys(shipped))
            for (const n of forms[form].extract(t.title))
                if (!shipped[form].has(n)) shipped[form].set(n, t);
    const out = [];
    for (const token of tokens) {
        if (allow.has(token)) continue;
        const { form, num } = classify(token);
        const hit = shipped[form].get(num);
        if (hit) out.push({ token, form, sha: hit.sha, title: hit.title });
    }
    return out.sort((a, b) => a.token.localeCompare(b.token));
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
                    FIXTURE_SHA.slice(0, 7) +
                    " 的真标题与真 CHANGELOG 做删除式(见头注)",
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
    const threw = (fn) => {
        try {
            fn();
            return false;
        } catch {
            return true;
        }
    };
    // 标记拼装而不写成整串:本脚本将来若扩成全仓扫描,写死的整串会让它入库即自炸。
    const mark = (tok) => "(pend" + "ing #" + tok + ")";
    const wrap = (body) => "<!-- =====\n" + body + "\n===== -->";

    let fixtureTitle = null;
    try {
        fixtureTitle = git(["log", "-1", "--format=%s", FIXTURE_SHA]).trim();
    } catch (e) {
        bad.push(
            "取不到夹具提交 " + FIXTURE_SHA + " 的标题(浅克隆?):" + e.message,
        );
    }

    if (fixtureTitle) {
        const titles = [{ sha: FIXTURE_SHA.slice(0, 7), title: fixtureTitle }];
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
            const naiveForms = {
                ...FORMS,
                SL: { ...FORMS.SL, extract: cardsNaive },
            };
            const blk = wrap("新增\n- 夹具条目" + mark("SL" + missed[0]));
            // 压缩写法的删除式:展开式必须抓到,朴素模式必须漏。
            // 注意这两条之外还要有**不传 forms** 的一条,否则把 `leaks` 的默认参数换成朴素
            // 模式(正是本卡要防的退化)自测照样绿 —— 验的是函数,不是生产走的是哪一个。
            check(
                leaks(blk, titles).some((l) => l.token === "SL" + missed[0]),
                "生产默认抓不到压缩写法里的 SL-" + missed[0] + " —— 判据没牙",
            );
            check(
                leaks(blk, titles, naiveForms).length === 0,
                "把展开换回朴素模式之后**照样抓到**了 —— 删除式没有区分度",
            );
            // 分号收尾那种标记必须照样认(漏了它就漏掉 SL-191 那一条)。
            check(
                leaks(
                    wrap(
                        "修复\n- 分号收尾(pend" +
                            "ing #SL" +
                            missed[0] +
                            ";随后由别的号补上)",
                    ),
                    titles,
                ).length === 1,
                "分号收尾的 pending 标记没被认出来 —— 模式把右括号写死了?",
            );

            // ---- 放行口:五种失效形态一条不落 ----------------------------
            const fp = exp.find((c) => !FIXTURE_EXPECT.includes(c));
            check(
                Boolean(fp),
                "夹具标题里已经没有「提到但不属于交付集」的卡号了 —— 放行口这一组失去真反例",
            );
            if (fp) {
                const fpBlock = (tail) =>
                    wrap(
                        "修复\n- 条目" +
                            mark("SL" + fp) +
                            "\n\n" +
                            ALLOW_HEAD +
                            "(号 —— 理由)\n" +
                            tail,
                    );
                check(
                    leaks(fpBlock("- (无)"), titles).length === 1,
                    "没写放行时 SL-" +
                        fp +
                        " 没被判成漏搬 —— 这一组守的不是真反例",
                );
                check(
                    leaks(fpBlock("- #SL" + fp + " —— 标题里提到而已"), titles)
                        .length === 0,
                    "写了理由的放行没生效 —— 假红解除不掉",
                );
                check(
                    threw(() => leaks(fpBlock("- #SL" + fp + " —— "), titles)),
                    "理由留空的放行被放过了 —— 静默豁免口",
                );
                // [SL-319] 占位理由**照抄即过**也要判负:样例行的受众就是照抄的人,
                // 样例本身不能成为绕过「理由必填」的路(复审【建议】)。
                check(
                    threw(() =>
                        leaks(
                            fpBlock(
                                "- #SL" +
                                    fp +
                                    " —— " +
                                    ALLOW_REASON_PLACEHOLDER,
                            ),
                            titles,
                        ),
                    ),
                    "占位理由原样粘上去被当成了合法理由 —— 写的人不必编理由,静默豁免口又开了",
                );
                // 三种写错都必须判负(#197 复审第 3 轮:前两版只堵了破折号那一种)。
                // `- SL-189 —— 理由` 是最可能写出来的一种:放行段上面那几行说明文字里,号一律
                // 写成 `SL-189` 这个**标题形态**,照着最近的字样抄就会落到这里。
                for (const wrong of [
                    "- #SL" + fp + " - 破折号写错了",
                    "- SL-" + fp + " —— 照抄了标题形态,没写成 token 形态",
                    "- #SL-" + fp + " —— 手滑多了个连字符",
                ])
                    check(
                        threw(() => leaks(fpBlock(wrong), titles)),
                        "写法不对的放行行被静默忽略了(" +
                            wrong +
                            ")—— 写的人只会看到「写了放行还是红」",
                    );
                check(
                    threw(() =>
                        leaks(
                            wrap(
                                ALLOW_HEAD +
                                    "(号 —— 理由)\n- #SL" +
                                    fp +
                                    " —— 条目早搬走了",
                            ),
                            titles,
                        ),
                    ),
                    "没有对应预写条目的放行被留着了 —— 豁免口会烂在这里",
                );
            }
        }

        // ---- PR 号形态:只认落地位,不认引用 -----------------------------
        const refTitles = [
            {
                sha: "0000000",
                title:
                    "fix(x): 顺带提一句 #" +
                    FIXTURE_UNMERGED_PR +
                    " 的老问题 (#999)",
            },
        ];
        const refBlock = wrap("修复\n- 条目" + mark(FIXTURE_UNMERGED_PR));
        check(
            leaks(refBlock, refTitles).length === 0,
            "标题里对 #" +
                FIXTURE_UNMERGED_PR +
                " 的**引用**被当成了落地位 —— 从未合并的 PR 会被判成已上线",
        );
        check(
            leaks(refBlock, refTitles, {
                ...FORMS,
                PR: { ...FORMS.PR, extract: prsAnywhere },
            }).length === 1,
            "把 PR 抽取换成裸 `#\\d+` 之后**没有**误报 —— 这条删除式没有区分度",
        );
        check(
            leaks(wrap("修复\n- 条目" + mark("999")), refTitles).length === 1,
            "尾部 `(#999)` 这个落地位没被认出来 —— PR 形态判据失效",
        );
        check(
            leaks(wrap("修复\n- 条目" + mark("77")), [
                { sha: "0000000", title: "Merge pull request #77 from x/y" },
            ]).length === 1,
            "老式 `Merge pull request #N from` 落地位没被认出来",
        );

        // ---- 裁定号形态 --------------------------------------------------
        check(
            leaks(wrap("修复\n- 条目" + mark("J88")), titles).length === 1,
            "裁定号 J88 在夹具标题里没被认出来 —— J 形态判据失效",
        );
        check(
            leaks(wrap("修复\n- 条目" + mark("J9999")), titles).length === 0,
            "没出现过的裁定号被误报",
        );
    }

    // ---- 「从未合并」的真样本必须恒绿(统筹裁定③①)----------------------
    // 用**真 CHANGELOG.md + 真提交标题**,不是合成夹具:#83 是这条判据的天然反例。
    try {
        const realBlock = draftBlock(fs.readFileSync(CHANGELOG, "utf8"));
        check(
            pendingTokens(realBlock).includes(FIXTURE_UNMERGED_PR),
            "真 CHANGELOG.md 的注释块里已经没有 `pending #" +
                FIXTURE_UNMERGED_PR +
                "` 了 —— 那是「命中 ≠ 已上线」的天然反例,它一走这条用例就失去牙齿。" +
                "**出路**:若确实要把它搬走/删掉,请另挑一个**从未合并**的 PR 号" +
                "(判据是**主线没有任何 subject 带 `(#N)`**;pwsh:" +
                "`git log --format=%s <base> | Select-String '(#N)' -SimpleMatch` 零命中," +
                "bash:`git log --format=%s <base> | grep -F '(#N)'`。" +
                "**别用 `git log --grep`** —— 它搜的是整条 message,而 #83 就有一条正文提到它的" +
                "提交(`ec53300`,自己的 subject 是 `(#87)`),用 --grep 会得出「已合并」的反结论)" +
                "写进 FIXTURE_UNMERGED_PR,并在注释块里给它留一条预写条目 —— " +
                "**不要直接把这条断言删掉**,那正是它要防的动作",
        );
        check(
            !leaks(realBlock, shippedTitles(base)).some(
                (l) => l.token === FIXTURE_UNMERGED_PR,
            ),
            "从未合并的 PR #" + FIXTURE_UNMERGED_PR + " 被判成漏搬 —— 误报",
        );
    } catch (e) {
        bad.push("真数据那一组跑不起来:" + e.message);
    }

    // [SL-315] 成功消息里不得出现方括号标记 —— 否则 gates 3i 的回显会多一行、
    // 计数会在 0 条时数出 1。这条不变量踩过两次,此前只有散文守着。
    // [SL-318] 标记集补上 WARN:本文件正文判定那半会打 WARN 行,而 gates 3i 把 WARN 数进
    // 汇总标签 —— 落在那一档的散文比 ALLOW 更贵(直接多报一档降级)。三个标记一份判据。
    check(
        !/\[(?:ALLOW|BASE|WARN)\]/.test(OK_MSG),
        "成功消息里出现了方括号标记 —— 会被 gates 3i 的标记回显/计数当成一条真信号" +
            "(#197 第 5 轮在 BASE 上踩过、`379de23` 在 ALLOW 上又踩一次);提到它们时去掉方括号",
    );

    // ---- [SL-316] 正文侧的四条分支要有断言 ------------------------------
    // 本卡合并之后正文里**再没有活体**(4 条 `#TBD` 全补掉了),于是实跑对「取号退回严格组
    // 模式」「`#TBD` 退回要求带括号」这类退化一律照绿 —— 判据会静默失去牙齿而没人看得见
    // (#203 复审【重要】)。夹具**现成、就在库里**,不造合成数据。
    try {
        const realMd = fs.readFileSync(CHANGELOG, "utf8");
        // 两条真夹具:一括号两个号 + 分号;号在括号尾但前面是散文。
        const FIX_SEMI = "(#116;起跳的柔化随后由 #123 补上,见「变更」)";
        const FIX_PROSE = "(认领 / 接管 / 心跳 / 代际,#38)";
        for (const f of [FIX_SEMI, FIX_PROSE])
            check(
                realMd.includes(f),
                "CHANGELOG.md 里已经没有夹具「" +
                    f.slice(0, 18) +
                    "…」了 —— 它一走,下面那条删除式就失去区分度",
            );
        // ★ 删除式:宽模式必须抓到,**第一版的严格组模式必须漏** —— 否则这一格证明不了
        //   放宽是必需的(与 cardsExpanded / cardsNaive 那对同款)。
        const naiveBody = (b) =>
            [...b.matchAll(/\(#(\d+)\)/g)].map((m) => m[1]);
        for (const f of [FIX_SEMI, FIX_PROSE]) {
            check(
                bodyPrRefs(f).length > 0,
                "宽模式抓不到夹具「" + f.slice(0, 18) + "…」里的号 —— 判据没牙",
            );
            check(
                naiveBody(f).length === 0,
                "严格组模式**也**抓到了「" +
                    f.slice(0, 18) +
                    "…」—— 这一格没有区分度,证明不了放宽是必需的",
            );
        }
        // 一个括号里两个号都要收(第一版只要求号串紧接 `)`,这条会掉一个)。
        check(
            bodyPrRefs(FIX_SEMI)
                .map((r) => r.num)
                .join(",") === "116,123",
            "一个括号里的多个号没有全收 —— 实得 " +
                bodyPrRefs(FIX_SEMI)
                    .map((r) => r.num)
                    .join(","),
        );
        // `#TBD` 三种写法都要认(第一版只认逐字 `(#TBD)`,绕过成本一个分号)。
        for (const t of ["(#TBD)", "(#TBD;等合并后补)", "改动摘要 — #TBD"])
            check(
                [...t.matchAll(BODY_TBD_RE)].length === 1,
                "`#TBD` 的这种写法没被认出来:" + t,
            );
        // 行号要真算得对(报错里没有位置的话,同一个错号会打出两条逐字相同的话)。
        check(
            lineOf("a\nb\nc", 4) === 3,
            "lineOf 算错行号 —— 实得 " + lineOf("a\nb\nc", 4),
        );
    } catch (e) {
        bad.push("正文侧那一组跑不起来:" + e.message);
    }

    // ---- [SL-319] 判定层本身的断言(此前**零覆盖**)---------------------------
    // 上面那组测的是**取号器**(BODY_PAREN_RE / BODY_TBD_RE / lineOf),测不到「取到号之后
    // 怎么判」。#203 复审往判定层注入四种退化,自测与实跑**全绿** —— 因为这一层当时长在
    // run() 里,输入要靠真 CHANGELOG + 真 git 历史凑,自测够不着。抽成纯函数就是为了这几格:
    // 每一格对应一种退化,注入那种退化时**必须是这一格红**。
    // ⚠ 「把 landed 收成入参」本身带来一个新盲区:①–⑦ 都自带 `landed`,于是「集合被抽空」
    // 这种退化它们一格都照不出来。那一格在 ⑧,钉的是 `landedPrs` 本身。上一版这段写
    // 「四种退化各有一格」是**假的** —— 复审实测:注入 `landedPrs = () => new Set()` 之后
    // 自测与实跑**双绿**。抽纯函数把输入变干净的同时,也把「输入是怎么来的」挪出了判定面。
    try {
        const L = (...ns) => new Set(ns.map(String));
        const NOALLOW = new Map();
        // ① 落地位集合要真被查:在集合里 ⇒ 不红。退化「跳过 landed.has」⇒ 本格红。
        check(
            judgeBody("- 改了点东西 (#500)", L("1000", "500"), "", NOALLOW)
                .fails.length === 0,
            "落地位集合里的号被判红了 —— `landed.has` 那一支没生效",
        );
        // ② 不在集合、且小于最大落地号 ⇒ 红(这是 `#106` 那一族的形态)。
        const miss = judgeBody("- 改了点东西 (#500)", L("1000"), "", NOALLOW);
        check(
            miss.fails.length === 1 && miss.warns.length === 0,
            "关掉没合的号没被判红 —— 实得 fails=" +
                miss.fails.length +
                " warns=" +
                miss.warns.length,
        );
        // ③ 报错话术要给出**放行口**(撞永久假红的人得知道往哪走)。
        check(
            !!miss.fails[0] && miss.fails[0].includes(BODY_ALLOW_HEAD),
            "正文判红的话术里没提放行口 —— 撞上永久假红的人没有指引",
        );
        // ④ 正文放行清单要真生效。退化「忽略 bodyAllow」⇒ 本格红。
        check(
            judgeBody(
                "- 改了点东西 (#500)",
                L("1000"),
                "",
                new Map([["500", "理由"]]),
            ).fails.length === 0,
            "正文放行清单没生效 —— 写了放行还是判红,那个口子等于不存在",
        );
        // ⑤ 本 PR 自己的号要放过(搬运规矩③ 要求这么写)。退化「忽略 selfPr」⇒ 本格红。
        check(
            judgeBody("- 改了点东西 (#500)", L("1000"), "500", NOALLOW).fails
                .length === 0,
            "selfPr 没生效 —— 按规矩③ 写成本 PR 号的条目会被判红,那条规矩就没法执行",
        );
        // ⑥ 大于最大落地号 ⇒ **WARN 不是 FAIL**,且要带得出行号(annotation 靠它锚)。
        const fut = judgeBody(
            "第一行\n- 同批交叉引用 (#9001)",
            L("1000"),
            "",
            NOALLOW,
        );
        check(
            fut.fails.length === 0 && fut.warns.length === 1,
            "大于最大落地号那一档判错了 —— 实得 fails=" +
                fut.fails.length +
                " warns=" +
                fut.warns.length,
        );
        check(
            !!fut.warns[0] && fut.warns[0].line === 2,
            "warn 没带对行号 —— `::warning file=,line=` 会锚到错的行;实得 " +
                (fut.warns[0] && fut.warns[0].line),
        );
        // ⑦ 裸 `#TBD`(不带括号)也要判红,且行号要对。
        const tbd = judgeBody(
            "第一行\n- 改动摘要 — #TBD",
            L("1000"),
            "",
            NOALLOW,
        );
        check(
            tbd.fails.length === 1 && tbd.fails[0].startsWith("CHANGELOG.md:2"),
            "裸 `#TBD` 没被判红或行号不对 —— 实得 " +
                tbd.fails.length +
                " 条,首条:" +
                (tbd.fails[0] || "").slice(0, 40),
        );
        // ⑧ **落地位集合本身要真抽得出来**。上面七格都自带 `landed`,所以「集合空」这种退化
        //    它们一格都照不出来:注入 `const landedPrs = () => new Set()` 之后 `maxLanded = 0`
        //    ⇒ 正文里**每一个**号都 `> 0` 走 WARN ⇒ `fails` 恒空 ⇒ **自测与实跑双绿**
        //    (实测 rc 都是 0、134 行 WARN)。整片正文判据能被一行注入静默摘掉,与 #203 那次
        //    逐字同形 —— 而上一版注释还写着「四种退化各有一格」(复审【重要】,那句已改)。
        //    顺带夹住「裸 `#N` 当落地位」:标题里的 `#124` 是**引用**,不该进集合。
        const lp = landedPrs([
            { title: "feat: x (#500)" },
            { title: "Merge pull request #501 from a/b" },
            { title: "补 #124 评审的 5 处 (#131)" },
        ]);
        check(
            lp.has("500") && lp.has("501") && lp.has("131"),
            "landedPrs 抽不出落地位 —— 集合变小 ⇒ maxLanded 变小 ⇒ 正文侧整片走 WARN、判据静默失效;实得 " +
                [...lp].join(","),
        );
        check(
            !lp.has("124"),
            "landedPrs 把标题里的**引用** `#124` 当成了落地位 —— 一句「见 #83」就能把从未合并的 PR 判成已上线",
        );
        // ⑨ 判红话术里给的样例行,自己要过得了 `ALLOW_LINE_RE` —— 否则照抄它的人会再撞一次
        //    「写法不对」,而这条话术恰恰是给第一次找放行口的人看的。
        check(
            ALLOW_LINE_RE.test(allowSampleFor("500")),
            "判红话术里的样例行过不了 ALLOW_LINE_RE:" + allowSampleFor("500"),
        );
        // ⑨b 写坏一行**正文放行**时,报错里给的样例必须是**纯数字**形态。给 SL 形态的话,
        //     照抄的人拿到 `#SL189`:`ALLOW_LINE_RE` 收得下,但 `bodyPrRefs` 只产纯数字,
        //     于是 `deadBody` 当场判负、话术还说成「正文里没有引用它 / 它已经落地」——
        //     两条都不是真因,连撞两次红且第二次指错方向(复审【建议】)。
        const badBodyBlock =
            "<!-- ===\n" +
            BODY_ALLOW_HEAD +
            "\n- SL-189 —— 照抄了标题形态\n\n=== -->";
        let bodySampleErr = "";
        try {
            allowList(badBodyBlock, BODY_ALLOW_HEAD);
        } catch (e) {
            bodySampleErr = e.message;
        }
        // ⑨c 三元的**另一边**也要钉:把它改成恒 `allowSampleFor("111")`,⑨b 照绿,而门禁那半
        //     从此教人写纯数字号 —— 那一段的号是 SL / J 形态,照抄写出 `#111`,`ALLOW_LINE_RE`
        //     收得下、块里却没有对应的 `pending #111` ⇒ 撞 `leaks()` 的 `dead`。与 ⑨b 完全对称,
        //     方向反过来(复审第 3 轮【建议】)。
        let gateSampleErr = "";
        try {
            allowList(
                "<!-- ===\n" +
                    ALLOW_HEAD +
                    "\n- SL-189 —— 照抄了标题形态\n\n=== -->",
            );
        } catch (e) {
            gateSampleErr = e.message;
        }
        // ⑨d 「理由留空」那条红的话术要**点名占位符**:照抄样例的人写的就是 `<理由>`,
        //     被判负时话术却说「没写理由」—— 他明明照着「正确写法」原样写了。这与 ⑨b/⑨c
        //     同一族(第二次红指错方向),入口就是上一条红给的那行样例(复审第 3 轮【建议】)。
        let phErr = "";
        try {
            allowList(
                "<!-- ===\n" +
                    ALLOW_HEAD +
                    "\n" +
                    "- #SL189 —— " +
                    ALLOW_REASON_PLACEHOLDER +
                    "\n\n=== -->",
            );
        } catch (e) {
            phErr = e.message;
        }
        // ⑨e [SL-326] **正文放行只吃纯数字 PR 号**:写 SL / J 形态要当场判负并说清真因。
        //     不拦的话它**静默不生效**(`bodyAllow` 的 key 是 `"SL189"`,而 `r.num` 只有纯数字),
        //     紧接着又撞 `deadBody` 判负、话术说成「正文里没有引用它 / 它已经落地」——
        //     两条都不是真因。反向:门禁那半照旧收 SL / J 形态。
        const bodyAllowBlock = (line) =>
            "<!-- ===\n" + BODY_ALLOW_HEAD + "\n" + line + "\n\n=== -->";
        let formErr = "";
        try {
            allowList(bodyAllowBlock("- #SL189 —— 真理由"), BODY_ALLOW_HEAD);
        } catch (e) {
            formErr = e.message;
        }
        check(
            formErr.includes("SL189") && formErr.includes(BODY_ALLOW_HEAD),
            "正文放行收下了 SL 形态的号 —— 它放行不掉,还会被 deadBody 当成「不需要的放行」判负,话术指错方向;实得:" +
                (formErr || "(没抛)").slice(0, 60),
        );
        check(
            allowList(bodyAllowBlock("- #111 —— 真理由"), BODY_ALLOW_HEAD).get(
                "111",
            ) === "真理由",
            "正文放行连纯数字号也收不下了 —— 收窄收过头,正当的放行没法写",
        );
        // ⑨e2 取号与收窄现在都从 `BODY_REF_NUM_RE` 派生,「放宽了取号却忘了放宽收窄」这条路
        //     **结构上不存在**。这一格钉的是**接线**:两侧确实都走了那一份真源 ——
        //     `bodyPrRefs` 真产出的号,必须过得了正文放行的收窄。
        //     ⚠ 上一版这里写的是「把两侧钉在一起」,而夹具输入写死 `(#111)`:真放宽 `bodyPrRefs`
        //     时它照样绿 —— **注释比判据强**,与本卡反复在修的「话术宣称的和实际发生的不是一回事」
        //     同形,只是从红时话术挪到了绿时注释(#210 第 2 轮【建议】)。现在改的是判据本身,
        //     不是把注释写软。
        const refNum = bodyPrRefs("- 正文条目 (#111)")[0];
        check(
            !!refNum &&
                allowList(
                    bodyAllowBlock("- #" + refNum.num + " —— 真理由"),
                    BODY_ALLOW_HEAD,
                ).has(refNum.num),
            "`bodyPrRefs` 产出的号过不了正文放行的收窄 —— 两处口径已经分叉,放行会静默失效;实得号 " +
                (refNum && refNum.num),
        );
        // ⑨f [SL-326] 「最大落地号」的推导**只此一份**。语义先钉住:空集合 ⇒ 0;
        //     比的是**数**不是字典序(否则 "9" 会大过 "10",基线与判据一起错)。
        check(
            maxLandedOf(new Set()) === 0 &&
                maxLandedOf(L("9", "10", "207")) === 207,
            "maxLandedOf 算错 —— 实得 " +
                maxLandedOf(new Set()) +
                " / " +
                maxLandedOf(L("9", "10", "207")),
        );
        // ⑨g 再钉「只有一份式子」:两处调用共用一个函数才谈得上基线与判据同源。
        //     搜索串**拼装**,免得这条断言自己被数进去(本仓「扫描器入库才炸」那一族)。
        const rawExpr = "Math.max(0, ..." + "[...landed].map(Number))";
        const selfSrc = fs.readFileSync(fileURLToPath(import.meta.url), "utf8");
        // ⚠ 边界:这一格只认**这一种字面写法**。抄成 `Math.max(0, ...Array.from(landed, Number))`
        //    照样过 —— 它挡的是「复制粘贴出第二份」,不是「另写一个等价实现」。
        //    两侧话术分开报:0 次的真因是**搜索串过期**(推导被改名/改写),不是「不同源」,
        //    对着 0 去找第二份推导是找不到的(复审【建议】,与本卡 ② 同一个毛病)。
        const rawHits = selfSrc.split(rawExpr).length - 1;
        check(
            rawHits === 1,
            rawHits === 0
                ? "⑨g 的搜索串对不上了(推导式被改名 / 改写?)—— 这一格已经量不到任何东西,要么同步 rawExpr,要么换一种钉法"
                : "「最大落地号」的推导式在本文件里出现了 " +
                      rawHits +
                      " 次(应当只有 maxLandedOf 一处)—— 基线与判据一旦不同源,基线比没有更坏",
        );
        // ⑨h [SL-327] **真源带 `|` 时,两处构造的分组要绑对**。这两条是「放宽那一天」才显形的,
        //     所以不能等到那天:helper 带默认参数,这里直接喂一个放宽了的真源量它。
        //     没有非捕获分组的话,`"^" + src + "$"` 拼出来是 `^SL\d+|\d+$` ≡ `(^SL\d+)|(\d+$)`,
        //     于是 `J189` 走第二支命中 ⇒ `wrongForm` 对 J 形态不再触发 ⇒ 放行放不掉 ⇒ 撞 deadBody,
        //     报出本卡一路在根除的那句指错方向的红(#210 第 3 轮【建议】)。
        // 放宽了的真源**从真源本身派生**,不手写转义(手写 `"\\d+"` 这一步本会话已经
        // 被工具层折叠过好几次;派生既免转义,又保证真源改了这个夹具跟着改)。
        const WIDE =
            "SL" + BODY_REF_NUM_RE.source + "|" + BODY_REF_NUM_RE.source;
        check(
            bodyRefAnchored(WIDE).test("SL189") &&
                bodyRefAnchored(WIDE).test("111") &&
                !bodyRefAnchored(WIDE).test("J189") &&
                !bodyRefAnchored(WIDE).test("xx111"),
            "真源带 `|` 时收窄的锚点绑错了(缺非捕获分组)—— J189 / xx111 会被放过," +
                "而它们本该落进 wrongForm",
        );
        // ⑨i 粗筛同理:真源放宽之后,`(见 #SL189)` 必须**进候选**,否则组内那一轮再宽也没机会跑,
        //     引用静默不进判定面 —— 不报错、不判红,只是少收几个号。
        check(
            // 断的是**匹配到的整段**,不是「匹配没匹配上」:去掉非捕获分组时
            // `\([^)]*#SL\d+|\d+[^)]*\)` 的第一支照样能命中 `(见 #SL189` 这个**前缀**,
            // `.test()` 因此照样为真 —— 只有比整段才分得开(实测:上一版用 .test 时这一格无牙)。
            bodyParenRe(WIDE).exec("(见 #SL189)")?.[0] === "(见 #SL189)" &&
                // 用**生产那份常量**,不是再调一次 helper —— 否则「helper 还在、
                // 但 BODY_PAREN_RE 被写回字面量」这条路没人守(实测过:那样注入时本格照绿)。
                new RegExp(BODY_PAREN_RE.source).test("(见 #111)") &&
                !new RegExp(BODY_PAREN_RE.source).test("(见 #SL189)"),
            "粗筛没跟着真源走 —— 放宽后 `(见 #SL189)` 仍不进候选(或收窄时反而收多了)",
        );
        // ⑨j 生产那份常量必须**真的由 helper 生成**。行为上验不出来:今天真源是 `\d+`,
        //     写回字面量 `/\([^)]*#\d+[^)]*\)/g` 与派生版**等价**,任何输入都分不开
        //     (实测:那样注入时上一格照绿)。所以这里比的是**构造出来的 source 串** ——
        //     派生版带 `(?:…)`,手写字面量没有,一比就分得开。
        check(
            BODY_PAREN_RE.source === bodyParenRe().source,
            "BODY_PAREN_RE 不是由 bodyParenRe() 生成的(被写回字面量?)—— 真源放宽那天它不会跟着动;" +
                "实得 " +
                BODY_PAREN_RE.source,
        );
        // ⑨k [SL-327] 三类问题**一次报完**:回退成「先到先抛」时这一格必须红。
        //     这是本卡四条里唯一差夹具的一条 —— PR 描述里那句「实测一次报出 3 类」是**手跑**的,
        //     合并之后就没有执行者了,与本文件一路在根除的「注释比判据强」同形(复审【重要】)。
        //     现有的 ⑨b / ⑨d / ⑨e 每格只走一类,谁也量不到「一次报完」。
        let threeErr = "";
        try {
            allowList(
                bodyAllowBlock(
                    "- #SL189 —— 形态不对" +
                        "\n" +
                        "- #111 -- 破折号写坏" +
                        "\n" +
                        "- #222 —— ",
                ),
                BODY_ALLOW_HEAD,
            );
        } catch (e) {
            threeErr = e.message;
        }
        check(
            threeErr.includes("SL189") &&
                threeErr.includes("破折号写坏") &&
                threeErr.includes("222"),
            "三类问题没有一次报完(回退成先到先抛?)—— 作者会改完一种、重跑才看见下一种,连撞几次红;实得:" +
                threeErr.slice(0, 70),
        );
        check(
            phErr.includes(ALLOW_REASON_PLACEHOLDER),
            "「理由留空」的话术没点名占位符 —— 照抄样例的人会被告知「没写理由」,而他确实照着写了",
        );
        check(
            gateSampleErr.includes(ALLOW_SAMPLE),
            "门禁放行写坏时给的样例不是 SL 形态 —— 样例按段选的三元被改成恒一边也没人红;实得:" +
                gateSampleErr.slice(-60),
        );
        check(
            bodySampleErr.includes(allowSampleFor("111")),
            "正文放行写坏时给的样例不是纯数字形态 —— 照抄它会再撞一次红,且话术指错方向;实得:" +
                bodySampleErr.slice(-60),
        );
        // ⑩ 过期的正文放行要判负(与门禁那半的 `dead` 同一条纪律)。两种过期形态各一格。
        check(
            judgeBody(
                "- 没有任何引用",
                L("1000"),
                "",
                new Map([["500", "理由"]]),
            ).fails.length === 1,
            "正文里已经没有引用的放行没被判负 —— 留着一个没人用的豁免口",
        );
        check(
            judgeBody(
                "- 改了点东西 (#500)",
                L("1000", "500"),
                "",
                new Map([["500", "理由"]]),
            ).fails.length === 1,
            "号都已经落地了、放行还留着,没被判负 —— 它此后替一个不需要豁免的号挡着",
        );
    } catch (e) {
        bad.push("判定层那一组跑不起来:" + e.message);
    }

    // 块首尾被改坏时必须 fail-closed,而不是「找不到块 ⇒ 没有泄漏 ⇒ 绿」。
    check(
        threw(() => draftBlock("# Changelog\n\n没有注释块\n")),
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
        "check-changelog-drafts --self-test:全部通过(压缩写法夹具 = " +
            FIXTURE_SHA.slice(0, 7) +
            " 的真标题;未合并样本 = 真 CHANGELOG 里的 #" +
            FIXTURE_UNMERGED_PR +
            ")",
    );
    process.exit(0);
}

// ---- 实跑 -----------------------------------------------------------------
try {
    const md = fs.readFileSync(CHANGELOG, "utf8");
    const block = draftBlock(md);
    const titles = shippedTitles(base);
    const tokens = new Set(pendingTokens(block));
    // 正文 = **整份文件**,只把注释块原地抹成同长空白(`[^\n]` 保住所有换行,行号照旧准确)。
    // 第一版切成「块之前那一段」,于是块**之后**的小节(`## [0.1.0]` 在 326-333 行)一个字都
    // 不看 —— 今天不漏(那节里没有 `(#N)`),但发版时若按 Keep-a-Changelog 把 `## [Unreleased]`
    // 改名再在上面开新的,块就被留在旧小节里,块之下的东西会越攒越多而没有任何提示
    // (#203 复审【建议】)。抹白比切片多零行代码,却把「覆盖的是正文的一半」这个静默边界去掉。
    const bi = md.indexOf(block);
    const body =
        md.slice(0, bi) +
        block.replace(/[^\n]/g, " ") +
        md.slice(bi + block.length);

    // [BASE] / [ALLOW] 都排在 `leaks()` **之前**:`leaks()` 自己会为「放行写法不对 / 理由留空 /
    // 放行的号已没有对应条目」三种情形**抛错**,排在它后面的话那三条红路径上一行都不显 ——
    // 而头注 §边界 承诺的是「红绿都打」(#197 复审第 7 轮【建议】)。
    // [BASE] 排在最前:它只要 titles 与 tokens,而 allowList 本身就可能抛。
    //   · 判红时它回答「这次是拿哪天的 base 比的」—— base 陈旧时报出来的漏搬表只是真集合的
    //     子集,而话术又叫人去 `git show <sha>` 核,那时正需要这条信息;
    //   · 号数并进这一行,否则它只活在成功消息里、而 gates 3i 按标记回显、成功消息不带标记
    //     ⇒ 本地看不见「块里是不是近乎空转」(#197 复审第 6 轮【建议】)。
    //   · 说的是**块里有几个 `pending #…` 标记**,不是「判定面内几个」—— 被放行的号走
    //     `continue`,恰恰不在判定面内,写成「判定面内」会高报(#197 复审第 7 轮【建议】)。
    //     放行了哪几个由紧跟其后的 [ALLOW] 行逐条说,不在这里做减法。
    // [SL-319 复审第 3 轮] 落地位集合的**大小与最大号**也报出来。下面那道 fail-closed 只挡
    // `size === 0`,而「集合被**缩小**」(`landedPrs(titles.slice(0, 3))`)失效形态一模一样:
    // maxLanded 变小 ⇒ 正文里几乎每个号都 `> max` 走 WARN ⇒ fails 恒空 ⇒ rc=0,而 ⑧ 喂自己的
    // 假标题、①–⑦ 自带 landed,都照绿。那一档**不是全静默**(会打上百行 WARN,小的那个号就在
    // 话术里),但要有人在一片 WARN 里觉得那个号偏小 —— **正常态没有基线就没有对照**。
    // 所以基线放在这里:绿的时候也报,下次不对劲一眼看得出来。
    const landed = landedPrs(titles);
    const maxLanded = maxLandedOf(landed);
    console.log(
        "  [BASE] " +
            base +
            "@" +
            baseStamp(base) +
            " —— " +
            titles.length +
            " 条提交标题(落地位 " +
            landed.size +
            " 个,最大 #" +
            maxLanded +
            ");块里 " +
            tokens.size +
            " 个待合并的号",
    );

    // 放行**永远打出来**(红绿都打):豁免不显形就等于没有豁免纪律。
    for (const [token, why] of allowList(block))
        console.log("  [ALLOW] #" + token + " 放行 —— " + why);

    // ---- 正文侧([SL-316]):`#TBD` 即红;每个 `(#N)` 必须在主线落过地 ----------
    // 与上面那半共用 `landedPrs`(落地位 = 尾部 `(#N)` 或老式 merge commit),
    // 所以「块里判漏搬」与「正文判错号」量的是同一个集合,不会互相打架。
    //
    // ⚠ **本 PR 自己的号还没落地** —— 这是第一版的阻断级缺陷(#203 复审【重要】):
    // `landed` 取自 base 分支、不含本 PR 的提交,而搬运规矩③ 要求作者在合并**前**把条目写成
    // `(#<本 PR 号>)`;`(#TBD)` 这条退路又被本卡封了 ⇒ **按规矩搬条目的 PR 没有任何合法写法**,
    // 而且红会落在下一个搬条目的 PR 上(本 PR 自己不搬,所以侥幸绿)。
    // 处置:CI 透传 `SCVB_CHANGELOG_SELF_PR`(见 format.yml)并入放行;本地没有这个变量时,
    // 对**大于当前最大落地号**的号打 [WARN] 而不判红 —— PR 号单调递增,本 PR 号必然大于任何
    // 已落地号;而 `#106 < #111` 这类**关掉没合**的号仍落在 ≤ max 一侧、照样判红,不丢牙齿。
    // [SL-319 复审第 2 轮] 第 ⑧ 格钉住的是 `landedPrs` **这个函数**,钉不住**这次调用**:
    // 把实参换成 `[]`(或 titles 被谁过滤/切片成空)⇒ ⑧ 自己喂假标题照绿、①–⑦ 自带 landed
    // 照绿、实跑 `maxLanded = 0` 让正文每个号都走 WARN ⇒ `fails` 恒空 ⇒ rc=0。
    // 与「函数体被掏空」那次逐字同形,只是注入点挪到了调用处;`[BASE]` 行报的是标题条数与
    // 块里的号数,**不报落地位集合大小**,所以输出上也照不出来。
    // 这一档有一条无争议的判据(不像「> 最大落地号」那档要拍常数):**有历史的仓库里,
    // 落地位集合不可能为空**。与 `shippedTitles` 的浅克隆那道同一条纪律,fail-closed。
    if (!landed.size)
        throw new Error(
            titles.length +
                " 条提交标题里一个落地位都抽不出来(尾部 `(#N)` 与老式 merge commit 全零命中)——" +
                "正常仓库不可能如此。落地位集合为空 ⇒ 最大落地号 = 0 ⇒ 正文里每个号都会被当成" +
                "「尚未合并」只打 WARN,整片正文判据静默失效,所以这里 fail-closed 而不是放行",
        );
    const selfPr = (process.env.SCVB_CHANGELOG_SELF_PR || "").trim();
    const bodyAllow = allowList(block, BODY_ALLOW_HEAD);
    for (const [num, why] of bodyAllow)
        console.log("  [ALLOW] 正文 #" + num + " 放行 —— " + why);
    // 判定层是纯函数(见上面 judgeBody):这里只负责**打**与**退**。
    const { fails: bodyBad, warns } = judgeBody(
        body,
        landed,
        selfPr,
        bodyAllow,
    );
    for (const w of warns) {
        console.log("  [WARN] " + w.text);
        // [SL-319] 「跑在 CI 上」的判据是 `GITHUB_ACTIONS`,不是「selfPr 恰好有值」。
        // 旧写法拿一个**业务变量有没有值**当运行环境的代理:`docs-truth` 同挂
        // `push → dev/feature/**`,push 事件下 `github.event.pull_request.number` 是空的,
        // 于是 push 那半根本不打 annotation —— 而那正是「本地绿、推上去才发现」最容易漏的一侧。
        // GitHub Actions 上 `GITHUB_ACTIONS` 恒为 "true",与事件类型无关。
        // `file=,line=`:裸 `::warning::` 只会挂在 workflow 顶上,点进去落不到具体文件;
        // 带锚点才会渲染成 CHANGELOG.md 那一行旁边的批注(#203 复审第 5 轮【建议】)。
        if (process.env.GITHUB_ACTIONS === "true")
            console.log(
                "::warning file=CHANGELOG.md,line=" + w.line + "::" + w.text,
            );
    }
    // 只打不退:与 `leaks()` 的结果**一起**在末尾判退出码 —— 在这里直接 exit 会把漏搬清单
    // 整段吞掉,让人「改号 → 重跑 → 又冒出一批漏搬」跑两轮(#203 复审【建议】)。
    // 打印排在 `leaks()` **之前**:`leaks()` 自己会抛(放行写法不对那三种),排在它后面的话
    // 这段在那条抛错路径上一个字都不显 —— 与 [BASE] / [ALLOW] 那条同一个理由。
    if (bodyBad.length) {
        console.error(
            "check-changelog-drafts:正文 " + bodyBad.length + " 处号不对:",
        );
        for (const b of bodyBad) console.error("  [FAIL] " + b);
    }

    const found = leaks(block, titles);
    if (found.length) {
        console.error(
            "check-changelog-drafts 失败(" + found.length + " 条预写条目漏搬):",
        );
        // 措辞只说判据**知道**的那件事。`shipped` 是先到先留、而 `git log` 是新→旧,所以
        // 这个 sha 是**最近一次在标题里提到这个号**的提交,不一定是把它带上线的那一条
        // (例:SL-301 落在 `c8e3140`/#190,而 `e51c2e9`/#195 的标题也提到它,报的会是后者)。
        // 写成「已随 <sha> 上线」就等于把本卡通篇强调的「标题提到 ≠ 上线」在自己的输出里说反了,
        // 而撞上红的人第一件事就是 `git show <sha>` 去核 —— 核出一条与落地无关的提交,结论会
        // 滑向「机检乱报」,下一步就是删条目(规矩⑤明禁)或滥用放行口(#197 复审第 4 轮【建议】)。
        for (const f of found)
            console.error(
                "  [FAIL] #" +
                    f.token +
                    "(" +
                    FORMS[f.form].label +
                    ")在主线标题里出现过,预写条目却还留在注释块里;最近一次提到它的是 " +
                    f.sha +
                    " —— " +
                    f.title,
            );
        console.error(
            "  搬法见 CHANGELOG.md 注释块开头那段「搬运规矩」:换成本 PR 号 → 追加到正文同名小节 → 块里整条删掉。",
        );
        console.error(
            "  若这是**假红**(合并标题只是顺带提了一句这个号,它本身并没有交付),走注释块里的「" +
                ALLOW_HEAD +
                "」:一个号一行、理由必填,例如\n    " +
                ALLOW_SAMPLE +
                "\n  放行会在每次运行时连理由一起打进输出。**不要靠删条目躲开**(规矩⑤)。",
        );
    }
    // 两半一起判退出码,一次把两边的清单都给全。
    if (bodyBad.length || found.length) process.exit(1);
    // 不再重复报号数,也不说「都还没上线」:被放行的号**恰恰在**已上线集合里(那正是它要放行
    // 的原因),把它算进「都还没上线」在有放行时就是一句假话 —— 与上面 [BASE] 那处
    // 「判定面内 → 块里」是同一句断言的两半,第 7 轮只改了一半(#197 复审第 8 轮【建议】)。
    // 号数由 BASE 行报一次、放行由 ALLOW 行逐条说,这里只声称判据真正知道的那件事。
    console.log(OK_MSG);
    process.exit(0);
} catch (e) {
    console.error("check-changelog-drafts 失败:" + e.message);
    process.exit(1);
}
