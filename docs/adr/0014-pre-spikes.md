# ADR-014 · 前置 spike(实施最前,fail-fast,全部在 feature/v1 下)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-014 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- S1 路由可靠性:最小 Input/Output 对,Reaper/Cubase/Live/Studio One × 实时/离线渲染,验证时间线对齐与静音通路排序假设
- S2 自动化写入:123 参数骨架 + gesture 写入,同矩阵验证 write/latch 录制与回读
- S3 WebView 承载力:一页 30+ 控件 + 曲线画布 + 波形渲染的帧率/内存
- S4 状态体量:满配 state(2 版曲线+特征内嵌)save/load 完整性与速度
任一 spike 失败 → 停下修正架构再继续(S1 失败的备选:预选 F2「仅分析+建议表 CSV 导出+Input 就地 gain」,详见 11-risks §4 与裁决 J25)

## 修订历史(摘自 constitution/ADR.md)

- **[J14→ADR-011/014]** 增补:pluginval 双插件对偶场景「对端缺席时不阻塞不崩溃」为 S1 测项与常规 CI 关注点。
- **[J15→ADR-014]** 停线粒度:S1 失败=停 T 线并 48h 内完成兜底选型;S2 失败=按 DAW 分 Tier 降级(Cubase 单独决策);S3/S4 失败=局部停(UI/state 域)。
- **[J25→ADR-014 清理]** 删除 v0 中「要求用户手动对齐」备选表述(无可操作含义)。