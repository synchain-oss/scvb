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

依赖:fontTools + brotli(解 woff2 必需):`pip install fonttools brotli`

用法:
    python scripts/check-font-names.py              # 扫 web/fonts,命中即非零退出(gates 3k)
    python scripts/check-font-names.py --self-test  # 先验门禁本身:坏样例必红、署名样例不误伤
    python scripts/check-font-names.py <字体目录>   # 指定目录(自测与 CI 之外一般用不到)
"""
import glob, os, shutil, sys, tempfile

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

# 与 fetch_fonts.py 对拍:生成侧与门禁侧各自独立声明(门禁不该把判据整个托付给被审查的
# 那一侧),但两处一旦漂移就会出现「生成侧按新词改名、门禁侧仍按旧词断言」的静默失效,
# 故在导入期就把差异变成硬失败。
for _fname, (_family, _ps, _rfn) in ff.RENAME.items():
    if RESERVED.get(_fname) != _rfn:
        sys.exit(
            "!! RESERVED 与 fetch_fonts.RENAME 对 %s 的 RFN 不一致(%r vs %r);两处须同步"
            % (_fname, RESERVED.get(_fname), _rfn)
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
        # ② 漏在 1/3/4/6/16/17 之外的槽位(版本串)⇒ 同样必须命中,否则扫描面太窄
        if not scan_font(sample(5, "Version 2.3 (IBM Plex Mono subset)"), "Plex"):
            failures.append("nameID 5 里的 RFN 竟未命中(扫描面窄到只剩呈现名)")
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

    if failures:
        for f in failures:
            print("!! " + f)
        sys.exit("字体保留名门禁自测失败(%d 项):门禁本身坏了,先修它" % len(failures))
    print("-> 自测通过:漏改的呈现名必红、署名记录不误伤、未登记字体必红")


def main():
    args = sys.argv[1:]
    # 未知开关一律硬失败:静默把 `--verbos` 当成字体目录去扫,会扫出一个不存在的目录 ——
    # 那条路径下面会判红,但红的理由是错的,排查要多走一圈。
    unknown = [a for a in args if a.startswith("-") and a != "--self-test"]
    if unknown:
        sys.exit("!! 无法识别的参数 %s;用法见文件头注" % unknown)
    argv = [a for a in args if a != "--self-test"]
    if len(argv) > 1:
        sys.exit("!! 最多接受一个字体目录参数,收到 %s" % argv)
    if "--self-test" in args:
        self_test()
        if not argv:
            return
    font_dir = os.path.abspath(argv[0] if argv else os.path.join(HERE, "..", "web", "fonts"))
    problems = check_dir(font_dir)
    if problems:
        print("")
        for p in problems:
            print("!! " + p)
        sys.exit(
            "字体保留名断言失败(%d 项);改名与重新生成见 web/fonts/README.md 的「保留字体名(RFN)」一节"
            % len(problems)
        )
    print("-> %s:四款字体的呈现名全部通过 OFL-1.1 §3 断言" % font_dir)


if __name__ == "__main__":
    main()
