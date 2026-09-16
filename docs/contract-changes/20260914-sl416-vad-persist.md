# 契约变更说明 —— 20260914-sl416-vad-persist

> **状态:待批(随实现 PR 挂 `status/frozen-contract`)。** 定谳:统筹 2026-09-15(**SL-416**,
> 用户 v5.6.15 回验 **A24**);修法裁定:六项**一起落盘**(省一次 abi)。本文档是该变更的契约面记录,
> 与实现放在**同一个 PR** 里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:`analysis.vad.*` 五项与 `analysis.transition_ramp_ms`
      都不是自动化参数(state-only),123/124 个参数的 ParamID / index / 顺序 / versionHint 一个字节
      都没碰。(该文件的注释面**有**改动:那两行的定义句补了「自本版起随工程落盘」,见「变更文件」。)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动:与跨进程共享内存无关。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 `analysis.vad` 五项与 `analysis.transition_ramp_ms`
      由「挂账未落盘」改为**随工程落盘**(§0.1 规则 3 的加法,`contractVersion` 不升主版本);
      §三 `CFGS` 尾扩一整档 24 字节、容器 **abi 4→5** + `migrate_4_to_5`(no-op,缺席档回落规格默认)。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**:§1.18 `setVadParams` 的入参、§1.20
      `setTransitionRamp` 的范围、§2.1 `scvb.state` 的 `analysis.vad` / `transition_ramp_ms` 载荷行
      **一个字都不改** —— 六项本来就在载荷里、本来就在规格里(02 §0.3 与 STATE_SCHEMA §一 一直列着),
      此前缺的只是「写盘/读盘」那一跳。载荷字段**只增不改**这条铁律在本 PR 上一次都没有被用到。
      (桥面**行为**有一处收窄:两处夹取的实参从字面量换成 codec 常量、并补了 R16 守卫,见下文。)
- [x] tests/golden/(golden 快照)—— **新增** `tests/golden/state/abi5.bin`;
      `abi1.bin` / `abi2.bin` / `abi3.bin` / `abi4.bin` **保留不动**作迁移基线(五份并存,不是替换)。
      ⚠ **金样锁的是容器头**(magic / abi / flags / chunkCount / TLV 框):夹具的 CFGS 载荷是
      `opaque("CONFIG")` 字面量,不是 `encodeOutputState` 的产物 —— 所以 `abi5.bin` 与 `abi4.bin`
      只差 **abi 那一个字节(offset 4:4 → 5)**是**必然**而非巧合,`vad`/`ramp` 那 24 个字节它一次
      都没见过。CFGS 载荷的 wire 布局由 `tests/core/test_output_session.cpp` 的长度断言
      (`78u` / `24u+5u+52u`)与 [SL-416] 那四格(往返 / 旧档缺席 / 越界回落计数 / 半截拒载)承担。
      改 `vad`/`ramp` 的编码顺序**不会**让金样红,别去金样那里找原因。

## 变更内容

**一句话**:`analysis.vad.{threshold_db, hysteresis_db, hangover_ms, padding_pre_ms, padding_post_ms}`
与 `analysis.transition_ramp_ms` **六项随工程保存** —— `CFGS` 尾部再扩**一整档 24 字节**
(`f32 vadThresholdDb` + `f32 vadHysteresisDb` + `u32 vadHangoverMs` + `u32 vadPaddingPreMs` +
`u32 vadPaddingPostMs` + `u32 transitionRampMs`),state 容器 abi 4→5,新增 no-op `migrate_4_to_5`。

**六项的类型与值域**(真源 = masterPlan `plan/02-dsp-spec.md` §0.3 常量表,经 U24 收敛;
该表 `vad.threshold_db` 与 `transition_ramp_ms` 两行已由统筹补 [SL-416] 落盘裁定注,commit `5b69824`):

