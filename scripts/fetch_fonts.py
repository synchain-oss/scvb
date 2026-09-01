# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-  (编码声明必须在前两行内,故 SPDX 头占第 1 行、它占第 2 行)
"""生成 web/fonts/ 下的离线子集 WOFF2(SCVB 版,移植自 Bridge scripts/fetch_fonts.py)。

**只有本脚本联网**——运行期的 web/ 一律离线 @font-face,
全仓禁止 Google Fonts 域名引用(tokens.css 文件头纪律,CI 用 grep 断言零命中)。

两条取字体路线,按家族分工(不是历史包袱,是被 URL 长度上限逼出来的):
  · 拉丁三款:Google Fonts CSS2 `text=` 接口,Google 直接返回子集好的 WOFF2。
    拉丁字符集有上界(ASCII + 法语重音 + 几个符号,约 150),URL 恒在 1KB 内,永不触顶。
    另一个好处:`text=` 返回的是**请求字重的静态实例**,拿到手即所需字重,无默认实例陷阱。
  · Noto Sans SC:下载 google/fonts 上游**全量可变字体**,本地用 fontTools 子集化。
    CJK 子集已 768 字且随词条只增不减,而 `text=` 走 GET —— 实测 URL 约 7.1KB 尚可、
    7.5KB 时 Google **静默忽略 `text=`**,改返回 101 段 unicode-range 分片 CSS;
    老脚本正则只取第一段 `src:`,于是下载到一个 12 字形的碎片当成完整子集写进仓库,
    全程退出码 0。这类「静默给错东西」比报错危险得多,故 CJK 侧彻底不走 `text=`。
    (上游全量字体的 fvar 默认实例同为 Thin(100),与旧产物一致 —— tokens.css 那条
    `font-weight: 300 700` 描述符仍是中文不发丝细的唯一依靠,见 web/fonts/README.md。)

因此本脚本需要 `fontTools` + `brotli`(仅 Noto 子集化与 woff2 压缩用);
`--print-charset` / `--help` 不碰这两个包。

与 Bridge 原脚本的唯一实质差异:字符集不再手写常量,而是**扫描 web/ 下全部 .js 与 .html**
的字符串字面量/可见文本自动求并集。SCVB 三语文案上千条,手抄汉字常量必然漏字;
改文案 → 重跑本脚本即可,不必再同步维护一份汉字表。
覆盖是否真的够,由 scripts/check-font-coverage.py(gates 3h)拿 fontTools 逐字对着产物断言。

用法:
    python scripts/fetch_fonts.py [输出目录]      # 默认输出 <仓库根>/web/fonts
    python scripts/fetch_fonts.py --print-charset # 只扫描并打印两个子集(拉丁 + CJK),不联网
    python scripts/fetch_fonts.py --help          # 打印用法后退出,不联网

字形有增改时重跑本脚本,再确认 web/shared/tokens.css 的 @font-face 文件名一致即可。
网络不通时的回退口径见 web/fonts/README.md「离线回退」一节。
"""
import io, os, sys, re, urllib.parse, urllib.request, urllib.error

# Windows 控制台默认 cp936,打印字符集样本与中文报错时可能编码失败,先放宽两个流
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WEB = os.path.join(ROOT, "web")

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"
)

# ---- 扫描源 ----------------------------------------------------------------
# i18n.js 是界面文案的唯一真源(词条真源再往上是 05-ui-spec.md §5),但**不是唯一上屏源**:
# 各页 app.js / tab-*.js / viz.js 里有直接拼进 DOM 的字面量(单位、分隔符、兜底文案、
# monitor 侧未走 i18n 的标签)。只扫 i18n.js 就会漏掉它们,漏的字运行期是方块,
# 而既有门禁一条都查不出 —— 故扫描面按 F12 原案扩到 web/ 下**全部 .js**,外加全部 .html。
# web/js/juce/** 是 vendored JUCE 前端库,实测纯 ASCII,扫进来对子集零影响,不为它单开例外。
I18N_JS = os.path.join(WEB, "shared", "i18n.js")
JS_GLOB_DIRS = [WEB]
HTML_GLOB_DIRS = [WEB, os.path.join(ROOT, "web-preview")]

