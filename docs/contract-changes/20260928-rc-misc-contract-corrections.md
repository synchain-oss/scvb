# 契约变更说明 —— 20260928-rc-misc-contract-corrections

> **状态:待批(随本 PR 挂 `status/frozen-contract`)。**
> 本文档记三处**纯订正**:冻结契约的文字与已经在跑的实现不一致,把文字改成实现的样子。
> **零行为变更、零布局、零 abi、零参数面变更**,C++ / JS 的行为代码不因本文档而改一行
> (同 PR 里的行为改动各有各的卡,与这里的文字订正无因果关系,见文末「与同 PR 其他改动的关系」)。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [x] docs/STATE_SCHEMA.md(state schema)—— 头注一句 + §4.1 FEAT「加载」一句 + 文首「最后更新」行。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.4「返回」行、§1.25「语义」行、§3.4「返回」与「拒绝态」行、
      §7 manifest 的两条 `returns`(`output.setGroupId`、`input.remoteSetPriority`)。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

### ① STATE_SCHEMA:升级提示「尚未接线」与 §三 正文自相矛盾

- 头注原文写「⚠ **升级提示的 UI 通路尚未接线**:Debug 仅 `DBG`,用户看不到解释 —— 登记 SL-412」;
  而同一文件 §三「迁移函数框架」写明 **Output 侧自 v5.6.15 起已接线**(`OutputEditor::emitNewerStateError()`
  发 `scvb.error{newerState}` → 红横幅④),Input 侧统筹 2026-09-16 裁「本版不做」。
  改成与 §三 一致:Output 侧已接线、Input 侧本版不做,指向 §三。
- §4.1 FEAT 节「加载」一句原文「codecVer 高于当前 → 该节按空处理 + 提示升级(**同上:UI 通路待接线**,SL-412)」。
  实况:FEAT 节 codecVer 更高时 `OutputProcessor` 只落一行 `DBG`、特征按空处理、原始字节原样保留
  (`preserveAndBail(/*newerCodec=*/true)`),**这一级没有任何 UI 提示**;[SL-412] 接上的是**容器级** abi。
  改成照实写「这一级没有 UI 提示」,不再用「同上」挂到头注上(头注改了之后「同上」会变成假话)。

### ② SCVB_CONTRACT §1.25:「覆盖 §0.9 左列的四类操作」

§0.9 左列自 [J82] / [J89] / [J95③a] 起早已不止四类(现为 `setPanCurve`、`editSegment`、`setTrackManual`、
`copyVersion`、`setVersionName`、`analyze`、`setVadParams` / `setSegmentation` 松手档)。
改成「覆盖 §0.9 左列的全部入栈操作」,以 §0.9 那张表为唯一枚举处,本行不再抄数字。
`web/output/tab-master.js` 撤销可用性头注里那句「须另开卡订正」同步改成指向本文档。

### ③ 返回并集缺形状(§0.8 第 5 条:「返回」行是完整并集)

| 函数 | 实现里会回、契约没登记的形状 | 实现位置 |
|---|---|---|
| Output `setGroupId(g)`(§1.4) | `{ok:false, reason:"badArg"}`(`g` 不在 1..8) | `src/output/OutputEditor.cpp` `handleSetGroupId` |
| Input `remoteSetPriority(n)`(§3.4) | `{queued:false, reason:"busy"}`(命令没能写进环:ctrl 段未打开 / 打开失败) | `src/input/InputProcessor.cpp` `bridgeRemoteSetPriority`(PR#54 R10 起) |
| Input `remoteSetPriority(n)`(§3.4) | `{ok:false, reason:"badArg"}`(`n` 不是整数) | `src/input/InputEditor.cpp` `handleRemoteSetPriority` |

**纯加法**:只往「返回」行与 §7 manifest 的 `returns` 里补形状,既有形状一个不删不改;`busy` / `badArg`
都已在 §5.6 的 reason 闭集里,不新增 reason。`node scripts/check-bridge-parity.mjs` 通过。
web 侧「凡是 `queued` 不为 true 都回滚乐观值」由另一张卡做(SL-458 / SL-20,PR #303),本文档不重复。

## 兼容性影响

无。三处都是把文字对齐到已经发出去的实现:既有工程、DAW 自动化、新旧版本互通都不受影响。
mock 后端不需要改(它不造 `busy` / `badArg` 的前提条件)。

## 与同 PR 其他改动的关系

同一 PR(rc-misc 小修合集)里还有几处**行为**改动(Output 发 `srMismatch` 错误、Output 版本号取
`JucePlugin_VersionString`、Monitor 写/读全局默认、撤销钮在改曲线 / 改名后点亮等),它们都落在契约
**已经承诺**的行为上(§5.1 横幅③、§10.1「形制同 §1.29/§1.30」、§1.1 `version.plugin`、§1.25 回执语义),
不改任何契约文字,不在本文档的审批范围内。

## 审批

挂 `status/frozen-contract`;三项都是纯订正(零行为变更),按统筹对「纯订正」的授权处理,合并前由统筹核对。
