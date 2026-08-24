# ADR-013 · 分支与任务命名(三仓库)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-013 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- feature 主支线:`feature/v1`;子支线:`feat/<TASK-ID>-<slug>`(如 `feat/T03-ipc-ring`)
- 任务编号 `T01…`,由 07-execution-plan 统一分配;Bridge 抽取仓任务 `B01…`,CLI 仓 `C01…`,主仓善后 `M01…`
- commit 规范沿用 `type(scope): 中文描述`

## 修订历史(摘自 constitution/ADR.md)

- **[J17→ADR-013]** 任务命名空间增 **O**(org/共享基建);编号分配权在 07-execution-plan(已行使,其映射表为准)。
- **[J13→ADR-013]** feature 主支线命名按仓库语义:SCVB=`feature/v1`,Bridge/CLI=`feature/extraction`。
- **[J39→ADR-013 附注]** required checks 宪法化:dev required=构建+测试+人审;bot 非 required(U8 可改)。**[J49 口径限定(2026-08-11)]**:「人审」指 D2 的用户人工审核与 merge 动作(由 `required_conversation_resolution: true` + 用户亲自 merge 承载),**不得**配置 `required_approving_review_count`——GitHub 不允许 PR 作者 approve 自己的 PR,单人 org 下会令所有 PR 永久无法合并(06 §7.2 的「关键坑」)。将来有第二位维护者时再开启 approval 强制。
- **[J42→ADR-013]** B/C 线主支线 `feature/extraction` 在各线首任务(仓库脚手架)创建,后续任务 base 于它。