# 契约变更说明 —— 20261011-a6-misalign-positive-presence

> **状态:随 A 线 A-6(E1b 正向在场与告警)同 PR 落地,挂 `status/frozen-contract`。**
> 变更实质已由用户在 2026-10-03 批准 A 线计划时一并批准(计划 §3「冻结契约 → 措辞补充」:SCVB_CONTRACT §2.3
> `misalignCount` 新增成因「已注入但持续读不到」,以及 IPC_CONTRACT §2「读」;同节第 4 条:E1b 告警沿用现有
> `misalignCount` 与「路由失准」横幅,零 UI 改动 —— 即设计稿 U4 的推荐项。批准计划即批准实质,措辞随代码同 PR
> 落地,用户在 A 线里程碑 PR(A→dev)里统一过目)。本文件只记**措辞**与它对应的实现落点。
> `suspended` 语义的调整归 A-8(`docs/contract-changes/20261011-a8-suspend-not-offline.md`),本文件不涉及。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 只改 §2「读」那一条下 [A-5] 子条目里的最后一项(「失准计数口径」)
      的**说明文字**:原句保留,后接一句 [A-6] 补充。段名、字段、偏移、大小、对齐、abi、`AudioRingHeader` /
      `OutputGlobalInfo` 代码块**零变化**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §2.3 `scvb.conn` 表新增一行「失准成因与恢复([A-6])」:`misalignCount` 的
      **取数口径**补一个成因、归零条件补一条。函数名 / 签名形态 / 事件名 / 载荷字段 / 字段类型**零变化**,
      `node scripts/check-bridge-parity.mjs` 的名字集合不变。
- [ ] tests/golden/(golden 快照)—— **不动**。

`docs/constitution/` 下的宪法只读副本不动(SL-505 先例:副本改动走修宪流程)。

## 变更内容

### 为什么要补

A 线 A-4(PR #376)在调度模拟器上证实了缺陷 **H3**:读方从一代开始就一直领先写头时(宿主先算装 Output 的总线、
人声轨没送进这条总线、Aux 位置没做延迟补偿一类),这一轨**无声且不报警** —— 读方在「本代还没读到过数据」
(primed 之前)的失败一律不计失准,这条规则原本只为换代交接、冷启动的短暂读不到而设,却也把「一直读不到」
吞掉了。实测:定位后持续先算 Output / 读方位置超前 4096,3 轨 136/136 帧缺席、条件持续 3 s 内失准一拍都没亮。

### SCVB_CONTRACT §2.3(`misalignCount` 的取数口径)

| 物理成因 | 变更前 | 变更后 |
|---|---|---|
| 写方套圈(要读的数据已被覆盖) | 每块记一笔 | 不变 |
| 本代已读到过数据之后,写方在推进却读不到 | 每块记一笔 | 不变 |
| **已注入但持续读不到**:本代还没读到过数据、写方在推进,同一段连续读失败超过宽限 | **不记**(H3:无声且不报警) | **超过宽限之后每块记一笔** |
| 换代交接 / 冷启动 / 先算 Output 的那几块,短于宽限 | 不记 | 不变(不记) |
| 写头停着(宿主挂起 Input / 用户 bypass) | 不记(归 `suspended`) | 不变 |
| 段头 `channels` 与读方快照不一致(布局正在改写、读方还没换绑) | 不记 | 不变(不记,也不算进宽限) |

宽限 = max(读方时间线 200 ms, 2 × 实测提前量上界 + 一块),**按样本计**(离线导出跑在墙钟前面多少倍都是同一条
规则);读方卡在已确认的本代起点之下、上一代尾段也接不住的那一形(H1 的安全网)再加一个提前量上界。取值依据:
A-4 实测「只在交接前后先算 Output」的已注入却读不到 < 150 ms(N2a),必须压在宽限之内;LS-14 / N2b 的告警
目标 ≤ 0.5 s。

**归零条件补一条**:原为「连续 1 s(`kMisalignRecoverMs`)无新缺口 ∧ 数据在推进」,补「∧ 该轨自最近一笔缺口
以来确实读出过数据(正向在场)」。理由:失准会把该轨踢出注入集,读方不再读它、缺口自然停止增长 —— 只看
「无新缺口」,持续读不到的轨会以约 1 s 的周期假恢复(1 s 后撤下横幅、放回注入集、宽限走完又亮;与 v4 P0-2
同族的假恢复)。「读出过」按块算、不按 [M] 拍算:缺口之后同一拍里紧接着读到的那一块也算,所以普通的瞬时失准
(这一代已经读到过数据、偶然缺了一块又接着读到)撤下的时机与修前相同。兜底:该轨在注入集里满 1 s 而 Output 一次都没读它(面板上关了这一轨,混音时被启用位挡掉;
或宿主没在调用 Output)时,「读出过数据」无从谈起,退回原条件 —— 免得一条用户自己关掉的轨把横幅永久钉住。

