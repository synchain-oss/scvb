# ADR-001 · 插件形态:两个插件目标 + 共享核心库

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-001 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

一个仓库,CMake 三个目标:`scvb_core`(静态库:DSP/分析/IPC/状态/版本引擎,可离线单测)、`SCVB Input`(VST3)、`SCVB Output`(VST3)。JUCE 一个 target 一个插件,不做单插件双模式(用户会同时开十几个实例,双模式易误操作)。
- PLUGIN_CODE:Input=`Scvi`,Output=`Scvo`(Snb1 已被 Bridge 占用;厂商码统一 `Snch`)
- BUNDLE_ID:`com.synchain.scvb.input` / `com.synchain.scvb.output`
- PRODUCT_NAME:"SCVB Input" / "SCVB Output"(显示名带 Synchain 由厂商列免)…最终名在 05/UI 文档定,插件码/bundle id 冻结

## 修订历史(摘自 constitution/ADR.md)

- **[J22→ADR-001 澄清]** 「三目标」指三主目标;juce_add_binary_data 资源目标与 scvb_tests 等辅助目标不违宪。