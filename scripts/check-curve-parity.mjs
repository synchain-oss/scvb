// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// check-curve-parity.mjs —— T34:JS 插值 vs scvb_core 对拍(容差 ≤0.01 dB)。
// -----------------------------------------------------------------------------
// 判据(07 T34 验收,逐字):
//   • CURVE-1…5 与 CURVE-6(side=right 镜像)同一批向量在 JS 侧复现;
//   • 点集覆盖 side 三值,并显式含 side=out 且 |P0|<5 的点(验证两侧确定性回退一致);
//   • 对拍 = JS↔C++ 实现差,**≤0.01 dB**(不是 CURVE-4 的 0.03 dB,不得放宽)。
//
// 两侧跑同一张 2049 点 LUT(PanCurve.h kPanCurveLutSize,CI #56 提密)与同一套公式:
//   - C++ 侧真源 = scripts/curve-parity-golden.json,由 tests/tools/pan_curve_vectors.cpp
//     (链接 scvb_core 的 PanCurve.{h,cpp})打印生成;
//   - JS 侧 = web/output/canvas/curve-editor.js 的 buildLut / lutGainDb / evalCurve。
//
// 生成 golden(在已配置 MSVC + cmake 的环境里):
//   cmake --build build-T34 --config Release --target pan_curve_vectors
//   build-T34/tests/tools/Release/pan_curve_vectors.exe > scripts/curve-parity-golden.json
//
// 用法:node scripts/check-curve-parity.mjs   (工作目录任意,脚本自解析仓库根)
// 退出码:任一 set 任一点差 > 0.01 dB 或点集与 golden 漂移 => 1;全绿 => 0。
// =============================================================================

import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = dirname(SCRIPT_DIR);

const CE = await import(
    pathToFileURL(join(REPO_ROOT, "web", "output", "canvas", "curve-editor.js"))
        .href
);

// =============================================================================
// parity 点集 —— 必须与 tests/tools/pan_curve_vectors.cpp 的 paritySets() 逐字节一致。
// 覆盖:side 三值、side=out 且 |P0|<5(确定性回退)、三 shape、cut 谐振、clamp 上限。
// =============================================================================
const PARITY_SETS = [
    {
        id: "curve-1",
        points: [{ angle: 30, gain_db: 6, shape: "bell", q: 3, side: "out" }],
    },
    {
        id: "curve-2",
        points: [
            { angle: -45, gain_db: -6, shape: "shelf", q: 2, side: "out" },
        ],
    },
    {
        id: "curve-3",
        points: [
            { angle: 0, gain_db: 8, shape: "bell", q: 1.5, side: "out" },
            { angle: 0, gain_db: 8, shape: "bell", q: 1.5, side: "out" },
        ],
    },
    {
        id: "curve-4",
        points: [{ angle: -40, gain_db: -12, shape: "cut", q: 2, side: "out" }],
    },
    {
        id: "curve-5",
        points: [
            { angle: -45, gain_db: -6, shape: "shelf", q: 2, side: "right" },
        ],
    },
    {
        id: "out-center-right",
        points: [{ angle: 2, gain_db: -6, shape: "shelf", q: 2, side: "out" }],
    },
    {
        id: "out-center-left",
        points: [{ angle: -3, gain_db: -8, shape: "cut", q: 3, side: "out" }],
    },
    {
        id: "mixed",
        points: [
            { angle: -70, gain_db: -4, shape: "shelf", q: 1, side: "left" },
            { angle: -30, gain_db: 3, shape: "bell", q: 2, side: "out" },
            { angle: -8, gain_db: -2, shape: "cut", q: 2.5, side: "out" },
            { angle: 15, gain_db: 2.5, shape: "bell", q: 1.5, side: "out" },
            { angle: 55, gain_db: -3, shape: "shelf", q: 0.8, side: "right" },
            { angle: 85, gain_db: 1.5, shape: "cut", q: 4, side: "right" },
        ],
    },
];

const GOLDEN_PATH = join(SCRIPT_DIR, "curve-parity-golden.json");
const TOLERANCE = 0.01; // 对拍容差(不是 CURVE-4 的 0.03)

let fail = 0;
const failm = (msg) => {
    fail++;
    console.error("  [FAIL]", msg);
};

