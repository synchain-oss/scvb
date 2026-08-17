# 契约变更说明 —— 20260817-t06-ipc-layout-golden

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [x] **tests/golden/(golden 快照)—— 新增 ipc-layout.txt(IPC 段布局 golden,T06)**
- [ ] docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)

## 变更内容

T06(PR #36,已合入)新建 `tests/golden/ipc-layout.txt`:IPC 共享内存段布局 golden(abi=1,ipc-contract-v0 v1.5)——RegistryHeader/InputSlot×15/OutputSlot/AudioRingHeader/FeatHeader/FeatFrame/OutputGlobalInfo/CtrlRecord 全字段偏移/尺寸/对齐 + magic 0x42564353 + 段名 g1/g8 + 预算常量。本记录为 #36 合并后的补登记(branch-gate 的 frozen-contract path guard 含 tests/golden/,收口 PR 需要同 PR 变更文档)。

## 兼容性影响

无(补登记;#36 已合入,布局未变)。

## 审批

- [x] 用户授权:2026-08-17「自动继续后续全部任务」+「comment 都说没问题就可以合」;#36 已按此合并。
- [x] DeepSeek native 评审:#36 三 bot 零红旗(2026-08-17)。