# ---- 兜底字符集(扫描结果之外必须保底存在的字形)-------------------------------
# 可打印 ASCII:数字/单位/占位符花括号/路径符号等,任何语言都用得到
ASCII_PRINTABLE = "".join(chr(c) for c in range(0x20, 0x7F))
# 法语重音(05 §5 fr 列用到的全集,大小写成对)
FRENCH = "ÀÂÄÇÈÉÊËÎÏÔŒÙÛÜàâäçèéêëîïôœùûü"
# UI 符号:间隔点/破折号/省略号/正负/度/警示三角(告警 banner、角度、dB 读数)
SYMBOLS = "·—…±°⚠"
LATIN_BASE = ASCII_PRINTABLE + FRENCH + SYMBOLS


def is_cjk(ch):
    """判定字符是否只能由 Noto Sans SC 提供字形(拉丁三款字体没有这些字形)。

    区间取宽:部首扩展/康熙部首/CJK 标点(、。「」·全角)/假名/统一表意文字/兼容表意/
    竖排标点/全角形式/扩展 B 以上。宁可多算进 CJK 子集,也不要漏字导致运行期方块。
    """
    o = ord(ch)
    return (
        0x2E80 <= o <= 0x9FFF
        or 0xF900 <= o <= 0xFAFF
        or 0xFE30 <= o <= 0xFE4F
        or 0xFF00 <= o <= 0xFFEF
        or 0x20000 <= o <= 0x3FFFF
    )


# 正则字面量判定:`/` 前的最后一个有效字符若属于这一集(或它在行首/文件首),`/` 开的是
# 正则而非除号。JS 无法只靠字符判除号/正则,必须看上文;这是通行的最小启发式。
REGEX_PREV = set("(,=:[!&|?{};+-*%~^<>") | {"\n"}
REGEX_PREV_WORDS = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "case", "do", "else", "yield", "await"}


def _regex_allowed(src, i):
    """src[i] == '/' 时,判断它是不是正则字面量的开头。"""
    j = i - 1
    while j >= 0 and src[j] in " \t\r":
        j -= 1
    if j < 0 or src[j] in REGEX_PREV:
        return True
    if src[j].isalnum() or src[j] == "_":  # 标识符/数字结尾 → 只可能是除号,除非是关键字
        k = j
        while k >= 0 and (src[k].isalnum() or src[k] == "_"):
            k -= 1
        return src[k + 1 : j + 1] in REGEX_PREV_WORDS
    return False


def js_strings(src):
    """从 JS 源码里取出全部字符串字面量的内容(跳过 // 与 /* */ 注释与正则字面量)。

    手写小状态机而非正则:注释里有大量中文说明(真源引文、纪律条款),那些字不是界面文案,
    混进子集会白白撑大 woff2;而 `https://` 这类内容又不能被「见到 // 就当注释」误伤。

    [F12] 扫描面从 i18n.js(纯字典,无正则字面量)扩到 web/ 全部 .js 后,**正则字面量必须
    单列一档**:`/https:\\/\\//` 这种里的 `\\/` 后面紧跟 `/`,当成「见到 // 即行注释」会把
    该行余下的字面量整段吞掉 —— 那是**漏字**,正是本脚本要防的那类线上方块。
    (反过来,把除号误判成正则至多多收几个字符,子集略大而已 —— 两侧不对称,故宁可多判正则。)
    """
    out, buf = [], []
    i, n = 0, len(src)
    state = None  # None / "line" / "block" / "regex" / 引号字符
    in_class = False  # 正则字符类 [...] 内,`/` 不终止正则
    while i < n:
        c = src[i]
        if state is None:
            if c == "/" and i + 1 < n and src[i + 1] == "/":
                state, i = "line", i + 2
                continue
            if c == "/" and i + 1 < n and src[i + 1] == "*":
                state, i = "block", i + 2
                continue
            if c == "/" and _regex_allowed(src, i):
                state, in_class = "regex", False
            elif c in "\"'`":
                state, buf = c, []
        elif state == "line":
            if c == "\n":
                state = None
        elif state == "block":
            if c == "*" and i + 1 < n and src[i + 1] == "/":
                state, i = None, i + 2
                continue
        elif state == "regex":
            if c == "\\":
                i += 2
                continue
            if c == "[":
                in_class = True
            elif c == "]":
                in_class = False
            elif c == "/" and not in_class:
                state = None
            elif c == "\n":  # 判错了(其实是除号):行尾收手,别把整个文件吞掉
                state = None
        else:  # 字符串内
            if c == "\\" and i + 1 < n:
                buf.append(src[i : i + 2])
                i += 2
                continue
            if c == "\n" and state != "`":
                # 只有模板串能跨行。走到这里说明引号是被误判开的(如判错的正则里含 '),
                # 行尾就收手 —— 否则会一路吞到下一个同名引号,把中间真正的字面量吃掉。
                out.append("".join(buf))
                state, buf = None, []
            elif c == state:
                out.append("".join(buf))
                state, buf = None, []
            else:
                buf.append(c)
        i += 1
    return out


ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "", "f": "", "v": "", "0": ""}


