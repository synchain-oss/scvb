# 契约变更说明 —— 20260914-sl411-segmentation-persist

> **状态:待批(随实现 PR 挂 `status/frozen-contract`)。** 定谳:统筹 2026-09-14(**SL-411**);
> 用户裁 ①「落盘,进 v5.6.14」。本文档是该变更的契约面记录,与实现放在**同一个 PR**里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:`analysis.segmentation.*` 三项都不是自动化参数
      (state-only),123/124 个参数的 ParamID / index / 顺序 / versionHint 一个字节都没碰。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动:与跨进程共享内存无关。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 `analysis.segmentation` 三项由「挂账未落盘」改为
      **随工程落盘**(§0.1 规则 3 的加法,`contractVersion` 不升主版本);§三 `CFGS` 尾扩一整档、
      容器 **abi 3→4** + `migrate_3_to_4`(no-op,缺席档回落规格默认)。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**:§1.19 `setSegmentation` 的入参与
      §2.1 `scvb.state` 的 `analysis.segmentation` 载荷行**一个字都不改** —— 三项本来就在载荷里、
      本来就在规格里(02 §0.3 与 STATE_SCHEMA §一 一直列着),此前缺的只是「写盘/读盘」那一跳。
      载荷字段**只增不改**这条铁律在本 PR 上一次都没有被用到。
- [x] tests/golden/(golden 快照)—— **新增** `tests/golden/state/abi4.bin`;
      `abi1.bin` / `abi2.bin` / `abi3.bin` **保留不动**作迁移基线(四份并存,不是替换)。
      ⚠ **金样锁的是容器头**(magic / abi / flags / chunkCount / TLV 框):夹具的 CFGS 载荷是
      `opaque("CONFIG")` 字面量,不是 `encodeOutputState` 的产物 —— 所以 `abi4.bin` 与 `abi3.bin`
      只差 abi 那一个字节是**必然**而非巧合,`segmentation` 那 12 个字节它一次都没见过。
      CFGS 载荷的 wire 布局由 `tests/core/test_output_session.cpp` 的长度断言(`54u` / `24u+5u+28u`)
      与 [SL-411] 那四格(往返 / 旧档缺席 / 越界回落计数 / 半截拒载)承担。改 `segmentation`
      的编码顺序**不会**让金样红,别去金样那里找原因。

## 变更内容

**一句话**:`analysis.segmentation.{mode, sensitivity, min_segment_ms}` 三项**随工程保存** ——
`CFGS` 尾部再扩**一整档 12 字节**(`u32 segMode` + `f32 segSensitivity` + `u32 segMinSegmentMs`),
state 容器 abi 3→4,新增 no-op `migrate_3_to_4`。

| 面 | 改前 | 改后 |
| --- | --- | --- |
| CFGS 尾部 | 28 字节头 + uiLanguage + `loudness_mode`/`center_slot_policy`(2×u32)+ `applied.*`(2×u32)= 尾长 16 | 再追加 `segMode`(u32)+ `segSensitivity`(f32)+ `segMinSegmentMs`(u32)= **尾长 28** |
| 接受的尾长 | 0 / 8 / 16 / 16+ | 0 / 8 / 16 / **28 / 28+**;落在 (0,8)、(8,16)、(**16,28**)一律整块拒载 |
| `kCurrentAbi` | 3 | **4** |
| 迁移链 | `[migrate_1_to_2, migrate_2_to_3]` | `[… , migrate_3_to_4]`(三个都是 no-op,靠 codec 的长度回退) |
| 旧工程(abi≤3)打开 | — | 三项**整档缺席** ⇒ 回落规格默认 valley / 50 / 120 且**不计回落** |
| 越界值(手改过的工程 / 未来版本) | — | 该字段回落**规格默认**并**计一次回落**;另外两个字段不受影响 |
| `OutputProcessor` | 三项只活在 `runtime_` | `getStateInformation` 写进 CFGS;`setStateInformation` 恢复回 `runtime_`(空档/预设路径不碰) |
| 用户可见 | 拖到 1000ms → 存盘 → 重开 ⇒ 滑杆跳回 120ms | 重开 ⇒ 滑杆与读数都还是 1000ms |

**为什么要落盘**:规格 02 §0.3 与 STATE_SCHEMA §一 一直把这三项列在 state 里,而实现侧只有
`OutputProcessor.h` 的 `runtime_` 这一份内存真身 —— 保存时 CFGS 不写、加载时不读。用户 v5.6.13
实测:MIN SEG 拖到 1000ms → 分析 → 存盘 → 重开,滑杆跳回 120ms;而**分析确实按 120 跑**。
于是「我设的值没生效」与「我的设置没被记住」在界面上长得一模一样,两次排查都指向错的那一半。
用户裁定:落盘,进 v5.6.14(**SL-411**)。

