// SPDX-License-Identifier: GPL-3.0-or-later
// check-design-box.mjs —— 设计盒常量防回退断言(零依赖,Node >= 18,ESM)。
//   真源 = web/shared/design-box.js(05 §1.2;07 T27 设计定稿回流① 的「单测断言」项)。
//   逐值断言 output 1180×780 + 7 档缩放、input 460×560 + 10 档缩放:
//   回流① 点名要防的就是后续 agent 把 Output 盒悄悄改回 1080×720,或顺手增删缩放档位;
//   档位数组同时校验严格递增且含 1(1x 档),避免复制粘贴写乱顺序。
//   用法:
//     node scripts/check-design-box.mjs [--help]
//   退出码:任一断言失败 = 1,全通过 = 0。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const USAGE = [
    "用法: node scripts/check-design-box.mjs [--help]",
    "  断言 web/shared/design-box.js 的 DESIGN 常量与 05 §1.2 定案逐值一致。",
    "  --help, -h   打印本说明并退出 0。",
].join("\n");

for (const a of process.argv.slice(2)) {
    if (a === "--help" || a === "-h") {
        console.log(USAGE);
        process.exit(0);
    }
    console.error("check-design-box: 无法识别的参数 " + a);
    console.error(USAGE);
    process.exit(1);
}

const nodeMajor = Number(process.versions.node.split(".")[0]);
if (nodeMajor < 18) {
    console.error(
        "check-design-box: 需要 Node >= 18(当前 " + process.versions.node + ")",
    );
    process.exit(1);
}

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SRC = path.join(REPO, "web", "shared", "design-box.js");
const rel = (p) => path.relative(REPO, p).replace(/\\/g, "/");

// 05 §1.2 表 + 07 T27 卡的定值,逐字抄写;改这里等于改定案,必须先改 05。
const EXPECT = {
    output: { w: 1180, h: 780, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
    input: {
        w: 460,
        h: 560,
        presets: [0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3],
    },
};

const errors = [];
const fail = (msg) => errors.push(msg);

if (!fs.existsSync(SRC)) {
    console.error("check-design-box: 未找到 " + rel(SRC));
    console.error(
        "  该文件是全项目设计盒的唯一真源(T27 交付物),应导出 export const DESIGN = { output, input }。",
    );
    console.error(
        "  确认当前分支是否 feat/T27-web-shared,或该文件是否被误删/误移。",
    );
    process.exit(1);
}

let mod;
try {
    mod = await import(pathToFileURL(SRC).href);
} catch (e) {
    console.error("check-design-box: 导入失败 " + rel(SRC));
    console.error("  " + (e && e.message ? e.message : String(e)));
    process.exit(1);
}

const DESIGN = mod.DESIGN;
if (!DESIGN || typeof DESIGN !== "object") {
    console.error(
        "check-design-box: " +
            rel(SRC) +
            " 未导出 DESIGN 对象(export const DESIGN = {...})",
    );
    process.exit(1);
}

// 单个设计盒的逐值断言:宽高严格相等,档位数组长度/取值/顺序全对。
function checkBox(name) {
    const exp = EXPECT[name];
    const got = DESIGN[name];
    if (!got || typeof got !== "object") {
        fail("DESIGN." + name + " 缺失或不是对象");
        return;
    }
    for (const dim of ["w", "h"]) {
        if (got[dim] !== exp[dim]) {
            fail(
                "DESIGN." +
                    name +
                    "." +
                    dim +
                    " = " +
                    JSON.stringify(got[dim]) +
                    ",期望 " +
                    exp[dim] +
                    "(05 §1.2 定案,不得回退)",
            );
        }
    }
    const p = got.presets;
    if (!Array.isArray(p)) {
        fail("DESIGN." + name + ".presets 不是数组");
        return;
    }
    if (p.length !== exp.presets.length) {
        fail(
            "DESIGN." +
                name +
                ".presets 长度 " +
                p.length +
                ",期望 " +
                exp.presets.length,
        );
    }
    if (
        p.length !== exp.presets.length ||
        p.some((v, i) => v !== exp.presets[i])
    ) {
        fail(
            "DESIGN." +
                name +
                ".presets = [" +
                p.join(", ") +
                "],期望 [" +
                exp.presets.join(", ") +
                "]",
        );
    }
    if (p.some((v, i) => i > 0 && !(v > p[i - 1]))) {
        fail("DESIGN." + name + ".presets 必须严格递增");
    }
    if (!p.includes(1)) {
        fail("DESIGN." + name + ".presets 必须含 1(1x 档 = 设计盒原尺寸)");
    }
}

checkBox("output");
checkBox("input");

if (errors.length > 0) {
    console.error("check-design-box 失败(" + errors.length + " 项):");
    for (const e of errors) console.error("  [FAIL] " + e);
    console.error(
        "  真源 = " + rel(SRC) + ";定案依据见 05 §1.2 与 07 T27 回流①。",
    );
    process.exit(1);
}

console.log(
    "check-design-box 通过: output " +
        DESIGN.output.w +
        "x" +
        DESIGN.output.h +
        "(" +
        DESIGN.output.presets.length +
        " 档)、input " +
        DESIGN.input.w +
        "x" +
        DESIGN.input.h +
        "(" +
        DESIGN.input.presets.length +
        " 档)",
);
process.exit(0);