def unescape(s):
    """还原字符串字面量里的转义:\\uXXXX / \\xXX 要还原成真字符,否则重音字母会漏进子集。"""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c != "\\" or i + 1 >= n:
            out.append(c)
            i += 1
            continue
        e = s[i + 1]
        if e == "u" and i + 2 < n and s[i + 2] == "{":
            j = s.index("}", i)
            out.append(chr(int(s[i + 3 : j], 16)))
            i = j + 1
        elif e == "u":
            out.append(chr(int(s[i + 2 : i + 6], 16)))
            i += 6
        elif e == "x":
            out.append(chr(int(s[i + 2 : i + 4], 16)))
            i += 4
        else:
            out.append(ESCAPES.get(e, e))
            i += 2
    return "".join(out)


def html_text(src):
    """粗剥 HTML:去掉注释与 script/style 块,再去标签,剩下的当可见文本。

    只用来兜字形,宁滥勿缺——多收几个字符只是子集略大,漏收就是运行期方块。
    """
    src = re.sub(r"<!--.*?-->", " ", src, flags=re.S)
    src = re.sub(r"<(script|style)\b.*?</\1>", " ", src, flags=re.S | re.I)
    return re.sub(r"<[^>]*>", " ", src)


def scan_sources():
    """返回 (字符集合, 来源明细列表)。i18n.js 缺失直接失败——它是文案唯一真源。"""
    if not os.path.isfile(I18N_JS):
        sys.exit("!! 找不到 %s;i18n.js 是文案真源,必须先有它才能定字符集" % I18N_JS)
    chars, detail = set(), []

    # [F12] 全部 .js,不再只有 i18n.js。明细按相对路径排序,保证不同机器上重跑输出稳定。
    js_paths = []
    for base in JS_GLOB_DIRS:
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                if fn.endswith(".js"):
                    js_paths.append(os.path.join(dirpath, fn))
    if not js_paths:
        sys.exit("!! %s 下一个 .js 都没扫到;扫描面坏了,不能拿这个结果去子集化" % WEB)
    for p in sorted(js_paths, key=lambda q: os.path.relpath(q, ROOT).replace("\\", "/")):
        with open(p, "r", encoding="utf-8") as f:
            lits = js_strings(f.read())
        text = "".join(unescape(s) for s in lits)
        chars |= set(text)
        detail.append((os.path.relpath(p, ROOT), len(lits), len(set(text))))

    for base in HTML_GLOB_DIRS:
        for dirpath, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if not fn.endswith(".html"):
                    continue
                p = os.path.join(dirpath, fn)
                with open(p, "r", encoding="utf-8") as f:
                    t = html_text(f.read())
                chars |= set(t)
                detail.append((os.path.relpath(p, ROOT), 1, len(set(t))))
    return chars, detail


