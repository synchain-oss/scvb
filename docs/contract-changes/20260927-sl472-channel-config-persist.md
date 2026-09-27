# 契约变更说明 —— 20260927-sl472-channel-config-persist

> **状态:待批(随实现 PR 挂 `status/frozen-contract`)。** 依据:用户 v5.6.18 真机实测 **J113**(「配对、
> 优先级、命名全部没有保存下来,主唱锁定也没有保存下来」)+ **SL-472**(`channels[15]` 整节从未接过持久化)。
> 契约 §一 本来就列了这八项,本次是**实现追上契约** + §三「挂账未落盘」一行订正。本文档与实现放在**同一个 PR** 里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:七项都不是自动化参数(state-only),ParamID / index /
      顺序 / versionHint 一个字节都没碰。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**:给 Input 的广播区(ctrl 段)布局与字段一律不变,
      本卡只是让载入后的配置**照既有路径**再广播一次。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 `channels[15]` 七项注明**随工程落盘**、`source_channels`
      注明**不落盘**(运行期重测);§三 `CFGS` 行把七项挪进「当前已落盘」、「挂账未落盘」一行订正
      (此前只认 `source_channels` / `participate_in_auto_pan` 两项,**漏记另外六项** —— 而 `source_channels`
      本来就不该落盘,见下);`CFGS` 尾扩一整档 1860 字节、容器 **abi 5→6** + `migrate_5_to_6`(no-op)、
      尾长分档表加 `(52,1912)` 一格。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**:§1.15 `setChannelConfig` 的入参与值域、§2.1
      `scvb.state.channels[]` 的载荷行**一个字都不改** —— 七项本来就在载荷里,缺的只是「写盘 / 读盘」那一跳。
      `contractVersion` **不升**:§0.1 第 3 条 / §9.0 第 3 条只在桥面「改名 / 删除 / 收窄」时升主版本,
      本卡桥面零改动;state 侧按 STATE_SCHEMA 的口径是加法(新增一整档可选尾字段)。
- [x] tests/golden/(golden 快照)—— **新增** `tests/golden/state/abi6.bin`;`abi1.bin` … `abi5.bin`
      **保留不动**作迁移基线(六份并存,不是替换)。
      ⚠ **金样锁的是容器头**(magic / abi / flags / chunkCount / TLV 框):夹具的 CFGS 载荷是
      `opaque("CONFIG")` 字面量,不是 `encodeOutputState` 的产物 —— 所以 `abi6.bin` 与 `abi5.bin`
      只差 **abi 那一个字节(offset 4:5 → 6)**是**必然**而非巧合。channels 档的 wire 布局由
      `tests/core/test_output_session.cpp` 的长度断言与 `[SL-472]` 六格承担。

## 变更内容

**一句话**:Output 轨道页的七项 —— `enabled`(启用)/ `label`(名称)/ `participate_in_auto_pan`(参与自动声像)/
`priority`(优先级)/ `lead_lock`(主唱锁定)/ `lead_vol_exempt`(音量豁免)/ `pair_id`(配对)—— **随工程保存**。
`CFGS` 尾部再扩**一整档 1860 字节**(15 条定长记录 × 124 字节),state 容器 abi 5→6,新增 no-op `migrate_5_to_6`。

**每轨一条定长记录**(小端;偏移相对记录起点;第一条记录起于 `CFGS` 载荷的 `76 + languageBytes`):

| 偏移 | wire 类型 | 字段 | 合法值 | 构造默认(= 缺席 / 非法时的回落值) |
| --- | --- | --- | --- | --- |
| +0 | u32 | `enabled` | 0 / 1 | 1(启用) |
| +4 | u32 | `participate_in_auto_pan` | 0 = 不参与 / 1 = 参与 / **2 = 未显式设置** | 2(⇒ [J83] 一律参与) |
| +8 | u32 | `priority` | 0..10 | 5 |
| +12 | u32 | `lead_lock` | 0 / 1 | 0 |
| +16 | u32 | `lead_vol_exempt` | 0 / 1 | 0 |
| +20 | u32 | `pair_id` | 0..7(0 = 无配对) | 0 |
| +24 | u32 | `labelBytes` | 0..96 | 0 |
| +28 | u8[96] | `label` | 严格 UTF-8、无 NUL、≤ 24 码点;前 `labelBytes` 字节有效,其余补 0 | 空串 |

