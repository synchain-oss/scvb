# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-  (编码声明必须在前两行内,故 SPDX 头占第 1 行、它占第 2 行)
"""字体保留名门禁(gates 3k / [SL-267]):断言分发的 woff2 `name` 表不含上游保留字体名。

**这道门禁存在的理由**:OFL-1.1 §3 规定 Modified Version「不得使用 Reserved Font Name(RFN)」,
而子集化就是修改。本仓四款字体全部经 `scripts/fetch_fonts.py` 子集化后随 `.vst3` 分发,
其中 IBM Plex 两款的 RFN 是 **"Plex"** 一词本身 —— 仓库转 public 时带着这个违规上了线
(THIRD-PARTY-NOTICES.md 早写明「转 public 前必处理」,却没有任何一道机检在看)。

**为什么只改文件名和 CSS family 不够**:§3 限制的是「呈现给用户的字体名」,那是字体
`name` 表里的家族名 / 唯一 ID / 全名 / PostScript 名(nameID 1/3/4/6/16/17),不是文件名。
文件叫 `ScvbSans.woff2` 而 name 表里仍写着 "IBM Plex Sans",装进系统字体册、被 DevTools
的字体面板或 PDF 导出读出来的依然是上游名 —— 违规照旧,而 diff 看着已经改完了。
woff2 是 brotli 压缩的,**grep 二进制不命中不等于名字已清除**,只有解出 name 表逐条比对
才算证据。`fetch_fonts.py` 的 `rename_font()` 在生成期做同样的断言;本脚本把它搬进门禁,
让「重新生成字体」与「审查既有分发物」两条路都被守住。

**署名记录不参与断言**(`ATTRIBUTION_IDS`):OFL 惯例的版权行(nameID 0)本身就写着
"with Reserved Font Name 'X'",商标声明(7)同理 —— 本仓的 `NotoSansSC.woff2` nameID 7
逐字是 "Source is a trademark of Adobe…"。这些是 §2 要求随分发**逐字保留**的署名,
不是残留的违规名;把它们算进断言只会制造恒红。除这四条以外的**全部** nameID 都要扫,
而不只扫 1/3/4/6/16/17:漏改一个槽位(如 nameID 5 的版本串)同样是呈现给用户的名字。

**分发的 CSS/JS 里的字体名同样受 §3 约束**:违规面共三处 —— 文件名、`@font-face` family
与字体栈字面量、`name` 表。前者由 `RESERVED` 的登记制守住、后者由解表比对守住,而中间那处
(`tokens.css` 的 `font-family` 与 `trajectory-chart.js` 的 `STYLE_FALLBACK.mono`)若只守两头
就没有机检:把 family 改回 "IBM Plex Sans" 而 woff2 一字不动,解表照样全绿,可随 `.vst3`
分发的 CSS 又把 RFN 呈现给用户了。故本脚本另扫 `web/` 下的文本资源(即 `ScvbWebAssets.cmake`
进包的那批 `.css`/`.js`/`.html`,vendored 的 `web/js/juce/` 除外)。
判据**只落在字体名上下文**:字体声明值(`font-family:` / `font:` 简写 / 驼峰 `fontFamily`
的赋值)、`--ff-*` 字体栈变量、含 CSS 通用族关键字的字符串字面量(含反引号模板串)、
以及 `src: local(…)` 里的家族名。
不是整文件 grep:RFN "Source" 是个常用词,整文件扫会在 `source_channels` /
`renderSource()` 这类标识符上刷出几十条假红,而假红最终会把整道扫描废掉。
读不出来的文件(非 UTF-8)判红而不是替换掉坏字节接着扫 —— 坏字节可能正落在 RFN 中间。

依赖:fontTools + brotli(解 woff2 必需):`pip install fonttools brotli`

用法:
    python scripts/check-font-names.py              # 扫 web/fonts 的 woff2 + web/ 的文本资源(gates 3k)
    python scripts/check-font-names.py --self-test  # 先验门禁本身:坏样例必红、署名样例不误伤
    python scripts/check-font-names.py <目录>       # 指定目录:woff2 与文本资源都扫它 ——
                                                   # 该目录须两样都有,只传 web/fonts 会红在
                                                   #「没扫到文本资源」上。自测与 CI 之外一般用不到
"""
import glob, os, re, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

