# ADR-010 · Pan 数学

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-010 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- equal-power pan(sin/cos),Output 内部实现,与宿主 pan law 无关(前提:人声轨与总线的宿主 pan 保持居中,文档写明)
- 期望宽度 = 几何角度缩放(把分配角乘以 width 系数),**不用 M/S 拉宽**(width>1 会对幅度声像源造成极性反转,见 research/07)
- pan 角度域增益曲线:x=pan 角[-100,+100],y=gain dB;点类型 bell/shelf/cut 三种,带 Q;实现按 EQ 曲线插值同构
- 自动分配:规则槽位生成(主唱锁中、成对对称、优先级高→角度大、中心可分配)+ 匈牙利指派;L/R 平衡用 ρ=cos(2θ) 杠杆闭式解迭代 3-5 次(research/07)
- 段间过渡:在段边界(停顿/换气)切换,默认 ramp 80ms(可调),避免可闻跳变

## 修订历史(摘自 constitution/ADR.md)

- **[J60→ADR-010 补充]** 自动分配:每轨 `participate_in_auto_pan` 开关(stereo 默认 false/mono 默认 true);参与的 stereo 轨以**中心点**入槽位分配(不区间化);全部轨参与 L/R 音量平衡(stereo 按实际双通道能量)。**[J83 修订,2026-08-26]** 默认档改「未显式设置一律参与」:检测值 source_channels 来自宿主总线布局而非素材声道,J60 前提不成立;是否排除由用户逐轨显式开关决定。