- **值域的来源**:`priority` 0..10 与 `pair_id` 0..7 是桥面 `handleSetChannelConfig` 的两处 `jlimit` 与广播区
  `publishConfigBroadcast` 的同款夹取;`label` 的 24 码点是桥面的 `substring(0, 24)`,UTF-8 最坏 4 字节/码点 ⇒
  槽 96 字节。本卡**只读**这几个值域,不改桥面。广播区给 Input 的镜像另有自己的 100 字节上限
  (`kCtrlLabelBytes`),与本处互不相干。
- **`label` 的截断**:编码侧取「≤ 24 码点且 ≤ 96 字节、不切半个码点」的最长前缀(与 `uiLanguage` 的超长截断
  同一类:能不能写下去,不是值域校验);遇到内存里的坏字节就在那里截止 —— 保证写下去的字节一定能被解码原样收回。
  生产路径上桥面已把名字限在 24 字符,所以正常操作**碰不到**截断。
- **`participate` 存三态**:内存里是 `participateAutoPanSet` + `participateAutoPan` 两个 bool;只存「参与与否」
  会把「用户从没动过」存成「用户选了参与」,[J83] 的默认档将来再变时,老工程就不再跟着默认走。
- **记录定长**(label 占满 96 字节槽,不按 `labelBytes` 变长):长度回退只认「remaining 够不够一整档」;
  变长记录会让「这一档到哪里结束」依赖档内字段,档内一个坏长度就把其后的未知尾部一起读歪。代价是每份工程
  多 1860 字节(对「几百 KB 量级」的 CRVS 可忽略)。
- **`source_channels` 不落盘**:它每拍由 `refreshSourceChannels` 从音频环段头重测,存了也会在下一拍被覆盖 ——
  所以 §三 旧「挂账未落盘」一行把它列成待落盘项本身就不对,本次订正为「不落盘(运行期)」。

**加载侧**:`setStateInformation` 在 `CFGS` 解码成功后逐轨写回 `runtime_.channels`(不叠第二道夹取 —— 解码
的出口只有「合法值」与「构造默认」),然后 `++configSeq` —— 与桥面 `setChannelConfig` 改完配置后做的
**同一件事**,不另造推送路径:
- 给 Input 的广播区:下一拍 `timerCallback → publishConfigBroadcast` 按 `configSeq` 变化门重写;
- 给本编辑器的 `scvb.state`:25Hz `emitState` 每拍全量比对下发(`channels` 在同一棵子树里);
- 启用位图 / 打印器逐轨启用位:`timerCallback` 每拍从 `runtime_.channels` 重算。

只带 `PRMS` 的轨道 / 参数预设(无 `CFGS`)走不到这一步,**不动**通道配置 —— 与其余 `CFGS` 字段同一个落点。

**失败态(与 [SL-411]/[SL-416] 同一族)**:

| 形态 | 处置 |
| --- | --- |
| 整档缺席(abi≤5 的旧工程,尾长 0/8/16/28/52) | 七项 × 15 轨取构造默认,**不计回落** |
| 半截(52 < 尾长 < 1912) | **整块拒载**(档内不许半截 —— 一整档是同一个构建写下去的) |
| 在席、某轨某项非法 | **只有那一项**回落构造默认并按字段计数(15 轨累加);同轨其余项、别的轨、前面几档都不受牵连 |
| 尾长 > 1912 | 多出来的字节进 `unknownTail`,保存时原样回写 |

**设计题:缺席与非法要不要回落到不同的值?**(SL-411 与 SL-279 在这一点上有意不同)本卡的结论是**不需要**,
两者回落到同一组构造默认:
- SL-279 的 `applied` 缺席时取「当前值」,是因为它有一个比默认更可信的来源(那份工程存着的当前档);
  这七项**没有**这样的第二来源 —— 它们就是「当前设置」本身,与 SL-411/SL-416 同类。
- 非法值只可能来自损坏或手改;构造默认正是「旧构建里这条轨的样子」,也是「我不知道时最保守」的那一个:
  `enabled` 回启用、`participate` 回「未显式设置」(保住 [J83] 语义不被一次坏值钉死)、`pair_id` 回无配对、
  `lead_lock` / `lead_vol_exempt` 回关。