try:
    from fontTools.ttLib import TTFont
    import brotli  # noqa: F401 —— 缺它时要等到 TTFont 打开 woff2 才炸,提前暴露
except ImportError as exc:
    # 硬失败,不是跳过。缺依赖判绿 = 假绿,比判红危险得多(同 check-font-coverage.py)。
    sys.exit("!! 缺依赖(%s):pip install fonttools brotli" % exc)

import fetch_fonts as ff

# 分发文件名 -> 上游 RFN。None = 上游版权行不含 "with Reserved Font Name",名字不受 §3 限制。
# 逐家族核验依据见 THIRD-PARTY-NOTICES.md 的字体一节。**新增字体必须登记**,
# 否则下面的「未登记」检查会判红 —— 这道门禁没有默认放行。
RESERVED = {
    "SpaceGrotesk.woff2": None,  # 上游版权行无 RFN 声明,二进制原样分发
    "ScvbSans.woff2": "Plex",  # IBM Plex Sans 子集,已按 §3 改名为 'SCVB Sans'
    "ScvbMono.woff2": "Plex",  # IBM Plex Mono 子集,已按 §3 改名为 'SCVB Mono'
    "NotoSansSC.woff2": "Source",  # 分发名本就不含它;登记在册是为了守住不回归
}

# 署名记录:0 版权 / 7 商标 / 13 许可证声明 / 14 许可证 URL。见模块头注。
ATTRIBUTION_IDS = (0, 7, 13, 14)

# 字体总得有个家族名和 PostScript 名。一款都扫不到 = 断言无对象,是**空转的门禁**而非通过。
REQUIRED_IDS = (1, 6)

# ---- 分发文本资源侧(见模块头注「分发的 CSS/JS 里的字体名同样受 §3 约束」)----
# 进包的文本类型,口径照 cmake/ScvbWebAssets.cmake 的 glob(html/js/css 三类)。
TEXT_ASSET_EXTS = (".css", ".js", ".html")
# vendored:JUCE 官方前端 helper 不是我们写的字体栈,上游怎么写就怎么进包,不归本门禁管。
TEXT_ASSET_SKIP_DIRS = (os.path.join("js", "juce"),)
# CSS 通用族关键字 —— 用来认出「这串文本是个字体栈」。JS 里的字体栈(STYLE_FALLBACK.mono)
# 没有 `font-family:` 前缀,只能靠它认;顺带兜住多行字体栈里不带声明名的那几行。
GENERIC_FAMILIES = (
    "sans-serif",
    "serif",
    "monospace",
    "ui-monospace",
    "ui-sans-serif",
    "system-ui",
    "cursive",
    "fantasy",
)
# 字体名上下文:① 字体声明值 ② --ff-* 字体栈变量值 ③ 含通用族关键字的字符串字面量
# ④ src: local(…) 的实参(见下面 _LOCAL_RE)。
# ①② 跨行(prettier 会把长字体栈折行),故值取到 `;` / `}` 为止而不是取到行尾。
# 声明名收三类:CSS 的 `font-family` 与 `font` 简写、DOM/对象字面量的驼峰 `fontFamily`;
# 分隔符收 `:` 与 `=` —— `el.style.fontFamily = "…"` 与 `ctx.font = ...` 都是赋值不是声明。
# 字符串字面量的引号含**反引号**:canvas 那条路已经在用模板串拼字体
# (trajectory-chart.js 的 `ctx.font`),字体栈哪天直接写进模板串是自然的下一步。
# 转义引号靠 `\\.` 吞掉:少了它,一条含 \" 的字体栈会整条掉出扫描面。
_DECL_RE = re.compile(
    r"(?:font-family|fontFamily|--ff-[\w-]*|font)\s*[:=]\s*([^;{}]{0,400})", re.S
)
_STRING_RE = re.compile(r"""(['"`])((?:\\.|(?!\1)[^\n]){0,400})\1""")
# ④ `@font-face { src: local("IBM Plex Sans"), url(…) }` —— 标准写法,指的是**装在用户机器上**
# 的那款上游字体,同样是把 RFN 呈现给用户;而它既不在声明值里(声明名是 src),
# 引号里也没有通用族关键字,前三条都认不出它。全仓现无 local() 调用,接进来零误伤。
_LOCAL_RE = re.compile(r"""local\(\s*(?:(['"])(.{0,200}?)\1|([^)]{0,200}?))\s*\)""", re.S)