def build_charsets():
    """扫描结果 + 兜底集 → (拉丁子集串, CJK 子集串)。两串都按码位排序,保证重跑 URL 稳定。"""
    scanned, detail = scan_sources()
    scanned = {c for c in scanned if c.isprintable() and not c.isspace()}
    cjk = sorted(c for c in scanned if is_cjk(c))
    latin = sorted(set(LATIN_BASE) | {c for c in scanned if not is_cjk(c)})
    return "".join(latin), "".join(cjk), detail


def fetch(url, tries=3, timeout=180):
    # 超时给到 180s:Noto 上游全量字体 17MB 左右,30s 在慢网上会假性失败
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    last = None
    for k in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except (urllib.error.URLError, OSError) as e:  # 含超时/DNS/被墙
            last = e
            print("   .. 第 %d 次失败:%s" % (k + 1, e))
    raise SystemExit(
        "!! 取字体失败(%s)。DAW 侧运行期不联网,但本脚本必须联网才能子集化。\n"
        "   回退口径见 web/fonts/README.md「离线回退」:先放 Bridge 的占位子集,"
        "网络恢复后必须重跑本脚本。" % last
    )


# Noto Sans SC 上游全量可变字体(google/fonts 是 Google Fonts 自己的发布仓,与 gstatic 同源同版本;
# 版本号写在 name ID 5,取回后打印,便于与 THIRD-PARTY-NOTICES.md 的版本格对账)。
NOTO_TTF_URL = (
    "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
)

# CSS2 `text=` 的 URL 安全上限。实测 7078 字节可用、7561 字节起 Google 静默改返回分片 CSS,
# 故阈值取 7000 并且**硬失败**:拉丁集有上界(约 150 字符 ≈ 600 字节),真撞上说明扫描面出了错,
# 那种时候继续跑只会把错东西写进仓库。
CSS2_URL_LIMIT = 7000


def fetch_css2_subset(family, text):
    """走 CSS2 `text=` 取一款子集 WOFF2 的字节。任何「不是我要的东西」一律硬失败。"""
    q = {"family": family, "display": "swap", "text": text}
    url = "https://fonts.googleapis.com/css2?" + urllib.parse.urlencode(
        q, quote_via=urllib.parse.quote
    )
    if len(url) > CSS2_URL_LIMIT:
        sys.exit(
            "!! %s 的 text= URL 长 %d 字节,超过安全上限 %d。\n"
            "   Google 不会报 414,而是**静默忽略 text=** 改返回 unicode-range 分片 CSS,\n"
            "   取第一段就得到一个几十字形的碎片。该家族需改走本地子集化(见 Noto 的做法)。"
            % (family, len(url), CSS2_URL_LIMIT)
        )
    css = fetch(url).decode("utf-8")
    # `text=` 生效时 Google 只回**一个** @font-face;被忽略时回的是按 unicode-range 切开的
    # 上百段分片(注意:两种情况都带 unicode-range,所以只有段数可用来判)。
    # 这一句是「静默给错东西」的第一道信号 —— 2026-08 那次 12 字形碎片进仓,
    # 缺的就是它(下载成功、退出码 0、文件也写了,只是内容是错的)。
    n_faces = css.count("@font-face")
    if n_faces != 1:
        sys.exit(
            "!! %s:Google 未按 text= 子集化(回了 %d 段 @font-face 的分片 CSS)。\n"
            "   多半是 URL 过长被忽略;分片里的任何一段都不是完整子集,不能写进仓库。"
            % (family, n_faces)
        )
    m = re.search(r"src:\s*url\((https://[^)]+)\)", css)
    if not m:
        sys.exit("!! CSS 里没找到 woff2 url:%s\n%s" % (family, css[:800]))
    data = fetch(m.group(1))
    assert_covers(data, family, text)
    return data