**尾字段的失败态(本 codec 里唯一一处「值越界 → 回落默认」)**:头五个字段(含 `langBytes`)越界
是**整块拒载**,`ui.scale` 是**原样透出、由上层夹取**,而这三项越界 → **回落该字段的规格默认并计数**。
理由是它们彼此独立、且老工程里本来就整档缺席:一格坏值不该让整份工程的段表读不出来。
**不做边界夹取** —— 夹取会把「这个值我没兑现」伪装成「已经按它办了」;宽值域的正确出路是升 abi
走迁移链(同一个道理写在 `OutputStateCodec.h` 那段「档内不许半截」里)。三个字段各有一个计数器,
**不合并**(合并之后诊断行会说「segmentation 回落了 1 次」,而实际是哪个字段,读日志的人分不出来);
`mode` 非 0/1 → valley;`sensitivity` 为 NaN/±Inf 或落在 [0,100] 外 → 50;`min_segment_ms` 落在
[50,2000] 外(含 0 与 u32 极值)→ 120。灵敏度那一格的 NaN 分支必须单独守:`x < lo || x > hi`
对 NaN **恒假**,只写范围比较的实现会静默放行,而 NaN 一旦进了 `PipelineConfig` 的灵敏度,
下游所有比较都是假 —— 那种坏法不报错,只是结果不对。

**`f32` 走位模式落盘**(`memcpy` 出 IEEE-754 位模式再按 u32 的小端规则写),不做类型双关
(`reinterpret_cast<float*>` 是严格别名 UB,且会引入对齐假设);读侧反过来。于是本 codec 的
字节序纪律仍然只有一条(全 u32),`f32` 不另开一套。`static_assert(sizeof(float) == 4)` 钉住前提。

**与 abi 2→3 那次取舍不同(有意,别照抄)**:`applied.*` 缺席时取**当前值**,因为它的语义是
「上次分析所用的那一档」—— 取默认会让存了非默认档的旧工程一打开就误报「需重新分析」;
而 `segmentation.*` 的语义就是「当前设置」本身,旧工程确实没存过,取**规格默认**(也正是旧构建
`runtime_` 的初值)才是真话,于是「旧工程打开后的行为与它保存时逐字相同」。

## 兼容性影响

- **加法,不升 `contractVersion` 主版本**:按 STATE_SCHEMA §0.1 规则 3,payload 新增可选字段属加法。
- **新版本读旧工程(abi ≤ 3)**:容器走 no-op 迁移链升到 abi=4(状态 `Migrated`),
  CFGS 按**长度回退**补齐 —— 三项缺席 ⇒ 规格默认 valley / 50 / 120,**不计回落**。
  abi=1/2 的工程照旧:先经它们各自那一级的回退,再落到这一级。
- **旧版本读新工程(abi=4)**:`hdr.abi > kCurrentAbi` 的既有分支照旧 —— **整块** `RejectedNewer`
  + `preservedOriginal` 原样回写(CLAUDE.md §7.3),**绝不静默降级**。
  ⚠ **[SL-411 R1] 升级提示的 UI 通路尚未接线**:`hasStateAbiMismatch()` / `stateAbiSeen()` 目前
  **零调用方**,`scvb.error` 的 `newerState` 码**没有生产者**(消费端 `web/output/app.js` 早就就绪),
  旧构建读 abi=4 只落一行 `DBG` —— 而 JUCE 的 `DBG` 在 Release 里是空语句,**用户看不到任何解释**
  (他看到的是一份「段表/曲线/组号全默认」的空工程)。该缺口**不是本 PR 引入的**(abi 1→2、2→3 时
  同样存在),本 PR 也没接线,只把它照实记账:登记 **SL-412**(接 `scvb.error newerState`,Output/Input
  两侧)。也就是说 v5.6.13 及更早的构建读一份 v5.6.14 的工程,不会「把 1000 悄悄夹成 500」——
  它根本不载入这份 state(段表/曲线同样按拒载处理,与本 PR 之前每一次升 abi 的行为逐字相同),
  但**它也不会告诉用户为什么**。
  这条路径**只在 v1 未发布期间成立**,也正因如此它不构成发布后的用户风险:一个 >500ms 的值
  只会出现在本构建(或更新)写出的工程里。
- **Input 侧连带**:Input 与 Output **共用容器 abi**,本 PR 之后新 Input 保存 state 一并写 abi=4;
  旧 Input 读新 Input state 整块 `RejectedNewer`(原样回写;**同上,升级提示同样待接线**,SL-412);
  新 Input 读旧工程经三个 no-op 迁移不受影响(Input CFGS 未变)。
- **Monitor 侧连带**:Monitor 也写 state 容器,共用同一份 `kCurrentAbi`
  (`MonitorProcessor.cpp` 的 `chunks.abi = scvb::state::kCurrentAbi`),所以本 PR 之后它保存的 state
  一并写 abi=4。它读高版本 blob 时走 `decideInputStateAbi(...) == RejectNewer` 那一支:**拒载、不回写**
  (源码里那句注释写得很清楚:「Monitor 是只读观察器……无需 `preservedOriginal` 回写 —— 丢的只是一个
  视图偏好,不是用户数据」,落点只有一行 `DBG`)。所以连带面比 Input 还小:旧 Monitor 读新工程丢的是
  「看哪一组 / 缩放 / 语言」,碰不到用户的段表与曲线。