# 与 fetch_fonts.py 对拍:生成侧与门禁侧各自独立声明(门禁不该把判据整个托付给被审查的
# 那一侧),但两处一旦漂移就会出现「生成侧按新词改名、门禁侧仍按旧词断言」的静默失效,
# 故在导入期就把差异变成硬失败。
for _fname, (_family, _ps, _rfn) in ff.RENAME.items():
    if RESERVED.get(_fname) != _rfn:
        sys.exit(
            "!! RESERVED 与 fetch_fonts.RENAME 对 %s 的 RFN 不一致(%r vs %r);两处须同步"
            % (_fname, RESERVED.get(_fname), _rfn)
        )
# 登记表的**键集**同样与生成侧对拍:RFN 是逐款人工核验的结论(不可派生),但「有哪些
# 文件」是生成侧说了算的。生成侧加一款而这里没登记,目录检查要等到那款字体真的落进
# web/fonts 才判红;对拍则在导入期就说清楚缺谁。
_produced = {_f for _f, _fam in ff.LATIN_OUTPUTS} | {ff.CJK_OUTPUT}
if set(RESERVED) != _produced:
    sys.exit(
        "!! RESERVED 的登记集与 fetch_fonts 的产出集不一致(缺登记 %s / 多登记 %s);两处须同步"
        % (sorted(_produced - set(RESERVED)), sorted(set(RESERVED) - _produced))
    )
if ATTRIBUTION_IDS != ff.NAME_IDS_ATTRIBUTION:
    sys.exit(
        "!! ATTRIBUTION_IDS 与 fetch_fonts.NAME_IDS_ATTRIBUTION 不一致(%r vs %r);两处须同步"
        % (ATTRIBUTION_IDS, ff.NAME_IDS_ATTRIBUTION)
    )


def scan_font(path, reserved):
    """返回 [(nameID, platformID, platEncID, langID, value)]:仍含 RFN 的呈现名记录。

    `reserved` 为 None 时不扫(上游无 RFN)。命中判据是**双侧 casefold 子串**:
    上游若写成 "plex" / "PLEX" 同样要命中,而 "Plexus" 这类含 RFN 的新造名也不放过 ——
    §3 的口径是「不得使用 RFN」,不是「不得等于 RFN」。
    """
    records = TTFont(path)["name"].names
    seen_ids = {r.nameID for r in records}
    missing = [i for i in REQUIRED_IDS if i not in seen_ids]
    if missing:
        raise ValueError("%s 的 name 表缺 nameID %s,无从断言" % (os.path.basename(path), missing))
    if reserved is None:
        return []
    needle = reserved.casefold()
    return [
        (r.nameID, r.platformID, r.platEncID, r.langID, r.toUnicode())
        for r in records
        if r.nameID not in ATTRIBUTION_IDS and needle in r.toUnicode().casefold()
    ]