- 同一条回落值的一个已知代价照实记:**非法值回落后再保存,磁盘上那个坏值就被默认值覆盖了**(与 SL-411/SL-416
  两档同一处境;SL-524 那种「拒载后保存覆盖」的问题在这里不成立,因为这里不是拒载、而是逐项接受了一个
  本构建能兑现的值)。

## 兼容性影响

- **新版本读旧工程(abi ≤ 5)**:容器走 no-op 迁移链升到 abi=6(状态 `Migrated`),`CFGS` 按长度回退补齐 ——
  channels 档缺席 ⇒ 七项取构造默认,**不计回落**。这正是旧构建重开后 `runtime_.channels` 的值,所以**旧工程
  在新版里打开后的行为与今天逐字相同**(名称为空、全部启用、优先级 5、主唱锁定 / 音量豁免关、无配对、参与
  自动声像按 [J83] 一律参与)。abi=1..4 的工程先经它们各自那一级的回退,再落到这一级。
- **旧版本(v5.6.18,abi=5)读新工程(abi=6)**:读代码的确定结论 ——
  `loadState` 在 `hdr.abi > kCurrentAbi` 时**整份** `RejectedNewer`,`decodeContainer` 都不会被调用;
  `OutputProcessor::setStateInformation` 于是置 `stateAbiMismatch_`、把整份原始字节存进 `preservedStateBlob_`
  后 return —— **一个字段都不载入**(参数、段表、版本名、VAD、七项全是那台实例的当前值 / 默认值);
  编辑器按 §2.9 发 `scvb.error{newerState, detail:{localAbi:5, projectAbi:6}}`,**弹「较新版本」横幅**
  ([SL-412] 已在 v5.6.15 接线);**保存时 `getStateInformation` 原样回写那份原始字节** —— 所以**工程文件里
  一个字节都不丢**,但用户在旧版里对这份工程做的任何改动都**不会**被存进去(存回去的永远是新版那份)。
  这是 CLAUDE.md §7.3「高版本拒载 + 提示升级,绝不静默降级」的既有行为,与 [SL-411](abi 3→4)、
  [SL-416](abi 4→5)两次升格**完全同一处境**,不是本卡新引入的形态。
  ⚠ 另一条路本卡**没有走**,照实记下供裁定:按 `OutputStateCodec.h` 头注,在尾部(≥52)追加**不升 abi** 也行 ——
  那样 v5.6.18 会照常载入其余一切、把 channels 档当 `unknownTail` 原样带走,旧版体验更好;代价是偏离
  「每一档对应一次 abi 升格」的既有做法、且旧版里对七项的改动会被那份 unknownTail 在下次新版打开时覆盖回去。
- **Input 侧连带**:Input 与 Output **共用容器 abi**,本 PR 之后新 Input 保存 state 一并写 abi=6
  (Input 自己的 CFGS 一个字节没变);旧 Input 读新 Input state 整块 `RejectNewer`:置 `stateAbiMismatch_`、
  原字节存进 `preservedStateBlob_` 供保存时原样回写(`InputProcessor::setStateInformation`),所以**工程里不丢
  字节**,但这台旧 Input 以当前值运行(组号 / 通道号 / 缩放 / 语言),且**只落一行 `DBG`、没有横幅**
  (升级横幅按统筹 2026-09-16 裁「本版不做」)。本卡未改 Input。
- **Monitor 侧连带**:Monitor 也写同一容器、共用 `kCurrentAbi`,本 PR 之后它保存的 state 一并写 abi=6。
  它读高版本 blob 走 `decideInputStateAbi(...) == RejectNewer` 那一支:**拒载、不回写**(丢的只是「看哪一组 /
  缩放 / 语言」这三项视图偏好,碰不到段表与曲线)—— 与 [SL-416] 那次同一处境。
- **参数面**:零影响。**桥面**:入参、值域、返回值零改动。**IPC 广播区布局**:零改动。
- **顺带修掉的一处继承缺陷(行为变化,用户可见)**:桥面 `setChannelConfig` 判「配置有没有变」时,participate 比的是
  **存储值**而不是生效值。一条从没动过的轨(未显式设置 ⇒ 生效 = 参与)在轨道页上只取消勾选「参与自动声像」时,
  存储值 false → false 被判成「没变」、不 bump `configSeq`,而给 Input 的广播区按 `configSeq` 做变化门 ⇒
  Input 页面上那一行只读摘要(`InputBridgeLogic.cpp` 的 `scvb.config.participate_in_auto_pan`,只喂显示)会一直
  显示「参与」,直到这份工程里别的配置项的**值**再变一次。本卡把这段比对收进持锁的
  `bridgeApplyChannelConfig` 时改为比 `participatesInAutoPan()` 前后值(HOST SL472 单字段格 + 删除式 D16)。
  Output 自己的分析与 `scvb.state` 回推不受这条影响(前者直接读生效值,后者 25Hz 全量比对,不看 `configSeq`)。

