# 契约变更说明 —— 20260831-j95-loudness-mode-a

## 变更了哪个冻结契约

| 文件 | 位置 | 驱动裁定 |
|---|---|---|
| `masterPlan/constitution/ADR.md`(原件) | 末尾新增 `# v2.2 修订(2026-08-31,J95 修宪并批)` 节,`[J95②→ADR-009 澄清]` 一条 | J95②a |
| `docs/constitution/ADR.md`(仓内只读副本) | 同上,逐字节同步(sha256 由 `check-constitution-sync.ps1` 断言) | 同步件 |
| `docs/adr/0009-loudness.md`(ADR-009 拆分镜像) | 「修订历史」小节追加同一条 | 同步件 |

**ADR-009 正文一行不动。** 第一条(段响度定义)与第三条(所有平衡计算在线性能量域做)
**逐字保留、继续完整适用**;本次只加一条澄清,说明这两条各自管的是什么。

**桥面契约零改动**:`SCVB_CONTRACT.md` / `PARAMETERS.md` / `STATE_SCHEMA.md` /
`IPC_CONTRACT.md` 一个字未动,state abi 仍为 2,不加迁移函数,IPC 结构体与 abi 无关。
(J94 曾批「§2.8 加第二指标字段」,**A 案下不需要,该项已由 J95② 撤销**。)

**批准链**:用户 2026-08-31 拍板走 A 案(J95②);措辞由「改写第三条」改为「只加不改的
澄清节」经统筹核准并入册 **J95②a**(理由见下「为什么不改第三条」)。本文档先出草稿过目、
后动冻结件;`masterPlan` 原件由统筹落笔,仓内副本与镜像由实施侧同步。

## 为什么

### 缺陷:`analysis.loudness_mode` 有名无实

用户 v5.6.3 实测第 19 条:「响度模式我看了三个模式出来的结果好像是一样的,但可能是巧合」。

**不是巧合,是断链。** SL-252 定谳(全部在 `490294a` 上实读):

| 环节 | 状态 |
|---|---|
| web 设置 → `setAnalysisConfig({loudness_mode})` | 通 |
| 桥面 → `runtime_.loudnessMode`(三值校验) | 通 |
| `runtime_.loudnessMode` → 落盘 CFGS / 读回 UI | 通 |
| **`runtime_.loudnessMode` → 分析引擎** | **完全不通** |

三条判据:

1. `PipelineConfig`(`AnalysisPipeline.h`)**根本没有响度口径字段**;
2. `startAnalysis` 从不传它 —— `cfg.*` 全量赋值只有 sampleRate / hopMs / range / vad.* /
   segmentation.* / balance.panCurve / **assign.centerSlotPolicy**。
   `centerSlotPolicy` 传了、`loudnessMode` 没传,这正好解释用户「中央槽策略正常」;
3. 三个 mode-aware 函数(`segmentLoudness` / `measureSegment` / `computeLoudnessMetric` /
   `computeSegmentLoudness`)的调用方**全部是 tests**,`src/` 下零命中。

而流水线实际算的是 `AnalysisPipeline.cpp` 的 `z = meanKw(...)` —— 写死公式、无 mode 参数。
三档结果**恒等**是结构性必然。

数学层本身分歧极大(现有 golden 向量 J69-1 实测,同一输入):

| 档 | 值 |
|---|---|
| `kw_integrated` | −13.7013 dB |
| `rms` | −13.9794 dB |
| `peak_dbfs` | −6.0206 dB |

`peak_dbfs` 与 `kw_integrated` 差 **7.68 dB** —— 不可能被误认成「一样」。
**函数会分歧 + 用户看到不分歧 ⇒ 函数没被调用**,推理闭合。

### 设计意图:它本就该改变分析结果

设计稿「第二响度指标 · 段响度用哪个口径」条的 note 逐字:

> 「影响分析时的**段间响度归一化基准**;改后需重分析。」

即:这个设置该决定平衡层的归一化基准,切档后重分析 pan/volDb 应当真变。用户按此期待,
而实现从未接通。用户 2026-08-31 拍板按设计意图接通(A 案)。

## 变更内容

### ① ADR-009 澄清(逐行)

**原文**:ADR-009 正文三条 + `[J18→ADR-009]` 修订条,**一字不动**。

**新增**(`ADR.md` 末尾新起一节):