| 字段 | wire 类型 | 值域 | 规格默认 | 落点(UI) |
| --- | --- | --- | --- | --- |
| `vad.threshold_db` | **f32**(位模式) | **−60..−10**(绝对门限 dB) | **−38** | Wave 页 THRESHOLD 滑杆 |
| `vad.hysteresis_db` | **f32**(位模式) | **3..12** | **6** | HYSTERESIS 滑杆 |
| `vad.hangover_ms` | u32 | **100..600** | **250** | HOLD 滑杆 |
| `vad.padding_pre_ms` | u32 | **20..400** | **120** | PAD PRE 滑杆 |
| `vad.padding_post_ms` | u32 | **50..400** | **200** | PAD POST 滑杆 |
| `analysis.transition_ramp_ms` | u32 | **20..300**(契约 §1.20) | **80** | Tab1 过渡时间滑杆(`tab-master.js` 的 `RAMP_MS`) |

⚠ `threshold_db` 的口径:§0.3 表那一行给的是 **`onDepth` 刻度**(默认 30 / 范围 15..45),而 state/UI 面
存的是**绝对门限 dB** —— 同一档的两个刻度,换算锚 = `OutputProcessor.cpp` 的 `kVadUiRefDb = −8`
(`depth = kVadUiRefDb − ui`,`ui = −38 ⇔ depth = 30`)。02 §… 「UI/state 侧 `vad.threshold_db`:
绝对门限 dB(滑杆 −60..−10,默认 −38)」逐字同口径。

| 面 | 改前 | 改后 |
| --- | --- | --- |
| CFGS 尾部 | 24 字节头 + uiLanguage + `loudness_mode`/`center_slot_policy`(2×u32)+ `applied.*`(2×u32)+ `segmentation`(u32+f32+u32)= 尾长 28 | 再追加 `vad` 五项 + `transition_ramp_ms`(f32+f32+u32×4)= **尾长 52** |
| 接受的尾长 | 0 / 8 / 16 / 28 / 28+ | 0 / 8 / 16 / 28 / **52 / 52+**;落在 (0,8)、(8,16)、(16,28)、(**28,52**)一律整块拒载 |
| `kCurrentAbi` | 4 | **5** |
| 迁移链 | `[migrate_1_to_2, migrate_2_to_3, migrate_3_to_4]` | `[… , migrate_4_to_5]`(四个都是 no-op,靠 codec 的长度回退) |
| 旧工程(abi≤4)打开 | — | 六项**整档缺席** ⇒ 回落规格默认 −38/6/250/120/200/80 且**不计回落** |
| 越界值(手改过的工程 / 未来版本) | — | 该字段回落**规格默认**并**计一次回落**;另外五个字段不受影响 |
| `OutputProcessor` | 六项只活在 `runtime_`(初值 −45/3/200/120/200/80) | 初值**引用 codec 的规格默认**;`getStateInformation` 写进 CFGS;`setStateInformation` 恢复回 `runtime_` |
| 桥面 `handleSetVadParams` | 五个字段直接 `static_cast<float/int>(var)`,**无值域夹取、无非有限守卫** | 逐个 `specClamp`:非数值/非有限 ⇒ 保留 rt 原值;有限 ⇒ **double 域**夹到规格值域再窄化(R16) |
| 桥面 `handleSetTransitionRamp` | `static_cast<float>(a[0])`,缺省字面量 `80.0f`;**无夹取、无非有限守卫** | 同上(夹到 §1.20 的 20..300,缺省取 rt 原值) |
| 用户可见 | 拖 VAD 五杆 → 存盘 → 重开 ⇒ 五杆全回默认(A24) | 重开 ⇒ 五杆与 Tab1 的过渡时间都还是存盘时那一份 |

**为什么要落盘**:规格 02 §0.3 与 STATE_SCHEMA §一 一直把这六项列在 state 里,而实现侧只有
`OutputProcessor.h` 的 `runtime_` 这一份内存真身 —— 保存时 CFGS 不写、加载时不读。用户 v5.6.15
回验 A24 实测:「存盘重开后 MIN SEG 回来了([SL-411] 修的),但 THRESHOLD −45 dB / HYSTERESIS 3 dB /
HOLD 200 ms / PAD PRE 120 ms / PAD POST 全部回默认」。与 [SL-411] 同一条病:设置没被记住,而
**分析确实按默认跑**,于是「我设的值没生效」与「我的设置没被记住」在界面上长得一模一样。
统筹裁定:**六项一起落,省一次 abi**。

