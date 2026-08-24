# ADR-005 · 版本系统:内部曲线是真身,参数是打印头

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-005 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- 分析结果 = 每版本每轨的时间线分段曲线(pan/vol),存 Output state
- 90 个轨道参数是可自动化面;其中 30 条(pan/vol)兼为"打印头"(J63),width 类 host 恒权威:输出开关 ON 时引擎按播放位置把当前版本曲线值经 gesture 写到参数(供 DAW write 录制);OFF 时参数忠实跟随 host(读 DAW 自动化)
- version 复制 = state 内曲线+配置复制,零 gesture、零自动化污染
- `version_active` 为非自动化 state(否则会被 write pass 自录进自动化)
- DSP 取值仲裁:输出开关 ON → 引擎值直接进 DSP(参数只是对外打印);OFF → host 参数值进 DSP

## 修订历史(摘自 constitution/ADR.md)

- **[J08→ADR-005/006 观察项]** S2 显式观察「输出开关 ON + 宿主 Read 模式」行为;异常则增补非 gesture 试听档(纯 state 字段,不动 81 参数)。