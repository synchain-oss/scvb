# 契约变更说明 —— 20261011-a8-suspend-not-offline

> **状态:随 A 线 A-8(E2「挂起 ≠ 掉线」)同 PR 落地,挂 `status/frozen-contract`。**
> 变更实质已由用户在 2026-10-03 批准 A 线计划时一并批准(计划 §3「冻结契约 → 措辞补充」:IPC_CONTRACT §4
> `connected_mask` 注;SCVB_CONTRACT §2.3 `suspended` 语义随之调整;批准计划即批准实质,措辞随代码同 PR 落地,
> 用户在 A 线里程碑 PR 里过目)。E2 的语义细节由用户裁定 U3:
>
> - **U3(a)**:只在「走带在跑 ∧ 心跳新鲜」时保持在线(不是只对离线导出生效);
> - **U3(b)-ii**:单轨旁路唯一一条 Input(更一般地:注入轨全部停滞)时,总线回到原声,进出都走 80 ms 交叉;
> - **U3(c)**:接受宿主挂起期间 Input 面板仍显示「已连接」。
>
> 本文件只记**措辞**与它对应的实现落点。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 只在 §4「健康 Output 判定(J12)」那一条下面加一个子条目
      「[A-8] `connected_mask` 注」(说明文字)。段名、字段、偏移、大小、对齐、abi、§1 `OutputSlot` 代码块(含
      `connected_mask` 那一行的行内注释)**零变化**(`node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0)。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— 只在 §2.3 `scvb.conn` 的表里新增一行「挂起与在线([A-8])」(说明文字)。
      载荷字段集合、字段名、字段类型、事件名、函数名**零变化**(`node scripts/check-bridge-parity.mjs` 退 0);
      「字段纪律」那一行一个字没动(A-6 同时在改它里面 `misalignCount` 的成因,两边不碰同一行)。
- [ ] tests/golden/(golden 快照)—— **不动**。

`docs/constitution/` 下的宪法只读副本不动(SL-505 先例:副本改动走修宪流程)。masterPlan `01-architecture.md`
里「CH_SUSPENDED → mask bit = 0」的设计口径与对应的 J 裁定由统筹在 masterPlan 仓另行更新,本 PR 不碰。

## 变更内容

改前,「写头冻结 ≥ 500 ms」(`CH_SUSPENDED`)一律让该轨掉线:清 `connected_mask` 位、退出注入集。宿主在
静音段暂停调用 Input(Cubase「无信号时挂起 VST3 处理」、无 region)时,这条规则带来两个可闻缺陷(A-4 / #376 实测):

- **E2-a(实时)**:下一句开头先要等 Output 发现写头动了、重新置位、再走 80 ms 淡入 —— 句首丢 85–150 ms;停得比
  约 5.5 s 长时 Input 已切直通,单轨工程里句首还先出一段未平衡原声;
- **E2-b(离线导出)**:墙钟 500 ms 门限与 [M] 一拍按倍速放大,恢复处一拍 [M] 的时间线内该轨缺席(实测 192 ms)。

改后(实现:`OutputSession::evaluateChannels`):

| 情形 | 改前 | 改后 |
| --- | --- | --- |
| 走带在跑,写头冻结 ≥ 500 ms,心跳新鲜、采样率一致、已绑定、未失准、不在看门狗 hold 里 | 清 `connected_mask` 位,退出注入集 | **保留** `connected_mask` 位与注入;`suspended` 照常上桥;Input 照旧静音;写方一恢复,读方同块读到 |
| 走带停住,写头冻结 ≥ 500 ms | 清位 | 清位(**不变**;LS-15 钉住) |
| 心跳陈旧 | 清位 | 清位(不变) |
| 失准(本次发作内有缺口) | 清位 | 清位(不变) |
| 停摆看门狗 hold 闩住(KI-6,A-7) | 清位 | 清位(不变;保持在线服从 hold) |
| 离线导出 | 同实时(墙钟门限按倍速放大) | 同一条规则:走带在跑 ⇒ 挂起不掉线,恢复同块读到 |

`suspended`(SCVB_CONTRACT §2.3)的判据不变(写头冻结 ≥ `kSuspendStallMs` ∧ 走带在跑 ∧ 本段停滞期内确有饿读),
变的是它与「已连接 / 在线」的关系:它不再意味着断开,只说明「此刻没有数据」。

**U3(b)-ii(实现:`OutputProcessor::renderSpan` 的总线目标)**:保持注入之后,单轨工程里旁路唯一一条 Input 时
注入集不再清空,Output 会一直拿一份空混音替换总线、把旁路出来的原声吃掉。现在注入轨**全部**停滞(都在 Output
[M] 报的「保持在线的挂起轨」里,且本段没有一条读到数据)、并且总线输入听得见(高于约 −100 dBFS)时,Output 经
既有的 80 ms 等功率交叉(ADR-010)把总线交回原声,闩到有注入轨重新读到数据为止,再交叉回混音。多轨时只有部分
轨停滞,停滞轨照旧退出混音、总线不回原声。总线输入是数字静音时(宿主挂起 / 无 region —— E2 要修的那一类)不回
原声:那时原声就是静音,Output 的空混音与它逐样本相同,拨回直通只会让写方恢复时多走一遍 80 ms 淡入、把句首吃掉。

| 措辞落点 | 说的是什么 | 实现落点 |
| --- | --- | --- |
| IPC_CONTRACT §4「[A-8] `connected_mask` 注」 | 「数据健康」不要求写头此刻在推进;走带在跑时挂起的轨保留该位;走带停住、心跳陈旧、失准、看门狗闩住照旧清位 | `OutputSession::evaluateChannels`(`keepThroughStall = transportPlaying_`,`online` 公式) |
| SCVB_CONTRACT §2.3「挂起与在线([A-8])」 | `suspended` 不再意味着断开;「已连接」判据照旧成立;注入轨全部挂起且总线听得见时总线交叉回原声 | `OutputSession::stalledMask()`、`OutputProcessor::renderSpan`(`allStalled` / `stalledRawLatched_`) |

**不属于契约、因此没写进冻结文档的**:`OutputSession::stalledMask()`(进程内 [M]→[A] 原子量)、
`OutputProcessor` 的「回原声」闩与静音门限常数(进程内)。

## 兼容性影响

- **布局 / abi / 段名零变化**;Input 侧代码零变化。`connected_mask` 仍是 Output 写、Input 读的同一个位,
  Input 的 J12 仲裁(心跳新鲜 ∧ 本位在 ⇒ 静音)逐字不变 —— 变的只是 Output 什么时候清这个位。
- **新旧互通**:新 Output + 旧 Input、旧 Output + 新 Input 都按各自的 Output 规则工作,不拒连、不半兼容
  (Input 侧没有改动)。
- **用户可感知的变化**(见 CHANGELOG「修复」与「⚠️ 契约变更」):
  - 宿主在静音段 / 无 region 处暂停调用 Input 之后,句首不再被吃掉、长静音之后也不再先出一段原声;离线导出同样修正
    (模拟宿主实测:同步各格句首 0 帧丢失;写方领先的各格最多丢一个写方块 ≈ 21 ms,与 A-5 的残余一致);
  - 宿主挂起期间 Input 面板仍显示「已连接」,Output 面板该轨显示「暂无数据(挂起)」(U3(c));
  - 单轨工程里旁路唯一一条 Input:约 0.5 秒后经 80 ms 交叉回到原声(与修前听感相同);多轨工程里旁路一条:
    该轨退出混音(与修前相同);
  - 已知残留:旁路超过约 5.5 秒后再解除旁路,修前 Input 已切直通、解除时按 J32 握手平滑交接;现在 Input 一直在静音档,
    解除旁路那一刻它的原声直接停掉,Output 的混音按 80 ms 交叉淡入(修前旁路短于 5.5 秒时本来就是这样)。
- 冻结项自查:`node scripts/check-ipc-doc-parity.mjs --strict-missing` 与 `node scripts/check-bridge-parity.mjs` 退 0;
  golden 与宪法副本未动。

## 审批

挂 `status/frozen-contract`。实质:用户 2026-10-03 批准 A 线计划(§3 措辞补充:IPC_CONTRACT §4 `connected_mask` 注、
SCVB_CONTRACT §2.3 `suspended` 语义随之调整);语义细节按用户裁定 U3(a) / U3(b)-ii / U3(c)。本文件与代码同 PR 落地,
用户在 A 线里程碑 PR(A→dev)里统一过目措辞。
