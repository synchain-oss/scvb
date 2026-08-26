// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— Tab2 轨道页交互接线冒烟(node,无 DOM;T32 Wave 2)
// =============================================================================
// 口径同 smoke-tab1-interactions.mjs:仓内零 node_modules(无 jsdom),故断言面
// 是**纯函数 + mock 端到端 + 源码级字面断言**三档;DOM 侧(属性写入、指针拖动、
// 确认条展开)归浏览器手测,由统筹在 8823 端口逐屏走。
//
// 跑什么:
//   ① 弹道纯函数(契约 §2.5 / 05 §2.2):-60 dB 地板、上行瞬时、下行 120 dB/s、
//      peak-hold 2200 ms 后 20 dB/s 衰减、停止态归零、>.86 转警戒红;
//   ② freeze 四态双向映射(契约 §1.12-§1.14:int 0-3,bit0=pan / bit1=vol);
//   ③ 首次确认的**三形态**(05 §2.2 R3 无条件触发):纯 auto 轨 / 纯 user_edited 轨 /
//      含 locked 段的轨 —— 三者都必须弹,第三形态还要带 {l} 计数;
//   ④ store → 15 行模型:状态灯五态、读回值逐维按 freeze 位分叉([J85]:冻结读参数面 /
//      未冻结优先读常值段)、配对「(满)」后缀与超员标、
//      多主唱计数、mono 轨 width no-op;
//   ⑤ 列头 key 与列序(05 §2.2 列序代码块)+ 卡箍/toggle/PRIO 命中区的源码级断言(RE-06);
//   ⑥ mock 端到端:setChannelConfig / setTrackManual / gesture 三段式 /
//      analyze(scope,{clearManual:true}) 按契约返回,且 §2.8 回推常值段;
//   ⑦ 词条:Wave 2 新增 key 三语齐、占位符三语一致、05 §5 禁词零命中;
//   ⑧ 撤销栈纪律(契约 §0.9 / §1.16):延迟提交计时器 per (ch,dim)、键盘与滚轮同口径、
//      拖拽零位移不空提交、Shift 微调按控件取值域、aria-valuenow 不漏浮点尾巴。
//
// 用法:node web-preview/tests/smoke-tab2-interactions.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有断言失败(逐条打印 [FAIL])。
// =============================================================================