```
# v2.2 修订(2026-08-31,J95 修宪并批)

- **[J95②→ADR-009 澄清]** 区分**上报段响度**与**平衡归一化基准**两件事:
  ① 正文第一条「段响度 = 段内 ungated K-weighted 积分响度」界定的是**上报口径 L_seg**,
     不随 `analysis.loudness_mode` 变化(J18 对拍口径同样不变);
  ② **平衡归一化基准** z 由 `analysis.loudness_mode` 选择 —— `kw_integrated`(默认)
     = `mean(kw)`,与本次修订前逐位相同;`rms` = `(mean(√kw))²`;`peak_dbfs` = `max(peak)²`。
     三档**均为线性能量量**,故正文第三条「所有平衡计算在线性能量域做(O(N) 算术)」
     **不受影响、继续完整适用**。
  依据:SL-252 定谳 —— 设计稿「第二响度指标」条注明本设置「影响分析时的段间响度归一化
  基准;改后需重分析」,而实现从未把它接进引擎(`PipelineConfig` 无该字段),三档结果恒等。
  用户 2026-08-31 拍板走 A 案(切档→重分析→pan/volDb 真变;默认档逐位不变)。
```

### 为什么不改第三条(J95②a 的由来)

原批措辞是把第三条改成「默认档在线性能量域;其他档为显式用户选择」。**不采用**,因为
按 A 案的实现,`z` 三档**都是线性能量量**:

| 档 | z | 量纲 |
|---|---|---|
| `kw_integrated` | `mean(kw)` | 线性能量 |
| `rms` | `(mean(√kw))²` | 线性能量 |
| `peak_dbfs` | `max(peak)²` | 线性能量 |

平衡算术**从未离开线性域**。原措辞会让后人读成「其他档可以不在线性域」——**那是假的**,
且把一条仍被完整遵守的硬约束削弱掉。真正含糊的是第一条的**适用范围**(它管的是上报口径,
不是归一化基准),故只加一条澄清把二分说清,正文两条都不动。

### ② z 为什么不能直接接 mode 的返回值

`AutoAssign` 对 `z` 的用法是 `zSum += t.z`、`zHat = t.z / zSum`、`zL = t.z / 2`、
`lCoef = c²·cos²θ·t.z` —— **必须是非负、可加、可求比的线性能量量**。

而 `computeLoudnessMetric` 三档返回的全是 **dB(负数)**,且**不在同一把尺上**:
`KIntegrated` 含 `kLufsOffset = −0.691`、走 `10·log10`(能量域);`Rms`/`PeakDbfs` 无偏移、
走 `20·log10`(幅度域)。把它塞进 `z` 会让 `zSum` 变负、`zHat` 失去意义。

故 A 案**不复用**那三个函数,另写一个返回**线性能量**的小纯函数(同样离线可单测)。
那三个既有函数按 J95② 裁定**保留并加头注**,说明它们服务的是「第二指标读数」这条
尚未落地的路 —— 免得下一个人当成死代码删掉。

### ③ 默认档逐位不变(硬要求)

`kw_integrated` 档的 `z` 必须**走与今天逐字相同的代码路径**(`mean(kw)`),
不得实现成 `10^(L/10)` 之类的等价换算 —— 那会引入浮点误差,让既有工程重分析后
pan/volDb 发生肉眼不可见但逐位不同的漂移。用例以「默认档结果与修订前逐位相等」反向钉住。

## 连带(同 PR,零契约文字变更)

`loudnessLufs` 上桥恒为 `0.0`(`OutputEditor.cpp` 的 `put(seg, "loudnessLufs", 0.0)`)——
§2.8 字段在、UI 消费点在、就是没有值,与 SL-177 修过的 `stale` 同族。
根因是 `applyAnalysisSegments` 把 `AnalysisSegment` 抄进 `state::Segment` 时丢掉了它,
而 `state::Segment` 没有响度字段(宪法 `params-v0.md` 定死持久化段字段)。

按 J95② 裁定走 **(c)**:**emit 时按 FEAT 特征重算真值**。字段已在 §2.8,
**零契约文字变更**、不修宪、不动 state abi;重开工程后照样有值(FEAT 随工程走);
改完边界后自动跟着变(缓存做不到这一点)。台账 SL-257 并入本卡。

## 落地清单(定稿:全部已完成)

