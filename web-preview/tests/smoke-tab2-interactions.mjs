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

    // 【SL-191 用户裁定 2026-08-27】柱头必须够得着白线 —— 模型这一半:
    // 只要本帧电平**超过**当前保持的峰值,液柱与峰线就取同一个 target(口径②上行瞬时
    // 跟随 + 口径③峰值被超越即顶到新值),于是 db 与 peakDb **逐位相等**。
    // 用户实测「柱头从来没到过白线」查下来不在这一层(这里一直是对的),而在渲染层的
    // CSS transition —— 那一半由 smoke-monitor 的 SL-191 组守(液柱/峰线都不带
    // transition ⇒ 渲染逐帧等于本模型)。两条合起来才是「柱头真能触线」的完整保证。
    for (const [prevDb, prevPeak, target] of [
        [-30, -8, -6], // 从低位一跃超过旧峰
        [-12, -12, -12], // 持平旧峰(>= 也算超越,同样归零计时)
        [-60, -60, 0], // 一路顶到 0 dBFS 满幅
    ]) {
        const st = MT.advance(
            { db: prevDb, peakDb: prevPeak, peakHeldMs: 900 },
            target,
            16.7,
        );
        eq(
            [st.db, st.peakDb, st.peakHeldMs],
            [target, target, 0],
            `SL-191 新极大值那一帧 db==peakDb(prev ${prevDb}/${prevPeak} → ${target})`,
        );
        eq(
            MT.dbToRatio(st.db),
            MT.dbToRatio(st.peakDb),
            `SL-191 该帧行程比也逐位相等(--lv 与 --pk 写出同一个数)`,
        );
    }

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

    // [J85] 用户裁定 2026-08-27(方案 A):**冻结通道不弹确认条**,未冻结通道照旧弹。
    // 两向都断:只断一边的话,「永远不弹」和「永远弹」各有一半能蒙混过去。
    // freeze 位:bit0=pan / bit1=vol。
    eq(
        TT.needsManualConfirm(0, "vol", false),
        true,
        "[J85] 未冻结 + 未确认过 ⇒ 弹(整表压成常值段是破坏性操作,要用户点头)",
    );
    eq(
        TT.needsManualConfirm(0, "pan", false),
        true,
        "[J85] 未冻结 pan + 未确认过 ⇒ 弹",
    );
    eq(
        TT.needsManualConfirm(2, "vol", false),
        false,
        "[J85] 冻结 vol ⇒ 不弹(不替换任何段、不入撤销栈,确认条正文两句都不成立)",
    );
    eq(TT.needsManualConfirm(1, "pan", false), false, "[J85] 冻结 pan ⇒ 不弹");
    // **逐维**而非整行:冻 pan 不该让 vol 那一维也免弹(vol 仍会整表压曲线)。
    eq(
        TT.needsManualConfirm(1, "vol", false),
        true,
        "[J85] 只冻 pan 时拖 vol ⇒ 仍要弹(逐维判定)",
    );
    eq(
        TT.needsManualConfirm(2, "pan", false),
        true,
        "[J85] 只冻 vol 时拖 pan ⇒ 仍要弹(逐维判定)",
    );
    // 「每轨每会话一次」优先级最高:确认过之后两条通道都不再弹。
    eq(
        TT.needsManualConfirm(0, "vol", true),
        false,
        "已确认过 ⇒ 不再弹(每轨每会话一次)",
    );
    // ------------------------------------------------------------------
    // [SL-199] native 侧 scvb.params / scvb.segments 的**调用点钉子**。
    //
    // 为什么一颗 C++ 钉子会落在 web 冒烟里:`OutputEditor.cpp` 编不进任何 C++ 测试目标
    // (它依赖 WebViewHost/WebView2,harness 也因此给 createEditor 一个空实现),
    // 于是 `BRIDGEARGS-SL199-*` 只守得住那几个纯函数本身,守不到「emitTick 还在调它们」
    // 这一跳 —— 退化改法(换回裸 `emitParams(first)`、或置了闩锁却不 settle 就清位)
    // 在 C++ 单测里全绿而 bug 原样回归。手法与 tab-wave.js 的 segEndS 调用点钉子同款,
    // 不新增门禁面(gate 3e 本来就跑这一套)。
    //
    // ⚠ 负向钉一律带 `^\s+…$` 的**行形态**约束:`src()` 读的是整个文件、不区分代码与注释,
    // 而 OutputEditor.cpp 的注释里就逐字写着退化改法的样子(「把下面这行换回 emitParams(first)」)。
    // 不约束行形态的话,哪天有人把注释补上一个分号,这颗钉就会在**没有任何行为退化**的情况下
    // 判红,而失败信息说的是「不再有裸 emitParams(first)」—— 排查的人得先怀疑代码再怀疑注释。
    {
        const oe = src("src/output/OutputEditor.cpp");
        // 判据形态见下面用它的那处 check。写成正则字面量(不是 new RegExp 字符串),
        // 免去一层转义:`//[^\r\n]*` 吃掉中间任意多行 // 注释。
        //
        // ⚠ 块体用**回火**写法 `(?:(?!processor_\.armResegment\()[^{}])*`,不是朴素的
        // `[^{}]*`:后者只禁大括号,块内多出一句 `processor_.armResegment(...)` 照样匹配 ——
        // 而「块内不含 armResegment」正是这颗钉自称要断言的两件事之一。
        const RE_ARM_OUTSIDE_CHANGED =
            /if \(changed\)\s*\{\s*\+\+rt\.configSeq;(?:(?!processor_\.armResegment\()[^{}])*\}\s*(?:\/\/[^\r\n]*[\r\n]+\s*)*processor_\.armResegment\(/;
        // 取**这一个函数自己的**函数体:从它的定义处到下一个 `void OutputEditor::` 定义为止。
        //
        // ⚠ 不能用「定义处起固定长度的窗口」:`handleSetVadParams` 只有约 1900 字符,
        // 取 4000 会把紧随其后的 `handleSetSegmentation` 整个吞进来 —— 于是 vad 那一轮的
        // 断言被 segmentation 的代码喂饱,**两者只要有一个写对就双双通过**。
        // (两处都是 [SL-255/viz-r4] 收口时重跑反向注入抓到的:把 vad 的调用挪回
        //  `if (changed)` 内 —— 正是本条要防的那个退化 —— 本例当时依旧全绿。)
        const fnBodyOf = (name) => {
            const start = oe.indexOf(`void OutputEditor::${name}(`);
            if (start < 0) return "";
            const rest = oe.slice(start);
            const end = rest.indexOf("\nvoid OutputEditor::", 1);
            return end < 0 ? rest : rest.slice(0, end);
        };
        const oeh = src("src/output/OutputEditor.h"); // [SL-255] 闩锁字段的类型在头文件里
        check(
            /scvb::output::raiseResendLatch\(visibleNow, wasVisible_, pendingParamsFull_\);/.test(
                oe,
            ) &&
                /scvb::output::raiseResendLatch\(visibleNow, wasSegVisible_, pendingSegmentsFull_\);/.test(
                    oe,
                ),
            "[SL-199] emitTick 为 params/segments 两条路各置一次闩锁",
        );
        check(
            /settleResendLatch\(emitParams\(first \|\| pendingParamsFull_\), pendingParamsFull_\);/.test(
                oe,
            ),
            "[SL-199] params 闩锁按「这一帧真的发出去了」清位(不是无条件清)",
        );
        check(
            /scvb::output::selectParamForEmit\(lastParamsValues_, id, value, forceFull\)/.test(
                oe,
            ),
            "[SL-199] emitParams 循环体走 selectParamForEmit(选择与基线推进同源)",
        );
        check(
            /scvb::output::segmentsResendNeeded\(first, pendingSegmentsFull_, pendingAnalyzed,/.test(
                oe,
            ), // pendingAnalyzed 现为由 reason 枚举派生的局部 bool(见下条),判定入参形态未变
            "[SL-199] scvb.segments 的重发判定走 segmentsResendNeeded 且带闩锁位",
        );
        // [SL-255] `pendingAnalyzed` 由 bool 升成三值枚举 `pendingAnalyzedReason_`
        // ——「完成了没」变成「哪一种完成」(analyze / vad / segmentation)。守的性质一字未变:
        // **发出去了才清位**。故判据跟着形态走,但两条断言的语义与 SL-199 当初立的完全相同。
        check(
            /settleResendLatch\(sent, pendingSegmentsFull_\);/.test(oe) &&
                /if \(sent\)\s+pendingAnalyzedReason_ =\s+ScvbOutputAudioProcessor::AnalysisDoneReason::None;/.test(
                    oe,
                ),
            "[SL-199] segments 与 analyzed 两个闩锁都按实际下发清位(analyzed 侧已升为 reason 枚举)",
        );
        check(
            /if \(analyzedReason != ScvbOutputAudioProcessor::AnalysisDoneReason::None\)\s+pendingAnalyzedReason_ = analyzedReason;/.test(
                oe,
            ),
            "[SL-199] analyzed 闩住(takeAnalysisDone 取走即清,隐藏期完成的分析不能丢 reason)",
        );
        // [SL-255] 闩住的必须是 **reason** 而不只是 bool:隐藏期完成的那一次若只闩「完成了」,
        // 恢复可见时 reason 会退化成 "analyze",而 Tab3 的倒计时撤条认的是 vad|segmentation|analyze
        // 里**对应的那一个**,松手档的条就撤不掉。
        check(
            !/bool pendingAnalyzed_ = false;/.test(oeh) &&
                /AnalysisDoneReason pendingAnalyzedReason_/.test(oeh),
            "[SL-255] 闩锁存的是 reason 枚举,不是 bool(退回 bool 会丢松手档的 reason)",
        );
        // [SL-255 复审②] `armResegment` 必须落在 `if (changed)` **之外**。
        //
        // 为什么只能在源码形态上钉:这两个 handler 属 `OutputEditor.cpp`,只编进插件目标,
        // 任何测试可执行文件都链不到它(`scvb_host_tests` 连 createEditor 都是桩)——
        // 「桥面 setter → 排防抖」这半条链没有可执行的落点。故按本仓既有做法
        // (SL-199 那一组同款)钉源码形态,并在 host 侧用「防抖窗内重复布防要重排」
        // 把**下半条链**的语义单独钉住,两边合起来才是完整的回归网。
        //
        // 判据形态:`if (changed)` 块体里**只剩** `++rt.configSeq;`(块内不含 armResegment),
        // 且紧随其后(可隔注释)就是 `processor_.armResegment(`。
        for (const setter of ["handleSetVadParams", "handleSetSegmentation"]) {
            const body = fnBodyOf(setter);
            check(
                body.length > 0 && RE_ARM_OUTSIDE_CHANGED.test(body),
                `[SL-255] ${setter}:armResegment 在 if (changed) 之外(防抖的是调用流,不是变化流)`,
            );
        }

        // 反面钉(带行形态约束,见上面的注释):旧写法不得残留。
        check(
            !/^\s+emitParams\(first\);\s*$/m.test(oe),
            "[SL-199] 不再有裸 emitParams(first)",
        );
        check(
            !/^\s+if \(first \|\| analyzed \|\|/m.test(oe),
            "[SL-199] 不再有手写的 segments 重发条件(已收进 segmentsResendNeeded)",
        );
    }

    // 源码级:requestManual 必须走这个判定,不许退回裸 manualConfirmed.has(ch)
    check(
        /function requestManual[\s\S]{0,400}?needsManualConfirm\(/.test(s),
        "requestManual 走 needsManualConfirm(不是裸 manualConfirmed.has)",
    );
    // **两个拖拽入口也必须走同一个判定**:拖拽落地走 endDrag → sendManual,**不经
    // requestManual**,所以它们各自持一份闸口。上一轮只钉了 requestManual,这两处就留着
    // 裸判定绿着漏过来了 —— pan 只在冻结态可拖,于是「必然已冻结」的 pan 首拖照弹一条
    // 按裁定不该弹的确认条,还把「每轨每会话一次」的额度(按轨记)烧掉,使得真该弹的
    // 未冻结 vol 首拖反而不弹(#106 终轮复审重要①)。
    for (const fn of ["beginVolDrag", "beginPanDrag"]) {
        check(
            new RegExp(
                `function ${fn}[\\s\\S]{0,700}?needsManualConfirm\\(`,
            ).test(s),
            `${fn} 走 needsManualConfirm(拖拽入口不经 requestManual,必须自己接上)`,
        );
        check(
            !new RegExp(
                `function ${fn}[\\s\\S]{0,700}?if \\(!local\\.manualConfirmed\\.has\\(ch\\)\\)`,
            ).test(s),
            `${fn} 不再留裸 manualConfirmed.has 闸口`,
        );
    }
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
    // [SL-188] `rowFromStore` 必须把 freeze **原值**交给 `freezeBits`,不许自己先 Math.trunc:
    // 1.9 截断成 1 = 只冻 pan,而 native `freezeBitsOf` 与 mock 都进位成 2 = 只冻 vol,
    // 同一个值在三侧给出两种答案(今天 freeze 是 AudioParameterInt 不可达,但这是本批
    // 「三侧同一个答案」不变量在仓里的唯一反例)。
    {
        const rowOfFreeze = (f) =>
            TT.rowFromStore(
                {
                    state: { global: { version_active: 1 }, channels: [] },
                    params: { values: { v1_t01_freeze: f }, versionActive: 1 },
                },
                1,
            );
        eq(
            [rowOfFreeze(1.9).fp, rowOfFreeze(1.9).fv],
            [0, 1],
            "[SL-188] freeze=1.9 ⇒ 行模型进位成 2(只冻 vol),与 freezeBitsOf 同答案",
        );
        eq(
            [rowOfFreeze(4).fp, rowOfFreeze(4).fv],
            [1, 1],
            "[SL-188] freeze=4 ⇒ 行模型钳成 3(两维都冻),不按位截成 0",
        );
        // 与纯函数逐值对拍:rowFromStore 这一跳不得再引入第二种口径
        for (const f of [0, 0.4, 1, 1.9, 2, 2.5, 3, 4, 99, -1]) {
            const b = TT.freezeBits(f);
            eq(
                [rowOfFreeze(f).fp, rowOfFreeze(f).fv],
                [b.pan ? 1 : 0, b.vol ? 1 : 0],
                `[SL-188] rowFromStore(freeze=${f}) 与 freezeBits 逐值一致`,
            );
        }
    }
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
    // 契约 §1.16「返回」行([J85]):非有限 value 一律 badArg,**不静默夹取** ——
    // 冻结维度上这个数就是 DSP 的音频目标值,夹到 0 会把「JS 侧算出了 NaN」藏起来。
    // 真桥 handleSetTrackManual 与 mock 两侧同款(CLAUDE.md §10)。
    for (const bad of [NaN, Infinity, -Infinity]) {
        eq(
            await bridge.setTrackManual(2, "vol", bad),
            { ok: false, reason: "badArg" },
            `[J85] 非有限 value(${bad})⇒ badArg`,
        );
    }
    // 轨号 / 维度名非法同属 badArg 这一档(与真桥同一个 if)。
    eq(
        await bridge.setTrackManual(99, "vol", 0),
        { ok: false, reason: "badArg" },
        "轨号越界 ⇒ badArg",
    );
    eq(
        await bridge.setTrackManual(2, "width", 0),
        { ok: false, reason: "badArg" },
        "维度名非法 ⇒ badArg",
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
    // ⚠ 本条断言有个**前提**:`setParamPlane` 带去重门(值没变就不发 `scvb.params`),
    // 与真桥 `emitParams` 的 diff 门同款。所以这一次写入的值必须**与上一次不同** ——
    // 上面写的是 −6、这里写 −12,故参数面帧一定会发。若把 −12 改成 −6,params 帧会被
    // 去重门吞掉、`paramIdx` 恒为 −1,断言变成「永远红」而不是「测出了时序问题」。
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
    //
    // ⚠ SL-190 的要害就在这个形状上:§1.6 的 `startS?` / `endS?` 是**可选**字段,解冻提示条
    // 的「重新识别(含手动段)」只给 tracksMask。真桥 parseAnalyzeScope 此前把两者都兜底成
    // 0.0 —— 范围 [0,0]、`startAnalysis` 在 `!(endS > startS)` 处当场回 {ok:false},一段都不
    // 重算(用户实测「点了没什么作用」)。mock 一直把缺省取成 ±∞,所以这条冒烟从来没红过。
    // 修复把真桥对齐到「缺省 = 未指定 = 走 "all" 那条推导」(纯函数与两向回归见 C++ 侧
    // `analyzeScopeRange` / test_segment_edit_service.cpp)。这里把 mock 侧的两向也钉住,
    // 免得哪天有人把 mock 改成 [0,0] 兜底、两侧又悄悄分叉回去。
    //
    // 先发**显式空范围**:它必须被拒 —— 「缺省」和「显式 [0,0]」不是一回事。
    // 顺序不能反:analyze 受理后会置 analysis_run.running,紧跟着的第二发会回 reason:"busy",
    // 那样这条断言就会因为错误的理由变绿。
    const emptyRange = await bridge.analyze(
        { tracksMask: 1 << 1, startS: 0, endS: 0 },
        { clearManual: true },
    );
    check(
        !!emptyRange && emptyRange.ok === false && !emptyRange.reason,
        `显式 [0,0] 范围 ⇒ 拒绝(§1.6 拒绝态不带 reason);实得 ${JSON.stringify(emptyRange)}`,
    );

    seen.segments.length = 0;
    const ar = await bridge.analyze(
        { tracksMask: 1 << 1 },
        { clearManual: true },
    );
    check(!!ar && "ok" in ar && !!ar.affected, "analyze 回受理回执 + 影响面");
    check(
        ar.ok === true,
        `缺省 startS/endS ⇒ 按整条时间线受理(不是空范围);实得 ${JSON.stringify(ar)}`,
    );
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
log("=== ⑥b 单轨重新识别的请求形状(SL-190)===");
{
    const s = src("web/output/tab-tracks.js");
    // 契约 §1.6:scope 对象形 = {tracksMask, startS?, endS?}。Tab2 解冻提示条只给轨掩码,
    // 范围交给 native 按「未指定」推导(= "all" 同款)。这条断言钉的是**请求形状**本身 ——
    // 真桥那侧的缺省口径由 C++ 的 analyzeScopeRange 保证,两边合起来才是完整的回归。
    // 若有人在这里补上 startS:0/endS:0「显式化」,native 会照单全收 → 空范围 → 又回到
    // 「点了没反应」。所以这里必须红。
    // ⚠ 这条断言**刻意**把「JS 行为」耦合到源码文本上(node 侧无 DOM,拿不到真实点击)。
    // 定位用「从 call("analyze" 起、最多 240 字符」的有界切片,不去数花括号 —— 数花括号的
    // 写法在 scope 里多一层对象时会静默失配(PR #112 评审建议⑤)。切片够长能覆盖整个调用,
    // 又不至于跨到下一个语句;真改写成别的形状时第一条断言先红,不会静默放行。
    const call = s.match(/call\(\s*"analyze",[\s\S]{0,240}/);
    check(!!call, "tab-tracks.js 里能定位到单轨重新识别的 analyze 调用");
    if (call) {
        check(
            /tracksMask:\s*1\s*<<\s*\(ch\s*-\s*1\)/.test(call[0]),
            "scope 只给 tracksMask(按轨掩码)",
        );
        check(
            !/startS|endS/.test(call[0]),
            "scope **不带** startS/endS —— 缺省即整条时间线,显式 [0,0] 会被 native 判成空范围",
        );
        // tracksMask 必须**非零**:0 在 native 侧是「不限轨」,而「不指名轨 + 不给范围」
        // 被 analyzeScopeRange 判成空范围 —— 否则 clearManual 会全轨全时间线清 origin,
        // 而 §1.6 明写「撤销:否」(PR #112 评审重要)。
        check(
            !/tracksMask:\s*0[,\s}]/.test(call[0]),
            "scope 的 tracksMask 不是字面 0(0 = 不限轨,配 clearManual 是全轨 origin 全清)",
        );
        check(
            /clearManual:\s*true/.test(call[0]),
            "opts 带 clearManual:true(§1.6「重新识别(含手动段)」)",
        );
    }
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
log(
    "=== ⑪ SL-211②:切进版本还没播放时,未冻结维度读**曲线起始值**而非出厂默认 ===",
);
{
    // 用户实测:复制版本并切进去后,播放前 15 轨声像全在中间;一播放又全对了。
    // 定谳:未冻结维度的读回值原先回落到**参数面**,而参数面在非 PRINT 态装的是宿主
    // 那一份 —— 刚切版本时就是出厂默认(pan 居中)。引擎开始驱动参数面之后才对上。
    const segCh = {
        segments: [
            { segIdx: 0, t0S: 0, t1S: 10, pan: -60, volDb: -3, origin: "auto" },
            { segIdx: 1, t0S: 10, t1S: 20, pan: 40, volDb: 2, origin: "auto" },
        ],
    };
    // 钳位口径逐条对齐 CurveEvaluator::valueAt(复审终轮③b)——
    // src/core/engine 那份是 DSP 真身,显示层跟它走才叫「显示权威」。
    eq(TT.curveSegmentAt(segCh, 0).pan, -60, "(a1)t=0 取曲线起点");
    eq(TT.curveSegmentAt(segCh, 5).pan, -60, "(a2)t 落在首段内");
    eq(TT.curveSegmentAt(segCh, 15).pan, 40, "(a3)t 落在次段内");
    eq(
        TT.curveSegmentAt(segCh, 999).pan,
        40,
        "(a4)**末段之后取末段**(不是首段;引擎那边就是「末段之后保持末段值」)",
    );
    {
        // 「曲线前」与「曲线后」必须分成两档 —— 原先混成一档都回落 segs[0],
        // 播放头停在曲线尾端时会显示曲线开头的值,和耳朵对不上。
        const gapped = {
            segments: [
                { t0S: 10, t1S: 20, pan: -60 },
                { t0S: 30, t1S: 40, pan: 5 },
                { t0S: 50, t1S: 60, pan: 80 },
            ],
        };
        eq(TT.curveSegmentAt(gapped, 0).pan, -60, "(a4a)首段之前 ⇒ 首段");
        eq(TT.curveSegmentAt(gapped, 99).pan, 80, "(a4b)末段之后 ⇒ 末段");
        eq(
            TT.curveSegmentAt(gapped, 25).pan,
            -60,
            "(a4c)段间空隙 ⇒ **前一段**(引擎:ramp 之前保持前段值)",
        );
        eq(TT.curveSegmentAt(gapped, 45).pan, 5, "(a4d)第二个空隙同理取前一段");
    }
    eq(
        TT.curveSegmentAt({ segments: [] }, 0),
        null,
        "(a5)段表为空 ⇒ null,调用方回落参数面(还没分析过,曲线本就不存在)",
    );
    eq(
        TT.curveSegmentAt({ segments: [{ t0S: 0, t1S: 0, pan: 7 }] }, 123).pan,
        7,
        "(a6)开放尾段(t1 缺值/哨兵)从 t0 一直算到底",
    );

    // 端到端:未冻结 + 有曲线 ⇒ 读曲线;冻结 ⇒ 仍读参数面(J85 不变)
    const store = (freeze, outputOn = true) => ({
        state: {
            channels: [{ ch: 1, label: "A" }],
            global: { version_active: 1, output_enabled: outputOn },
        },
        params: {
            values: {
                [TT.paramIdOf(1, 1, "pan")]: 0, // 参数面 = 出厂默认居中
                [TT.paramIdOf(1, 1, "freeze")]: freeze,
            },
            versionActive: 1,
        },
        segments: { channels: [{ ch: 1, segments: segCh.segments }] },
        playhead: { timeS: 0, isPlaying: false },
    });
    eq(
        TT.rowFromStore(store(0), 1).pan,
        -60,
        "(b1)未冻结 + 未播放 ⇒ 显示曲线起始值 −60(不是参数面的 0)",
    );
    eq(
        TT.rowFromStore(store(1), 1).pan,
        0,
        "(b2)pan 冻结 ⇒ 仍读参数面(J85:冻结维度的权威在参数面)",
    );
    const playing = store(0);
    playing.playhead = { timeS: 15, isPlaying: true };
    eq(TT.rowFromStore(playing, 1).pan, 40, "(b3)播放头进次段 ⇒ 读该段的值");
    const noSeg = store(0);
    noSeg.segments = { channels: [{ ch: 1, segments: [] }] };
    eq(TT.rowFromStore(noSeg, 1).pan, 0, "(b4)还没分析过 ⇒ 回落参数面");

    // [复审终轮③a 裁定] 未冻结那一支**按输出档分叉** —— 显示的永远是该档的权威:
    //   ON(写入自动化)= 引擎按曲线驱动 ⇒ 显示曲线;
    //   OFF(跟随宿主)= 引擎不驱动、声音跟宿主参数面 ⇒ 显示参数面。
    //   只在 ON 档对上还不够:OFF 档显示曲线就成了「看着曲线、听着宿主」。
    eq(
        TT.rowFromStore(store(0, true), 1).pan,
        -60,
        "(b5)输出 ON ⇒ 显示曲线(引擎按曲线驱动)",
    );
    eq(
        TT.rowFromStore(store(0, false), 1).pan,
        0,
        "(b6)输出 OFF ⇒ 显示参数面(该档权威 = 宿主,与 DSP 一致)",
    );
    // 冻结维度不受输出档影响:两档都读参数面(J85)
    for (const on of [true, false]) {
        eq(
            TT.rowFromStore(store(1, on), 1).pan,
            0,
            `(b7)pan 冻结 + 输出 ${on ? "ON" : "OFF"} ⇒ 都读参数面`,
        );
    }
}

// =============================================================================
log(
    "=== ⑫ R4:SL-229 切版本读回命名空间 / SL-230 轨道页「恢复自动」持久入口 ===",
);
{
    const tw = src("web/output/tab-tracks.js");
    const P = (v, ch, d) => TT.paramIdOf(v, ch, d);

    // ---- SL-229:切版本的那一帧不许闪出厂默认 ----------------------------
    // 病:state.global.version_active 与 scvb.params 是两个事件。state 一说「现在是
    // V2」,读回值立刻改查 v2_* 的 id —— 可 params 手上还是上一版那 63 个(§2.2:
    // 切版本由 C++ 全量重发),查空 ⇒ num(undefined, def) 回落**居中**,15 轨齐刷刷
    // 跳到中间;下一帧全量到达才跳回真值。
    {
        const R = TT.readbackVersion;
        const v1 = { [P(1, 1, "pan")]: -70 };
        const v2 = { [P(2, 1, "pan")]: 33 };
        eq(
            R(v1, 2, 1),
            1,
            "(a1)params 还没带 V2 的 id ⇒ 仍读 V1(= 切出时那一版)",
        );
        eq(R(v2, 2, 1), 2, "(a2)params 带上 V2 的 id ⇒ 原子翻到 V2");
        eq(R(v1, 1, 1), 1, "(a3)没切版本时照旧");
        eq(R({}, 0, 3), 3, "(a4)state 没给版本 ⇒ 用 params 那一版");
        eq(R({}, 0, 0), 1, "(a5)两边都没给 ⇒ 回落 1(不炸)");
        eq(R(null, 2, 0), 2, "(a6)vals 缺失 ⇒ 用 state 那一版(没有更好的信息)");
    }
    // 端到端:切版本那一帧读到的是**源版本的值**,不是居中
    {
        const st = (active) => ({
            state: {
                channels: [{ ch: 1 }],
                global: { version_active: active, output_enabled: false },
            },
            params: {
                values: { [P(1, 1, "pan")]: -70, [P(1, 1, "freeze")]: 0 },
                versionActive: 1,
            },
            segments: { channels: [{ ch: 1, segments: [] }] },
            playhead: { timeS: 0 },
        });
        eq(TT.rowFromStore(st(1), 1).pan, -70, "(b1)切换前");
        eq(
            TT.rowFromStore(st(2), 1).pan,
            -70,
            "(b2)切换那一帧仍是 −70(不闪居中 0)—— 这就是 SL-229 的验收",
        );
    }

    // ---- SL-230:轨道页「恢复自动」持久入口 --------------------------------
    // 定谳:单轨 clearManual 此前**唯一**的入口是解冻那一下的临时提示条,
    // 只在 freeze 位 1→0 时挂起、点「知道了」就永久消失 —— 用户因此找不到。
    {
        check(
            /data-gb="\$\{gb\("restore-auto-row"\)\}"/.test(tw),
            "(c1)行内有持久入口",
        );
        // [复审②] 触发钮**在行内**、不是浮条:.tracks-row__hint 是 absolute+top:100%,
        // 浮在下一行上 —— 常驻之后两条以上手动轨会各盖住下一行,下一行点不动,
        // 正是本卡要消灭的「点了没反应」。行高 44px 是设计常量,浮条改占位会顶掉布局。
        check(
            /<button type="button" class="tracks-row__restore"/.test(tw),
            "(c2)触发钮是行内小件(落在 label 单元格,与既有条件角标同族)",
        );
        // [#148 复审【建议】2] 只读观察态 / srErr 死轨要一并挡掉:`doReidentify` 的写面
        // 守卫会静默返回,钮再露出来又是一枚「点了没反应」。`dis` = 整行 disabled 位。
        check(
            /const canRestore = !!row\.manualConst && dis !== "1";/.test(tw) &&
                /function syncConfirm\(n, row, t, ch, dis\)/.test(tw),
            "(c2b)该轨仍被手动常值驱动**且整行可写**才出",
        );
        // 确认浮条一次只出一条(它和解冻提示条同族,两条同时展开会互相盖)
        check(
            /restoreConfirm: 0,/.test(tw) &&
                /local\.restoreConfirm === ch/.test(tw),
            "(c2c)确认态是**单值**不是 Set —— 浮条一次只出一条",
        );
        // 三枚钮不受整行 disabled 约束(它们是撤下确认的出口),写面守卫在 doReidentify
        check(
            /if \(part === "restore-auto"\) \{/.test(tw) &&
                /if \(part === "restore-auto-ok"\) \{/.test(tw),
            "(c3)三枚钮已接线",
        );
        check(
            /doReidentify\(ch\); \/\/ 与解冻提示条逐字同一条路/.test(tw),
            "(c4)走既有 analyze(tracksMask,{clearManual:true}),不另造一条路",
        );
        // 锁定的手动常值:clearManual 碰不了(§1.6 locked 免疫)⇒ 只说不做,
        // 否则又是一个「点了没反应」——正是 SL-230 本身的病根
        // 锁定档:钮置灰 + title 说原因(口径同 SL-193 的灰钮),不给一次
        // 「展开确认再什么都不发生」—— clearManual 对 locked 段免疫
        check(
            /const lockedConst = !!row\.manualConstLocked;/.test(tw) &&
                /attr\(n\.restoreAuto, "data-disabled", lockedConst \? 1 : 0\);/.test(
                    tw,
                ),
            "(c5)锁定档把钮置灰",
        );
        check(
            /tracks\.restoreAutoLocked/.test(tw) &&
                /attr\(n\.restoreAuto, "title", tip\);/.test(tw),
            "(c5b)灰着的原因走 title(不让人干瞪一个点不动的钮)",
        );
        check(
            /if \(btn && btn\.getAttribute\("data-disabled"\) === "1"\) return;/.test(
                tw,
            ),
            "(c5c)灰钮点下去不展开确认(入口再复检一次)",
        );
        // [#148 二轮【重要】] 解冻提示条的「重新识别轨 {n}」是**同一个动作**,得给同一个结论。
        // 复审② 撤掉了两者互斥、复审③ 又让 native 真的对 locked 段免疫 —— 两下叠加之后,
        // 这枚钮在锁定档上点下去什么都不会变,而 doReidentify 已先把提示条永久撤掉,
        // 用户拿到零反馈;且它与旁边那枚置灰钮同屏给出相反结论。
        check(
            /attr\(\s*n\.manualdrivenReidentify,\s*"data-disabled",\s*lockedConst \? 1 : 0,?\s*\);/.test(
                tw,
            ) &&
                /const rb = \(local\.rows\.get\(ch\) \|\| \{\}\)\.manualdrivenReidentify;/.test(
                    tw,
                ),
            "(c5d)提示条的「重新识别轨 {n}」在锁定档同样置灰 + 入口复检",
        );
        // 置灰得**看得出来**:.tracks-row__relink 原样是可点长相,只加 data-disabled
        // 而没有配套样式的话,用户看到的是一枚长得能点、点了没反应的钮 —— 比明着灰更糟。
        check(
            /\.tracks-row__relink\[data-disabled="1"\]\s*\{[^}]*opacity:\s*0\.4/.test(
                src("web/output/index.html"),
            ) &&
                /\.tracks-row__relink\[data-disabled="1"\]:hover\s*\{/.test(
                    src("web/output/index.html"),
                ),
            "(c5d2)灰档有配套样式(含撤掉 hover 高亮)",
        );
        // [#148 二轮【建议】1] 被 c/hint 临时顶掉期间也要复位:不复位的话那两件一撤,
        // 确认浮条自己弹回来、直接停在「取消 / 继续」上(用户没点过却已经在问他)。
        check(
            /local\.restoreConfirm === ch && \(!canRestore \|\| c \|\| hint\)/.test(
                tw,
            ),
            "(c5e)展开态在被临时顶掉时一并复位",
        );
        // [#148 二轮【建议】2] 置灰要带语义:原生 <button> 的 data-disabled 不摘 tab 序、
        // 不挡 Enter/Space,只做视觉的话键盘用户拿到的正是「按了没反应」。
        check(
            /attr\(\s*n\.restoreAuto,\s*"aria-disabled",\s*lockedConst \? "true" : "false",?\s*\);/.test(
                tw,
            ),
            "(c5f)置灰同时落 aria-disabled(键盘/读屏面)",
        );
        // rowFromStore 要把 locked 位带出来,否则上一条判据永远为假
        {
            const seg = (locked) => ({
                channels: [
                    {
                        ch: 1,
                        segments: [
                            {
                                t0S: 0,
                                t1S: 99,
                                pan: 5,
                                origin: "user_edited",
                                locked,
                            },
                        ],
                    },
                ],
            });
            const mk = (locked) => ({
                state: {
                    channels: [{ ch: 1 }],
                    global: { version_active: 1, output_enabled: false },
                },
                params: {
                    values: { [P(1, 1, "pan")]: 0, [P(1, 1, "freeze")]: 0 },
                    versionActive: 1,
                },
                segments: seg(locked),
                playhead: { timeS: 0 },
            });
            eq(
                TT.rowFromStore(mk(false), 1).manualConst,
                1,
                "(c6)手动常值判位",
            );
            eq(
                TT.rowFromStore(mk(false), 1).manualConstLocked,
                0,
                "(c7)未锁 ⇒ manualConstLocked=0(可恢复)",
            );
            eq(
                TT.rowFromStore(mk(true), 1).manualConstLocked,
                1,
                "(c8)已锁 ⇒ manualConstLocked=1(只说不做)",
            );
        }
    }

    // ---- SL-230 mock 对拍:手动常值**不上锁**,与真桥一致 -------------------
    // 真桥 makeManualConstantSegment 写的是 makeSegmentFlags(UserEdited, **false**);
    // mock 原先写 locked=true,于是 clearManual(对 locked 免疫)在 web-preview 里对
    // 它自己造出来的手动常值完全无效 —— 点了没反应,而真机上是有效的。
    {
        const mock = src("web-preview/mock/juce-bridge-mock.js");
        const svc = src("src/output/SegmentEditService.h");
        check(/proto\.locked = false;/.test(mock), "(d1)mock 的手动常值不上锁");
        check(
            /makeSegmentFlags\(scvb::state::SegmentOrigin::UserEdited, false\)/.test(
                svc,
            ),
            "(d2)真桥同款(本条一红说明两侧又分叉了)",
        );
    }
}

// =============================================================================
log(fail === 0 ? "\n全部通过 ✅" : `\n失败 ${fail} 条 ❌`);
process.exit(fail === 0 ? 0 : 1);
