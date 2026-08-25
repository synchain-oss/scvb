# ANALYSIS —— SCVB 分析算法规格(转正文档)

> 状态: 稳定
> 最后更新: 2026-08-24(蒸馏自 ADR-007..010 + 计划 02/03)
> 真源: 本文件(由 `docs/constitution/ADR.md` + masterPlan/plan/02/03 蒸馏)

本文蒸馏 SCVB 的分析算法面:特征提取 → VAD → 分段 → 段响度 → 自动分配 → L/R 平衡 → pan 曲线。每条语义标注 ADR 编号;实现级细则(滤波器系数、状态机、测试向量)以计划 02 为准,本文只给契约级口径。

## 1. 分析流水线总览

```text
特征流(每 hop) → VAD(能量域双阈值) → 分段(能量谷/换气容忍) → 段响度
    → 自动分配(槽位 + 匈牙利) → L/R 平衡(ρ=cos2θ 迭代) → pan 曲线(角度域 EQ)
```

采集**只存特征不存音频**(ADR-007);分析产物 = 每版本每轨的分段时间线曲线(pan/vol),是「真身」,存 Output state(ADR-005)。

## 2. K-weighting 与 hop 特征提取(ADR-007/009)

- **hop = 10ms @48k**;每 hop 提取 K-weighted mean-square + peak(ADR-007)。
- **K-weighting 自研**:两个 biquad,系数来自 BS.1770(ADR-009);libebur128(MIT)仅作测试对拍参考,不进运行时依赖。对拍口径:**M(400ms)+ungated,禁用 global gated**(J18)。
- stereo 轨特征按 BS.1770 多通道求和(J57);VAD 后验 `vad_posterior` 为**可选缓存**(可由 kw_ms 按 ADR-008 离线重算,序列化时允许省略,J06)。

## 3. VAD v1(能量域,ADR-008)

- K-weighted 能量域**双阈值 + 滞回 + hangover + 前后 padding**,默认宁多勿少;阈值/灵敏度用户可调、可实时预览(因为存的是连续特征)。
- 前处理与自适应基准、精确状态机、旋钮↔内部参数映射、测试向量见计划 02 §2;出厂默认值(阈值/滞回/hangover/padding 前后/最小段长六项)经 U24 收敛,S1 后按真实素材校准。
- **v2 升级路径**:Silero VAD(ONNX,MIT)作精确模式,不进 v1(ADR-008)。

## 4. 分段(Segmentation,ADR-008)

- 能量谷检测 + 最小段长 + 换气容忍。
- **用户手动改边界** → 该段标记 `origin=user_edited`(新建段 `user_created`),可另加 `locked`;自动重分析**不覆盖** manual 边界(除非用户显式要求重识别)。
- segments 语义字段 = `{t0_samples, t1_samples, pan, vol_db, origin, locked}`,由 origin+locked 承载「重分析不覆盖」语义(J34,取代 v0 的 manual_edited)。

## 5. 段响度(ADR-009)

- 段响度 = 段内 **ungated K-weighted 积分响度**(EBU Tech 3341:M/S 本身不 gating)。
- 所有平衡计算在线性能量域做(O(N) 算术)。
- 响度口径选项集 `analysis.loudness_mode`(J69/U24①,设计定稿回流,state 字段非自动化,值用桥面真值):`kw_integrated`(默认)| `rms` | `peak_dbfs`。

## 6. 自动分配(ADR-010)

- **规则槽位生成**(主唱锁中、成对对称、优先级高→角度大、中心可分配)+ **匈牙利指派**(代价函数见计划 02 §5)。
- 每轨 `participate_in_auto_pan` 开关(J60):stereo 默认 false / mono 默认 true;参与的 stereo 轨以**中心点**入槽位分配(不区间化);全部轨参与 L/R 音量平衡(stereo 按实际双通道能量)。
- 中心槽策略选项集 `analysis.center_slot_policy`(J69/U24④,值用桥面真值):`priority_queue`(默认)| `lead_exclusive` | `even_spread`。
- 连续性滞回:相邻全局区间维持原布局的代价优势(见计划 02 §5)。

## 7. L/R 平衡(ADR-010)

- L/R 平衡用 **ρ=cos(2θ) 杠杆闭式解**迭代 3-5 次(计划 02 §6 能量模型与推导)。
- 参与能量模型:stereo 轨按实际双通道能量(J60)。

## 8. Pan 数学(ADR-010)

- **equal-power pan(sin/cos)**,Output 内部实现,与宿主 pan law 无关(前提:人声轨与总线的宿主 pan 保持居中,文档写明)。
- **不用 M/S 拉宽**:width>1 会对幅度声像源造成极性反转(research/07);期望宽度 = 几何角度缩放(把分配角乘以 width 系数)。
- **stereo dual-pan+width 模型**(J57):L/R 各自 equal-power pan,pan 参数 = 弧中心,每轨 width = 张开度,width=0 收成 mono。
- **pan 角度域增益曲线**:x=pan 角[-100,+100],y=gain dB;点类型 bell/shelf/cut 三种,带 Q;实现按 EQ 曲线插值同构(J07 增 side: out|left|right)。
- **段间过渡**:在段边界(停顿/换气)切换,默认 ramp 80ms(可调),避免可闻跳变。