| # | 事项 | 状态 |
|---|---|---|
| 1 | `masterPlan/constitution/ADR.md` 新增 v2.2 节(统筹落笔) | ✅ 已落笔 |
| 2 | `docs/constitution/ADR.md` 逐字节同步(头注行除外) | ✅ **机械重建**:头注行 + 原件逐字节,不手抄 |
| 3 | `docs/adr/0009-loudness.md` 修订历史追加同一条 | ✅ 文件头「最后更新」跟到 v2.2 |
| 4 | `check-constitution-sync.ps1` 通过(gate 1) | ✅ 三份全 PASS |
| 5 | `BalanceConfig` 加口径字段 → `startAnalysis` 传 → z 按档求值 | ✅ `analysis/BalanceBasis.h` |
| 6 | `loudnessLufs` 按 (c) 真值化 | ✅ `OutputProcessor::segmentLoudnessLufs` |
| 7 | 三个既有 mode-aware 函数保留 + 头注 | ✅ 两个头文件各一条 |
| 8 | 用例 | ✅ 见下 |

### 实装要点(与草案的差异,均为收紧)

- **断链的那一行坐实了**:`startAnalysis` 的 `cfg.*` 赋值里 `cfg.assign.centerSlotPolicy`
  传了、`loudnessMode` 没传。补上的就是这一行,字符串→枚举走 core 既有的 `parseLoudnessMode`
  (唯一真源,不造第二套判定)。
- **默认档逐位不变**做成了「**直接调同一个 `meanKw`**」:`meanKw` 真身从 `AnalysisPipeline.cpp`
  上提到 `BalanceBasis.h`,上报口径那一支(澄清 ①)调的仍是它,一个字没改。
- **`lufsFromMeanKw` 只留一份**:原为 `AnalysisPipeline.cpp` 匿名命名空间的静态函数,
  emit 侧要用同一个换算,故移出并由 `AnalysisPipeline.h` 导出 —— 各写一份就是下一个口径漂移。
- **`loudnessLufs` 用 `t1Effective` 而非裸 `s.t1`**:「无末端」段的 `t1` 是 `1<<40` 哨兵
  (≈2290 万秒),按 hop 积分若用裸值单段要循环 23 亿次 —— 正是 P0-A 那一族。

### 用例与反向验证

`[SL252]` 19 断言:默认档**逐位**等于 `meanKw`(`==`,不是 `Approx`)、三档两两不等、
三档数学口径逐条对齐 v2.2 公式、三档同为非负线性能量、空窗/越界窗回 0.0 且不产 NaN。

**反向验证实跑**:把默认档改成 `10^(10·log10(m)/10)` 的等价换算后,断言打印

```
CHECK( zK == meanKw(kw, b, e) )
with expansion:  0.0102999998 == 0.0102999998
```

—— **肉眼完全一样却判不等**。这就是「既有工程重分析后 pan/volDb 发生肉眼不可见但逐位不同的
漂移」的真面目,也是这一条必须用 `==` 而不是 `Approx` 的理由。

核心全量 `scvb_tests` **全过**(含 golden 向量)。

> 断言总数**随批次浮动**(约 96 万量级:实测见过 960547 / 965428 / 976216 等)——
> 套件里有随机化/性质类用例,**这个数不是稳定基线,别拿它当门槛**(#168 复审【建议】D:
> 变更文档与 commit message 各引了一个数、看起来像对不上,其实是不同时点的同一套件)。

### 顺带发现(不在本卡范围,已另立卡)

**SL-262**:`analysis/Loudness.h` 与 `analysis/LoudnessMode.h` **各自定义了同名同命名空间的
`LoudnessMode` 与 `SegmentLoudness`**,且内容不同(枚举名 `kIntegrated/kRms/kPeakDbfs` vs
`KIntegrated/Rms/PeakDbfs`;`SegmentLoudness` 成员完全不同)。两份都编进 `scvb_core`,
今天靠「没有哪个 TU 同时 include 两头」侥幸不炸,按标准已是 ODR 违规。**本卡把雷往前推了
一步**(`BalanceConfig` 现在 include 了 `LoudnessMode.h`),故用例改放进不含 `Loudness.h` 的
`test_analysis_pipeline.cpp` 绕开,并在用例头注写明原因。爆破半径实测:2 个文件、23 处旧拼写。

附注:`Loudness.h` 的注释里其实早写着「rms / peak_dbfs 档 z = `10^(L/10)`」—— 与本案的
`(mean(√kw))²` / `max(peak)²` **数学等价**(已验算),但它走 dB 往返,正是上面那个逐位漂移的
写法。设计意图一直在,只是既没接线也没人按它实现。

> **修宪串行**:本卡与 SL-254 同动 `ADR.md`。按统筹安排 SL-254(J95①)先行,其同步合并后
> 再写入 J95② 的 v2.2 节。**两条不在同一处**:J95① 已按 fp-r2 措辞落成 `[J32→ADR-002]` 的
> **行内附注**(不进修订节);本卡新增的 `v2.2` 节**仅含 J95②**。谁后合谁 rebase。
