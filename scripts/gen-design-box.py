# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-  (编码声明必须在前两行内,故 SPDX 头占第 1 行、它占第 2 行)
"""gen-design-box.py — 从 web/shared/design-box.js 生成 src/core/DesignBox.h。

设计盒常量唯一真源 = web/shared/design-box.js(05 §1.2;07 T27 设计定稿回流①)。
C++ 侧(src/core/DesignBox.h)由本脚本在构建/提交期生成,任何位置不得二次硬编码
这些数字(消除 Bridge 双处硬编码的技术债,research/02 §3.1)。

本脚本确定性输出:同一份 design-box.js 永远生成逐字节相同的 DesignBox.h。
design-box.js 被纪律约束为「单个对象字面量 + 纯数字」(见该文件头注释),因此本脚本
用一个小型递归下降解析器静态解析,不执行 JS、不引入依赖。

用法:
    python scripts/gen-design-box.py              # 生成 src/core/DesignBox.h
    python scripts/gen-design-box.py --check      # 与已提交的 DesignBox.h 对拍,不一致退出 1
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DESIGN_BOX_JS = REPO_ROOT / "web" / "shared" / "design-box.js"
DESIGN_BOX_H = REPO_ROOT / "src" / "core" / "DesignBox.h"

_IDENT_RE = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*")
_NUM_RE = re.compile(r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")


def _tokenize(text):
    """把 JS 对象字面量切成 token 流。数字保留原始字符串以便逐字回写。"""
    tokens = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c in "{}[]:,":
            tokens.append(c)
            i += 1
            continue
        m = _NUM_RE.match(text, i)
        if m:
            tokens.append(("num", m.group(0)))
            i = m.end()
            continue
        m = _IDENT_RE.match(text, i)
        if m:
            tokens.append(("ident", m.group(0)))
            i = m.end()
            continue
        raise ValueError("design-box.js 出现无法解析的字符 %r(偏移 %d)" % (c, i))
    return tokens


class _Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.i = 0

    def peek(self):
        return self.tokens[self.i] if self.i < len(self.tokens) else None

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def parse_object(self):
        obj = {}
        while True:
            tok = self.take()
            if tok == "}":
                return obj
            if not (isinstance(tok, tuple) and tok[0] == "ident"):
                raise ValueError("期望对象键,得到 %r" % (tok,))
            key = tok[1]
            if self.take() != ":":
                raise ValueError("键 %r 后缺少 ':'" % (key,))
            obj[key] = self.parse_value()
            sep = self.take()
            if sep == "}":
                return obj
            if sep != ",":
                raise ValueError("键 %r 后缺少 ',' 或 '}'" % (key,))

    def parse_array(self):
        arr = []
        while True:
            tok = self.take()
            if tok == "]":
                return arr
            if not (isinstance(tok, tuple) and tok[0] == "num"):
                raise ValueError("数组只允许纯数字,得到 %r" % (tok,))
            arr.append(tok[1])
            sep = self.take()
            if sep == "]":
                return arr
            if sep != ",":
                raise ValueError("数组元素后缺少 ',' 或 ']'")

    def parse_value(self):
        tok = self.take()
        if tok == "{":
            return self.parse_object()
        if tok == "[":
            return self.parse_array()
        if isinstance(tok, tuple) and tok[0] == "num":
            return tok[1]
        raise ValueError("期望对象/数组/数字,得到 %r" % (tok,))


def parse_design(text):
    """从 design-box.js 全文解析出 DESIGN 字面量。

    design-box.js 被纪律约束为「单个对象字面量 + 纯数字」,字面量内无字符串/注释,
    因此可用花括号深度匹配截取顶层对象(避免把赋值后的 ';' 等残留 token 喂给解析器)。
    """
    m = re.search(r"DESIGN\s*=\s*\{", text)
    if not m:
        raise ValueError("未找到 'DESIGN = {' 赋值")
    start = m.end() - 1
    depth = 0
    end = start
    for idx in range(start, len(text)):
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
            if depth == 0:
                end = idx
                break
    if depth != 0:
        raise ValueError("DESIGN 对象字面量的花括号不配对")
    tokens = _tokenize(text[start:end + 1])
    parser = _Parser(tokens)
    if parser.take() != "{":
        raise ValueError("期望以 '{' 开始")
    design = parser.parse_object()
    if parser.peek() is not None:
        raise ValueError("解析结束后仍有残留 token: %r" % (parser.peek(),))
    return design


def _as_int(design, box, key):
    v = design[box][key]
    if "." in v or "e" in v or "E" in v:
        raise ValueError("DESIGN.%s.%s 必须是整数,得到 %r" % (box, key, v))
    return int(v)


def _validate(design):
    for box in ("output", "input"):
        if box not in design:
            raise ValueError("DESIGN 缺少 %r 块" % (box,))
        box_data = design[box]
        for key in ("w", "h", "presets"):
            if key not in box_data:
                raise ValueError("DESIGN.%s 缺少 %r" % (box, key))
        w = _as_int(design, box, "w")
        h = _as_int(design, box, "h")
        if w <= 0 or h <= 0:
            raise ValueError("DESIGN.%s 的 w/h 必须为正整数" % (box,))
        presets = box_data["presets"]
        if not presets:
            raise ValueError("DESIGN.%s.presets 不能为空" % (box,))
        values = [float(x) for x in presets]
        if any(values[i] >= values[i + 1] for i in range(len(values) - 1)):
            raise ValueError("DESIGN.%s.presets 必须严格递增" % (box,))
        if 1.0 not in values:
            raise ValueError("DESIGN.%s.presets 必须含 1(1x 档)" % (box,))
    return design


def render(design):
    out = design["output"]
    inp = design["input"]
    out_w, out_h = _as_int(design, "output", "w"), _as_int(design, "output", "h")
    in_w, in_h = _as_int(design, "input", "w"), _as_int(design, "input", "h")
    out_p = out["presets"]
    in_p = inp["presets"]
    out_p_line = ", ".join(out_p)
    in_p_line = ", ".join(in_p)

    return (
# REUSE-IgnoreStart —— 下方字符串是生成文件(C++ 头)的 SPDX 头模板,
# 含转义 \n 与引号,REUSE 会误读为无效表达式,故整段忽略扫描。
        "// SPDX-License-Identifier: GPL-3.0-or-later\n"
# REUSE-IgnoreEnd
        "// 本文件由 scripts/gen-design-box.py 从 web/shared/design-box.js 自动生成 —— 请勿手工编辑。\n"
        "// 设计盒常量唯一真源 = web/shared/design-box.js(05 §1.2;07 T27 设计定稿回流①)。\n"
        "// 改值请编辑 design-box.js 后重跑:python scripts/gen-design-box.py\n"
        "#pragma once\n"
        "\n"
        "#include <array>\n"
        "\n"
        "namespace scvb::design\n"
        "{\n"
        "// Output 设计盒(1x,05 §1.2):%d×%d,%d 档缩放。\n" % (out_w, out_h, len(out_p))
        + "inline constexpr int kOutputDesignW = %d;\n" % out_w
        + "inline constexpr int kOutputDesignH = %d;\n" % out_h
        + "inline constexpr std::array<double, %d> kOutputPresets = {%s};\n" % (len(out_p), out_p_line)
        + "\n"
        + "// Input 设计盒(1x,05 §1.2):%d×%d,%d 档缩放。\n" % (in_w, in_h, len(in_p))
        + "inline constexpr int kInputDesignW = %d;\n" % in_w
        + "inline constexpr int kInputDesignH = %d;\n" % in_h
        + "inline constexpr std::array<double, %d> kInputPresets = {%s};\n" % (len(in_p), in_p_line)
        + "\n"
        + "// 缩放档位计数(与 design-box.js 数组长度一致)。\n"
        + "inline constexpr int kOutputPresetCount = %d;\n" % len(out_p)
        + "inline constexpr int kInputPresetCount = %d;\n" % len(in_p)
        + "} // namespace scvb::design\n"
    )


def main(argv):
    if not DESIGN_BOX_JS.exists():
        print("gen-design-box: 未找到真源 %s" % DESIGN_BOX_JS, file=sys.stderr)
        return 1
    text = DESIGN_BOX_JS.read_text(encoding="utf-8")
    design = _validate(parse_design(text))
    generated = render(design)

    if "--check" in argv:
        current = DESIGN_BOX_H.read_text(encoding="utf-8") if DESIGN_BOX_H.exists() else ""
        if current == generated:
            print("gen-design-box --check: %s 与真源一致" % DESIGN_BOX_H.relative_to(REPO_ROOT))
            return 0
        print("gen-design-box --check: %s 与 design-box.js 不一致" % DESIGN_BOX_H.relative_to(REPO_ROOT), file=sys.stderr)
        print("  请重跑 python scripts/gen-design-box.py 重新生成。", file=sys.stderr)
        return 1

    DESIGN_BOX_H.parent.mkdir(parents=True, exist_ok=True)
    DESIGN_BOX_H.write_text(generated, encoding="utf-8", newline="\n")
    print("gen-design-box: 已生成 %s" % DESIGN_BOX_H.relative_to(REPO_ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