ctrl 段 `OutputGlobalInfo.gap_count[15]`(进程寿命累计)与 native `gapCount()` 同口径累计这一成因。

### IPC_CONTRACT §2「读」

A-5 补的子条目「失准计数口径不变:只在本代已读到过数据之后、写方仍在推进却读不到或被套圈时计;换代交接期与
写头停滞不计」在 A-6 之后不再完整。原句保留,后接一句 [A-6] 补充(内容同上表第 3、6 行与宽限的定义)。

### 实现落点

| 措辞 | 实现 |
|---|---|
| 已注入但持续读不到 → 计缺口 | `ShmRingMixSource::countFailure` 末尾:primed 之前的一段连续读失败(`unreadRun_` / `unreadFrom_`),(a) 读方在锚点之上、写头没写到块尾 → 宽限 `unreadGrace()`;(b) 读方在锚点之下、上一代窗接不住 → 提前量上界 + 宽限;超过且写方在推进 ⇒ `gapCount_` +1(另计 `persistentFailCount_`) |
| 宽限按样本、读方时间线 | `unreadGrace()`:`max(sr × kUnreadGraceMs / 1000, 2 × leadHiRef() + n)`;这段失败在读成功 / 换代 / 重绑 / 读方跳变 / 读方漏读块时重新起算 |
| 写头停着不计 | 只在 `producerAdvancing` 的块计(P1-7 判别不变) |
| 段头 `channels` 不一致不计、不算进宽限 | `read()` 里该分支:单独计 `layoutMismatchCount_`(A-5 时混在 `handoverLossCount_` 里),起点往后挪一块(不续长),不重新起算(不掩盖) |
| 归零要求正向在场 | `OutputSession::evaluateChannels` 的恢复判定多一个 `present`;`OutputSession::positivePresence`:`ShmRingMixSource::readOkSinceLastGap()`(读方在计缺口的同一处 `countGap()` 清掉、读成功时置上,精确到块),或在注入集里满 `kUnreadInjectedFallbackMs` 而 `readAttemptCount` 一次没动 |
| online 公式 | **不变**(只经由 `misaligned` 受影响:告警期间该轨照旧不在线、不注入) |

**不属于契约、因此没写进两份冻结文档的**:四个读方进程内计数的定义(A-5)、新增的 `layoutMismatchCount` /
`persistentFailCount` / `readAttemptCount` 三个进程内计数、`OutputSession::readOkCount` 测试访问器。

## 兼容性影响

- **布局 / abi / 段名零变化**;写方(Input)行为零变化;桥面字段、类型、事件名零变化;UI 零改动(沿用
  `misalignCount > 0` 与「路由失准」横幅、逐行 ⚠)。新旧版本 Input 与 Output 互通不受影响。
- 用户可感知的变化:已连接、但 Output 一直读不到数据的人声轨,以前静音且不报警,现在约 0.5 秒内亮「路由失准」,
  并持续到读到数据为止(常见原因:人声轨没有送进装 Output 的总线)。见 CHANGELOG「修复」与「⚠️ 契约变更」。
- `misalignCount` / `gap_count` 的数值在上述情形下会**变大**(以前恒 0);其余情形不变。
- 零误报:调度模拟器 LS-1/2/3/4(含 12 s 圈 × Δ=48000)/5/6/7/8/9/10/12/13 全部格、harness 的 L-6a / L6 定位 /
  P1-7 / P0-2 / L-6b / N2a / SL-254 ①–④ 失准恒 0(用例见 PR)。
- 冻结项自查:`node scripts/check-ipc-doc-parity.mjs --strict-missing` 与 `node scripts/check-bridge-parity.mjs`
  退 0;golden 与宪法副本未动。

## 审批

挂 `status/frozen-contract`。实质:用户 2026-10-03 批准 A 线计划(§3 措辞补充:SCVB_CONTRACT §2.3
`misalignCount` 新增成因「已注入但持续读不到」、IPC_CONTRACT §2「读」;E1b 告警沿用 `misalignCount` 与
「路由失准」横幅,零 UI 改动);本文件与代码同 PR 落地,用户在 A 线里程碑 PR(A→dev)里统一过目措辞。
