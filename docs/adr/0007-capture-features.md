# ADR-007 · 采集:存特征不存音频

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-007 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- 特征流(per-hop,hop=10ms@48k):K-weighted mean-square + peak;VAD 存连续后验(阈值离线可调,无需重采集)
- 采集开关 ON 且 transport 播放时才写入;按时间线寻址合并(重播覆盖同区间,新数据优先)
- 持久化分层:分段/区间/2 版曲线+配置(≈几百 KB)进 state;特征流压缩后默认内嵌 state,超 8MB 转 sidecar 文件(sessionGUID 自生成,存于 state;sidecar 放系统应用数据目录)
- 局部重采集/重分析:按(轨道 × 时间区间)为单位失效与重算,**绝不触碰其他区间已有结果**;分析结果数据结构按轨道独立分段列表组织