**尾字段的失败态(与 [SL-411] 同一族)**:头五个字段(含 `langBytes`)越界是**整块拒载**,
`ui.scale` 是**原样透出、由上层夹取**,而这六个字段越界 → **回落该字段的规格默认并计数**。
**不做边界夹取** —— 夹取会把「这个值我没兑现」伪装成「已经按它办了」;宽值域的正确出路是升 abi
走迁移链。六个字段各有独立计数器,**不合并**(合并之后诊断行会说「vad 回落了 1 次」,而实际是哪个
字段,读日志的人分不出来)。两个 f32 字段的**非有限**分支必须单独守(NaN 与 ±Inf 走
`!(x >= lo && x <= hi)` 同一支):`x < lo || x > hi` 对 NaN **恒假**,只写范围比较的实现会静默放行,
而 NaN 一旦进了 `runtime_` → `cfg.vad.thresholdDb`,下游所有比较都是假 —— 那种坏法不报错,只是结果不对。

**`f32` 走位模式落盘**(沿用 [SL-411] 的手法):`memcpy` 出 IEEE-754 位模式再按 u32 的小端规则写,
不做类型双关(`reinterpret_cast<float*>` 是严格别名 UB,且引入对齐假设);读侧反过来。

**与 abi 1→2 / 2→3 那两次取舍不同(有意,别照抄)**:那两级的缺席分别是「回落默认」与
「`applied := 当前值`」;而这一级与 [SL-411] 同类 —— 六项的语义就是「当前设置」本身,旧工程确实没存过,
取**规格默认**才是真话。

## 兼容性影响

- **加法,不升 `contractVersion` 主版本**:按 STATE_SCHEMA §0.1 规则 3,payload 新增可选字段属加法。
- **新版本读旧工程(abi ≤ 4)**:容器走 no-op 迁移链升到 abi=5(状态 `Migrated`),CFGS 按**长度回退**
  补齐 —— 六项缺席 ⇒ 规格默认,**不计回落**。abi=1/2/3 的工程照旧:先经它们各自那一级的回退,再落到这一级。
- **⚠ 行为面(本卡有意的一处连带)**:六项的**引擎初值**此前是 T29 遗留的 `−45 / 3 / 200`(与 02 §0.3
  的出厂档、与 web 滑杆的 `def` 都不一致);本卡把**引擎初值 / 滑杆 def / decode 缺席与越界回落**三处
  收到**同一个真源**(codec 的 `kOutputVad*Default` = §0.3 的 **−38 / 6 / 250**)。**分家的不是引擎侧**:
  `src/core/analysis/EnergyVad.h:19-20`(分析引擎自己那一份默认)**本来就是 6 / 250**,与 02 §0.3 一致;
  本卡收的是**桥面 / `runtime_` 那一份**(`OutputProcessor.h` 的 `−45 / 3 / 200`)。因此:
  - 旧工程(abi≤4)打开后这六项是 §0.3 的出厂档,而**不再是**旧构建 `runtime_` 的那一组;
  - web 滑杆的**行程**按 §0.3 收正:hysteresis `0..20` → `3..12`、hangover `0..500` → `100..600`、
    pad_pre `0..500` → `20..400`、pad_post `0..500` → `50..400`(threshold 与 ramp 一字未动);
  - 依据:§0.3 表是 U24 收敛的**出厂推荐值**(「宁多勿少」),而 [SL-411] 的先例也是「回落规格默认
    (也正是旧构建 `runtime_` 的初值)」—— 这里两组值不一致,本卡选**规格**那一侧,并把不一致本身收掉。
  - 实跑复核(防「静默改调音」):core `~[ipc],~[.]` 与 host 全量套件**零回归** —— 具体格数/断言数
    **见 PR 描述的「本机读数」一节**(由统筹在沙箱外实跑补入;本文档第 1 稿里的那几个数取自不同次的
    实跑、彼此对不上,已按统筹裁定撤下,只留「零回归」这个结论)。
- **旧版本读新工程(abi=5)**:`hdr.abi > kCurrentAbi` 的既有分支照旧 —— **整块** `RejectedNewer`
  + `preservedOriginal` 原样回写(CLAUDE.md §7.3),**绝不静默降级**。
  ⚠ **[SL-411 R1] 升级提示的 UI 通路仍未接线**(`hasStateAbiMismatch()` / `stateAbiSeen()` 零调用方、
  `scvb.error` 的 `newerState` 码没有生产者),旧构建读 abi=5 只落一行 `DBG` —— 而 JUCE 的 `DBG` 在
  Release 里是空语句,**用户看不到任何解释**。该缺口**不是本 PR 引入的**,本 PR 也没接线,登记 **SL-412**。
