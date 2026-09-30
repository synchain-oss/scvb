# 契约变更说明 —— 20260926-j106-j107-sl505-contract-supplement

> **状态:已批并随 #287 合入**(挂 `status/frozen-contract`)。 变更实质用户均已裁定(2026-09-26):
>
> - **[J106]**(「契约需要补就补」):§1.9 补「切版本与读入工程会取消在途分析」;§1.18 补
>   「撤销 / 重做 / 切版本 / 读入工程会丢弃尚未触发的重新分段」。
> - **[J110]**(「8取消吧」,对 [SL-532]):撤销 / 重做真的动了栈时**也取消**在途分析,写进 §1.9 同一处。
> - **[J107]**(「5 6 7按你说的做」,对 [SL-509]):§1.2 `noTimeline` 期间**允许关采集、拒绝开采集**。
> - **[SL-505]** 契约半边:`docs/IPC_CONTRACT.md` 里「几何写定后运行期不变」那句与已实现行为
>   (SL-482 / SL-486)矛盾,按实现订正。代码注释半边已由 #286 改完。
>
> 本 PR **只改文档**,不改任何 C++ / JS 代码。其中两项的实现在本 PR 合入时**尚未落地**,契约文字里
> 曾就地标明时态;两项随后由 #289 实现,时态注已由 #289 删除(见下「实现时态」)。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— §2 `AudioRingHeader` 代码块里 `channels` 与
      `epoch` 两行的**行尾注释**,外加 §2 列表新增一条「几何纪律」。**段名、字段、偏移、大小、对齐、
      abi 零变化**(`node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0)。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.2 / §1.3 拒绝态行、§1.9 新增「取消在途分析」一行、
      §1.18 / §1.19 / §1.25 语义行、§5.1 `noTimeline` 行的 UI 落点列。函数名 / 参数 / 返回形态 /
      事件名 / 载荷字段 / `reason` 闭集 / §7 manifest **零变化**。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

### [J106] + [J110] §1.9:三种场合取消在途分析

§1.9 表里在「拒绝态」之后新增一行「取消在途分析」,列三种场合(结果整份丢弃、不写入任何版本、
`analysis_run.running` 回到 `false`):

| 场合 | 条件的精确边界 | 实现 |
| --- | --- | --- |
| ① 真切换版本 | `v` 与当前激活版本不同,且没有被 PRINT 拒绝;`v` 相同不取消 | 已实现:#279([SL-490]),`ScvbOutputAudioProcessor::setVersionActive` 在 PRINT 判据之后、`changing && analysisRunning_` 时调 `cancelAnalysis()` |
| ② 宿主读入工程或预设 | `setStateInformation` **接受**了这份 state;高版本 abi 拒载与损坏数据拒载什么都不改、不取消 | 已实现:#279 复审([SL-491]),`setStateInformation` 在两道拒载 `return` 之后 bump `analysisGeneration_` 并清运行态;**不 signal 作业线程**,它跑完后的结果被代号门丢掉 |
| ③ 撤销 / 重做真的动了栈 | `undo()` / `redo()` 返回 `{ok:true}`;栈为空不取消 | 本 PR 合入时未实现([J110] 裁定);**已由 #289([SL-532])实现**:`undo()` / `redo()` 返回 true 时调 `abandonAnalysisInFlight()` |

§1.25 语义行加一句指路,把 ③ 与 §1.18 的丢弃列为 `{ok:true}` 时的两条副作用。

### [J106] §1.18 / §1.19:丢弃尚未到点的松手档重分段防抖

§1.18 语义行末尾追加一段「丢弃(不是抑制)」:防抖已排、尚未到点时,**撤销 / 重做真的动了栈**、
**真切换版本**、**宿主读入工程或预设**任一发生,这一次重分段直接作废;阈值本身保留新值,段表
不按它重算。实现:#284([SL-531])的 `discardPendingResegment()`,调用点 `undo()` / `redo()`
(返回 true 时)、`setVersionActive`(`versionActive_ != version` 时)、`setStateInformation`
(两道拒载之后)。

**「只有」的张力怎么处理**:§1.18 原句「抑制条件**只有** PRINT 态或分析进行中(J47)」保留原样。
新段落明确把两件事分开 —— **抑制看状态**(`armResegment` 排程时与 `tickResegmentDebounce` 到点时
各判一次 PRINT / 分析中,成立即不跑),**丢弃看事件**(上述几件事发生的那一刻撤掉已排的那一次),
丢弃不是新增的抑制条件,抑制条件的集合没有变,因此「只有」仍然成立。

§1.19 原文「两段式与抑制条件同 §1.18」改为「两段式、抑制条件与丢弃条件同 §1.18(防抖计时与
§1.18 共用一个)」—— 两者共用 `resegmentDueAtMs_` / `resegmentReason_` 一份排程,丢弃对两类
滑杆同时生效;不改这一句的话,读 §1.19 的人会以为分段滑杆不受丢弃约束。

### [J107] §1.2 / §1.3 / §5.1:`noTimeline` 期间允许关采集

| 位置 | 改前 | 改后 |
| --- | --- | --- |
| §1.2 拒绝态 | `noTimeline` 场景下 UI 侧 disabled;C++ 侧收到调用时返回 `{ok:false, reason:"noTimeline"}` 并不改 state;……`on` 取 `true`/`false` 一律拒;判序 `observer` → `noTimeline` → `badArg` | **允许关、拒绝开**:`on=true` 拒,`on=false` 照常受理;UI 采集开关只挡「打开」,开着时仍可关;判序 `observer` → `noTimeline`(仅 `on=true` 时)→ `badArg`;另加实现时态注 |
| §1.3 拒绝态 | 同 1.2 | 同 1.2,**唯一差别**:输出开关 `on` 取 `true`/`false` 仍**一律拒**([J107] 只裁了采集开关) |
| §5.1 `noTimeline` 行 UI 落点 | 采集/输出开关 disabled | 输出开关 disabled;采集开关只挡「打开」,开着时仍可关 |

§1.3 必须一起改:它原文是「同 1.2」,只改 §1.2 会让输出开关跟着被读成「允许关」,而用户只裁了
采集开关。

**判序里的「仅 `on=true` 时」**:`noTimeline` 场景下给一个不是严格布尔的 `on`,改前回
`noTimeline`,按新判序落到 `badArg`。这是把「只拒打开」写成可判定判据时的自然结果;实现 PR
须与此一致(或回帖要求改这一句)。

### [SL-505] IPC_CONTRACT §2 几何纪律

| 位置 | 改前 | 改后 |
| --- | --- | --- |
| `channels` 行尾注释 | `1=mono 2=stereo(prepareToPlay 写定,运行期不变)` | `1=mono 2=stereo(claim 方在 prepareToPlay 写定;布局变化时原地改写,见下「几何纪律」)` |
| `epoch` 行尾注释 | 时间线跳变(定位/循环回跳)时 +1 | 时间线跳变(定位/循环回跳)**或几何改写**时 +1 |
| §2 列表 | —— | 新增「几何纪律」一条 |

新增条目的每句断言与实现落点:

- 「claim 方在 prepareToPlay 写定」—— `InputSession::createSegments` 的 `initData`。
- 「同一 channel 重新 prepare 时采样率或声道布局变了 ⇒ 原地改写」—— `InputSession::prepare` 同组同
  channel 分支,`sampleRate != lastSampleRate_ || channels != lastChannels_` 时调
  `rebuildAudioGeometry`(写 `sample_rate` / `channels`、`write_head_samples` 归零、`epoch`+1、重发布快照)。
- 「attach 到几何不符的存活旧段 ⇒ 原地改写」—— `createSegments` 里按值比对
  `sample_rate` / `channels` / `ring_frames`,不一致即写新几何、归零、`epoch`+1([SL-482])。
- 「段恒按 stereo 容量创建,改写不重开段、不扩容」—— `audioSegmentSize()` =
  `sizeof(AudioRingHeader) + kDefaultRingFrames * 2 * sizeof(float)`;`src/core/ipc/AudioRing.h` 头注同文。
- 「音频线程只用 attach 时发布的不可变快照,不每块回读段头几何」—— `src/core/ipc/Registry.h`
  几何纪律第 1 条;`ShmRingMixSource` 按绑定指针变化重置。
- 「读方非实时线程周期按值比对,不一致即重绑」—— `OutputSession::refreshAudioGeometry`([SL-486]),
  由 `attachAudioRings()` 对已绑定的 channel 调用,后者挂在 Output 的 [M] 25Hz 路径上。

`ring[ring_frames*channels]` 那一行没改:它描述的是**在用**区域的地址算式,与「段按 stereo 容量建」
不矛盾,新条目已写明 mono 只用前半。

**不处理**:`docs/constitution/ipc-contract-v0.md`(修宪源的只读副本)第 46 行与第 261 行仍是旧口径。
本文件头部的仲裁规则写明两者分歧时以已冻结实现与 golden 为准;副本改动须走修宪流程,不在本 PR 范围。

## 实现时态

| 条目 | 状态 | 契约里怎么标 |
| --- | --- | --- |
| §1.9 ① ② | 已在 feature/v1(#279) | 不标 |
| §1.9 ③(J110) | 本 PR 合入时未实现;**已由 #289(SL-532)实现** | 本 PR 曾就地注「实现随 [SL-532] 另行落地;落地之前撤销 / 重做**不**取消在途分析」,#289 已删 |
| §1.18 丢弃 | #284 实现,**#284 已合入**(先于本 PR) | 注「实现见 #284」 |
| §1.2 允许关(J107) | 本 PR 合入时未实现;**已由 #289(SL-509)实现** | 本 PR 曾就地注「落地之前 C++ 侧、mock 与 UI 仍按改前口径一律拒」,#289 已删 |
| IPC 几何纪律 | 已在 feature/v1(#277 的 SL-482 / SL-486) | 不标 |

「落地之前……」的写法是条件句:实现合入后它不会变假,但应由实现 PR 顺手删掉这两处时态注 —— #289 已删(连同 §1.25 语义行与 §5.1 `noTimeline` 行里指向时态注的两处指路)。
实现 PR 还要同改的代码侧旧口径(本 PR 不碰代码):`src/output/OutputEditor.cpp` `handleSetCaptureEnabled`
头注「开和关都拒」、`src/output/BridgeArgs.h` 的「采集/输出开关 disabled」、
`web-preview/tests/smoke-output-stale-page.mjs` 的同文注释、`web-preview/mock/juce-bridge-mock.js`
`setCaptureEnabled` 的 `noTimeline` 分支、`web/output/tab-master.js` 的 `isSwitchBlocked()`。这五处 #289 已改。

## 兼容性影响

**`contractVersion` 保持 `1.0`**。依据 §0.1 第 3 条与 §9.0 第 3 条:主版本 +1 只由禁止面触发 ——
改名、改参数顺序、改既有字段语义、删除函数 / 事件 / 字段、收窄取值域。逐项对照:

- **J106 / J110**:给 `setVersionActive` / `undo` / `redo` 与宿主载入补写副作用;不改名、不删、不动
  任何参数 / 返回形态 / 载荷字段。
- **J107**:`noTimeline` 下 `setCaptureEnabled(false)` 由拒改为受理 —— 受理面**放宽**,不是收窄;
  返回形态集合不变(`noTimeline` 这一支仍在,只是不再对 `on=false` 出现),§7 manifest 的 `returns`
  串不用改。
- **SL-505**:IPC 行尾注释与说明条目,不改段名 / 布局 / abi;桥面零变化。

先例:`20260922-sl478-notimeline.md`(给拒绝态补判据、补返回支,保持 1.0);[J90] 变更文档「1.0 =
这一版冻结表面」的约定。§9.0 第 4 条:§7 manifest 无需改动(名字、参数、返回串、枚举均未变),
`node scripts/check-bridge-parity.mjs` 退 0。

**用户可见**:本 PR 自身零行为变化。J107 / J110 的用户可见变化随各自实现 PR 进 CHANGELOG。

## 审批

挂 `status/frozen-contract`。实质:J106 / J107 / J110(用户 2026-09-26 裁定);SL-505 为按已实现
行为订正文字,零布局变化。

## 关联

- 卡:SL-505、SL-509、SL-531、SL-532,`masterPlan/review/suggestion-ledger.md`。
- 实现:#277(SL-482 / SL-486)、#279(SL-490 / SL-491)、#284(SL-531,已合)、#289(SL-532 / SL-509,删时态注)、#286(SL-505 代码注释半边)。