import { pathToFileURL, fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { readFileSync } from "node:fs";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const u = (p) => pathToFileURL(join(ROOT, p)).href;
const src = (p) => readFileSync(join(ROOT, p), "utf8");

const TT = await import(u("web/output/tab-tracks.js"));
// §2.9 轨级 error 的键与消费(errorStoreKey / lowSampleChannels)在 tab-master.js ——
// Tab2 轨行与 Tab3 轨头**消费同一份**,故纯函数落在两 tab 的共同上游(T33)。
const TM = await import(u("web/output/tab-master.js"));
const MT = await import(u("web/output/canvas/meter.js"));
const { createBridge } = await import(u("web/shared/bridge.js"));
const { T } = await import(u("web/shared/i18n.js"));
const { DEMO_LABELS, localizeDemoChannels } = await import(
    u("web/shared/mock-data.js")
);

let fail = 0;
const log = (...a) => console.log(...a);
function check(cond, msg) {
    if (!cond) {
        fail++;
        console.error("  [FAIL]", msg);
    }
    return cond;
}
const eq = (a, b, msg) =>
    check(
        JSON.stringify(a) === JSON.stringify(b),
        `${msg}: 实得 ${JSON.stringify(a)},期望 ${JSON.stringify(b)}`,
    );
const near = (a, b, tol, msg) =>
    check(Math.abs(a - b) <= tol, `${msg}: 实得 ${a},期望 ${b}±${tol}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// =============================================================================
log("=== ① 电平弹道(契约 §2.5 + 05 §2.2 弹道常数)===");
{
    eq(MT.METER_FLOOR_DB, -60, "地板 -60 dB");
    eq(MT.FALL_DB_PER_S, 120, "fast-follow 120 dB/s");
    eq(MT.PEAK_HOLD_MS, 2200, "peak-hold 2200 ms");
    eq(MT.PEAK_DECAY_DB_PER_S, 20, "peak 衰减 20 dB/s");
    eq(MT.PEAK_ALERT_RATIO, 0.86, "峰线警戒阈 .86");

    // 地板映射:-60 → 0,0 dB → 1;越界夹取
    eq(MT.dbToRatio(-60), 0, "dbToRatio(-60)=0");
    eq(MT.dbToRatio(0), 1, "dbToRatio(0)=1");
    eq(MT.dbToRatio(-90), 0, "低于地板夹到 0");

    // 上行瞬时(不做 attack 平滑)
    let s = MT.advance(MT.restState(), -12, 33);
    eq(s.db, -12, "上行瞬时跟随");

    // 下行 120 dB/s:100ms 恰好落 12 dB
    s = MT.advance({ db: -12, peakDb: -12, peakHeldMs: 0 }, -60, 100);
    near(s.db, -24, 1e-9, "下行 100ms 落 12 dB");

    // 下行不穿地板
    s = MT.advance({ db: -12, peakDb: -12, peakHeldMs: 0 }, -60, 10000);
    eq(s.db, -60, "下行不穿地板");

    // peak-hold:保持期内不动
    s = { db: -30, peakDb: -3, peakHeldMs: 0 };
    for (let i = 0; i < 60; i++) s = MT.advance(s, -30, 33, -30); // 1980ms
    near(s.peakDb, -3, 1e-9, "保持期(<2200ms)内峰值不动");

    // 保持到点后按 20 dB/s 衰减(只算超出保持期的那段时间)
    s = MT.advance(s, -30, 300, -30); // 2280ms:超出 80ms
    near(s.peakDb, -3 - 20 * 0.08, 1e-9, "到点后按 20 dB/s 衰减");

    // 峰值被超越 ⇒ 顶到新值并清零保持计时
    s = MT.advance(s, -30, 33, -1);
    eq([s.peakDb, s.peakHeldMs], [-1, 0], "峰值被超越则重置保持计时");

    // 峰值永不低于液柱
    s = MT.advance({ db: -60, peakDb: -60, peakHeldMs: 0 }, -6, 33, -60);
    eq(s.peakDb, -6, "峰值不低于液柱");

    // 停止态归零 = restState()
    eq(MT.restState(), { db: -60, peakDb: -60, peakHeldMs: 0 }, "停止态复位值");

    // 渲染器在无 DOM 环境下 attach/tick/stop 不抛(node 侧 body=null)
    const r = MT.createMeterRenderer({ body: null, getStore: () => ({}) });
    r.attach();
    r.push({
        tracks: Array.from({ length: 15 }, () => ({ db: -6, peakDb: -3 })),
    });
    r.tick(0);
    r.tick(33);
    near(r.stateOf(1).db, -6, 1e-9, "renderer 推进第 1 轨");
    r.stop();
    eq(r.stateOf(1), MT.restState(), "stop() 归零");

    // 非激活 tab:整帧早退,弹道不推进也不写 DOM(Tab2 不在前台时别白烧 60 fps)
    const idleR = MT.createMeterRenderer({
        body: null,
        getStore: () => ({}),
        isActive: () => false,
    });
    idleR.push({
        tracks: Array.from({ length: 15 }, () => ({ db: -6, peakDb: -3 })),
    });
    idleR.tick(0);
    idleR.tick(33);
    eq(idleR.stateOf(1), MT.restState(), "isActive=false ⇒ 该帧不推进");
}

// =============================================================================
log("=== ② freeze 四态双向映射(契约 §1.12-§1.14:bit0=pan / bit1=vol)===");
{
    const table = [
        [0, false, false],
        [1, true, false],
        [2, false, true],
        [3, true, true],
    ];
    for (const [v, pan, vol] of table) {
        eq(TT.freezeBits(v), { pan, vol }, `freezeBits(${v})`);
        eq(TT.freezeValue(pan, vol), v, `freezeValue(${pan},${vol})`);
    }
    // 越界/脏值一律 &3(C++ 侧夹取,UI 侧不得因此画出第五态)
    eq(TT.freezeBits(7), { pan: true, vol: true }, "freezeBits(7) 截到 3");
    eq(
        TT.freezeBits(undefined),
        { pan: false, vol: false },
        "freezeBits(缺值)",
    );
    // ParamID 形状:两位零填充(t = "01".."15")
    eq(TT.paramIdOf(1, 1, "freeze"), "v1_t01_freeze", "ParamID 轨 1");
    eq(TT.paramIdOf(2, 15, "width"), "v2_t15_width", "ParamID 轨 15 / 版本 2");

    // 解冻提示的位账(05 §2.2 R2):1→0 记上、0→1 抹掉、清零撤提示
    eq(TT.unfreezeHintBits(0, 3, 1), 2, "vol 位 1→0 ⇒ 记 vol 位");
    eq(TT.unfreezeHintBits(0, 1, 0), 1, "pan 位 1→0 ⇒ 记 pan 位");
    eq(TT.unfreezeHintBits(0, 3, 0), 3, "两位同时解冻 ⇒ 两位都记");
    eq(TT.unfreezeHintBits(2, 0, 2), 0, "只用 vol 的轨冻回去 ⇒ 提示撤下");
    eq(TT.unfreezeHintBits(3, 0, 2), 1, "冻回 vol 只抹 vol 位,pan 位仍在");
    eq(TT.unfreezeHintBits(3, 0, 3), 0, "两位都冻回 ⇒ 撤下");
    eq(TT.unfreezeHintBits(1, 1, 1), 1, "无变化 ⇒ 位账不动");
    eq(TT.unfreezeHintBits(0, 0, 0), 0, "全程未冻结 ⇒ 不触发");
}

// =============================================================================
log("=== ③ setTrackManual 首次确认的三形态(05 §2.2 R3,无条件)===");
{
    const seg = (o) => ({
        segIdx: 0,
        t0S: 0,
        t1S: 100,
        pan: 0,
        volDb: 0,
        origin: "auto",
        locked: false,
        loudnessLufs: -20,
        ...o,
    });
    // 形态 A:纯 auto 段 —— 弹,且无 locked 计数
    const A = {
        channels: [{ ch: 1, segments: [seg({}), seg({ segIdx: 1 })] }],
    };
    eq(TT.lockedCountOf(A, 1), 0, "A 形态 locked 计数 0");
    eq(TT.manualConstantOf(TT.segmentsOfCh(A, 1)), null, "A 形态非手动常值");

    // 形态 B:纯 user_edited(无 auto 段)—— 仍必须弹(R3 删掉了 origin=auto 前置条件)
    const B = {
        channels: [
            { ch: 1, segments: [seg({ origin: "user_edited", t1S: 999 })] },
        ],
    };
    eq(TT.lockedCountOf(B, 1), 0, "B 形态 locked 计数 0");
    check(
        TT.manualConstantOf(TT.segmentsOfCh(B, 1)) !== null,
        "B 形态 = 单段全时限 user_edited 常值(解冻提示的触发面)",
    );

    // 形态 C:含 locked 段 —— 弹且正文追加「(含 {l} 个锁定段)」
    const C = {
        channels: [
            {
                ch: 1,
                segments: [
                    seg({ origin: "user_edited", locked: true }),
                    seg({ segIdx: 1, locked: true }),
                    seg({ segIdx: 2 }),
                ],
            },
        ],
    };
    eq(TT.lockedCountOf(C, 1), 2, "C 形态 locked 计数 2");
    eq(
        TT.manualConstantOf(TT.segmentsOfCh(C, 1)),
        null,
        "C 形态 3 段 ⇒ 不是手动常值",
    );

    // 「每轨每会话一次」与「无条件」两条语义在源码里必须有对应实现
    const s = src("web/output/tab-tracks.js");
    check(
        /manualConfirmed\.has\(ch\)/.test(s) &&
            /manualConfirmed\.add\(/.test(s),
        "源码有「每轨一次」的会话集合",
    );
    check(
        !/origin\s*===\s*"auto"[\s\S]{0,120}openConfirm/.test(s),
        "确认条不带 origin=auto 前置条件(R3 无条件)",
    );
    check(
        /tracks\.manualOverwriteConfirm\.locked/.test(s),
        "locked 变体词条已接线",
    );
}

// =============================================================================
log("=== ④ store → 15 行模型 ===");
{
    // 红旗防回归(PR #60):labelPlaceholder 传字典路径曾因漏导入 format 抛
    // ReferenceError——空轨名是 mock 与真机的常态默认,必须直接断言三语输出。
    for (const lang of ["zh", "en", "fr"]) {
        const out = TT.labelPlaceholder(7, T[lang]);
        check(
            typeof out === "string" && out.includes("07"),
            `labelPlaceholder(${lang}) 含两位轨号(实得 ${out})`,
        );
        check(!out.includes("{n}"), `labelPlaceholder(${lang}) 占位已替换`);
    }
    eq(TT.labelPlaceholder(3, null), "Track 03", "无字典回落设计稿原文");
}

{
    // 状态灯五态(05 §2.2 / 契约 §2.3)
    eq(TT.trackStatusOf(null), "idle", "无 conn ⇒ idle");
    eq(
        TT.trackStatusOf({ slotState: 0, heartbeatFresh: false }),
        "idle",
        "空闲 slot",
    );
    // slotState=1(ipc §1「已声明、未活跃」)**不算 active** —— 否则行上亮绿脉冲
    // 却不进 header 的 N/15(契约 §2.3 UI 消费行,J01),两处口径打架
    eq(
        TT.trackStatusOf({ slotState: 1, heartbeatFresh: true }),
        "idle",
        "已声明但未活跃 ⇒ 未连接(与 N/15 同口径)",
    );
    eq(
        TT.trackStatusOf({
            slotState: 1,
            heartbeatFresh: true,
            misalignCount: 2,
        }),
        "idle",
        "已声明未活跃 + 失准计数 ⇒ 仍是未连接",
    );
    eq(
        TT.trackStatusOf({ slotState: 2, heartbeatFresh: false }),
        "lost",
        "心跳陈旧 ⇒ 失联",
    );
    eq(
        TT.trackStatusOf({
            slotState: 2,
            heartbeatFresh: true,
            misalignCount: 3,
        }),
        "warn",
        "失准计数 ⇒ 琥珀⚠",
    );
    eq(
        TT.trackStatusOf({
            slotState: 2,
            heartbeatFresh: true,
            srMismatch: true,
        }),
        "srErr",
        "采样率不一致优先于一切",
    );

    // 配对:「(满)」后缀 vs 行上「超员」琥珀标
    const counts = TT.pairCounts([
        { pair_id: 1 },
        { pair_id: 1 },
        { pair_id: 2 },
        { pair_id: 3 },
        { pair_id: 3 },
        { pair_id: 3 },
    ]);
    eq(counts[1], 2, "A 组 2 轨");
    check(TT.isPairFullOption(counts, 1, 0), "别的轨看 A 组 = (满)");
    check(!TT.isPairFullOption(counts, 1, 1), "已在 A 组的轨看 A 组 ≠ (满)");
    check(!TT.isPairFullOption(counts, 2, 0), "B 组只有 1 轨 ⇒ 无后缀");
    check(!TT.isPairOverflow(counts, 1), "恰好 2 轨不算超员");
    check(TT.isPairOverflow(counts, 3), "3 轨 ⇒ 行上出琥珀超员标");
    eq(TT.pairLetter(7), "G", "pair_id 7 ⇒ G");
    eq(TT.pairIdOf("G"), 7, "G ⇒ pair_id 7");
    eq(TT.pairLetter(0), "", "pair_id 0 = 无配对");

    // 多主唱(lead_lock ≥2 ⇒ 图例行琥珀 badge)
    eq(
        TT.leadLockCount([{ lead_lock: true }, {}, { lead_lock: true }]),
        2,
        "多主唱计数",
    );

    // 行模型([J85]):冻结维度读**参数面**(冻结通道只写参数面),未冻结维度优先读常值段
    const store = {
        state: {
            group_id: 1,
            global: { version_active: 1 },
            versions: [{ name: "主版" }, { name: "V2" }],
            channels: Array.from({ length: 15 }, (_, i) => ({
                enabled: i !== 4,
                label: i === 0 ? "主唱" : "",
                source_channels: i === 2 ? 2 : 1,
                participate_in_auto_pan: true,
                priority: i === 0 ? 9 : 3,
                lead_lock: i === 0,
                lead_vol_exempt: i === 1,
                pair_id: 0,
            })),
        },
        conn: {
            channels: Array.from({ length: 15 }, (_, i) => ({
                slotState: i < 3 ? 2 : 0,
                heartbeatFresh: i < 3,
                misalignCount: i === 1 ? 4 : 0,
                srMismatch: i === 2,
            })),
        },
        params: {
            values: {
                lead_select: 3,
                v1_t01_freeze: 3, // 轨 1 两维度都冻结
                v1_t01_pan: 55, // 冻结维度的静态值只存这里([J85]),读回就该读到它
                v1_t01_vol: 6,
                v1_t02_pan: -40,
                v1_t02_vol: -6,
                v1_t03_width: 42,
            },
            versionActive: 1,
        },
        segments: {
            channels: [
                {
                    ch: 1,
                    segments: [
                        {
                            segIdx: 0,
                            t0S: 0,
                            t1S: 180,
                            pan: -20,
                            volDb: -3,
                            origin: "user_edited",
                            locked: true,
                        },
                    ],
                },
            ],
        },
        errors: new Map([["lowSample", { code: "lowSample", ch: 2 }]]),
    };
    const rows = TT.rowsFromStore(store);
    eq(rows.length, 15, "15 行");
    // [J85] 冻结维度读参数面:段表里那条常值段可能是**冻结前**接管手动时留下的旧值,
    // 而冻结中的调整只落参数面 —— 读段表就会「看着没改、听着改了」。
    eq(rows[0].pan, 55, "[J85] 轨 1 pan 冻结 ⇒ 读参数面(不是陈旧常值段的 -20)");
    eq(rows[0].volDb, 6, "[J85] 轨 1 vol 冻结 ⇒ 读参数面");
    eq([rows[0].fp, rows[0].fv], [1, 1], "轨 1 冻结两位");
    eq(rows[1].pan, -40, "轨 2 无常值段 ⇒ 读参数面");
    // 未冻结维度仍优先读常值段:「未冻结轨拖卡箍」(05 允许,走一次性确认)写的是曲线真身,
    // 改读参数面会在下一帧被 25 Hz 的旧参数值弹回去。
    {
        const unfrozen = TT.rowsFromStore({
            state: { global: { version_active: 1 }, channels: [] },
            params: {
                values: { v1_t01_freeze: 0, v1_t01_vol: 3, v1_t01_pan: 55 },
                versionActive: 1,
            },
            segments: store.segments,
        });
        eq(unfrozen[0].volDb, -3, "未冻结 + 常值段 ⇒ 仍读常值段(vol)");
        eq(unfrozen[0].pan, -20, "未冻结 + 常值段 ⇒ 仍读常值段(pan)");
        eq(
            [unfrozen[0].fp, unfrozen[0].fv],
            [0, 0],
            "冻结两位仍只由参数面决定",
        );
    }
    eq(rows[1].low, 1, "轨 2 命中 lowSample(§2.9 轨级 error)");

    // T33 §2.9 lowSample = **code+ch 复合键**(t32/deviations §N):轨级 error 会同时
    // 命中多轨,按裸 code 存一条时后到的轨把先到的覆盖掉,Tab2/Tab3 只剩一枚黄标。
    {
        const many = {
            ...store,
            errors: new Map([
                ["lowSample#2", { code: "lowSample", ch: 2 }],
                ["lowSample#7", { code: "lowSample", ch: 7 }],
                ["lowSample#15", { code: "lowSample", ch: 15 }],
                // 同表里的页级 code 一律**保持裸 code 既有行为**
                ["sidecarMissing", { code: "sidecarMissing" }],
            ]),
        };
        const r = TT.rowsFromStore(many);
        eq(
            [r[1].low, r[6].low, r[14].low],
            [1, 1, 1],
            "多轨同时低采样 ⇒ 三轨各自挂标(复合键不再互相覆盖)",
        );
        eq(
            r.filter((x) => x.low).length,
            3,
            "只有报到的三轨挂标,其余 12 轨干净",
        );
        eq(
            [...TM.lowSampleChannels(many.errors)].sort((a, b) => a - b),
            [2, 7, 15],
            "lowSampleChannels 扫出轨号集合(Tab2/Tab3 消费同一份)",
        );
        // 键形解耦:裸 `lowSample` 键(旧写法 / 载荷缺 ch)同样能被消费侧读到
        eq(
            [...TM.lowSampleChannels(new Map([["lowSample", { ch: 9 }]]))],
            [9],
            "按值扫描 ⇒ 裸 code 键同样命中(消费侧不必知道键怎么拼)",
        );
        eq([...TM.lowSampleChannels(null)], [], "无 errors 表 ⇒ 空集,不抛错");
        eq(
            TM.errorStoreKey({ code: "lowSample", ch: 7 }),
            "lowSample#7",
            "errorStoreKey:lowSample 走 code+ch 复合键",
        );
        eq(
            TM.errorStoreKey({ code: "lowSample" }),
            "lowSample",
            "errorStoreKey:载荷缺 ch ⇒ 回落裸 code(不拼出 lowSample#NaN)",
        );
        for (const code of ["srMismatch", "channelConflict", "secondOutput"]) {
            eq(
                TM.errorStoreKey({ code, ch: 3 }),
                code,
                `errorStoreKey:${code} 保持裸 code 既有行为`,
            );
        }
        // active:false 撤下(契约 §2.9 没保证解除事件必带 ch —— 不带 ch 的解除
        // 必须把该 code 的**全部**轨级条目一并撤掉,否则黄标永不熄灭)
        {
            const errs = new Map([
                ["lowSample#2", { code: "lowSample", ch: 2 }],
                ["lowSample#7", { code: "lowSample", ch: 7 }],
                ["sidecarMissing", { code: "sidecarMissing" }],
            ]);
            eq(
                TM.errorKeysToDrop(errs, {
                    code: "lowSample",
                    ch: 7,
                    active: false,
                }),
                ["lowSample#7"],
                "errorKeysToDrop:带 ch ⇒ 只撤该轨",
            );
            eq(
                TM.errorKeysToDrop(errs, {
                    code: "lowSample",
                    active: false,
                }).sort(),
                ["lowSample", "lowSample#2", "lowSample#7"],
                "errorKeysToDrop:不带 ch ⇒ 裸键 + 全部复合键一并撤下",
            );
            eq(
                TM.errorKeysToDrop(errs, {
                    code: "sidecarMissing",
                    active: false,
                }),
                ["sidecarMissing"],
                "errorKeysToDrop:页级 code 仍是裸键一条(既有行为不变)",
            );
            // 端到端:撤下后消费侧一轨不剩
            for (const k of TM.errorKeysToDrop(errs, {
                code: "lowSample",
                active: false,
            })) {
                errs.delete(k);
            }
            eq(
                [...TM.lowSampleChannels(errs)],
                [],
                "不带 ch 的解除事件走完 ⇒ lowSampleChannels 空集(黄标熄灭)",
            );
        }
        // app.js 存删同口径(delete 走 errorKeysToDrop,存走 errorStoreKey)
        const appJs = src("web/output/app.js");
        check(
            /errorKeysToDrop\(store\.errors, e\)[\s\S]{0,120}errors\.delete\(k\)[\s\S]{0,160}errors\.set\(errorStoreKey\(e\), e\)/.test(
                appJs,
            ),
            "app.js 的 §2.9 存走 errorStoreKey、删走 errorKeysToDrop(active:false 撤得下来)",
        );
    }

    eq(rows[1].misalign, 4, "轨 2 失准计数入行模型");
    eq(rows[1].status, "warn", "轨 2 = 琥珀失准");
    eq(rows[2].status, "srErr", "轨 3 = 采样率不一致(整行 disabled)");
    eq(rows[2].st, 1, "轨 3 stereo ⇒ ST 标");
    eq(rows[2].w, 42, "轨 3 width 跟随参数面");
    eq(rows[2].leadCenter, 1, "lead_select=3 ⇒ 轨 3 行首居中标记");
    eq(rows[3].status, "idle", "轨 4 未连接");
    eq(rows[4].on, 0, "轨 5 enabled=false ⇒ 整行 .3");
    eq(rows[0].prio, 9, "优先级跟随 state");
    eq(
        rows[1].volPart,
        0,
        "lead_vol_exempt ⇒ 音量参与开关显示 OFF(参与语义,显示层取反)",
    );
    eq(rows[0].volPart, 1, "未豁免 ⇒ 参与开关显示 ON");
    eq(rows[0].multiLead, 0, "只有 1 轨 lead_lock ⇒ 无多主唱 badge");

    // 值域与量化(契约 §1.16 的 value 域;05 §2.2 的双击回默认)
    eq(
        [TT.VOL_RANGE.min, TT.VOL_RANGE.max, TT.VOL_RANGE.def],
        [-24, 12, 0],
        "vol 域",
    );
    eq(
        [TT.PAN_RANGE.min, TT.PAN_RANGE.max, TT.PAN_RANGE.def],
        [-100, 100, 0],
        "pan 域",
    );
    eq(
        [TT.WIDTH_RANGE.min, TT.WIDTH_RANGE.max, TT.WIDTH_RANGE.def],
        [0, 100, 100],
        "width 域(双击回 100)",
    );
    eq(TT.quantize(TT.VOL_RANGE, -3.44), -3.4, "vol 量化到 0.1 dB");
    eq(TT.quantize(TT.VOL_RANGE, 99), 12, "vol 上界夹取");
    eq(TT.quantize(TT.PAN_RANGE, -999), -100, "pan 下界夹取");
    eq(TT.clampPriority(11), 10, "priority 上界 10");
    eq(TT.clampPriority(-1), 0, "priority 下界 0");
    // 卡箍行程:0 dB 落 2/3(J03),-24 dB 落 0,+12 dB 落满
    eq(TT.volPercent(-24), 0, "vol -24 dB ⇒ 0%");
    near(TT.volPercent(0), 200 / 3, 1e-9, "vol 0 dB ⇒ 2/3");
    eq(TT.volPercent(12), 100, "vol +12 dB ⇒ 100%");
}

// =============================================================================
log("=== ⑤ 列头 key / 列序 / 命中区(05 §2.2 列序代码块 + RE-06)===");
{
    eq(
        TT.TRACK_COLS.filter((c) => !c.divider).map((c) => c.key),
        [
            "light",
            "ch",
            "label",
            "pan",
            "width",
            "vol",
            "prio",
            "lead",
            "pair",
            "volexempt",
            "autopan",
            "freezepan",
            "freezevol",
            "on",
        ],
        "列序(不得重排)",
    );
    eq(
        TT.TRACK_COLS.filter((c) => c.t).map((c) => c.t),
        [
            "tracks.colCh",
            "track",
            "tracks.colPan",
            "tracks.colW",
            "tracks.colVolLevel",
            "tracks.colPrio",
            "tracks.colLead",
            "pair",
            "tracks.colVolExempt",
            "tracks.colAutoPan",
            "tracks.colFreezePan",
            "tracks.colFreezeVol",
            "tracks.colOn",
        ],
        "列头词条 key 逐列",
    );
    for (const c of TT.TRACK_COLS) {
        if (c.t) check(c.t in T.zh, `列头 key ${c.t} 在字典中`);
        if (c.aria) check(c.aria in T.zh, `列头 aria key ${c.aria} 在字典中`);
    }
    eq(
        TT.TRACK_COLS.filter((c) => c.divider).length,
        3,
        "三条分组竖分隔线(B15)",
    );
    check(
        TT.rowTotalWidth() <= TT.TRACKS_VIEWPORT_W,
        "行总宽不超可用宽(零横向溢出)",
    );

    // RE-06:小靶区一律加**透明命中扩展**,不缩靶区不改视觉尺寸(源码级)
    const html = src("web/output/index.html");
    check(
        /\.tracks-row \.sc-tube__collar::before/.test(html),
        "卡箍有 ::before 命中扩展层",
    );
    check(
        /\.tracks-row \.sc-tube__collar::before\s*\{[^}]*left:\s*-6\.5px[^}]*right:\s*-6\.5px/.test(
            html,
        ),
        "卡箍命中区左右各 +6.5px(11 → 24px)",
    );
    check(
        /\.tracks-row \.sc-toggle::before\s*\{[^}]*top:\s*-5px[^}]*bottom:\s*-5px/.test(
            html,
        ),
        "toggle 命中区上下各 +5px(15 → 25px)",
    );
    check(
        /\.tracks-row__prio button::before\s*\{[^}]*width:\s*24px[^}]*height:\s*24px/.test(
            html,
        ),
        "PRIO ± 命中区 24×24",
    );
    // 卡箍必须在液柱之上(T32 验收硬要求②:满幅电平仍可辨)
    const css = src("web/shared/base.css");
    const z = (sel) => {
        const m = new RegExp(`\\${sel}\\s*\\{[^}]*z-index:\\s*(\\d+)`).exec(
            css,
        );
        return m ? Number(m[1]) : -1;
    };
    check(z(".sc-tube__collar") > z(".sc-tube__liquid"), "卡箍层序在液柱之上");
}

// =============================================================================
log("=== ⑥ mock 端到端(契约 §1.15 / §1.16 / §1.12-§1.14 / §1.6)===");
{
    const driver = await import(u("web-preview/mock/state-driver.js"));
    const session = driver.createPreviewSession({
        role: "output",
        params: "fixture=fifteen-tracks",
    });
    const bridge = createBridge({ role: "output", mockBackend: session.mock });
    const seen = { params: null, segments: [], meters: null };
    bridge.on("scvb.params", (p) => (seen.params = p));
    bridge.on("scvb.segments", (s) => seen.segments.push(s));
    bridge.on("scvb.meters", (m) => (seen.meters = m));
    session.start();
    const snap = await bridge.requestInitialState();
    check(!!snap && Array.isArray(snap.channels), "首帧快照带 channels[15]");
    await sleep(80);

    // §2.5 载荷形状
    check(
        seen.meters && seen.meters.tracks.length === 15 && seen.meters.bus,
        "scvb.meters 首帧 = 15 轨 + bus",
    );
    for (const t of (seen.meters || { tracks: [] }).tracks) {
        check(t.db >= MT.METER_FLOOR_DB, "meters 不低于地板");
    }

    // §1.15 setChannelConfig:逐字段
    for (const patch of [
        { label: "主唱A" },
        { priority: 9 },
        { lead_lock: true },
        { lead_vol_exempt: true },
        { participate_in_auto_pan: false },
        { pair_id: 3 },
        { enabled: false },
    ]) {
        const res = await bridge.setChannelConfig(1, patch);
        eq(res, { ok: true }, `setChannelConfig ${Object.keys(patch)[0]}`);
    }
    // 不可写字段 ⇒ badArg(source_channels 是 Input 实测值)
    eq(
        await bridge.setChannelConfig(1, { source_channels: 2 }),
        { ok: false, reason: "badArg" },
        "source_channels 不可写",
    );

    // §1.12-§1.14 gesture 三段式:width 与 freeze 四态
    const wid = TT.paramIdOf(snap.global.version_active, 2, "width");
    eq(await bridge.beginParamGesture(wid), { ok: true }, "beginParamGesture");
    eq(await bridge.setParam(wid, 42), { ok: true }, "setParam width");
    eq(await bridge.endParamGesture(wid), { ok: true }, "endParamGesture");
    const frz = TT.paramIdOf(snap.global.version_active, 2, "freeze");
    for (const v of [1, 2, 3, 0]) {
        await bridge.beginParamGesture(frz);
        eq(await bridge.setParam(frz, v), { ok: true }, `setParam freeze=${v}`);
        await bridge.endParamGesture(frz);
    }
    // pan/vol **不在** gesture 通道(契约 §1.12-§1.14「不在本通道」行)
    eq(
        await bridge.setParam(TT.paramIdOf(1, 2, "pan"), 10),
        { ok: false, reason: "badArg" },
        "轨 pan 不走参数通道",
    );
    eq(
        await bridge.setParam(TT.paramIdOf(1, 2, "vol"), 3),
        { ok: false, reason: "badArg" },
        "轨 vol 不走参数通道",
    );

    // §1.16 setTrackManual:回 replacedSegments/replacedLocked,并经 §2.8 回推常值段
    seen.segments.length = 0;
    const res = await bridge.setTrackManual(2, "vol", -6);
    check(
        res && res.ok === true && Number.isInteger(res.replacedSegments),
        "setTrackManual 回 {ok, replacedSegments, replacedLocked}",
    );
    await sleep(30);
    const tm = seen.segments.find((s) => s.reason === "trackManual");
    check(!!tm, "§2.8 回推 reason=trackManual");
    const constSeg = TT.manualConstantOf(TT.segmentsOfCh(tm, 2));
    check(!!constSeg, "回推的是单段全时限 user_edited 常值(04 §1.5 方案 A)");
    eq(constSeg.volDb, -6, "常值段 volDb = 写入值");
    // 读回路径([J85] 逐维按 freeze 位分叉):
    //   • 未冻结维度 → 读常值段(手动接管通道写的就是曲线真身);
    //   • 冻结维度   → 读**参数面**(冻结通道只写参数面,段表里那条常值段可能是旧的 ——
    //     读它就会把把手弹回旧值,而耳朵听到的是参数面上的新值)。
    const rowsOf = (freeze, volParam) =>
        TT.rowsFromStore({
            state: { global: { version_active: 1 }, channels: [] },
            params: {
                values: { v1_t02_freeze: freeze, v1_t02_vol: volParam },
                versionActive: 1,
            },
            segments: tm,
        });
    eq(rowsOf(0, 0)[1].volDb, -6, "未冻结 vol ⇒ 行模型读常值段");
    eq(
        rowsOf(2, -3)[1].volDb,
        -3,
        "[J85] 冻结 vol ⇒ 行模型读参数面(不读陈旧常值段)",
    );
    eq(
        rowsOf(1, -3)[1].volDb,
        -6,
        "只冻 pan 时 vol 仍读常值段(逐维分叉,不是整行分叉)",
    );

    // [J85] 冻结通道:再写一次 vol,段表**一个字节都不许变**,值只落参数面。
    // 事件时序也一并记下来:冻结通道的新值只在 `scvb.params` 里,它必须**不晚于**
    // trackManual 段表帧到达 —— 晚一拍 UI 就会先丢乐观值再读到旧参数面,旋钮回弹(重要3)。
    const frames = [];
    bridge.on("scvb.params", (p) => frames.push({ kind: "params", p }));
    bridge.on("scvb.segments", (s) => frames.push({ kind: "segments", s }));
    // 冻结位走**与 toggleFreeze 同款的 gesture 三段式**(契约 §1.12-§1.14):UI 里没有裸
    // setParam 这条路,冒烟也不许走 —— 否则测的是一条真机上不存在的写入路径(建议⑨)。
    const frzId = TT.paramIdOf(1, 2, "freeze");
    eq(
        [
            await bridge.beginParamGesture(frzId),
            await bridge.setParam(frzId, 2), // 冻 vol 维
            await bridge.endParamGesture(frzId),
        ],
        [{ ok: true }, { ok: true }, { ok: true }],
        "freeze 在 §1.12 gesture 白名单里,三段式逐段回 ok",
    );
    const beforeFrozen = JSON.stringify(TT.segmentsOfCh(tm, 2));
    seen.segments.length = 0;
    frames.length = 0;
    const frozenWrite = await bridge.setTrackManual(2, "vol", -12);
    eq(
        frozenWrite.replacedSegments,
        0,
        "[J85] 冻结通道不替换任何段 ⇒ replacedSegments=0",
    );
    await sleep(30);
    const tm2 = seen.segments.find((s) => s.reason === "trackManual");
    check(!!tm2, "冻结通道仍按 §2.8 回推 reason=trackManual");
    eq(
        JSON.stringify(TT.segmentsOfCh(tm2, 2)),
        beforeFrozen,
        "[J85] 冻结中调整:段表逐字节不变(解冻即回引擎曲线)",
    );
    const paramIdx = frames.findIndex(
        (f) =>
            f.kind === "params" && f.p.values && f.p.values.v1_t02_vol === -12,
    );
    const segIdx = frames.findIndex(
        (f) => f.kind === "segments" && f.s.reason === "trackManual",
    );
    check(paramIdx >= 0, "[J85] 冻结中调整:值落参数面并经 scvb.params 回推");
    check(
        segIdx >= 0 && paramIdx < segIdx,
        "[J85] 参数面帧不晚于 trackManual 段表帧(真桥 emitParams 排在 emitSegments 之前;晚一拍 = 旋钮回弹)",
    );

    // §1.6 单轨重新识别:analyze({tracksMask}, {clearManual:true})
    seen.segments.length = 0;
    const ar = await bridge.analyze(
        { tracksMask: 1 << 1 },
        { clearManual: true },
    );
    check(!!ar && "ok" in ar && !!ar.affected, "analyze 回受理回执 + 影响面");
    // 契约 §1.6:native function 只回受理回执,**结果一律经 scvb.segments 回推**
    // (mock 的 [W] 流水线退化成 4 拍进度 + 800ms 后一次段表重算)。
    await sleep(1000);
    check(
        seen.segments.some((s) => s.reason === "analyze"),
        "§2.8 回推 reason=analyze",
    );

    session.stop();
}

// =============================================================================
log("=== ⑦ 词条(Wave 2 新增 key + 占位符 + 禁词)===");
{
    const KEYS = [
        "tracks.reidentifyConfirm",
        "tracks.reidentifyOne",
        "tracks.manualOverwriteConfirm",
        "tracks.manualOverwriteConfirm.locked",
        "tracks.manualDrivenHint",
        "tracks.multiLead",
        "tracks.pairNone",
        "tracks.pairFullSuffix",
        "tracks.pairOverflow",
        "tracks.panAutoHint",
        "tracks.monoWidthNoop",
        "tracks.labelEdit",
        "tracks.misaligned",
        "tracks.srErr",
        "lowSample.full",
        "common.continue",
        "common.cancel",
        "reidentify",
    ];
    for (const k of KEYS) {
        for (const lang of ["zh", "en", "fr"]) {
            check(
                typeof T[lang][k] === "string" && T[lang][k].length > 0,
                `${lang}.${k} 非空`,
            );
        }
        const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join(",");
        eq(ph(T.en[k]), ph(T.zh[k]), `${k} en 占位符与 zh 一致`);
        eq(ph(T.fr[k]), ph(T.zh[k]), `${k} fr 占位符与 zh 一致`);
    }
    // 05 §5 禁词:查的是**词条值**(注释里为解释纪律而引用禁词是允许的,i18n.js 就有三处)
    const BAD = ["写入完成", "推子后", "post-fader", "六条"];
    for (const lang of ["zh", "en", "fr"]) {
        for (const [k, v] of Object.entries(T[lang])) {
            for (const bad of BAD) {
                check(
                    !String(v).toLowerCase().includes(bad.toLowerCase()),
                    `${lang}.${k} 含禁词「${bad}」`,
                );
            }
        }
    }
    // Wave 1 的静态填数路径必须已删净(留的是「已删除」那句说明,不是数据)
    const s = src("web/output/tab-tracks.js");
    check(
        !/^const WAVE1_ROWS|^export const WAVE1_SAMPLE/m.test(s),
        "WAVE1_SAMPLE / WAVE1_ROWS 静态填数路径已整段删除",
    );
    check(
        !/\[T32 Wave 2\]/.test(s) &&
            !/\[T32 Wave 2\]/.test(src("web/output/canvas/meter.js")),
        "Wave 2 注释桩已全部兑现",
    );
}

// =============================================================================
log("=== ⑧ 提交节流与空提交(撤销栈纪律,契约 §0.9 / §1.16)===");
{
    // 接线回归(源码级;Reviewer Guide 两条新行为):
    const tt = src("web/output/tab-tracks.js");
    check(
        /d\.lastVal = next;[\s\S]{0,400}?if \(t - d\.lastSent < 20\) return;/.test(
            tt,
        ),
        "width 末值:lastVal 记录在节流 return 之前(补发才有值可补)",
    );
    check(
        /const freezeId = paramIdOf\(activeVersion\(\), ch, "freeze"\);/.test(
            tt,
        ) && /res\.ok === true && wasUnfrozen/.test(tt),
        "auto-freeze:请求前捕获 freezeId+wasUnfrozen,回调双条件",
    );
}
{
    const s = src("web/output/tab-tracks.js");

    // 延迟提交的计时器必须 **per (ch,dim)**:共享单句柄时,轨 1 滚完 300ms 内去滚轨 2
    // 会把轨 1 的待提交 clearTimeout 掉 —— 乐观值留在 UI 上,引擎里从没写进去
    eq(TT.MANUAL_COMMIT_MS, 300, "手动值延迟提交窗口 300ms");
    check(!/wheelTimer/.test(s), "不再有共享的单个滚轮计时器句柄");
    check(
        /manualTimers:\s*new Map\(\)/.test(s),
        "延迟提交计时器按 (ch,dim) 分别持有",
    );
    check(
        /function queueManual\(ch, dim, value\)[\s\S]{0,400}manualTimers\.set\(\s*k,/.test(
            s,
        ),
        "queueManual 以 manualKey(ch,dim) 为键排程",
    );
    // 直接提交(拖动松手 / 双击 / 确认条)要吃掉同键的待提交,否则稍后再发一遍旧值
    check(
        /function sendManual\([\s\S]{0,900}manualTimers\.delete\(manualKey\(ch, dim\)\)/.test(
            s,
        ),
        "sendManual 先吃掉同 (ch,dim) 的待提交",
    );
    // 键盘与滚轮同口径:按住方向键时 OS 自动重复 ~30Hz,逐次发会灌满撤销栈
    check(
        /requestManual\(ch, "vol", currentVolDb\(ch\) \+ dir \* step, true\)/.test(
            s,
        ) &&
            /requestManual\(ch, "pan", currentPan\(ch\) \+ dir \* step, true\)/.test(
                s,
            ),
        "键盘方向键走延迟提交(与滚轮同款)",
    );
    check(
        /requestManual\(h\.ch, "pan", currentPan\(h\.ch\) \+ dir, true\)/.test(
            s,
        ),
        "滚轮档走延迟提交",
    );
    // 双击回默认是一次明确动作,必须立刻落(不 defer)
    check(
        /requestManual\(h\.ch, "vol", VOL_RANGE\.def\)/.test(s) &&
            /requestManual\(h\.ch, "pan", PAN_RANGE\.def\)/.test(s),
        "双击回默认立刻提交(不防抖)",
    );

    // endDrag:零位移(down+up 原地单击)不得把留存的 manualEcho 原值再写一遍
    check(
        /function endDrag\(\)[\s\S]{0,700}if \(!d\.moved\) return;/.test(s),
        "endDrag 有位移判定(原地单击不发 setTrackManual)",
    );
    check(
        /d\.kind === "vol" \? e\.clientX !== d\.startX : e\.clientY !== d\.startY/.test(
            s,
        ),
        "位移判定按控件实际读的那根轴",
    );

    // Shift 微调倍率按 kind 取各自值域(今天三者同为 .2,分叉了也不能错位)
    check(
        !/const fine = e\.shiftKey \? VOL_RANGE\.fine : 1/.test(s),
        "Shift 微调不再对三种控件一律用 VOL_RANGE.fine",
    );
    check(
        /const fine = e\.shiftKey \? rng\.fine : 1/.test(s),
        "Shift 微调取该控件自己的值域常量",
    );

    // sendParam 与 sendManual 同款非成功回滚:写被拒时 25Hz 回推不会点名该 id,
    // 乐观值会挂死(冻结开关显示已开、引擎从未接受)——钉住「echo 仍是本次值才删」
    check(
        /function sendParam\([\s\S]{0,600}paramEcho\.get\(id\) === value[\s\S]{0,120}paramEcho\.delete\(id\)/.test(
            s,
        ),
        "sendParam 非成功回滚乐观值(且仅当 echo 仍是本次值)",
    );

    // aria-valuenow 不得漏出 f32 浮点尾巴(-3.4000000000000004)
    check(
        /aria-valuenow",\s*quantize\(VOL_RANGE, volDb\)/.test(s),
        "卡箍 aria-valuenow 经 quantize",
    );
    eq(
        TT.quantize(TT.VOL_RANGE, -3.4000000000000004),
        -3.4,
        "浮点尾巴被量化掉",
    );
}

// =============================================================================
// ⑨ 渲染性能批(T33;t32/deviations §N)——**纯性能改造,行为不变**
// =============================================================================
log("\n=== ⑨ 渲染调度:rAF 合帧 + 按行增量(T33 性能批)===");
{
    const s = src("web/output/tab-tracks.js");
    const appJs = src("web/output/app.js");

    // (a) 单行模型:rowOf 不得再整套重建 15 行
    check(
        /function rowOf\(ch\) \{\s*return rowFromStore\(getStore\(\), ch\);/.test(
            s,
        ),
        "rowOf 走单行 rowFromStore(不再 rowsFromStore(...)[ch-1])",
    );
    check(
        !/rowsFromStore\(getStore\(\)\)\[ch - 1\]/.test(s),
        "旧的「每步进重建 15 行」路径已删",
    );
    // 单行与整页必须**逐字同结果**(纯性能改造的核心不变式)
    {
        const st = {
            state: {
                global: { version_active: 1 },
                channels: [
                    { label: "主唱", lead_lock: true, pair_id: 1, priority: 9 },
                    { pair_id: 1, lead_vol_exempt: true },
                    { pair_id: 1, source_channels: 2 },
                ],
                recapture: { armed: true, tracksMask: 0b101 },
            },
            params: {
                values: { lead_select: 3, v1_t01_freeze: 3, v1_t02_pan: -40 },
                versionActive: 1,
            },
            conn: { channels: [{ slotState: 2, heartbeatFresh: true }] },
            errors: new Map([["lowSample#2", { code: "lowSample", ch: 2 }]]),
        };
        const all = TT.rowsFromStore(st);
        const ctx = TT.rowContext(st);
        let same = true;
        for (let ch = 1; ch <= 15; ch++) {
            if (
                JSON.stringify(TT.rowFromStore(st, ch, ctx)) !==
                    JSON.stringify(all[ch - 1]) ||
                JSON.stringify(TT.rowFromStore(st, ch)) !==
                    JSON.stringify(all[ch - 1])
            ) {
                same = false;
            }
        }
        check(
            same,
            "rowFromStore(ch) ≡ rowsFromStore()[ch-1](传 ctx 与不传 ctx 都一致)",
        );
    }

    // (b) 拖动期按行增量:pan/vol/width 三条拖动路径都不请求整页
    check(
        /requestRowRender\(d\.ch\)/.test(s) &&
            /sendParam\(d\.id, next, d\.ch\)/.test(s),
        "pan/vol/width 拖动期走按行增量(requestRowRender / sendParam 带 rowCh)",
    );
    check(
        /function requestRowRender\(ch\)[\s\S]{0,500}dirtyRows\.add\(ch\)[\s\S]{0,400}requestAnimationFrame/.test(
            s,
        ),
        "requestRowRender 有 rAF 合帧(同帧多次只跑一次)",
    );
    check(
        /function renderRows\([\s\S]{0,300}dirtyRows\.clear\(\)/.test(s),
        "整页 renderRows 跑过后作废排队中的单行增量(不重复投影)",
    );
    check(
        /function frameCtx\(st\)[\s\S]{0,600}rowContext\(st\)/.test(s),
        "整页与按行增量共用同一份跨行公共量 frameCtx(15 行只算一次)",
    );

    // (c) 外壳:rAF 合帧 + 只投影当前激活 tab
    check(
        /function requestRender\(\) \{\s*if \(renderQueued\) return;[\s\S]{0,300}requestAnimationFrame\(run\)/.test(
            appJs,
        ),
        "app.js requestRender 是 rAF 合帧(同帧多次请求只跑一次 render)",
    );
    for (const ev of [
        "scvb.state",
        "scvb.params",
        "scvb.conn",
        "scvb.groups",
        "scvb.playhead",
        "scvb.segments",
        "scvb.captureProgress",
        "scvb.error",
    ]) {
        const sub = new RegExp(
            `bridge\\.on\\("${ev.replace(".", "\\.")}"[\\s\\S]*?\\n    \\}\\);`,
        ).exec(appJs);
        check(
            !!sub && !/(?<![A-Za-z])render\(\);/.test(sub[0]),
            `${ev} 订阅不再同步直呼 render()(一律经 requestRender 合帧)`,
        );
    }
    check(
        /switch \(content\.getAttribute\("data-tab"\)\)[\s\S]{0,400}tabWave\.render\(\)/.test(
            appJs,
        ),
        "render() 只投影当前激活 tab(四面板同在 DOM,按需渲染)",
    );
    check(
        /if \(was !== name\) requestRender\(\);/.test(appJs),
        "切 tab 补一次整页 render(按需渲染的配对不变式)",
    );
    // §2.5 30Hz meters 照旧不触发 render(T31 起的既有纪律,别被改回去)
    {
        const sub = /bridge\.on\("scvb\.meters"[\s\S]*?\n    \}\);/.exec(appJs);
        check(
            !!sub && !/[Rr]ender\(\)/.test(sub[0]),
            "scvb.meters 仍只喂 rAF 弹道,不触发任何整页渲染",
        );
    }
    // Tab3 不许再叠第二层 rAF(外壳已合帧;叠了会把拖动反馈推迟两帧)
    check(
        !/renderQueued/.test(src("web/output/tab-wave.js")),
        "tab-wave 的本地 rAF 合帧已并入外壳(全页唯一合帧点)",
    );

    // (d) meter 弹道循环自停 —— **行为断言**,不是源码正则。
    //     性能改造唯一真正改运行时行为的编辑就是这条循环;「行为不变」必须由
    //     驱动 rAF 的端到端序列兜底,否则「只升不降」这种 red 能一路绿灯进来。
    {
        // 假 rAF:queue 里的回调在 frame() 里按 16.7ms 步进统一喂时间戳
        const realRaf = globalThis.requestAnimationFrame;
        const realCaf = globalThis.cancelAnimationFrame;
        let now = 0;
        let queue = [];
        let reqs = 0;
        globalThis.requestAnimationFrame = (fn) => {
            queue.push(fn);
            reqs++;
            return reqs;
        };
        globalThis.cancelAnimationFrame = () => {};
        const frame = () => {
            const q = queue;
            queue = [];
            now += 16.7;
            for (const fn of q) fn(now);
        };
        try {
            const store = { playhead: { isPlaying: true, timeS: 0 } };
            const m = MT.createMeterRenderer({
                body: null,
                getStore: () => store,
                isActive: () => true,
            });
            const ev = (db, pk) => ({
                tracks: Array.from({ length: 15 }, (_, i) =>
                    i === 0 ? { db, peakDb: pk } : { db: -60, peakDb: -60 },
                ),
            });
            m.attach();
            m.start();
            // ① 先让弹道在地板上停住(自停条件成立 = 空闲零 rAF 的设计意图)
            for (let i = 0; i < 6; i++) {
                m.push(ev(-60, -60));
                frame();
            }
            const idleBase = reqs;
            for (let i = 0; i < 30; i++) {
                m.push(ev(-60, -60));
                frame();
            }
            eq(reqs - idleBase, 0, "稳态 30 帧零新增 rAF(循环确实自停)");
            // ② 停住之后来一次瞬态再回落:**下行必须跟得动**。
            //    start() 把 lastMs 置 null ⇒ 起帧首帧恒为 dt=0,而 advance() 在
            //    dt=0 时只做上行瞬时跟随、下行一步不走;首帧若参与自停判据,
            //    「push → dt=0 → 不动 → 自停」会死循环,液柱永远卡在瞬态电平。
            m.push(ev(-8, -8));
            frame();
            frame();
            eq(m.stateOf(1).db, -8, "瞬态:上行瞬时跟随(口径②上行档)");
            for (let i = 1; i <= 20; i++) {
                m.push(ev(-8 - 2.5 * i, -8 - 2.5 * i));
                frame();
                frame();
            }
            const st = m.stateOf(1);
            check(
                st.db <= -50,
                `回落 20 个事件后液柱跟到底(口径② fast-follow;实得 ${st.db}）`,
            );
            check(
                st.peakDb === -8 && st.peakHeldMs > 0,
                "峰线仍按 peak-hold 保持在 -8(口径③保持期计时在走)",
            );
            // ③ 停止态归零(口径④)——自停不得把复位那一帧也吃掉
            store.playhead = { isPlaying: false, timeS: 0 };
            m.push(ev(-8, -8));
            frame();
            frame();
            eq(
                m.stateOf(1).db,
                MT.METER_FLOOR_DB,
                "停止态复位归零(口径④),循环自停不影响",
            );
        } finally {
            globalThis.requestAnimationFrame = realRaf;
            globalThis.cancelAnimationFrame = realCaf;
        }
    }
}

// =============================================================================
log("=== ⑩ 主列表 demo 轨名本地化(store 层;三语 + 非 demo 不改写)===");
{
    const cjk = /[\u4e00-\u9fff]/;
    eq(DEMO_LABELS.length, 15, "demo 轨名 15 条");
    // demo 快照 channels 形状(label = zh demo 原值;其余字段随它)
    const demoChannels = DEMO_LABELS.map((label, i) => ({ ch: i + 1, label }));
    for (const lang of ["en", "fr"]) {
        const names = localizeDemoChannels(demoChannels, lang, T).map(
            (c) => c.label,
        );
        check(names.length === 15, `${lang} 主列表 15 轨名齐备`);
        check(
            !names.some((n) => cjk.test(n)),
            `${lang} 主列表 15 轨名无 CJK(实得 ${names.join(" / ")})`,
        );
        check(new Set(names).size === 15, `${lang} 主列表 15 轨名两两互异`);
    }
    // zh 原值不变。
    eq(
        localizeDemoChannels(demoChannels, "zh", T).map((c) => c.label),
        DEMO_LABELS,
        "zh 主列表轨名保持原值",
    );
    // 真实/用户轨名(非 demo 词条)永不改写:撞名误伤的回归门禁。
    const edited = [
        { ...demoChannels[0], label: "My Vocal" },
        ...demoChannels.slice(1),
    ];
    eq(
        localizeDemoChannels(edited, "en", T)[0].label,
        "My Vocal",
        "用户改过的 label 不被本地化覆盖",
    );
    // 无字典不本地化(仍返回 zh 原值)。
    eq(
        localizeDemoChannels(demoChannels, "en", null).map((c) => c.label),
        DEMO_LABELS,
        "不传字典保持原 label",
    );
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n失败 ${fail} 条 ❌`);
process.exit(fail === 0 ? 0 : 1);