- **Input 侧连带**:Input 与 Output **共用容器 abi**,本 PR 之后新 Input 保存 state 一并写 abi=5;
  旧 Input 读新 Input state 整块 `RejectedNewer`(原样回写;升级提示同样待接线,SL-412);
  新 Input 读旧工程经四个 no-op 迁移不受影响(Input CFGS 未变)。
- **Monitor 侧连带**:Monitor 也写 state 容器,共用同一份 `kCurrentAbi`,所以本 PR 之后它保存的 state
  一并写 abi=5。它读高版本 blob 时走 `decideInputStateAbi(...) == RejectNewer` 那一支:**拒载、不回写**
  (丢的只是「看哪一组 / 缩放 / 语言」的视图偏好,碰不到用户的段表与曲线)。
- **参数面**:零影响 —— 六项都不是自动化参数,ParamID / index / 顺序 / versionHint 全部不动;
  值域**数值**按 §0.3 落进 codec,桥面那两处夹取的实参从字面量换成常量。
- **状态面**:`OutputUiState.h` 的长度纪律同步到**四级 52 字节**(PRMS 那条纪律不受影响)。

## 判据与机检(每条都实跑,删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| core 搬运 | `tests/core/test_output_session.cpp` `[SL-416]` 四格 | 六项往返(含两个 f32 位模式逐字节)/ abi=4 旧档 ⇒ 六默认且不计回落 / 越界**逐字段**回落 + 独立计数(含 NaN 与 +Inf)/ 半截(28<remaining<52)拒载 + 边界 28 与 52 可解 |
| core 容器 | `tests/core/test_state_codec.cpp` `STATE-GOLDEN StateAbiCompat` | abi1/abi2/abi3/abi4 四份旧金样仍 `Migrated` 到当前 abi;**abi5.bin 格式锁**(重编码逐字节相等) |
| 生产两跳 | `tests/host/test_host_harness.cpp` `HOST SL416` | 存(`getStateInformation` 写 runtime_ 六项)→ 新实例载(`setStateInformation` 恢复)→ 六项逐项一致,**且重开后的分析真的按持久值跑**(判别档 loose 5 段 / tight 1 段 / 存盘档 **5 段 = loose 那一组**;重开后 **5 段**。`nDefault` 那一档量出来是 4 段,`CHECK(nSaved != nDefault)` 正是这条判据的前提 —— 若写成「存盘档 4 段」,它自己就会红) |
| 页面级 | `web-preview/tests/smoke-seg-restore-page.mjs` 的 `[SL-416]` 一节 | 工程 state 里 `vad.threshold_db=-30` ⇒ THRESHOLD 滑杆的 `aria-valuenow` 与读数文本**真的变成 −30 / −30 dB**(用户看得见的那一半) |
| 值域对拍 | `web-preview/tests/smoke-tab3-interactions.mjs` 的 ⑮ (a)/(b)/(c)/(d) 四组 | 五杆的 `min`/`max`/`def` 与 `DEFAULT_VAD_PARAMS`、`tab-master.js` 的 `RAMP_MS`、以及 **mock 的 state 初始快照**(`web/shared/mock-data.js` 的 `analysis.vad` / `analysis.transition_ramp_ms`),与 **codec 定义行**(`kOutputVad*{Min,Max,Default}` / `kOutputTransitionRampMs*`)逐值相等 |
| 桥面引用 | 同上 ⑮ 的 **(f)** / **(g)** | `handleSetVadParams` 的函数体里**出现那十对 codec 常量名**、且**没有裸数字 `jlimit(`**;`handleSetTransitionRamp` 同钉;`handleSetSegmentation` 两处**都用 `specClamp`** 且函数体内没有第二份自写夹取 —— 把桥面改回字面量 / 改回自写版时 (a)–(d) 全绿而这一格红 |
| 旧档语料 | `tests/golden/state/abi1..abi4.bin` | 四份**保留不动**;迁移用例跑在**真的旧文件**上,不是现造一个「假装是旧版」的字节串 |

