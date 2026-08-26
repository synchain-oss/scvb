# ARCHITECTURE —— SCVB 架构设计(转正文档)

> 状态: 稳定
> 最后更新: 2026-08-24(蒸馏自 `docs/constitution/ADR.md` v2.0 + 计划 01/04)
> 真源: 本文件(由 `docs/constitution/ADR.md` + masterPlan/plan/01 蒸馏)

本文从宪法 ADR-001..014 与计划 01/04 蒸馏出 SCVB 的架构总览,供贡献者快速建立心智模型。每个架构断言都标注 ADR 编号;契约细节以 `docs/SCVB_CONTRACT.md` / `docs/IPC_CONTRACT.md` / `docs/PARAMETERS.md` / `docs/STATE_SCHEMA.md` 为准。

## 1. 一眼看懂(音频管线)

```text
人声轨1 ─[SCVB Input]─┐                          ┌─→ 总线 ─[SCVB Output]─→ 主输出
人声轨2 ─[SCVB Input]─┼─(连接健康时静音)─────────┤       ▲ 按 [t0,t1) 时间线读环
人声轨N ─[SCVB Input]─┘  写入共享内存(时间线寻址)│       │ gain/pan → 求和 → 替换总线输入
                                      └── 环形缓冲(每 channel 一条)──┘
```

Input 插在人声轨插件链**最后一格**(J45),捕获后向下游输出静音(仅在检测到健康 Output 时),保住 DAW 依赖图「先人声轨、后总线」的排序;Output 插在总线**第一格**,按自身 block 的时间线区间从各 channel 环形缓冲读取、逐轨 gain/pan 求和后替换总线输入(ADR-002)。

## 2. 三个 target

一个仓库,CMake 三个主目标(ADR-001):

- `scvb_core`(静态库):DSP / 分析 / IPC / 状态 / 版本引擎,可离线 Catch2 单测(不链接 `juce_audio_plugin_client`,不引用 `AudioProcessor`)。
- `SCVB Input`(VST3):PLUGIN_CODE=`Scvi`,BUNDLE_ID=`com.synchain.scvb.input`。
- `SCVB Output`(VST3):PLUGIN_CODE=`Scvo`,BUNDLE_ID=`com.synchain.scvb.output`。

**为什么不做单插件双模式**:用户会同时开十几个实例,双模式易误操作(ADR-001)。`juce_add_binary_data` 资源目标与 `scvb_tests` 等辅助目标不违「三主目标」宪法(J22)。

## 3. 数据流四条链路

1. **音频链**:Input 捕获 → 音频环段 → Output 按 [t0,t1) 读 → gain/pan → 求和 → 替换总线输入(ADR-002)。
2. **特征链**:Input 每 10ms hop 算 K-weighted mean-square + peak → 特征段 → Output 分析时快照入 state(ADR-007;VAD 后验由 Output 离线从 kw_ms 计算,ADR-008)。
3. **控制链**:Output state(唯一真源)→ ctrl 广播区 → Input UI 显示;Input 改动 → 命令环 → Output 消息线程落 state(ADR-004)。
4. **自动化链**:引擎曲线 →(25Hz Timer,gesture 三段式)→ 123 个参数 → DAW write 录制(ADR-005/006)。

## 4. 为什么是「总线集中处理」而不是「就地处理」

D5 的三条理由(ADR-002 细则):

1. **依赖图排序**:Input 必须静音下游、Output 替换总线输入,才能保证 DAW 在离线渲染与预测性多线程下按「先人声轨、后总线」顺序处理。
2. **时间线对齐**:集中式 Output 按时间线寻址读环,天然对齐,不需要 PDC(Output 不报告额外 latency,ADR-002)。
3. **跨轨协调**:gain/pan 分配、L/R 平衡、主唱锁等是**跨轨全局**决策,必须在单一 Output 进程内完成(ADR-010)。

## 5. 时间线寻址 vs 裸 FIFO

共享内存环按**时间线绝对样本位置**寻址(ADR-002):写入地址 = `(timeline_pos & (ring_frames-1)) * channels + c`(stereo interleaved 时 c 为通道下标),读侧按自身 block 的 [t0,t1) 读取。裸 FIFO 在离线渲染 / REAPER 预测性多线程下会因「提前写 + 乱序消费」错位;时间线寻址 + `epoch` 计数器(跳变时 +1,读方丢弃跨代数据)天然容忍预测引擎的提前写(ADR-002/D5)。

## 6. 失准检测与语义(绝不静默出错)

