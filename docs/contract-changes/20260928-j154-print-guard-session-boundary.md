# 契约变更说明 —— 20260928-j154-print-guard-session-boundary

> **状态:已批(随本 PR 挂 `status/frozen-contract`)。** 用户裁定 **J154**(发布前 17 条拍板的第 14 条
> 「加载守卫 §1.34 措辞」):原话「14按你说的做」,指的是 #309 描述里列出、统筹建议的选项 **(a)** ——
> 把「宿主重灌状态算新的一次会话」补进契约;未取的是选项 (b)「改实现为同一实例确认过就不再重置」。
> **实现侧不动**:#309 第一个实现提交起就是这个行为;本变更只补契约里的一句,外加一格 host 用例把它钉住。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:`print_guard` 仍是运行时态,不入 state chunk。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.34 `confirmPrintGuard()` 的「语义」格**补一句**,界定
      「本工程会话」在宿主重灌状态时的边界。该格其余文字逐字未动;函数名 / 参数 / 返回 / 事件名 /
      载荷字段零变化。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`,§7 manifest 不动:没有改名、删除或收窄取值域;`print_guard.pending` 的
含义仍是「待确认」。补的这句只回答原文没回答的问题:宿主在同一实例上再次灌入状态,算不算
「本工程会话」之内。

## 变更内容

**加在哪里**:§1.34「语义」格,「确认后若满足 PRINT 三与条件……即恢复正常 PRINT。」之后、
「**零 gesture**」之前。

**补的一句(原文照录)**:

> **「本工程会话」的边界(J154)**:宿主对同一实例再次灌入插件状态(`setStateInformation` 恢复出
> `output_enabled=ON`,如带插件状态的 DAW 撤销、A/B 对比、载入预设),就本条而言算作新的一次工程会话 ——
> 守卫重新置位,横幅⑦再次出现,须再次确认。

**为什么是 (a)**:processor 只看得到 `setStateInformation` 这一次调用,分不出这是「重开工程」还是
「宿主重灌状态」。可选的做法只有两种:每次恢复出 ON 都重新置守卫,或者确认过一次就永远不再置。
后者的代价是同一实例载入**另一个**输出为开的工程时也不再守卫 —— 这正是加载守卫要防的那种覆盖。
(a) 最坏只是多要一次确认(只会少写),所以统筹建议 (a),用户按此拍板。

## 实现对照(本 PR 已有,未改)

- `OutputProcessor::setStateInformation`:CFGS 解码成功后 `printGuardPending = outputEnabled_`
  (恢复 ON ⇒ 置位,恢复 OFF ⇒ 清除)。**不看此前是否确认过**,所以同一实例上再灌一次 ON 就重新置位。
- `OutputProcessor.h` `OutputRuntimeState::printGuardPending` 头注写明了这个选择与理由。
- `setStateInformation` 的几条早退路 —— 拒载(较新版本 / 损坏)、没有 CFGS、CFGS 解码失败 —— 都在
  写 `outputEnabled_` 之前 return,守卫也不写;这些路上没有「恢复出 ON」这件事,所以不在本句的范围内。

## 判据

- `tests/host/test_host_harness.cpp`「HOST 加载守卫:恢复 OFF 不设守卫;关输出解除;开输出不算确认」
  末尾新增一段(`[J154]`):同一实例先灌一份输出为开的状态、`confirmPrintGuard()`,再灌一次 ⇒
  `printGuardPending()` 重新为真。放在用例最后,是因为选项 (b) 会让这个实例此后每次重灌都不再置位,
  放在前面会连带把后面各段的前置 REQUIRE 一起弄红。
- 反向注入:把实现改成选项 (b)(同一实例确认过之后 `setStateInformation` 不再写守卫)⇒ 只有这一格红;
  复原 ⇒ 绿。读数见 PR 评论。

## 用户文档

`docs/USER_GUIDE.md` / `docs/USER_GUIDE.zh-CN.md` 同一条已有「宿主重新载入插件状态后横幅会再出现」
(#309 前一个提交)。本次补上前提「载入的状态里输出开关为开」,并把 A/B 对比列进例子,与契约这句对齐。

## 没改、说法相同的地方(不在本仓)

计划侧 05-ui-spec 的 §1.4 函数表(`confirmPrintGuard()` 行)与 §2.0 横幅⑦也写着「本工程会话 / 本次会话
不再出现」。那是计划侧文档,不在本仓,由统筹决定是否跟改。本契约 §9.1 P-1 那一行是
**起草期原文留档**(「争议原文」),照旧不改。

## 兼容性影响

零行为变化:实现、state、桥面载荷都没动。对用户可见的只是文档:契约与 USER_GUIDE 现在都写明
「宿主重灌一份输出为开的状态后,横幅⑦会再出现」。

## 审批

- 用户裁定 J154(masterPlan `plan/adjudications.md`),原话「14按你说的做」→ 选项 (a)。
- 本 PR 挂 `status/frozen-contract`。