## 判据与机检(每条都实跑,删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| core 搬运 | `tests/core/test_output_session.cpp` `[SL-472]` 六格 | 七项往返(每项至少一格非默认、label 含中文与 24 码点 / 96 字节边界)/ 编码侧截断 / abi≤5 各级旧档 ⇒ 构造默认且不计回落 / 非法值**逐字段**回落 + 独立计数 + 15 轨累加 / label 九种非法形态 / 半截 (52,1912) 拒载 + 边界 52 与 1912 可解 |
| core 容器 | `tests/core/test_state_codec.cpp` `STATE-GOLDEN StateAbiCompat` | abi1..abi5 五份旧金样仍 `Migrated` 到当前 abi;**abi6.bin 格式锁** |
| 生产几跳 | `tests/host/test_host_harness.cpp` `HOST SL472` | 设七项 → `getStateInformation` → 新实例 `setStateInformation` → 运行态逐项一致;推给 Input 的广播区逐项一致;abi=5 形态的旧工程 ⇒ 七项回构造默认(并与 codec 的缺省值逐项对拍);同一份工程里 [SL-485] 的 44.1k 采集率随载入恢复、再存仍记 44100,两者互不覆盖 |

⚠ **离线不可达、本卡没有改变的那一跳**(照实登记,别读成已覆盖):`OutputEditor::buildStateSubtree` 把
`runtime_.channels` 装进 `scvb.state` 回推 web —— 依赖 WebView2,不在 host 套件的 TU 清单里。它是**纯读**
`runtime_.channels`,host 用例对 runtime_ 的逐项断言就是它的输入;装配那几行本卡未改。

## 变更文件

- `src/core/state/OutputStateCodec.{h,cpp}`(第五档 channels:定长记录、七项值域常量、`OutputChannelState`、
  七个按字段的回落计数器、严格 UTF-8 校验与编码侧截断、长度回退第五级、头注布局)
- `src/core/state/StateCodec.h`(`kCurrentAbi` 5→6;容器头注)
- `src/core/state/StateMigration.{h,cpp}`(`migrate_5_to_6` no-op;`kMigrators` 五项)
- `src/output/OutputProcessor.{h,cpp}`(保存侧写七项 / 加载侧写回 + `++configSeq` + 回落计数的 DBG 行;
  持 `lifecycleMutex_` 的 `bridgeApplyChannelConfig` / `channelsSnapshot` 两个口 —— `label` 是 `juce::String`,
  本卡起它被宿主线程上的 get/setStateInformation 读写,消息线程侧的写与读必须同锁,否则是 use-after-free)
- `src/output/OutputEditor.cpp`(`setChannelConfig` 阶段 2 改走 `bridgeApplyChannelConfig`:比对、赋值、bump 逐项搬移,
  **唯一有意的行为差是 participate 的变化判定**,见「兼容性影响」最后一条;
  `buildStateSubtree` 与建议表导出读 label 改走 `channelsSnapshot`)
- `src/output/OutputUiState.h`(CFGS 长度纪律更新到五级 1912 字节)
- `docs/STATE_SCHEMA.md`(abi 5→6、§一 七项已落盘 / source_channels 不落盘、§三 CFGS 行与挂账订正、尾长分级、迁移链五条)
- `CHANGELOG.md`(预写块一条)
- `tests/golden/state/abi6.bin`(**新增**;abi1–abi5 不动)
- `tests/core/test_output_session.cpp`、`tests/core/test_state_codec.cpp`、`tests/host/test_host_harness.cpp`

## 审批

待批:随实现 PR 挂 `status/frozen-contract`,由用户明确批准后合入(06 §3.7)。依据:用户实测 J113 + SL-472;
契约 §一 本来就列了这八项,本次是实现追上契约 + §三 挂账订正。「旧版读新工程整份拒载(原样回写 + 横幅)」
与「不升 abi 的尾部追加」二选一,本 PR 按既有做法选了前者,一并报批。