- **参数面**:零影响 —— 三项都不是自动化参数,ParamID / index / 顺序 / versionHint / 参数值域
  (`OutputEditor` 的 `jlimit`)全部不动。
- **行为面**:唯一的行为变化就是本卡要的那一条 —— 重开工程后 `analysis.segmentation` 三项
  保持上次保存的值。三项的**语义**、默认值、值域、消费方(`cfg.vad.minSegmentMs` /
  `cfg.segmentation.*`)一个字都没改。

## 判据与机检(每条都实跑,删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| core 搬运 | `tests/core/test_output_session.cpp` `[SL-411]` 四格 | 三项往返(含 f32 位模式逐字节)/ abi=3 旧档 ⇒ 三默认且不计回落 / 越界逐字段回落 + 独立计数(含 NaN)/ 半截(16<remaining<28)拒载 |
| core 容器 | `tests/core/test_state_codec.cpp` `STATE-GOLDEN StateAbiCompat` | abi1/abi2/abi3 三份旧金样仍 `Migrated` 到当前 abi;**abi4.bin 格式锁**(重编码逐字节相等) |
| 生产两跳 | `tests/host/test_host_harness.cpp` `HOST SL411` | 存(`getStateInformation` 写 runtime_ 三项)→ 新实例载(`setStateInformation` 恢复)→ 三项逐项一致,**且重开后的分析真的按持久值跑**(段数与 120 档不同、与存盘那一档相同) |
| 页面级 | `web-preview/tests/smoke-seg-restore-page.mjs` 的 `[SL-411]` 一节 | 工程 state 里 `min_segment_ms=1000` ⇒ MIN SEG 滑杆的 `aria-valuenow` 与读数文本**真的变成 1000**(用户看得见的那一半) |
| 旧档语料 | `tests/golden/state/abi1.bin` / `abi2.bin` / `abi3.bin` | 三份**保留不动**;迁移用例跑在**真的旧文件**上,不是现造一个「假装是旧版」的字节串 |

⚠ **离线不可达、本卡没有改变的那一跳**(照实登记,别读成已覆盖):`OutputEditor.cpp` 的
`p.getProperty("min_segment_ms")` → `rt.segmentationMinSegmentMs`(桥面 → runtime)与
`scvb.state` 回声里那三行的**装配**都在 `OutputEditor`,它依赖 WebView2、不在 host 套件的 TU 清单里。
前者由 `smoke-tab3-interactions.mjs` 的源码级对拍 + 它自己的删除式兜住,后者由上面那一格页面级
冒烟兜住(走 mock 侧);「桥面 → runtime」这一跳仍是**缺口不是覆盖**(与 `HOST SL391` 头注同一笔账)。

⚠ **[SL-411 R1] 同族句子还有两处不在本 PR 的改动面内**,别以为全仓都收干净了:
`docs/PARAMETERS.md` 的规则摘要行(冻结文档,本 PR 的契约面声明是「PARAMETERS 不动」)与
`docs/CONTRIBUTOR_ONBOARDING.md` 里那一句 —— 后者说的是 **IPC abi**(`Registry::kAbiMismatch`),
那条通路**是接了的**,不是同一件事,留着不改。前者的措辞随 **SL-412** 接线时一并收。

## 变更文件

- `src/core/state/OutputStateCodec.{h,cpp}`(尾扩一整档 12 字节、长度回退第三级、f32 位模式编解码、
  三个独立回落计数器、头注的布局与「**档内不许半截** / 偏移 28 之后任意长度尾巴由 `unknownTail` 收下」
  这条纪律——R8 收敛了措辞,原来的「追加必须整档」比实际严格)
- `src/core/state/StateCodec.h`(`kCurrentAbi` 3→4;容器头注与真源指针同步)
- `src/core/state/StateMigration.{h,cpp}`(`migrate_3_to_4` no-op;`kMigrators` 三项)
- `src/output/OutputProcessor.cpp`(保存侧写三项 / 加载侧恢复三项 + 回落计数的 DBG 行)
- `docs/STATE_SCHEMA.md`(abi 3→4、§一 三项改为已落盘、§三 CFGS 行与尾长分级、迁移链三条)
- `docs/USER_GUIDE.zh-CN.md` + `docs/USER_GUIDE.md`(分段那一段的「不随工程保存」反向收掉)
- `CHANGELOG.md`(契约变更小节 + SL-398 那条的值域句)
- `docs/contract-changes/20260911-sl398-min-segment-2000.md`(兼容性两段:该卡写下的
  「无降级路径」已被本卡取代,照实标注)
- `tests/golden/state/abi4.bin`(**新增**;abi1/abi2/abi3 不动)
- `tests/core/test_output_session.cpp`、`tests/core/test_state_codec.cpp`、
  `tests/host/test_host_harness.cpp`、`web-preview/tests/smoke-seg-restore-page.mjs`

## 审批

待批:随实现 PR 挂 `status/frozen-contract`,由用户明确批准后合入(06 §3.7)。定谳来源:统筹
2026-09-14 **SL-411**(用户裁 ①「落盘,进 v5.6.14」);金样新增已获用户批准。
