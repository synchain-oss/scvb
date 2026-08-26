# ADR-006 · 自动化写入

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-006 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- gesture 三段式(beginChangeGesture/setValueNotifyingHost/endChangeGesture),**只在消息线程**(25Hz Timer,v5.2 前为 50Hz);音频线程只发布 playhead 快照(SPSC)
- 推荐用户用 DAW 的 Write/Latch;文档标注各 DAW 已知坑(Cubase 车道位置、REAPER 关 GUI 不写、Pro Tools 循环只录第一遍等,见 research/08)
- host echo 防回环:写入期间忽略参数回调对引擎的影响(引擎为源);写入结束后参数回归 follow 语义

## 修订历史(摘自 constitution/ADR.md)

- **[J08→ADR-005/006 观察项]** S2 显式观察「输出开关 ON + 宿主 Read 模式」行为;异常则增补非 gesture 试听档(纯 state 字段,不动 81 参数)。