# 拿到的字体至少要覆盖所请求字符的这个比例。够不够是 gates 3h 逐字判的,这里只拦「明显不是
# 我要的那份东西」:分片碎片的覆盖率是个位数百分比,而正常子集只会漏掉该家族本身没有的
# 那几个符号(如 ⚠ ① ② 不在拉丁三款里),覆盖率 97% 以上。
MIN_COVERAGE = 0.90


def assert_covers(font_bytes, family, text):
    """拿 fontTools 读回刚下载的字体,断言它确实覆盖了绝大部分所请求字符。"""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit("!! 需要 fontTools 与 brotli:pip install fonttools brotli")
    with TTFont(io.BytesIO(font_bytes), lazy=True) as f:
        cmap = f.getBestCmap()
    want = set(text)
    got = {c for c in want if ord(c) in cmap}
    ratio = len(got) / len(want) if want else 1.0
    if ratio < MIN_COVERAGE:
        sys.exit(
            "!! %s:取回的字体只覆盖了 %d/%d(%.0f%%)个请求字符,不是完整子集,拒绝写入。"
            % (family, len(got), len(want), ratio * 100)
        )
    lack = sorted(want - got)
    if lack:
        # 不判负:该家族本身没有的字形由字体栈逐字回退到 Noto(tokens.css 的三条栈都以它兜底)
        print("   %-26s 该家族无字形 %d 个,交由字体栈回退:%s" % (family, len(lack), "".join(lack)))


def subset_local(ttf_bytes, text):
    """用 fontTools 把上游全量可变字体子集成 WOFF2 字节。

    延迟 import:`--print-charset` / `--help` 不该因为没装 fontTools 就跑不了。
    保留 fvar/gvar/HVAR(可变轴)与全部 layout feature —— CJK 字重要跟随元素,
    子集掉 wght 轴会让 tokens.css 的 `font-weight: 300 700` 失效,中文变发丝细体。
    """
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit("!! Noto 侧需要 fontTools 与 brotli:pip install fonttools brotli")

    src = io.BytesIO(ttf_bytes)
    ver = TTFont(src, lazy=True)["name"].getDebugName(5)
    src.seek(0)
    print("   Noto 上游全量:%d bytes,%s" % (len(ttf_bytes), ver))

    opts = subset.Options()
    opts.flavor = "woff2"
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]  # 保留版权/许可名条目:OFL-1.1 §2 要求的署名随产物分发
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.notdef_outline = True  # 缺字画方框而不是画空白:上屏方块是能看见的 bug,空白不是
    font = subset.load_font(src, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=text)
    subsetter.subset(font)
    buf = io.BytesIO()
    subset.save_font(font, buf, opts)
    return buf.getvalue()


# ---- OFL-1.1 §3 改名 --------------------------------------------------------
# [SL-267] 只有「上游带 RFN 且分发名要去掉 RFN」的家族才进这张表。
# 输出文件名 -> (分发家族名, 分发 PostScript 名, 上游保留字体名 RFN)
#
# Space Grotesk 不在表内:版权行无 "with Reserved Font Name",名字不受 §3 限制。
# Noto Sans SC 也不在表内:它的 RFN 是 "Source"(nameID 0 逐字写着),而分发名
# 'Noto Sans SC' 本就不含该词 —— 无需改名,但仍由 check-font-names.py 守住不回归。
RENAME = {
    "ScvbSans.woff2": ("SCVB Sans", "ScvbSans-Regular", "Plex"),
    "ScvbMono.woff2": ("SCVB Mono", "ScvbMono-Regular", "Plex"),
}

