# 契约变更说明 —— 20260929-j156-sl463-claim-failure-receipt

> **状态:实质已批(随本 PR 挂 `status/frozen-contract`)。** 用户 2026-09-29「发布前 17 条拍板」第 17 条,
> 裁定 **J156**(卡 **SL-463**),裁定表(masterPlan `plan/adjudications.md`)记的结论原文:
> 「**批契约变更,v1 做** —— §3.2/§3.3 返回并集加 {ok:false,reason}(kUnavailable 等),Input UI 给出提示」。
> 裁定表对这一条**没有录用户原话**,只录了上面这句结论;本文不替用户补写原话。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。段名、段布局、abi 一个字节都没改。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— Input `setChannelId`(§3.2)/ `setGroupId`(§3.3)的**返回并集加宽**,
      §5.6 `reason` 闭集加两个值,§5.2 两行 UI 落点补一句,§7 manifest 两行 `returns` 随改。
      函数名 / 参数 / 事件名 / 事件载荷字段**零变化**。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`:返回并集只**加**形状、`reason` 闭集只**加**值,不改名、不删、不收窄
(§0.1 第 3 条的禁止面一条都不命中)—— 与 [J81] 给 §5.6 加 `cancelled`/`noData`/`ioError` 同一种变更。

## 变更内容

### 缺陷(SL-463)

`InputEditor` 的两个 handler 写的是 `st == kConflict ? {conflict:true} : {ok:true}`,而处理器的请求结果
有五种(`kActive` / `kUnassigned` / `kConflict` / `kAbiMismatch` / `kUnavailable`)—— 后两种**失败**被回成了
`{ok:true}`。契约的返回并集当时只有 `{ok:true}` | `{conflict:true}`,没有第三种形状可回,所以这不是
实现写漏,是契约没给出口。用户可见的后果:

| 失败 | 此前界面上看到的 |
| --- | --- |
| `kAbiMismatch`(本组 registry 由另一 abi 的 SCVB 建) | 红 pill「版本不匹配」+ abi 横幅 —— 有提示,但点卡那一下没有任何反馈 |
| `kUnavailable`,首次选通道(段打不开 / 映射失败 / `claimInput` 非冲突失败 / `createSegments` 失败) | 只剩灰 pill「未选择通道」,**没有专门提示** |
| `kUnavailable`,换通道且补偿式回滚成功 | **什么都不变**:会话回到原通道、`claim` 仍是 `active`,点了卡就像没点 |
| 改组时新组的 ctrl 段打不开 | **什么都不变**:组号没换、会话留在旧组,处理器还回了会话此刻的 `kActive` |

最后一行是实现一侧的同族缺陷:`ScvbInputAudioProcessor::setGroupId()` 在 ctrl 段打不开时回
`session_.state()`,本 PR 改为按 ctrl 段的失败原因回 `kAbiMismatch` / `kUnavailable`(见下)。

### 逐条

| # | 位置 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | §3.2 返回 | `{ok:true}` 或 `{conflict:true}` | 另加 `{ok:false, reason:"abiMismatch"}`、`{ok:false, reason:"unavailable"}`、`{ok:false, reason:"badArg"}` |
| ② | §3.2 语义 | —— | 补「释放不会失败」与「**返回值报的是这次请求本身的结果**,补偿式回滚成功时回执照样是失败形状」 |
| ③ | §3.2 拒绝态 | 只写 `{conflict:true}` 与「abi 不符 ⇒ claim 置 abiMismatch」 | 逐个写三种失败的触发条件与 UI(抖卡 + 一次性 toast) |
| ④ | §3.3 返回 / 语义 / 拒绝态 | `{ok:true}` 或 `{conflict:true}` | 同 ① 加宽;语义补「失败分两支,组号去向不同」:ctrl 段打不开 ⇒ **不改组**;新组 claim 失败 ⇒ **组号已换** |
| ⑤ | §5.2 `idle` ② 支 / `abiMismatch` 两行的 UI 落点 | —— | 补一句「由点卡 / 切组触发的那一次,回执另带 reason,UI 另弹一次性 toast」 |
| ⑥ | §5.6 `reason` 闭集 | 十一值 | **十三值**:加 `abiMismatch`、`unavailable`,写明出处 [J156] |
| ⑦ | §7 manifest `input.setChannelId` / `input.setGroupId` 的 `returns` | `{ok} \| {conflict:true}` | `{ok} \| {conflict:true} \| {ok:false,reason:"abiMismatch"} \| {ok:false,reason:"unavailable"} \| {ok:false,reason:"badArg"}` |

**命名**:`abiMismatch` 与 §5.2 的 claim 值同拼写、同含义(abi 不符拒连),UI 与诊断看到同一个词;
`unavailable` 取自 C++ 的 `InputClaimState::kUnavailable` / `Registry::ClaimResult::kUnavailable`,对应 §5.2 `idle`
的「段不可用」那一支。没有复用既有的 `ioError`:那一个是 §1.36 导出落盘的文件 I/O 失败,语义不同。

**`badArg` 是顺带补登,不是新行为**:两个 handler 解析参数失败时一直回 `{ok:false, reason:"badArg"}`
(§0.8 第 2 条的通用规则),只是 §3.2/§3.3 的「返回」行没写 —— 既然本次重写的就是这两行的完整并集
(§0.8 第 5 条:「返回」行登记的是可枚举的完整并集),就一并写全。同批 #315 给 Output §1.4 补 badArg 是同一种订正。

### 实现

- `src/input/InputBridgeLogic.{h,cpp}`:新增 `claimRequestResponse(InputClaimState)` —— 五种请求结果到桥面形状的
  映射表;枚举外的值按 `unavailable` 报,**不回 `{ok:true}`**。
- `src/input/InputEditor.cpp`:`handleSetChannelId` / `handleSetGroupId` 改为 `complete(bridge::claimRequestResponse(st))`。
- `src/input/InputProcessor.cpp`:`setGroupId()` 的 ctrl 段失败分支按 `InitResult` 回 `kAbiMismatch` / `kUnavailable`,
  不再回 `session_.state()`。**这是本 PR 唯一的处理器行为改动**,它只改返回值:组号、会话、ctrl 段的回退动作
  一行没动。`setChannelId()` 的返回值此前就是请求结果(`requestResult`),只改了注释。
- `web/input/app.js`:点卡 / 切组拿到 `{ok:false, reason:"abiMismatch"|"unavailable"}` ⇒ 被点的卡 / 胶囊抖一下 +
  一次性 toast(与冲突提示共用同一个 toast 槽)。其余 `reason`(`badArg`:UI 只发合法整数,走不到)不弹。
  **没有乐观态要回滚**:点卡 / 切组都等回执,选中卡、组胶囊与 pill 一律由随后的 `scvb.state` 决定。
- `web/shared/i18n.js`:新增 `ch.claimFailed.unavailable` / `ch.claimFailed.abiMismatch` 三语各两条;
  中文措辞避开了字体子集里没有的字(「共享」的「享」不在子集里,改写成「插件间通信用的内存段」),
  本 PR 不给字体子集引入新字(`check-font-coverage.py` 改前改后所需字符集相同)。
- mock(`web-preview/mock/`):新增 `caps.claimFailure` 与两个场景 `claim-unavailable` / `claim-abi-mismatch`,
  按真桥的失败顺序与状态形状造回执(abi 不符先于占用判定;建段失败在占用判定之后;换通道失败时会话回滚)。

## 兼容性影响

- **新旧版本互通**:桥面是插件与自己内嵌网页之间的接口,两端随同一个二进制发布,不存在「新插件配旧网页」。
  即便假设旧网页收到新形状:旧 `app.js` 只认 `res.conflict === true`,`{ok:false, …}` 走「什么都不做」那支,
  与改动前的观感一致。
- **IPC / state / 参数面**:零变化。
- **行为**:只多了失败时的提示;成功与冲突两条路径的回执形状逐字不变(用例钉着)。

## 验证

- `tests/webview/test_input_bridge.cpp`:`claimRequestResponse` 五种结果 + 枚举外兜底逐支钉形状;
  另一格**源码级**判据读 `InputEditor.cpp`,钉两个 handler 都把返回值交给映射表、都不再自拼 `okResponse()`。
- `tests/host/test_host_harness.cpp`(真 Processor + 真命名段,用 abi 不符的同名段造失败):
  首次选通道遇 registry abi 不符 ⇒ `kAbiMismatch`;换通道遇目标 audio 段打不开 ⇒ `kUnavailable` 且会话回滚到原通道;
  改组遇新组 ctrl 段 abi 不符 ⇒ `kAbiMismatch` 且组号不变;改组遇新组 audio 段打不开 ⇒ `kUnavailable` 且组号已换。
- `web-preview/tests/smoke-input.mjs` ⑥(mock 回执形状)与新增页面级 `smoke-input-claim-failure-page.mjs`
  (抖卡 / 抖胶囊 / toast 词条与文案 / 回滚后选中不变 / 成功不弹的反向对照)。
- 删除式按落点逐格做,读数见 PR 描述。

## 不在本次范围(留账)

- **载入工程路径上的 `kUnavailable` 仍只有灰 pill**:载入(`setStateInformation` / `prepareToPlay`)没有 RPC 回执可挂,
  要提示得给 `scvb.error` 加一个 code(§5.1 现有各码之外),那是另一次契约变更,J156 没批到那一层。
- **05-ui-spec 的词条表**(§5)与 Input §3 的 Channel 选择器 / 组选择器两行,需要统筹按本文回填两条新词条与
  「非冲突失败 ⇒ 抖动 + 一次性 toast」的落点(i18n.js 头注的纪律是「改文案先改 05」,05 在 masterPlan,不在本仓)。

## 审批

- 用户 2026-09-29 裁定 J156(masterPlan `plan/adjudications.md`),结论原文见文首。
- 本 PR 挂 `status/frozen-contract`。
