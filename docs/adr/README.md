# docs/adr/

本目录是架构决策记录(ADR)的仓内逐条拆分形态,在 T39a 转正时由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)按 ADR-001..014 拆成 14 个独立文件:

| 文件 | 决策 |
|---|---|
| `0001-plugin-form.md` | ADR-001 插件形态:两个插件目标 + 共享核心库 |
| `0002-routing-architecture.md` | ADR-002 路由架构 |
| `0003-channel-semantics.md` | ADR-003 声道语义 |
| `0004-parameter-layout.md` | ADR-004 参数布局 |
| `0005-version-system.md` | ADR-005 版本系统 |
| `0006-automation-write.md` | ADR-006 自动化写入 |
| `0007-capture-features.md` | ADR-007 采集:存特征不存音频 |
| `0008-vad-segmentation.md` | ADR-008 VAD 与分段 |
| `0009-loudness.md` | ADR-009 响度度量 |
| `0010-pan-math.md` | ADR-010 Pan 数学 |
| `0011-tech-stack-quality-gates.md` | ADR-011 技术栈与质量门 |
| `0012-repo-structure.md` | ADR-012 仓库结构(骨架) |
| `0013-branch-task-naming.md` | ADR-013 分支与任务命名 |
| `0014-pre-spikes.md` | ADR-014 前置 spike |

**唯一权威文本仍是 `docs/constitution/ADR.md` 只读副本**(review bot prompt 的「审查前先读」清单既列 docs/adr/ 也列 docs/constitution/ADR.md)。本目录各文件头部「真源」行即指向它;修订必须走修宪流程(改 constitution 原文 → 升版本号 → 同步只读副本 → 重新拆本目录),不得单独改某个拆分文件。

注:本目录与三份冻结契约文档(`docs/PARAMETERS.md` / `docs/IPC_CONTRACT.md` / `docs/STATE_SCHEMA.md`)的权威关系不同:后者以宪法副本为**修订源**、转正文档为**实现/审查基准**;本目录的 ADR 拆分文件是宪法原文的**派生**,权威仍在宪法副本。

跨条目的宪法元信息(修宪流程、版本历史、J61 连锁修订等)保留在 `docs/constitution/ADR.md`,不随拆分复制。
