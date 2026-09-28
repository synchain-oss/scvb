# 契约变更说明 —— 20260928-sl535-analysis-connected-only

> **状态:已批(用户 2026-09-28,J119,原话「B吧那就。」)。** 依据:用户 v5.6.19 实测(轨从通道 10 改到
> 通道 1 之后,通道 10 已无 Input,旧采集数据仍进分析,把唯一出声的通道 2 挤到左边)+ **SL-535**。
> 用户在 A / B / C 三案中选 B:分析只认此刻连着 Input 的通道;否决 A(自动关 `enabled`,改落盘设置、
> 重开误关)与 C(断开即清采集数据,会误删)。本文档与实现放在**同一个 PR** 里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**:判据读的是既有 registry 槽位状态与心跳。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:不改 `enabled`、不清采集数据,没有新落盘字段。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.5 `previewAnalyze` 的「语义」行补**参与面**定义;§1.6 `analyze`
      的「拒绝态」行注明 `coverage` 的参与面同 §1.5、「有数据但都没连上」同样落既有拒绝态。
      **函数名、参数、返回形状、reason 枚举一个字都不改**;`contractVersion` 不升(§0.1 第 3 条 / §9.0 只在
      桥面「改名 / 删除 / 收窄」时升主版本,本次载荷零改动)。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

**一句话**:§1.5 / §1.6 的 `range ∩ coverage` 里,`coverage` 只计 scope 内 **`enabled` 且此刻已连接 Input** 的轨。

- **「已连接」** = §2.3 `scvb.conn` 同轨 `slotState=2 ∧ heartbeatFresh`(`heartbeatAgeMs ≤ 2000`)。实现里是
  `isConnectedForDisplay`,与界面显示「未连接」同一个函数。**不用** registry 的 `connected_mask`:那一份还剔掉
  停流(`suspended`)与失准的轨,而宿主在静音段挂起 Input 是常态;`suspended` 的判定前提本身就是「心跳新鲜」,
  所以挂起的轨仍在参与面里。
- **`enabled`** 这一条是**既有行为**(dry-run 与真跑此前就跳过未启用的轨),此前没写进 §1.5 文字,借本次一并写明。
- **不改 `enabled`、不清采集数据**:没连上的轨只是这一次不进分析;Input 连回来,下一次分析自动回到参与面。
- **拒绝态不新增**:scope 内有采集数据的轨都没连上时,落 §1.6 **既有**的 `{ok:false, affected:{0,0,0}}`
  (不带 reason);dry-run 同条件返回 `tracks:0`,「预览说能跑 = 真跑受理」的对应关系不变。
- **时间采样窗口**:dry-run 与真跑各自采一次时钟,两次之间 Input 断开 / 连上会让两侧结论不同;后果只是真跑
  落回拒绝态(或预览偏保守到下一次节流刷新),与界面「未连接」显示同口径。心跳判据是 2 s 的**显示口径**,
  不是接管判据(接管走 5000 ms + pid 探测):宿主消息线程卡住 > 2 s 后紧接着的那次分析会把陈旧轨当未连接。

## 兼容性影响

- **桥面**:入参、返回形状、reason 枚举零改动;旧页面照常解析回执。
- **行为**:此前「有采集数据但没连上 Input」的轨会进分析,现在不进。用户可感知面见 CHANGELOG「修复」小节
  SL-535 那条(#292)。已存工程的段表、采集数据、`enabled` 均不受影响;只有**下一次**分析的参与面变了。
- **state / IPC / 参数面 / golden**:零影响。

## 判据与机检(删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| 生产几跳 | `tests/host/test_host_harness.cpp` `HOST SL-535` | 释放两条 Input(数据仍在)后:dry-run `tracks==1`、真跑受理且 `tracks==1`、幸存轨居中;全断开:dry-run 0 轨、真跑 `ok==false`、无分析在跑;接回三条后回到 3 轨 |
| web 纯函数 / 接线 | `web-preview/tests/smoke-tab1-interactions.mjs` | 原因句判据接 dry-run `tracks`、预览指纹覆盖 scope / 启用轨掩码 / 已连接轨号 / 有覆盖轨号、`scvb.conn` 与 `scvb.captureProgress` 两条订阅都触发重取 |

⚠ mock(`web-preview/mock/juce-bridge-mock.js`)**不建模**连接判据,「有数据但没连上」这一态在预览世界里不可达,
它的 UI 面**没有页面级冒烟覆盖**(已在 `affectedOf` 登记)。

## 变更文件

- `docs/SCVB_CONTRACT.md`(§1.5 语义行、§1.6 拒绝态行)
- 实现与测试见 PR #292 的其余文件(`src/output/OutputProcessor.{h,cpp}`、`src/output/OutputEditor.cpp`、
  `web/output/*`、`web/shared/*`、i18n、用户指南、`tests/host/test_host_harness.cpp`、`web-preview/tests/*`)

## 审批

**已批**(用户 2026-09-28,J119,原话「B吧那就。」):选方案 B,分析只认「启用 ∧ 有采集 ∧ 当前已连接」。
本文档把这一实质补进冻结契约文字;随实现 PR 挂 `status/frozen-contract`(06 §3.7)。