# OFL-1.1 §2 要求「每份拷贝都包含上述版权声明与本许可证」。上游子集的 name 表里
# 有 nameID 0(版权)但可能缺 13(许可证声明),改名时一并补齐。
OFL_LICENSE_DESC = (
    "This Font Software is licensed under the SIL Open Font License, Version 1.1. "
    "This license is available with a FAQ at https://scripts.sil.org/OFL"
)
# 呈现给用户的主字体名相关记录:家族 / 唯一 ID / 全名 / PostScript 名 / 排版家族与子族
NAME_IDS_FAMILY = (1, 3, 4, 6, 16, 17)
# 署名记录:0 版权 / 7 商标 / 13 许可证声明 / 14 许可证 URL。一张表两重身份:
#   • **永不改写**,随分发逐字保留(OFL-1.1 §2 的随附义务);
#   • **不参与下面的 RFN 复核** —— OFL 惯例的版权行本身就写着 "with Reserved Font Name 'X'",
#     商标声明同理(本仓 NotoSansSC 的 nameID 7 逐字是 "Source is a trademark of Adobe…"),
#     那是要求保留的署名,不是残留的违规名;算进断言只会制造恒红。
# scripts/check-font-names.py 用同一口径做门禁侧断言,并在导入时与 RENAME 对拍,不会静默漂移。
NAME_IDS_ATTRIBUTION = (0, 7, 13, 14)


def _unique_id(existing, ps_name):
    """nameID 3 惯例是 "<version>;<vendor>;<postscript name>",只换后两段。"""
    version = existing.split(";")[0].strip() if existing else ""
    return ";".join(p for p in (version, "Synchain", ps_name) if p)


def rename_font(path, family, ps_name, reserved):
    """就地重写子集 woff2 的 `name` 表,去掉上游保留字体名(OFL-1.1 §3)。

    §3 限制的是「呈现给用户的主字体名」,故**必须改 name 表** —— 只改文件名与 CSS
    family 不够:装到系统里、或被 DevTools/PDF 导出读出来的仍是上游名。
    §2 要求的署名(NAME_IDS_ATTRIBUTION)原样保留,并补齐可能缺失的 13。
    """
    from fontTools.ttLib import TTFont  # 仅改名家族需要;见模块顶部「依赖」

    font = TTFont(path)
    name = font["name"]
    # §2 的署名记录:改名前后必须逐字不变(下面 keep_before/keep_after 对比)
    keep_before = {
        (r.nameID, r.platformID, r.platEncID, r.langID): r.toUnicode()
        for r in name.names
        if r.nameID in NAME_IDS_ATTRIBUTION
    }
    # name 记录按 (platformID, platEncID, langID) 分槽,逐槽改,Mac/Windows 记录都覆盖到
    for slot in sorted({(r.platformID, r.platEncID, r.langID) for r in name.names}):

        def current(nid, default="", _slot=slot):
            rec = name.getName(nid, *_slot)
            return rec.toUnicode() if rec is not None else default

        subfamily = current(2, "Regular")
        values = {
            1: family,
            3: _unique_id(current(3), ps_name),
            4: "%s %s" % (family, subfamily),
            6: ps_name,
            16: family,
            17: subfamily,
        }
        for nid in NAME_IDS_FAMILY:
            if name.getName(nid, *slot) is not None:
                name.setName(values[nid], nid, *slot)
        if name.getName(13, *slot) is None:
            name.setName(OFL_LICENSE_DESC, 13, *slot)

    # fail-closed 复核:署名以外的**每一条** name 记录都不得残留 RFN —— 不只查
    # NAME_IDS_FAMILY 那六条,漏改的槽位(如 nameID 5 的版本串)同样会呈现给用户。
    # 大小写双侧 casefold:上游若写成 "plex"/"PLEX" 同样要命中。
    needle = reserved.casefold()
    stray = sorted(
        {
            r.toUnicode()
            for r in name.names
            if r.nameID not in NAME_IDS_ATTRIBUTION and needle in r.toUnicode().casefold()
        }
    )
    if stray:  # 改名没改干净就不要产出一个仍带 RFN 的分发物
        raise SystemExit("!! %s 的 name 表仍含保留字体名 %r: %s" % (path, reserved, stray))
    keep_after = {
        (r.nameID, r.platformID, r.platEncID, r.langID): r.toUnicode()
        for r in name.names
        if r.nameID in NAME_IDS_ATTRIBUTION
    }
    if any(keep_after.get(k) != v for k, v in keep_before.items()):  # §2 署名被动过 = 不合规
        raise SystemExit("!! %s 的署名记录(nameID %s)被改动" % (path, NAME_IDS_ATTRIBUTION))
    font.save(path)