def font_contexts(text):
    """从一份文本资源里摘出所有「字体名上下文」,供 RFN 断言。见 `_DECL_RE` / `_STRING_RE` 注。"""
    out = [m.group(1) for m in _DECL_RE.finditer(text)]
    out += [
        m.group(2)
        for m in _STRING_RE.finditer(text)
        if any(g in m.group(2) for g in GENERIC_FAMILIES)
    ]
    out += [m.group(2) or m.group(3) or "" for m in _LOCAL_RE.finditer(text)]
    return out


def check_text_assets(root, verbose=True):
    """扫 `root` 下随分发进包的文本资源:字体名上下文里不得出现任何登记在册的 RFN。

    判据面故意窄到「字体名上下文」而不是整文件 —— 理由见模块头注(RFN "Source" 是常用词)。
    扫描面则取全部 RFN(不分家族):CSS 里写的是家族名,与哪个 woff2 文件对应无从判断,
    因此只要是登记过的上游保留名,出现在字体栈里就判红。
    """
    needles = sorted({r.casefold() for r in RESERVED.values() if r})
    problems = []
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        if any(
            rel_dir == d or rel_dir.startswith(d + os.sep) for d in TEXT_ASSET_SKIP_DIRS
        ):
            dirnames[:] = []
            continue
        for fname in sorted(filenames):
            if not fname.endswith(TEXT_ASSET_EXTS):
                continue
            path = os.path.join(dirpath, fname)
            rel = os.path.relpath(path, root)
            try:
                with open(path, encoding="utf-8") as fh:
                    text = fh.read()
            except UnicodeDecodeError as exc:
                # 判红,而不是 errors="replace" 接着扫:读不干净的文件里 RFN 可能正好落在
                # 被替换掉的那几个字节上 —— 那就成了「扫过了但没扫到」。全仓 UTF-8 是既有
                # 纪律,真出现非 UTF-8 说明别处坏了,让它红在这句人话上,别抛 traceback。
                problems.append("%s 不是 UTF-8,读不出来无从断言(%s)" % (rel, exc))
                continue
            scanned += 1
            for ctx in font_contexts(text):
                folded = ctx.casefold()
                for needle in needles:
                    if needle in folded:
                        problems.append(
                            "%s 的字体名上下文含保留字体名 %r:%r"
                            % (rel, needle, " ".join(ctx.split()))
                        )
    # 一份都没扫到 = 目录写错 / 扩展名口径漂了,是**空转的门禁**而非通过(同 REQUIRED_IDS)。
    if not scanned:
        problems.append("%s 下没扫到任何 %s 文本资源(断言无对象)" % (root, "/".join(TEXT_ASSET_EXTS)))
    elif verbose and not problems:
        print("OK   %-20s %d 份文本资源的字体栈均不含保留字体名" % ("(css/js/html)", scanned))
    return problems


def check_dir(font_dir, verbose=True):
    """扫一个字体目录,返回问题清单(空 = 通过)。"""
    problems = []
    present = {os.path.basename(p) for p in glob.glob(os.path.join(font_dir, "*.woff2"))}

    # 目录里出现未登记的字体 = 覆盖缺口(新增家族没核 RFN),同样 fail-closed。
    for extra in sorted(present - set(RESERVED)):
        problems.append("%s 未登记 RFN(补进本脚本的 RESERVED 与 THIRD-PARTY-NOTICES.md)" % extra)

    for fname in sorted(RESERVED):
        if fname not in present:
            problems.append("%s 不存在于 %s(登记在册却没随分发)" % (fname, font_dir))
            continue
        reserved = RESERVED[fname]
        try:
            hits = scan_font(os.path.join(font_dir, fname), reserved)
        except ValueError as exc:
            problems.append(str(exc))
            continue
        for nid, pid, eid, lid, value in hits:
            problems.append(
                "%s nameID %d(platform %d/%d lang %d)仍含保留字体名 %r:%r"
                % (fname, nid, pid, eid, lid, reserved, value)
            )
        if verbose:
            if reserved is None:
                print("SKIP %-20s 上游无保留字体名" % fname)
            elif not hits:
                print("OK   %-20s 呈现名均不含 %r" % (fname, reserved))
    return problems


