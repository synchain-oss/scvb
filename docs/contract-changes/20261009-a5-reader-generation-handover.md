# 契约变更说明 —— 20261009-a5-reader-generation-handover

> **状态:随 A 线 A-5(E1 读方换代处理)同 PR 落地,挂 `status/frozen-contract`。**
> 变更实质已由用户在 2026-10-03 批准 A 线计划时一并批准(计划 §3「冻结契约 → 措辞补充」第一条:
> IPC_CONTRACT §2「读」;批准计划即批准实质,措辞随代码同 PR 落地,用户在 A 线里程碑 PR 里过目)。
> 本文件只记**措辞**与它对应的实现落点。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 只改 §2 列表里「读」那一条的**说明文字**(原句保留,
      后接「[A-5] 措辞补充」五个子条目)。段名、字段、偏移、大小、对齐、abi、`AudioRingHeader` 代码块
      **零变化**(`node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0)。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**(失准计数的新成因归 A-6,措辞随 A-6 落地)。
- [ ] tests/golden/(golden 快照)—— **不动**。

`docs/constitution/` 下的宪法只读副本不动(SL-505 先例:副本改动走修宪流程)。

## 变更内容

改前 §2「读」只有一句:

> 读(Output 音频线程):按自身块的 [t0,t1) 读取;区间未被覆盖(write_head 落后或 epoch 不符)→ 该轨该块静音 + 失准计数(UI 警告)

这句在 A-5 之后有两处不再准确:

1. 「epoch 不符 → 静音」—— A-5 起,epoch 只差 1 时**上一代**已确认写过、新一代不可能覆盖到的尾段继续可读
   (设计稿 §3.3 不变式 I3)。这正是修 H1 的手段:写方领先时循环回绕,读方还在播的上一圈尾段不再整段丢掉。
2. 「区间未被覆盖」—— A-5 起「覆盖」不再只看写头,还要看读方能不能确认**本代从哪里开始写**:写方单独换代、
   读方自己没跳时(H4:写方领先时从停调恢复),写头之下的部分可能是没写过的旧环槽,读方只从确认过的位置起读。

原句保留(它对读方的「静音 + 失准」口径仍然成立),后接五个子条目:

| 子条目 | 说的是什么 | 实现落点 |
| --- | --- | --- |
| 「覆盖」按代判 | 只交出每一帧都能确认是写方为该位置写下的块(哪一代都行),否则整块静音 | `ShmRingMixSource::read` → `certify`(当前窗 ∪ 上一代窗逐帧认领) |
| 当前一代 | 从能确认的本代起点(≥ 写方本代真实第一帧)到 `write_head`;确认不了起点时只从已确认是本代的写头起读 | `tryAnchor`:同步规则 / 推迟锚定 / 锁步写方单独换代 / 保守规则 |
| 上一代未被覆盖的尾段可读 | epoch 只差 1 时,上一代已确认写过、新一代写不到同一环槽的那一段可读;`write_head` 归 0(几何改写)时上一代一律不读 | `newGeneration` 降级当前窗;`prevAliasFree` 别名判据;`firstObservation` / `sameGeneration` 的归零清窗 |
| 段头 `channels` 比对 | 与读方几何快照不一致 → 该块不读;段头值只比对、不寻址 | `read` 里 volatile 读 `header->channels`(对齐 32 位单次访问原子的说明见代码注释) |
| 失准口径不变 | 本代已读到过数据之后、写方仍在推进却读不到或被套圈才计;交接期与写头停滞不计 | `countFailure`(旧实现 P1-7 判别逐字保留) |

**不属于契约、因此没写进 IPC_CONTRACT 的**:`IMixSource::read` 新增的「读方自身时间线代号」参数(进程内接口)、
读方内部的提前量测量与四个进程内计数(`readOk` / `unprimedFail` / `stuckBelowAnchor` / `handoverLoss`,
A-5 只计数、不报警,告警口径归 A-6 并随 A-6 补 SCVB_CONTRACT §2.3 的措辞)。

## 兼容性影响

- **布局 / abi / 段名零变化**;写方(Input)行为零变化。新旧版本 Input 与 Output 互通不受影响 ——
  A-5 只改 Output 侧怎么**解读**同一组 epoch / 写头。
- 用户可感知的变化只在读方:写方领先(某条轨被宿主提前处理)时,循环回绕、播放中回跳不再丢整圈;
  从静音段 / 无 region 恢复时不再在句首前混进旧音频(换成句首最多少一个写方突发块,设计稿 §8 第 4 条)。
  见 CHANGELOG「修复」。
- 冻结项自查:`node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0;golden 与宪法副本未动。

## 审批

挂 `status/frozen-contract`。实质:用户 2026-10-03 批准 A 线计划(§3 措辞补充:IPC_CONTRACT §2「读」);
本文件与代码同 PR 落地,用户在 A 线里程碑 PR(A→dev)里统一过目措辞。
