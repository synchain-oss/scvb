# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-  (编码声明必须在前两行内,故 SPDX 头占第 1 行、它占第 2 行)
"""生成 web/fonts/ 下的离线子集 WOFF2(SCVB 版,移植自 Bridge scripts/fetch_fonts.py)。

用 Google Fonts CSS2 `text=` 接口:Google 直接返回按传入字符子集好的 WOFF2,
无需本地安装 fonttools/brotli。**只有本脚本联网**——运行期的 web/ 一律离线 @font-face,
全仓禁止 Google Fonts 域名引用(tokens.css 文件头纪律,CI 用 grep 断言零命中)。

与 Bridge 原脚本的唯一实质差异:字符集不再手写常量,而是**扫描 web/shared/i18n.js**
(三语字典的全部字符串字面量)自动求并集。SCVB 三语文案上千条,手抄汉字常量必然漏字;
改文案 → 重跑本脚本即可,不必再同步维护一份汉字表。

用法:
    python scripts/fetch_fonts.py [输出目录]      # 默认输出 <仓库根>/web/fonts
    python scripts/fetch_fonts.py --print-charset # 只扫描并打印两个子集(拉丁 + CJK),不联网
    python scripts/fetch_fonts.py --help          # 打印用法后退出,不联网

字形有增改时重跑本脚本,再确认 web/shared/tokens.css 的 @font-face 文件名一致即可。
网络不通时的回退口径见 web/fonts/README.md「离线回退」一节。
"""
import os, sys, re, urllib.parse, urllib.request, urllib.error

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
# i18n.js 是全部界面文案的唯一真源(词条真源再往上是 05-ui-spec.md §5);
# T28 静态页落地后,页面里若有不走 data-t 的字面文本,把 .html 一并扫进来(下方 html_text)。
I18N_JS = os.path.join(WEB, "shared", "i18n.js")
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


def js_strings(src):
    """从 JS 源码里取出全部字符串字面量的内容(跳过 // 与 /* */ 注释)。

    手写小状态机而非正则:注释里有大量中文说明(真源引文、纪律条款),那些字不是界面文案,
    混进子集会白白撑大 woff2;而 `https://` 这类内容又不能被「见到 // 就当注释」误伤。
    前提:i18n.js 是纯字典 + 两个函数,不含正则字面量(含了会把 / 误判成除号,无副作用但需留意)。
    """
    out, buf = [], []
    i, n = 0, len(src)
    state = None  # None / "line" / "block" / 引号字符
    while i < n:
        c = src[i]
        if state is None:
            if c == "/" and i + 1 < n and src[i + 1] == "/":
                state, i = "line", i + 2
                continue
            if c == "/" and i + 1 < n and src[i + 1] == "*":
                state, i = "block", i + 2
                continue
            if c in "\"'`":
                state, buf = c, []
        elif state == "line":
            if c == "\n":
                state = None
        elif state == "block":
            if c == "*" and i + 1 < n and src[i + 1] == "/":
                state, i = None, i + 2
                continue
        else:  # 字符串内
            if c == "\\" and i + 1 < n:
                buf.append(src[i : i + 2])
                i += 2
                continue
            if c == state:
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

    with open(I18N_JS, "r", encoding="utf-8") as f:
        lits = js_strings(f.read())
    text = "".join(unescape(s) for s in lits)
    chars |= set(text)
    detail.append((os.path.relpath(I18N_JS, ROOT), len(lits), len(set(text))))

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


def fetch(url, tries=3):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    last = None
    for k in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except (urllib.error.URLError, OSError) as e:  # 含超时/DNS/被墙
            last = e
            print("   .. 第 %d 次失败:%s" % (k + 1, e))
    raise SystemExit(
        "!! 取字体失败(%s)。DAW 侧运行期不联网,但本脚本必须联网才能子集化。\n"
        "   回退口径见 web/fonts/README.md「离线回退」:先放 Bridge 的占位子集,"
        "网络恢复后必须重跑本脚本。" % last
    )


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
        print("   %-28s 字面量 %4d 条,去重字符 %4d" % (rel, nlit, nchar))
    print("拉丁子集 %d 字符;CJK 子集 %d 字符" % (len(latin), len(cjk)))
    if "--print-charset" in flags:
        # 两个子集都打印:法语重音有没有真的扫进来,只能从拉丁集看出来
        print("LATIN:", latin)
        print("CJK:", cjk)
        return

    fonts = [
        # (输出文件名, Google 家族+字重, 子集字符)
        # 字重按 tokens.css 三分工取单一档:Grotesk 只用于标题/数值/CTA(600),
        # Sans/Mono 正文与标签(400)。@font-face 声明的 font-weight 区间比这宽,
        # 是给字体栈回退留口子——配 body 的 font-synthesis:none,缺字重时宁可回退不合成伪粗。
        ("SpaceGrotesk.woff2", "Space Grotesk:wght@600", latin),
        ("IBMPlexSans.woff2", "IBM Plex Sans:wght@400", latin),
        ("IBMPlexMono.woff2", "IBM Plex Mono:wght@400", latin),
        # 可变字重 400..700:CJK 字重跟随元素(小标签 400,标题/CTA 600),单文件覆盖全区间
        ("NotoSansSC.woff2", "Noto Sans SC:wght@400..700", cjk + latin),
    ]

    os.makedirs(out, exist_ok=True)
    for fname, family, text in fonts:
        q = {"family": family, "display": "swap", "text": text}
        url = "https://fonts.googleapis.com/css2?" + urllib.parse.urlencode(
            q, quote_via=urllib.parse.quote
        )
        if len(url) > 7500:
            # text= 走 GET,URL 过长会被 Google 以 414 拒掉;真到这一步就得改子集策略
            print("!! URL 长 %d 字节,可能超服务端上限" % len(url))
        css = fetch(url).decode("utf-8")
        m = re.search(r"src:\s*url\((https://[^)]+)\)", css)
        if not m:
            print("!! CSS 里没找到 woff2 url:", family)
            print(css[:800])
            sys.exit(2)
        data = fetch(m.group(1))
        with open(os.path.join(out, fname), "wb") as f:
            f.write(data)
        print("OK %-20s %7d bytes" % (fname, len(data)))
    print("-> ", out)


if __name__ == "__main__":
    main()