USAGE = """用法: python scripts/fetch_fonts.py [输出目录] [--print-charset] [--help]
  默认输出 <仓库根>/web/fonts,覆盖四个 woff2。
  --print-charset  只扫描并打印拉丁/CJK 两个子集,不联网、不写文件。
  --help, -h       打印本说明并退出 0。"""


def main():
    # 显式解析:本脚本会联网并覆盖 web/fonts/ 四个文件,参数打错必须报错退出,
    # 不能像「非 -- 开头即目录、-- 开头即 flag」那样把 --help / -h / 拼错的 flag 静默吞掉
    # (-h 曾会被当成输出目录,在 cwd 下建一个名为 -h 的目录)。口径与同批两个 .mjs 一致。
    args, flags = [], set()
    for a in sys.argv[1:]:
        if a in ("--help", "-h"):
            print(USAGE)
            return
        if a == "--print-charset":
            flags.add(a)
        elif a.startswith("-"):
            sys.exit("!! 无法识别的参数 %s\n%s" % (a, USAGE))
        else:
            args.append(a)
    if len(args) > 1:
        sys.exit("!! 只接受 0 或 1 个位置参数(输出目录)\n%s" % USAGE)
    out = os.path.abspath(args[0] if args else os.path.join(WEB, "fonts"))

    latin, cjk, detail = build_charsets()
    print("扫描来源:")
    for rel, nlit, nchar in detail:
        print("   %-40s 字面量 %5d 条,去重字符 %4d" % (rel, nlit, nchar))
    print("拉丁子集 %d 字符;CJK 子集 %d 字符" % (len(latin), len(cjk)))
    if "--print-charset" in flags:
        # 两个子集都打印:法语重音有没有真的扫进来,只能从拉丁集看出来
        print("LATIN:", latin)
        print("CJK:", cjk)
        return

    latin_fonts = [
        # (输出文件名, Google 家族+字重)
        # 字重按 tokens.css 三分工取单一档:Grotesk 只用于标题/数值/CTA(600),
        # Sans/Mono 正文与标签(400)。@font-face 声明的 font-weight 区间比这宽,
        # 是给字体栈回退留口子——配 body 的 font-synthesis:none,缺字重时宁可回退不合成伪粗。
        #
        # [SL-267] 输出名不再用上游名:IBM Plex 两款的 RFN 是 "Plex"(见 RENAME 与
        # THIRD-PARTY-NOTICES.md),子集化 = Modified Version,按 OFL-1.1 §3 不得使用 RFN。
        ("SpaceGrotesk.woff2", "Space Grotesk:wght@600"),
        ("ScvbSans.woff2", "IBM Plex Sans:wght@400"),
        ("ScvbMono.woff2", "IBM Plex Mono:wght@400"),
    ]

    os.makedirs(out, exist_ok=True)
    for fname, family in latin_fonts:
        data = fetch_css2_subset(family, latin)
        path = os.path.join(out, fname)
        with open(path, "wb") as f:
            f.write(data)
        if fname in RENAME:
            rename_font(path, *RENAME[fname])
            print("OK %-20s %7d bytes  (CSS2 text= + OFL §3 改名)" % (fname, os.path.getsize(path)))
        else:
            print("OK %-20s %7d bytes  (CSS2 text=)" % (fname, len(data)))

    # Noto 的字符集是 CJK + 全部拉丁:三条字体栈都以 'Noto Sans SC' 为最后一道内嵌回退,
    # 拉丁三款没有的字形(如 ⚠ ① ②)全靠它兜住,所以它必须是四款里字符集最全的一款。
    noto = subset_local(fetch(NOTO_TTF_URL), cjk + latin)
    with open(os.path.join(out, "NotoSansSC.woff2"), "wb") as f:
        f.write(noto)
    print("OK %-20s %7d bytes  (上游全量 + 本地 fontTools 子集)" % ("NotoSansSC.woff2", len(noto)))
    print("-> ", out)


if __name__ == "__main__":
    main()