def self_test():
    """先验门禁本身:坏样例必被抓、署名样例不误伤、未登记文件必红。

    失效模式是**静默放行** —— 断言写坏(比如把 ATTRIBUTION_IDS 放宽到全部 nameID、
    或把子串比对写成相等比对)后扫描照样退 0,门禁看着绿其实什么都没拦。故与
    `check-privacy.mjs --self-test` 同款:自测排在扫描之前,它红比扫描红更要紧。
    坏样例由仓内真字体就地改一条 name 记录合成,不引入任何外部素材。
    除呈现名外还逐个钉死几个描述性槽位(5 版本 / 9 设计者 / 10 描述)确实在扫 ——
    它们被塞进 ATTRIBUTION_IDS 是「让门禁少扫一格」的唯一途径,得先在这里绊一跤。
    """
    src = os.path.join(HERE, "..", "web", "fonts", "ScvbMono.woff2")
    if not os.path.exists(src):
        sys.exit("!! 自测取不到样本字体:%s" % os.path.abspath(src))
    failures = []
    with tempfile.TemporaryDirectory() as tmp:

        def sample(nid, value, name="ScvbMono.woff2"):
            path = os.path.join(tmp, name)
            shutil.copyfile(src, path)
            font = TTFont(path)
            font["name"].setName(value, nid, 3, 1, 1033)
            font.save(path)
            return path

        # ① 改名漏了 nameID 1(最典型的「只改了文件名」形态)⇒ 必须命中
        if not scan_font(sample(1, "IBM Plex Mono"), "Plex"):
            failures.append("nameID 1 写回 'IBM Plex Mono' 竟未命中(RFN 断言失效)")
        # ② 漏在 1/3/4/6/16/17 之外的槽位 ⇒ 同样必须命中,否则扫描面太窄。
        # 逐个槽位钉死,是因为**放宽 ATTRIBUTION_IDS 是唯一能让扫描面少一格的手段**:
        # 将来上游若在这些描述性槽位里带上 RFN(如 Noto 在 nameID 10 写 "Source Han Sans"),
        # 门禁会红,而最省事的「解法」正是把该 nameID 塞进豁免表 —— 那等于悄悄放行。
        # 钉死之后谁这么改都会先被本档绊倒,而不是让扫描静默少扫一格。
        for _nid, _bad in (
            (5, "Version 2.3 (IBM Plex Mono subset)"),  # 版本串
            (9, "IBM Plex Type Team"),  # 设计者
            (10, "Subset of IBM Plex Mono, generated for SCVB"),  # 描述
        ):
            if not scan_font(sample(_nid, _bad), "Plex"):
                failures.append("nameID %d 里的 RFN 竟未命中(该槽位被排除出扫描面)" % _nid)
        # ③ 大小写变体 ⇒ 双侧 casefold 必须命中
        if not scan_font(sample(1, "ibm PLEX mono"), "Plex"):
            failures.append("大小写变体竟未命中(casefold 比对失效)")
        # ④ 版权行里的 "with Reserved Font Name" ⇒ 必须**不**命中(§2 要求逐字保留)
        if scan_font(sample(0, "Copyright 2017 IBM Corp., with Reserved Font Name 'Plex'"), "Plex"):
            failures.append("版权行(nameID 0)被判违规(署名豁免失效,门禁会恒红)")
        # ⑤ 商标声明同理 —— NotoSansSC 的 nameID 7 逐字就是这个形态
        if scan_font(sample(7, "Plex is a trademark of IBM Corp."), "Plex"):
            failures.append("商标声明(nameID 7)被判违规(署名豁免失效)")
        # ⑥ 未登记的新字体 ⇒ 目录检查必须判红,不能默认放行
        shutil.copyfile(src, os.path.join(tmp, "ScvbMono.woff2"))
        shutil.copyfile(src, os.path.join(tmp, "Unregistered.woff2"))
        if not any("Unregistered.woff2" in p for p in check_dir(tmp, verbose=False)):
            failures.append("未登记字体竟未判红(新增家族会绕过 RFN 核验)")

        # ⑦ 分发文本资源里的 family 字面量 ⇒ 必须命中。woff2 一字不动、只把 CSS 的 family
        #    改回上游名,是「解表全绿而分发物仍呈现 RFN」的那条漏网路径。
        assets = os.path.join(tmp, "assets")
        os.makedirs(os.path.join(assets, "js", "juce"))
        with open(os.path.join(assets, "bad.css"), "w", encoding="utf-8") as fh:
            fh.write('@font-face {\n  font-family: "IBM Plex Sans";\n}\n')
        with open(os.path.join(assets, "bad.js"), "w", encoding="utf-8") as fh:  # 无声明名,靠通用族关键字认出
            fh.write("const F = { mono: '\"IBM Plex Mono\", ui-monospace, monospace' };\n")
        # 模板串 / 驼峰属性 / 转义引号 / font 简写:都是这个代码库自然会走到的写法,
        # 漏掉哪一种都等于给 RFN 留一条门。**每种写法一个文件、一条断言** —— 合成一个文件
        # 数命中条数是没牙的:同一行常被声明名与字符串两条路重复命中,删掉一种支持后
        # 条数仍够,断言照样绿。故每个样例都刻意只有一条路认得出它:
        with open(os.path.join(assets, "bad_tpl.js"), "w", encoding="utf-8") as fh:
            # 反引号模板串,属性名 `mono` 不是声明名 ⇒ 只有 _STRING_RE 的反引号支持认得出
            fh.write("const F = { mono: `\"IBM Plex Mono\", ui-monospace, monospace` };\n")
        with open(os.path.join(assets, "bad_camel.js"), "w", encoding="utf-8") as fh:
            # 栈里没有通用族关键字 ⇒ 只有 _DECL_RE 的驼峰 fontFamily 认得出
            fh.write('el.style.fontFamily = "IBM Plex Sans";\n')
        with open(os.path.join(assets, "bad_escape.js"), "w", encoding="utf-8") as fh:
            # 转义引号开头 ⇒ 只有 _STRING_RE 的 `\\.` 支持能把整条字体栈圈进来
            fh.write('const s = "\\"IBM Plex Mono\\", ui-monospace, monospace";\n')
        with open(os.path.join(assets, "bad_shorthand.css"), "w", encoding="utf-8") as fh:
            # CSS `font:` 简写 ⇒ 只有 _DECL_RE 收了 font 才认得出
            fh.write('.x { font: 600 12px "IBM Plex Mono"; }\n')
        with open(os.path.join(assets, "bad_local.css"), "w", encoding="utf-8") as fh:
            # src: local(…) 指的是装在用户机器上的上游字体;声明名是 src、引号里也没有
            # 通用族关键字 ⇒ 只有 _LOCAL_RE 认得出
            fh.write('@font-face { src: local("IBM Plex Sans"), url("../fonts/ScvbSans.woff2"); }\n')
        # vendored 目录里的同样内容 ⇒ 必须**不**命中(上游怎么写就怎么进包)
        with open(os.path.join(assets, "js", "juce", "vendored.css"), "w", encoding="utf-8") as fh:
            fh.write('a { font-family: "IBM Plex Sans", sans-serif; }\n')
        hits = check_text_assets(assets, verbose=False)
        if not any("bad.css" in p for p in hits):
            failures.append("CSS 的 font-family 里的 RFN 竟未命中(分发物仍会呈现上游名)")
        if not any("bad.js" in p for p in hits):
            failures.append("JS 字体栈字面量里的 RFN 竟未命中(STYLE_FALLBACK 那条路没被守住)")
        for _f, _why in (
            ("bad_tpl.js", "模板串里的字体栈(canvas 的 ctx.font 已在用模板串)"),
            ("bad_camel.js", "驼峰 el.style.fontFamily 的赋值"),
            ("bad_escape.js", "含转义引号的字体栈"),
            ("bad_shorthand.css", "CSS `font:` 简写"),
            ("bad_local.css", "@font-face 的 src: local(…)"),
        ):
            if not any(_f in p for p in hits):
                failures.append("%s 里的 RFN 竟未命中(%s 不在扫描面内)" % (_f, _why))
        if any("vendored" in p for p in hits):
            failures.append("vendored 的 web/js/juce 被判违规(排除失效,门禁会恒红)")
        # ⑧ 常用词不误伤:RFN "Source" 在标识符里到处都是,整文件 grep 会刷出几十条假红,
        #    而假红最后一定以「把这条扫描关掉」收场。判据必须只落在字体名上下文。
        with open(os.path.join(assets, "ok.js"), "w", encoding="utf-8") as fh:
            fh.write('import { sourceKind } from "./source-kind.js";\nconst n = cfg.source_channels;\n')
        if any("ok.js" in p for p in check_text_assets(assets, verbose=False)):
            failures.append("标识符里的 'source' 被判违规(判据溢出字体名上下文,会制造假红)")
        # ⑨ 读不出来的文本资源 ⇒ 判红。样例把坏字节塞在 RFN **词中间**:换成
        #    errors="replace" 接着扫的话,"IBM Ple?x Sans" 里 needle 找不到,门禁会静默放行。
        enc = os.path.join(tmp, "enc")
        os.makedirs(enc)
        with open(os.path.join(enc, "broken.css"), "wb") as fh:
            fh.write(b'a { font-family: "IBM Ple\xffx Sans"; }\n')
        if not any(
            "broken.css" in p and "UTF-8" in p for p in check_text_assets(enc, verbose=False)
        ):
            failures.append("非 UTF-8 的文本资源竟未判红(读不干净 = 无从断言,不能算通过)")

    if failures:
        for f in failures:
            print("!! " + f)
        sys.exit("字体保留名门禁自测失败(%d 项):门禁本身坏了,先修它" % len(failures))
    print(
        "-> 自测通过:漏改的呈现名必红(含版本/设计者/描述槽)、CSS/JS 字体栈里的 RFN 必红、"
        "署名记录与常用词不误伤、未登记字体必红"
    )


