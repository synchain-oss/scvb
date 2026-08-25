# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-  (编码声明必须在前两行内,故 SPDX 头占第 1 行、它占第 2 行)
"""字体子集覆盖门禁(gates 3h / F12):重算所需字符集,逐字对着 web/fonts/*.woff2 断言。

**这道门禁存在的理由**:web/fonts/README.md 自己写着「不重跑 fetch_fonts.py 就会上屏方块,
而 CI 查不出」—— 那句话在 2026-08-17 到 2026-08-25 之间被兑现了:i18n.js 连改四批词条,
子集没重跑,feature/v1 主线带着 98 个无字形字符(96 个汉字,含「卡箍」;`’`/`✕` 四款全缺)
合入,没有任何一道机检发现。人眼审 PR diff 永远看不出「这个新汉字字体里没有」。

判定口径(与 tokens.css 的三条字体栈一致):
  三条栈都是「拉丁族 → 'Noto Sans SC' → 系统字体」,且 Noto 子集按请求含全部拉丁字符,
  故只要某字**四款里一款都没有**,运行期必然落到系统字体或方块 —— 这是 FAIL。
  某字仅拉丁三款没有、Noto 有,是**设计内的逐字回退**(如 `⚠`,见 README),记 [INFO] 不判红。

真源单一:所需字符集直接 import fetch_fonts.build_charsets(),与生成侧同一份代码。
本脚本另立一档而不并进 fetch_fonts.py,是因为 fetch_fonts.py 刻意零依赖(不需要 fontTools),
而校验必须读 woff2 的 cmap —— 依赖只压在被校验的这一侧。

用法:
    python scripts/check-font-coverage.py            # 缺字即非零退出(gates 3h)
    python scripts/check-font-coverage.py --verbose  # 另打印每款字体的逐字亏空明细
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

try:
    from fontTools.ttLib import TTFont
except ImportError:
    # 硬失败,不是跳过。缺依赖判绿 = 假绿,比判红危险得多(同 gates 3d/3e 的 python/node 守卫)。
    sys.exit(
        "!! 缺 fontTools:pip install fonttools brotli\n"
        "   (woff2 解压需要 brotli;两者缺一本门禁都跑不了,而跑不了必须判红。)"
    )

import fetch_fonts as ff

# 与 fetch_fonts.py 的 fonts 表一一对应;改文件名两处同步
LATIN_FONTS = ["SpaceGrotesk.woff2", "IBMPlexSans.woff2", "IBMPlexMono.woff2"]
CJK_FONT = "NotoSansSC.woff2"
ALL_FONTS = LATIN_FONTS + [CJK_FONT]

# ---- 四款上游家族本身就没有的字形 ------------------------------------------------
# 只放**重跑 fetch_fonts.py 也不可能补上**的字:实测 Space Grotesk 2.000 / IBM Plex Sans
# 3.201 / IBM Plex Mono 2.3 / Noto Sans SC 2.004 的全量字体 cmap 里都没有这两个码位。
# 它们现在靠宿主系统字体上屏(Windows/WebView2 = Segoe UI Symbol,画得出,但不是我们的字面),
# 真正的修法是把词条换成四款都有的字形(`✕`→`×` U+00D7,`⇕`→`↕` U+2195),属 UI 侧改动。
#
# 这张表会自己保鲜,不会烂成「加进去就永远绿」的白名单:下面两条反向断言都判红 ——
#   · 条目其实有字形了(换了字体/上游补了字)→ 必须删条目;
#   · 条目已不再被任何文案用到 → 必须删条目。
KNOWN_ABSENT_UPSTREAM = {
    "⇕": "纵向缩放读数(tab-wave.js 的「⇕ 34」);建议换 ↕ U+2195",
    "✕": "选区 chip / 面板标题栏的关闭键(output/index.html);建议换 × U+00D7",
}


def font_charset(path):
    """读 woff2 的 cmap,返回它真正有字形的码位集合。"""
    with TTFont(path, lazy=True) as f:
        return set(f.getBestCmap().keys())


def fmt(chars):
    """把缺字列表排成 `字(U+XXXX)` 串,评审能直接看出缺的是哪个字。"""
    return " ".join("%s(U+%04X)" % (c, ord(c)) for c in sorted(chars))


def main():
    verbose = "--verbose" in sys.argv[1:]
    for a in sys.argv[1:]:
        if a != "--verbose":
            sys.exit("!! 无法识别的参数 %s\n用法: python scripts/check-font-coverage.py [--verbose]" % a)

    fonts_dir = os.path.join(ROOT, "web", "fonts")
    missing_files = [f for f in ALL_FONTS if not os.path.isfile(os.path.join(fonts_dir, f))]
    if missing_files:
        sys.exit("!! web/fonts/ 缺文件: %s" % ", ".join(missing_files))

    latin, cjk, _detail = ff.build_charsets()
    required = set(latin) | set(cjk)

    have = {}
    for f in ALL_FONTS:
        have[f] = font_charset(os.path.join(fonts_dir, f))

    union = set().union(*have.values())
    tofu = {c for c in required if ord(c) not in union}

    print("所需字符 %d(拉丁 %d + CJK %d);四款字体合计有字形 %d" % (len(required), len(latin), len(cjk), len(union)))
    for f in ALL_FONTS:
        print("   %-20s 字形 %5d" % (f, len(have[f])))

    # Noto 是三条栈共同的兜底,按请求应覆盖全部所需字符;它缺什么就是真缺什么
    noto_gap = {c for c in required if ord(c) not in have[CJK_FONT]}
    latin_only_gap = {c for c in set(latin) if all(ord(c) not in have[f] for f in LATIN_FONTS)}

    if latin_only_gap - tofu:
        print("[INFO] 拉丁三款无字形、由 Noto 逐字回退(设计内,见 web/fonts/README.md):%s"
              % fmt(latin_only_gap - tofu))
    if verbose:
        for f in ALL_FONTS:
            gap = {c for c in (set(latin) if f in LATIN_FONTS else required) if ord(c) not in have[f]}
            print("[明细] %s 亏空 %d:%s" % (f, len(gap), fmt(gap)))

    # ---- 白名单保鲜:两条反向断言,防它烂成「加进去就永远绿」 ----
    stale = []
    for c in sorted(KNOWN_ABSENT_UPSTREAM):
        if ord(c) in union:
            stale.append("%s(U+%04X)现在有字形了,请从 KNOWN_ABSENT_UPSTREAM 删除" % (c, ord(c)))
        elif c not in required:
            stale.append("%s(U+%04X)已无任何文案使用,请从 KNOWN_ABSENT_UPSTREAM 删除" % (c, ord(c)))
    if stale:
        print("[FAIL] 上游缺字白名单已过期:")
        for s in stale:
            print("       " + s)
        return 1

    allowed = {c for c in tofu if c in KNOWN_ABSENT_UPSTREAM}
    if allowed:
        print("[INFO] 四款上游家族本身没有、由宿主系统字体上屏(白名单,重跑也补不上):")
        for c in sorted(allowed):
            print("       %s(U+%04X) — %s" % (c, ord(c), KNOWN_ABSENT_UPSTREAM[c]))

    tofu -= allowed
    if tofu:
        print("[FAIL] %d 个字符四款字体都没有字形,运行期必然上屏方块/回退系统字体:" % len(tofu))
        print("       %s" % fmt(tofu))
        print("       修复:python scripts/fetch_fonts.py(需联网),再同步 web/fonts/README.md 的字符计数。")
        return 1
    if noto_gap - allowed:
        # 走不到这里(noto_gap ⊆ tofu ∪ allowed),留着防将来把 Noto 的请求集改窄而无人察觉
        print("[FAIL] Noto 兜底缺 %d 字:%s" % (len(noto_gap - allowed), fmt(noto_gap - allowed)))
        return 1
    print("[PASS] 所需 %d 个字符全部有字形(白名单 %d 个除外,见上)" % (len(required), len(allowed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
