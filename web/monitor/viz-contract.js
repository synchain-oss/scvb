// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Monitor · viz 段契约的 **JS 侧镜像**(T46;真源 = T44)
// -----------------------------------------------------------------------------
// 【真源与镜像纪律】
// 段布局的**唯一真源**是 native 侧:
//   • `docs/contract-changes/20260825-viz-segment.md`(T44 的变更真源,待批准转正进
//     `docs/IPC_CONTRACT.md` §6);
//   • `src/core/ipc/VizPlane.h`(结构体与常量);
//   • `tests/golden/ipc-layout.txt`(逐行冻结的 size/align/offset)。
// 本文件是那份契约在 JS 侧的**镜像副本**,与 `web/shared/track-colors.js` 的
// `FALLBACK_TRACK_COLORS` ↔ `tokens.css` 是同一类纪律:**两处必须同改**,且漂了不会静默 ——
// `web-preview/tests/smoke-monitor.mjs` 的「② viz 契约 parity」节把本文件的每一条常量与
// 字段名逐条比对 `tests/golden/ipc-layout.txt`。
//
// **容错口径照 T25/T28 的 `check-bridge-parity.mjs` 先例**:golden 文件里还没有 viz 段时
// (T44 尚未合入 feature/v1),parity 断言打印 [SKIP] 并跳过、不影响退出码;文件里**有**
// viz 段却与本文件对不上 —— 那才是红。这样 T44 一合并、本分支一 rebase,机检自动生效,
// 不需要谁记得回来打开它。
//
// 【为什么 UI 侧收到的是秒,不是样本】
// 段里的时间量全是**样本**(`*_samples` + `sample_rate`)。契约 §0.2 第 3 条:
// **「UI 永不见样本、只收秒」**。故 samples → 秒的换算发生在 **T45 的 C++ 桥**里,
// 本页收到的 `scvb.viz` 事件已经是秒。下面 `SAMPLES_TO_SECONDS_NOTE` 写着逐字段的换算式,
// 供对表时逐条核对(mock 侧按同一组式子造数,等于把换算也跑了一遍)。
//
// 【本文件不做的事】不解析二进制、不认 DOM、不认 i18n。它只是一组常量 + 一份字段清单。
// =============================================================================

/** 段头 magic 的 JS 侧表示(段内是 u32 `kScvbMagic`;桥投影成可读串)。 */
export const VIZ_MAGIC = "SCVB";

/**
 * 本机认得的 viz 段 abi。
 *
 * T44 的 abi 策略:**挂总 abi**(`VizHeader.abi === kScvbAbi === 1`),新增段不触发 abi+1。
 * 于是「拒连 + 横幅、绝不半兼容」(J40)的行为免费得到,判据也只有一条:
 *   • `abi > VIZ_ABI` ⇒ 对端比我新,字段可能整体挪位,读了就是读错 ⇒ **停止读取** + 红横幅;
 *   • `abi <= VIZ_ABI` ⇒ 我认得,照读。
 */
export const VIZ_ABI = 1;

/** 降采样列数(`kVizColumns`)。改动 = 布局改动 ⇒ abi+1 且段名 v2。 */
export const VIZ_COLUMNS = 1024;

/** 位图字数(`kVizCoverageWords` = 1024/32)。 */
export const VIZ_COVERAGE_WORDS = 32;

/** pan 定点标度(`kVizPanScale`):段内 int16 = round(clamp(pan,−100,100) × 100)。 */
export const VIZ_PAN_SCALE = 100;

/** 车道哨兵(`kVizPanNone` = INT16_MIN):该轨整条无数据(无分段 / 曲线缺失 / 轨未启用)。 */
export const VIZ_PAN_NONE = -32768;

/** 轨数(`kMaxChannels`;契约 §0.2 第 4 条 ch = 1..15,J59)。 */
export const VIZ_TRACKS = 15;

/** 段大小(`kVizSegmentSize`,64 KB;尾部 32640 B 预留给后续增补)。 */
export const VIZ_SEGMENT_BYTES = 65536;

