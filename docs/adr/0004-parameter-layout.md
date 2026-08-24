# ADR-004 · 参数布局:自动化参数全部在 Output(冻结顺序;v2.0/J59 改写)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-004 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- 自动化参数共 **123 个**:全局 3(width/ms_balance/lead_select)+ 15 轨 × 2 版本 × (Pan+Vol+Width+Freeze)= 120。详见 params-v0.md v2.1,ID/顺序冻结点=首个公开 rc(J21),全部 versionHint=1
- 宿主可见 124(+bypass,J02);Ableton Live 128 上限余 **4(J65 后预算封顶)**;**绝对禁止再加自动化参数**,新需求一律走 state
- 引擎 write 仅打印 30 条(15 轨×pan/vol,J63);width/ms_balance/lead_select 可被用户自动化但引擎不打印(host 恒权威)
- Input 无自动化参数;channel id / 输入端配置全在 state
- 曲线编辑器、阈值、分段灵敏度、优先级、主唱锁、配对、每轨 auto 开关、采集/输出开关、version 选择:全部 state(非自动化)
- 配置类数据的唯一真源在 **Output state**(大脑);Input state 只存 channel id + UI 偏好。优先级等即使在 Input UI 上显示/可调,实际读写的是 Output 的值(经控制面 IPC)

## 修订历史(摘自 constitution/ADR.md)

- **[J59→ADR-004 改写]** 参数布局 v2.0:**2 版本 × 15 轨 × (Pan/Vol/Width) + 全局 width/ms_balance/lead_select = 93 声明/94 宿主可见**(Live 余 34);versions 4→2(ADR-005 的曲线真身/打印头/复制语义不变,仅版本数减半);轨道数 10→15(IPC registry 15 slots、UI、槽位算法同步)。
- **[J58→新增语义]** `lead_select` 全局自动化参数(0=遵循分析,1-15=强制该轨实时居中,其余轨不重分布);与分析期 `lead_lock`(逐段)双层;`lead_vol_exempt` 为**独立**每轨选项,不与任何 lead 机制强制关联(用户澄清)。