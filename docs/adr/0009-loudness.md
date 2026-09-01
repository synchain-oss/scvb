# ADR-009 · 响度度量

> 状态: 冻结
> 最后更新: 2026-08-31(对应 constitution/ADR.md v2.2)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-009 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- 段响度 = 段内 ungated K-weighted 积分响度(EBU Tech 3341:M/S 本身不 gating)
- K-weighting 自研(两个 biquad,系数来自 BS.1770),libebur128(MIT)作为测试对拍参考,不进运行时依赖
- 所有平衡计算在线性能量域做(O(N) 算术)

## 修订历史(摘自 constitution/ADR.md)

- **[J18→ADR-009]** libebur128 对拍口径:M(400ms)+ungated,禁用 global gated。
- **[J95②→ADR-009 澄清]**(v2.2,2026-08-31)区分**上报段响度**与**平衡归一化基准**两件事:
  ① 正文第一条「段响度 = 段内 ungated K-weighted 积分响度」界定的是**上报口径 L_seg**,
     不随 `analysis.loudness_mode` 变化(J18 对拍口径同样不变);
  ② **平衡归一化基准** z 由 `analysis.loudness_mode` 选择 —— `kw_integrated`(默认)
     = `mean(kw)`,与本次修订前逐位相同;`rms` = `(mean(√kw))²`;`peak_dbfs` = `max(peak)²`。
     三档**均为线性能量量**,故正文第三条「所有平衡计算在线性能量域做(O(N) 算术)」
     **不受影响、继续完整适用**。
  依据:SL-252 定谳 —— 设计稿「第二响度指标」条注明本设置「影响分析时的段间响度归一化
  基准;改后需重分析」,而实现从未把它接进引擎(`PipelineConfig` 无该字段),三档结果恒等。
  用户 2026-08-31 拍板走 A 案(切档→重分析→pan/volDb 真变;默认档逐位不变)。