/** `playhead_flags` 位(`VizPlayheadFlag`)。 */
export const VIZ_FLAG_PLAYING = 1 << 0;
export const VIZ_FLAG_LOOPING = 1 << 1;
export const VIZ_FLAG_LOOP_VALID = 1 << 2;

/**
 * 写方停摆判据(ms)。
 *
 * 判据是 **`publishMs` 不再前进**,不是「有没有收到事件」—— 段是 4Hz 低频发布,
 * 单看事件到没到会把正常的发布间隔误判成停摆;而 `VizFrame.publish_ms` 的注释逐字写着
 * 「读方据此判断写方是否停摆」,那就是它的用途。
 * 3 秒 ≈ 4Hz 的十二拍,不会被一次调度抖动打红。
 */
export const VIZ_STALE_MS = 3000;

/**
 * `attachReadOnly()` 的三种结果(T44 变更文档「读写方约定」表)。
 * **`failed` 与 `abiMismatch` 必须可区分**,这是 T44 显式设计的降级路径:
 *   • `failed`      —— 段不存在(该组没有 Output 在跑 / 旧版 Output 不建 viz 段)⇒ **空态**;
 *   • `abiMismatch` —— abi 或几何自检对不上 ⇒ **拒连横幅**,停止读取。
 */
export const VIZ_ATTACH = Object.freeze(["ok", "failed", "abiMismatch"]);

/**
 * 段内字段清单 —— 与 `tests/golden/ipc-layout.txt` 的 `field <Struct>.<name> offset <n>` 行
 * **逐条对拍**(顺序即偏移升序)。JS 侧收到的是桥投影后的 JSON,字段名会驼峰化、
 * 时间量会换成秒,故这里同时给出 JSON 侧的对应名,parity 断言两边都查。
 *
 * `json: null` = 该段内字段**不投影到 UI**(纯 native 侧机制:seqlock 的 seq 由桥消化、
 * `_reserved` 是填充)。写成 null 而不是省略,是为了让 parity 能断言「段里的每一个字段
 * 都被显式表过态」—— 漏掉一个新字段时,机检会说「golden 里有它、镜像表里没有」。
 */
export const VIZ_FIELDS = Object.freeze([
    // ---- VizHeader(64 B,cacheline 0)
    { struct: "VizHeader", name: "magic", json: "magic" },
    { struct: "VizHeader", name: "abi", json: "abi" },
    { struct: "VizHeader", name: "generation", json: "generation" },
    { struct: "VizHeader", name: "column_count", json: "columnCount" },
    { struct: "VizHeader", name: "track_count", json: "trackCount" },
    { struct: "VizHeader", name: "pan_scale", json: "panScale" },
    { struct: "VizHeader", name: "_reserved", json: null },
    // ---- VizFrame(128 B;seqlock 临界区)
    { struct: "VizFrame", name: "seq", json: "seq" },
    { struct: "VizFrame", name: "playhead_flags", json: "playheadFlags" },
    { struct: "VizFrame", name: "publish_ms", json: "publishMs" },
    { struct: "VizFrame", name: "window_start_samples", json: "windowStartS" },
    { struct: "VizFrame", name: "window_span_samples", json: "windowSpanS" },
    { struct: "VizFrame", name: "playhead_samples", json: "playheadS" },
    { struct: "VizFrame", name: "loop_start_samples", json: "loopStartS" },
    { struct: "VizFrame", name: "loop_end_samples", json: "loopEndS" },
    { struct: "VizFrame", name: "sample_rate", json: "sampleRate" },
    { struct: "VizFrame", name: "version_active", json: "versionActive" },
    { struct: "VizFrame", name: "playhead_epoch", json: "playheadEpoch" },
    { struct: "VizFrame", name: "track_online_mask", json: "onlineMask" },
    { struct: "VizFrame", name: "track_covered_mask", json: "coveredMask" },
    { struct: "VizFrame", name: "track_stereo_mask", json: "stereoMask" },
    { struct: "VizFrame", name: "lane_revision", json: "laneRevision" },
    { struct: "VizFrame", name: "_reserved", json: null },
    // ---- VizTrackColors(64 B)
    { struct: "VizTrackColors", name: "index", json: "colorIndex" },
    // ---- VizCoverage(1920 B):15 × 32 u32,LSB 优先
    { struct: "VizCoverage", name: "bits", json: "coverage" },
    // ---- VizLanes(30720 B):15 × 1024 int16 定点
    { struct: "VizLanes", name: "pan", json: "lanes" },
]);