- **缺口**:区间未被覆盖(write_head 落后或 epoch 不符)→ 该轨该块静音 + 失准计数 + UI 警告(ADR-002)。
- **重叠**:取时间线正确者(ADR-002)。
- **采样率不一致**:Input slot 写入自己的 sample_rate,Output 发现不一致 → 该轨禁用 + UI 错误(ipc §5)。
- **心跳陈旧**:>2000ms 视为陈旧(UI 显示),slot 接管需 ≥5000ms 且 pid 存活探测失败(双阈值,J10)。

## 7. 版本系统:内部曲线是真身,参数是打印头(ADR-005)

分析结果 = 每版本每轨的时间线分段曲线(pan/vol),存 Output state。123 个自动化参数是可自动化面;其中 30 条(15 轨×pan/vol)兼为「打印头」(J63):输出开关 ON 时引擎按播放位置把当前版本曲线值经 gesture 写到参数(供 DAW write 录制);OFF 时参数忠实跟随 host。`version_active` 为非自动化 state(防 write 自录);版本复制 = state 内曲线+配置复制,零 gesture、零自动化污染。

## 8. 线程模型表

| 线程 | 允许做什么 | 禁止做什么 |
|---|---|---|
| **音频线程 [A]**(processBlock) | 预分配缓冲上的定长运算、`juce::ScopedNoDenormals`、无锁 SPSC 环(acquire/release)、`std::atomic`、发布 playhead 快照(SPSC) | 堆分配/释放、任何锁、文件/网络/日志 I/O、抛/捕获异常、MessageManager / beginChangeGesture 系列、阻塞等待 |
| **消息线程 [M]**(25Hz Timer) | gesture 三段式(beginChangeGesture/setValueNotifyingHost/endChangeGesture)、落 state、消费命令环 | — |
| **UI(WebView)** | 轮询 ctrl 广播区(25Hz)显示、写操作经命令环发回 | 直接写音频数据 |

依据:ADR-006(gesture 只在消息线程)、ADR-011(跨线程原子 + lock-free)、CLAUDE.md §8。

## 9. 已知限制(v1)

- Output 不向 host 报告额外 latency;对齐靠时间线寻址,不用 PDC(ADR-002)。
- 同一时间只能有一个生效的 Output 实例(**同组内**语义;不同组各自独立,J66);第二个实例进只读观察 + UI 警告(ADR-002)。
- 轨道上限 15(InputSlot ×15,J59);参数预算封顶:Ableton Live 128 上限余 4,绝对禁止再加自动化参数(ADR-004)。
- 原始音频不存,只存特征(ADR-007);8MB 以上转 sidecar(ADR-007)。

## 10. 相关文件速查表(源码路径 ↔ 文档 ↔ ADR 编号)

| 源码路径 | 对应文档 | ADR 编号 |
|---|---|---|
| `src/core/CMakeLists.txt` / `ScvbCore.h/.cpp` | ARCHITECTURE(§2) | ADR-001/012 |
| `src/core/DesignBox.h`(由 web/shared/design-box.js 生成) | SCVB_CONTRACT | ADR-011 |
| `src/core/analysis/`(AutoAssign / CoverageMap / EnergyVad / FeatureExtractor / FrameStore / Hungarian / IVadBackend / Loudness / LoudnessMode / PanCurve / Reanalysis / Segmentation) | ANALYSIS | ADR-007/008/009/010 |
| `src/core/dsp/`(KWeighting / PanMath / ParamSmoother) | ANALYSIS | ADR-009/010 |
| `src/core/engine/`(AuthorityMode / CurveEvaluator / DspArbiter / PlayheadShot / VersionStore) | ARCHITECTURE(§7) | ADR-004/005/006 |
| `src/core/input/`(InputSession / OutputStage) | ARCHITECTURE(§1) | ADR-002 |
| `src/core/ipc/`(AudioRing / CtrlPlane / FeatRing / GroupProbe / ISegmentBackend / Registry / RegistryProbe / SegmentBackendInProcess / SegmentBackendWin32 / SegmentLayout) | IPC_CONTRACT | ADR-002 |
| `src/core/output/`(BusXfade / IMixSource / MixMath / OutputSession / ShmRingMixSource) | ARCHITECTURE(§1) | ADR-002 |
| `src/core/state/`(FeaturesCodec / InputStateCodec / OutputStateCodec / SegmentEdit / SidecarStore / StateCodec / StateMigration) | STATE_SCHEMA | ADR-005/007 |

`src/core/` 下七个子目录(`analysis / dsp / engine / input / ipc / output / state`)全部覆盖如上;`src/input/` 与 `src/output/` 为两个插件的 Processor/Editor,`web/` 为 WebView UI,`web-preview/` 为浏览器 mock 预览。
