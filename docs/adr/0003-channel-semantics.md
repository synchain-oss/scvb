# ADR-003 · 声道语义:v1 支持 mono 与 stereo 源(v2.0/J57 改写)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-003 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- Input 检测轨道布局:mono 轨捕获单通道;**stereo 轨捕获/转发双通道**(IPC 环 interleaved,channels=1|2)
- Output 总线固定 stereo;mono 源经 equal-power pan 摆入 L/R;**stereo 源用 dual-pan+width 模型**(L/R 各自 equal-power pan:pan 参数=弧中心,每轨 width 参数=张开度,width=0 收成 mono;**不用 M/S 拉宽**,避极性反转)
- 特征提取按 BS.1770 多通道求和;stereo 轨参与音量平衡按 J64 近似(求和能量+摆位理论分布)

## 修订历史(摘自 constitution/ADR.md)

- **[J57→ADR-003 改写]** v1 **支持立体声源**:Input 检测 stereo 轨→捕获/转发双通道(IPC 环 channels=1|2);Output 对 stereo 源用 **dual-pan+width** 模型(L/R 各自 equal-power pan,pan 参数=弧中心,每轨 width 参数=张开度,width=0 收成 mono;**不用 M/S 拉宽**,避极性反转);特征提取按 BS.1770 多通道求和。「真立体声延后 v2」的原限制作废。