⚠ **离线不可达、本卡没有改变的那一跳**(照实登记,别读成已覆盖):`OutputEditor.cpp` 的
`p.getProperty("threshold_db")` → `rt.vadThresholdDb`(桥面 → runtime)与 `scvb.state` 回声里那六行的
**装配**都在 `OutputEditor`,它依赖 WebView2、不在 host 套件的 TU 清单里。前者由 ⑮ 的 (f) 对拍
(「桥面确实引用那些常量」)+ 它自己的删除式(D2b)兜住,后者由页面级那一格兜住(走 mock 侧);
「桥面 → runtime」这一跳仍是**缺口不是覆盖**(与 `HOST SL391` / `HOST SL411` 头注同一笔账)。

## 变更文件

> 本清单按 `git diff --name-only origin/feature/v1...HEAD` **逐条核过**(第 1 轮复审教训:
> `git ls-files --error-unmatch` 只验「文件在库里存在」、**不验它真的被改了** —— 第 1 稿把
> `web/output/tab-master.js` 列进来,而它零命中,已删)。

- `src/core/state/OutputStateCodec.{h,cpp}`(尾扩一整档 24 字节、长度回退第四级、六个独立回落计数器、
  六项的值域/默认常量、头注的布局与「**档内不许半截** / 偏移 52 之后任意长度尾巴由 `unknownTail` 收下」)
- `src/core/state/StateCodec.h`(`kCurrentAbi` 4→5;容器头注与真源指针同步)
- `src/core/state/StateMigration.{h,cpp}`(`migrate_4_to_5` no-op;`kMigrators` 四项)
- `src/output/OutputProcessor.{h,cpp}`(保存侧写六项 / 加载侧恢复六项 + 回落计数的 DBG 行;runtime 初值
  与 `setTransitionRamp` 的夹取改引用 codec 常量)
- `src/output/OutputEditor.cpp`(`specClamp` 统一守卫 + `handleSetVadParams` 五个字段逐项夹取 /
  `handleSetTransitionRamp` 改走同一道守卫 / `handleSetSegmentation` **收编**到同一道守卫 —— 非数值实参
  改为「保留 rt 原值」)
- `src/output/OutputUiState.h`(CFGS 长度纪律更新到四级 52 字节)
- `web/output/tab-wave.js`(五杆 `min`/`max`/`def` 与 `DEFAULT_VAD_PARAMS` 按 §0.3 收正;`SLIDERS` 头注的
  真源与行程比改成实测值)
- `web/output/index.html`(五杆的静态 `aria-valuemin/max/now` 与 `--p` 行程比同步)
- `web/shared/mock-data.js`(state 初始快照的 `hangover_ms` 180 → 250,与 codec 的规格默认同值)
- `web-preview/tests/smoke-tab3-interactions.mjs`(新增 ⑮ 组:五杆 + ramp 的值域/默认对拍、(d) mock
  初始快照对拍、(f)/(g) 桥面源码级对拍;七杆行程比按新值域更新)
- `web-preview/tests/smoke-seg-restore-page.mjs`(新增 [SL-416] 页面级一格)
- `docs/STATE_SCHEMA.md`(abi 4→5、§一 六项改为已落盘、§三 CFGS 行与尾长分级、迁移链四条)
- `docs/PARAMETERS.md`(`vad` / `transition_ramp_ms` 两行的定义句补「自本版起随工程落盘」)
- `docs/USER_GUIDE.zh-CN.md` + `docs/USER_GUIDE.md`(「随工程保存」那句从三项扩到 VAD 五项 + 过渡时间)
- `CHANGELOG.md`(契约变更小节)
- `tests/golden/state/abi5.bin`(**新增**;abi1–abi4 不动)
- `tests/core/test_output_session.cpp`、`tests/core/test_state_codec.cpp`、
  `tests/host/test_host_harness.cpp`

## 审批

待批:随实现 PR 挂 `status/frozen-contract`,由用户明确批准后合入(06 §3.7)。定谳来源:统筹
2026-09-15 **SL-416**(用户 v5.6.15 回验 A24);六项一起落盘、省一次 abi = 统筹裁定;
金样新增与「引擎初值对齐 §0.3」两处连带一并报批。
