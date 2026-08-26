# 契约变更说明 —— 20260825-v5-misalign-stall-semantics

> **2026-08-26 更新(v5.3,统筹裁定「丙案」)**:本文档原先只改 `misalignCount` 的**取数口径**;
> 真机两轮下来证明那条路走不通 —— 见文末「第二次修订」一节。现改为**新增一个字段**把
> 「挂起」与「失准」彻底分开,`misalignCount` 回到只数真失准。两节都保留:前一节是过程记录,
> **以「第二次修订」为准**。

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

---

# 第二次修订(2026-08-26,v5.3)—— 以本节为准

## 变更内容

**`scvb.conn.channels[]` 新增布尔字段 `suspended`**;`misalignCount` 回到「**只数真失准**」。

| 物理成因 | 上桥字段 | UI 呈现 |
|---|---|---|
| 写方套圈(数据已被覆盖,读方跟丢) | `misalignCount`(> 0 即当前失准) | 红/琥珀告警 ⚠(不变) |
| 写头停着(宿主在无信号段挂起 Input / 用户 bypass / 轨未激活) | **`suspended`**(新增) | **中性提示**(灰蓝、不脉冲、不带 ⚠),词条 `tracks.suspended` |

`suspended` 的判定(`OutputSession::evaluateChannels`)= 写头冻结 ≥ `kSuspendStallMs`(500ms)
∧ 走带在跑 ∧ 本段停滞期内确有饿读。写头一恢复推进即刻撤下 —— 它描述的是「此刻在不在出数据」,
没有失准那种 1s 恢复窗迟滞。

## 为什么改(前两次都没走对)

`misalignCount` 一直被当成「这条轨有没有问题」的**唯一**位,而它实际要表达**三件性质不同的事**:
① 读方跟丢(真失准)② 宿主在乐句间隙挂起 Input(完全正常)③ 用户自己 bypass 了 Input(用户
自己干的)。一个位表达不了三件事,于是每换一次口径就顾此失彼:

- **v4 P0-2**:修「bypass 期间警告不得假恢复」→ 让停流持续刷新失准态;
- **v5 P1-7**:静音段每个乐句间隙都闪一次失准 → 改成「短停不报、坐实才报」;
- **v5.1 P1-E**:长静音(间奏、尾奏)照样落在「坐实」那一侧,**仍然误报**。

而②与③在 Output 侧的**每一个可观察量上完全相同** —— 心跳都在(Input 的 Timer 照跑)、
写头都冻、都有饿读。**没有任何信号能把它们分开**,所以这不是判据没写好,是位不够用。
统筹裁定:给它们各自的位(丙案)。

## 兼容性影响

- **新增字段,不改既有字段的类型与语义**。旧 UI 不读 `suspended` 时行为退化为「挂起的轨看起来
  只是没有电平」——不会误显示成故障(这正是修复目标)。
- `misalignCount` 的数值会**变小**:此前含停流笔数,现在只含真失准。「当前是否失准」这个
  UI 判据(`misalignCount > 0`)语义不变,只是不再被挂起污染。
- **「不得假恢复」的保证不受影响**:它的落点一直是 `injectMask`(数据没在推进的轨不进总线),
  而 `suspended` 现在把这件事**明确说出来**,比从前靠一个失准计数暗示要准确。
- 无 state / 自动化 / IPC 布局改动。

## 回归

- `HOST L-6b:bypass 断流 → 挂起态亮起;恢复 → 自行撤下`(断言面由 misalignCount 改为 suspended,
  并加断言「全程不是失准」);
- `HOST P0-2:持续 bypass 期间不得假恢复`(每一轮都断言 suspended 仍亮 **且** 该轨已退出注入集);
- `HOST P1-7:宿主在静音段挂起 Input,短暂停流不报失准`(不变);
- `挂起(suspended)与失准分开呈现:bypass 走挂起,不走失准`(单测,原
  `misalignCountRecent:失准发作后恢复健康 → 归零` 改写)。

## 审批

挂 `status/frozen-contract` 标签;用户明确批准后合入。§2.3 正文由统筹在批准后回填
(与 `20260825-t37-r3-misalign-semantics.md` 同流程)。