def main():
    args = sys.argv[1:]
    # 未知开关一律硬失败:静默把 `--verbos` 当成字体目录去扫,会扫出一个不存在的目录 ——
    # 那条路径下面会判红,但红的理由是错的,排查要多走一圈。
    unknown = [a for a in args if a.startswith("-") and a != "--self-test"]
    if unknown:
        sys.exit("!! 无法识别的参数 %s;用法见文件头注" % unknown)
    argv = [a for a in args if a != "--self-test"]
    if len(argv) > 1:
        sys.exit("!! 最多接受一个目录参数,收到 %s" % argv)
    if "--self-test" in args:
        self_test()
        if not argv:
            return
    # 默认口径:woff2 在 web/fonts,文本资源在 web/(进包面见 cmake/ScvbWebAssets.cmake)。
    # 给了目录参数就两样都扫它 —— 少扫一样等于门禁只剩一半,而输出看不出来。
    web_root = os.path.abspath(argv[0] if argv else os.path.join(HERE, "..", "web"))
    font_dir = web_root if argv else os.path.join(web_root, "fonts")
    problems = check_dir(font_dir) + check_text_assets(web_root)
    if problems:
        print("")
        for p in problems:
            print("!! " + p)
        sys.exit(
            "字体保留名断言失败(%d 项);改名与重新生成见 web/fonts/README.md 的「保留字体名(RFN)」一节"
            % len(problems)
        )
    print("-> %s:四款字体的呈现名与分发文本资源的字体栈全部通过 OFL-1.1 §3 断言" % web_root)


if __name__ == "__main__":
    main()