/**
 * golden 里的标量常量行(`<key> <value>`)→ 本文件的镜像值。parity 逐条比对。
 */
export const VIZ_GOLDEN_CONSTS = Object.freeze({
    viz_columns: VIZ_COLUMNS,
    viz_coverage_words: VIZ_COVERAGE_WORDS,
    viz_pan_scale: VIZ_PAN_SCALE,
    viz_pan_none: VIZ_PAN_NONE,
    budget_viz_bytes: VIZ_SEGMENT_BYTES,
});

/**
 * samples → 秒的换算式(T45 的 C++ 桥执行;此处逐条写明供对表)。
 *
 * `sample_rate === 0`(未 prepare)时**一律给 null / 0**,不做除法 —— 除零会把整帧变成
 * NaN,而 NaN 在画布上是「什么都不画」,与「没有数据」长得一模一样,查起来极痛苦。
 */
export const SAMPLES_TO_SECONDS_NOTE = Object.freeze({
    windowStartS: "window_start_samples / sample_rate",
    windowSpanS: "window_span_samples / sample_rate(0 ⇒ 无有效窗口)",
    playheadS:
        "playhead_samples / sample_rate;playhead_samples < 0 ⇒ null(无时间线)",
    loopStartS:
        "loop_start_samples / sample_rate;未置 kVizLoopValid 或 < 0 ⇒ null",
    loopEndS: "loop_end_samples / sample_rate;同上",
});

/**
 * **待 T44 补的字段(分布图的数据源)** —— 见 PR #90 描述与发给 T44 的对表信。
 *
 * J75 C 要求 Monitor 的上半张图是「声像 / 音量分布图(实时,**同 Tab1 规格**含配色)」。
 * Tab1 的那三个值来自 `scvb.params`,而 **Monitor 没有 params 通路**;T44 v1 段里
 * 也没有它们(有 `track_stereo_mask` 与 `VizTrackColors`,没有 vol / width / 轨名):
 *
 *   • `volDb`    每轨当前音量(−24..+12 dB)—— 柱高;
 *   • `widthPct` 每轨当前 width(0..100)   —— 立体声张开横线;
 *   • `label`    每轨轨名                  —— 图例(否则 15 条线只剩两位轨号)。
 *
 * **建议的落段方式(不动任何既有偏移)**:段尾还有 32640 B 预留,新增一个
 * `VizTrackScalars` 区块(15 × int16 vol 定点 + 15 × int16 width 定点 = 60 B)放在
 * `kVizLanesOffset + 30720 = 32896`,轨名另开 15 × 32 B UTF-8 定长槽 = 480 B。
 * 两者都落在预留区,既有五个区块的 offset 一字不动 —— 正是 T44 自己写的
 * 「尾部留白,后续增补不改既有偏移」的用法,也不触发 abi+1。
 *
 * **在补齐之前本页的行为**(已由 smoke 锁死,不是「碰运气不崩」):
 * `tracks` 缺失 ⇒ 分布图**画空**、图例只列轨迹图画了的轨、轨迹图与其余一切照常。
 * 不猜、不填 0(填 0 会画出一排居中的「幽灵柱」,那是**假数据**)。
 */
export const VIZ_PENDING_TRACK_FIELDS = Object.freeze([
    "volDb",
    "widthPct",
    "label",
]);
