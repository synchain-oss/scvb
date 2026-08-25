# ADR-009 · 响度度量

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-009 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- 段响度 = 段内 ungated K-weighted 积分响度(EBU Tech 3341:M/S 本身不 gating)
- K-weighting 自研(两个 biquad,系数来自 BS.1770),libebur128(MIT)作为测试对拍参考,不进运行时依赖
- 所有平衡计算在线性能量域做(O(N) 算术)

## 修订历史(摘自 constitution/ADR.md)

- **[J18→ADR-009]** libebur128 对拍口径:M(400ms)+ungated,禁用 global gated。