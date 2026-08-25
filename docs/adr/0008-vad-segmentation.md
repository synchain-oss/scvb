# ADR-008 · VAD 与分段

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-008 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- v1:K-weighted 能量域双阈值 + 滞回 + hangover + 前后 padding,默认宁多勿少,阈值/灵敏度用户可调可实时预览(因为存的是连续特征)
- 分段:能量谷检测 + 最小段长 + 换气容忍;用户手动改边界后该段标记 manual,自动重分析不覆盖 manual 边界(除非用户显式要求重识别)
- v2 升级路径:Silero VAD(ONNX,MIT)作精确模式,不进 v1

## 修订历史(摘自 constitution/ADR.md)

- **[J34→ADR-008]** segments 语义字段改 origin+locked(见 params v1.1)。