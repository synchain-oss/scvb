# 契约变更说明 —— 20260825-v5-misalign-stall-semantics

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] docs/SCVB_CONTRACT.md §2.3 `scvb.conn.channels[].misalignCount` 的**取数口径**
      (字段名/类型/UI 判据均不变;正文待批准后由统筹回填)

## 变更内容

`misalignCount` 仍是「本轨**本次故障发作**内的故障笔数」,UI 判据仍是无状态的
`misalignCount > 0`,归零条件仍是「连续 1s(`kMisalignRecoverMs`)无新故障 ∧ 数据在推进」。
**变的是「哪些物理事件算一笔」**:

| 物理成因 | 变更前 | 变更后 |
|---|---|---|
| 写方套圈(`w - t0 > ring_frames`,要读的数据已被覆盖) | 每块记一笔 | 不变,每块记一笔 |
| 写头停滞 **< 500ms**(`kSuspendStallMs`) | 每块记一笔 | **不记** |
| 写头停滞 **≥ 500ms**(坐实 CH_SUSPENDED) | 每块记一笔 | **每次发作记一笔**(不按块累加) |

判据配套两条(实现见 `ShmRingMixSource::read` 与 `OutputSession::evaluateChannels`):

- 停流记账要求**确有饿读**(Output 真的去要过写方没写到的数据)。走带停止时 15 条轨的写头
  本来就该冻着,而 Output 读的是冻住的 `t0`(数据早已写过),一次饿读都不会有 —— 不报。
- 停流记账还要求**走带在跑**(`OutputSession::setTransportPlaying`,每拍由 playhead 快照喂入)。

## 为什么改

v5 真机(Cubase 15)P1-7:「音频在但 −inf 的区域,两个 Input 偶发短暂失准后自愈」。

成因链:宿主的「无信号时挂起 VST3 处理」在静音段停调 Input 的 `processBlock` → `write_head`
冻结 → Output 侧 `covered` 判据在 CH_SUSPENDED 的 500ms 判定窗内**每块**失败并各记一笔缺口 →
失准横幅亮起 → 本轨转 suspended 退出注入集、`read()` 不再被调、缺口停止增长 → 1s 恢复窗过后
自行撤下。于是**每个静音边界都闪一次失准**。

`covered` 失败其实有两个物理上**相反**的成因:写方没写到(停滞)与写方套圈(真失准)。
`OutputSession::evaluateChannels` 里 CH_SUSPENDED 的行注一直写着「宿主跳过该轨处理,**不计失准**」——
本次变更是让实现追上这条早已写明的口径,而不是新立规矩。

## 兼容性影响

- **UI**:无字段增删、无类型变化、无新词条。唯一可观察差异 = bypass 一个 Input 后,失准 ⚠
  从「立刻亮」变成「约 0.5s 后亮」(那 0.5s 正是用来区分「宿主静音挂起」与「真的断流」的)。
- **既有工程 / DAW 自动化**:零影响(本字段不落 state、不落自动化)。
- **诊断口径**:ctrl 段 `OutputGlobalInfo.gap_count[15]`(进程寿命累计)语义不变 ——
  但它现在只累计**真失准**(套圈),不再把宿主的静音挂起算进去,数值会比从前小。
  这是修正而非丢数:停流有独立记账(`OutputSession` 的 `stallCount_`)。
- **回归**:
  - `HOST P1-7:宿主在静音段挂起 Input,短暂停流不报失准`(新增);
  - `HOST L-6b` / `HOST P0-2`(持续 bypass 仍报警、仍不假恢复)断言面不变,只把「立刻」改成「等到坐实」;
  - `ShmRingMixSource 写头停滞 → 记饿读,不记失准` / `ShmRingMixSource 写方套圈 → 真失准计数`(单测拆成两条,对照两种成因);
  - `misalignCountRecent:失准发作后恢复健康 → 归零` 按新口径重写。
  逐条做过反向验证(改回旧写法即红,数值可解释:12 块 output-only → 旧实现恰好记 12 笔)。

## 审批

挂 `status/frozen-contract` 标签;用户明确批准后合入。§2.3 正文由统筹在批准后回填
(与 `20260825-t37-r3-misalign-semantics.md` 同流程)。