let golden;
try {
    golden = JSON.parse(readFileSync(GOLDEN_PATH, "utf8"));
} catch (e) {
    console.error("check-curve-parity: 读 golden 失败 " + GOLDEN_PATH);
    console.error("  " + e.message);
    console.error(
        "  生成方式见脚本头注释(tests/tools/pan_curve_vectors.cpp)。",
    );
    process.exit(1);
}

if (golden.lutSize !== CE.LUT_SIZE) {
    failm("golden lutSize=" + golden.lutSize + " ≠ JS LUT_SIZE=" + CE.LUT_SIZE);
}
const goldenById = new Map((golden.sets || []).map((s) => [s.id, s]));
if (goldenById.size !== PARITY_SETS.length) {
    failm(
        "golden set 数与 PARITY_SETS 不一致(" +
            goldenById.size +
            " vs " +
            PARITY_SETS.length +
            ")",
    );
}

let worst = 0;
let worstWhere = "";
for (const set of PARITY_SETS) {
    const ref = goldenById.get(set.id);
    if (!ref) {
        failm("golden 缺 set " + set.id);
        continue;
    }
    // 点集漂移自检:golden 内记录的点必须与脚本侧一致(数值用容差——0.8 等值在
    // float32 与 double 的 JSON 字面不同,0.8f → "0.800000012";shape/side 逐字)。
    const samePoints =
        ref.points.length === set.points.length &&
        ref.points.every((rp, i) => {
            const sp = set.points[i];
            return (
                Math.abs(rp.angle - sp.angle) < 1e-5 &&
                Math.abs(rp.gain_db - sp.gain_db) < 1e-5 &&
                Math.abs(rp.q - sp.q) < 1e-5 &&
                rp.shape === sp.shape &&
                rp.side === sp.side
            );
        });
    if (!samePoints) {
        failm(set.id + " 点集与 golden 漂移(两处硬编码失同步)");
    }

    const jsLut = CE.buildLut(set.points);
    let maxLut = 0;
    for (let i = 0; i < CE.LUT_SIZE; i++) {
        const d = Math.abs(jsLut[i] - ref.lut[i]);
        if (d > maxLut) maxLut = d;
    }
    let maxSample = 0;
    let sampleWhere = "";
    for (const [pan, db] of ref.samples) {
        const d = Math.abs(CE.lutGainDb(jsLut, pan) - db);
        if (d > maxSample) {
            maxSample = d;
            sampleWhere = "pan=" + pan;
        }
    }
    const setWorst = Math.max(maxLut, maxSample);
    if (setWorst > worst) {
        worst = setWorst;
        worstWhere =
            set.id +
            (maxLut >= maxSample ? " (LUT)" : " (sample " + sampleWhere + ")");
    }
    console.log(
        "  " +
            set.id.padEnd(18) +
            " maxLut=" +
            maxLut.toExponential(2).padStart(9) +
            "  maxSample=" +
            maxSample.toExponential(2).padStart(9),
    );
    if (setWorst > TOLERANCE) {
        failm(set.id + " 对拍超差 " + setWorst + " dB > " + TOLERANCE + " dB");
    }
}

// side=out 且 |P0|<5 的确定性回退:JS 侧 out(angle≥0)≡right、out(angle<0)≡left
// (与 tests/core/test_pancurve.cpp 的 "side=out with |P0|<5 is deterministic" 同判据)。
{
    const probe = (angle, shape, q, side) => ({
        angle,
        gain_db: -6,
        shape,
        q,
        side,
    });
    for (const p of [-100, -50, 0, 50, 100]) {
        const a = CE.evalShape(probe(2, "shelf", 2, "out"), p);
        const b = CE.evalShape(probe(2, "shelf", 2, "right"), p);
        const c = CE.evalShape(probe(-3, "shelf", 2, "out"), p);
        const d = CE.evalShape(probe(-3, "shelf", 2, "left"), p);
        if (Math.abs(a - b) > 1e-9) failm("out(angle=+2) ≠ right @ pan=" + p);
        if (Math.abs(c - d) > 1e-9) failm("out(angle=-3) ≠ left @ pan=" + p);
    }
}

console.log(
    "check-curve-parity: 最坏偏差 " +
        worst.toExponential(3) +
        " dB @" +
        worstWhere +
        " (容差 " +
        TOLERANCE +
        " dB)",
);
if (fail > 0) {
    console.error("check-curve-parity 失败(" + fail + " 项)");
    process.exit(1);
}
console.log("check-curve-parity 通过");
process.exit(0);
