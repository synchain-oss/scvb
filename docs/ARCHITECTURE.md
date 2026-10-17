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
3. **控制链**:Output state(唯一真源)→ ctrl 广播区 → Input UI 显示;Input 改动 → 命令环 → Output 消息线程落 state(ADR-004)。[J150] 另有一条只读提示:Input 从宿主拿到的 DAW 轨道名 → ctrl 段轨道名区 → Output 给用户没亲手改过名的通道填 `label`(填不填由 Output 决定,真源仍是 Output state;IPC_CONTRACT §4)。
4. **自动化链**:引擎曲线 →(25Hz Timer,gesture 三段式)→ 123 个参数 → DAW write 录制(ADR-005/006)。

## 4. 为什么是「总线集中处理」而不是「就地处理」

D5 的三条理由(ADR-002 细则):

1. **依赖图排序**:Input 必须静音下游、Output 替换总线输入,才能保证 DAW 在离线渲染与预测性多线程下按「先人声轨、后总线」顺序处理。
2. **时间线对齐**:集中式 Output 按时间线寻址读环,天然对齐,不需要 PDC(Output 不报告额外 latency,ADR-002)。
3. **跨轨协调**:gain/pan 分配、L/R 平衡、主唱锁等是**跨轨全局**决策,必须在单一 Output 进程内完成(ADR-010)。

## 5. 时间线寻址 vs 裸 FIFO

共享内存环按**时间线绝对样本位置**寻址(ADR-002):写入地址 = `(timeline_pos & (ring_frames-1)) * channels + c`(stereo interleaved 时 c 为通道下标),读侧按自身 block 的 [t0,t1) 读取。裸 FIFO 在离线渲染 / REAPER 预测性多线程下会因「提前写 + 乱序消费」错位;时间线寻址 + `epoch` 计数器(时间线跳变或环几何改写时 +1,读方丢弃跨代数据;几何改写见 IPC_CONTRACT §2「几何纪律」)天然容忍预测引擎的提前写(ADR-002/D5)。

**读方换代(A-5,设计口径)**:读方只能看到 `epoch` 与 `write_head` 两个数,看不出写方在新一代里「从哪一帧开始写」。所以读方按「代」认数据,一帧只有能确认是写方为这个位置写下的才读,确认不了的整块不读:

- **两个可读窗**:当前窗 = 本代已连续写过的 `[锚点, 写头)`;上一代窗 = 上一代最后一次观测到的写头之下、本身没被套圈、且新一代不可能覆盖到的尾段。换代之后上一代没被覆盖的尾段继续可读,所以循环回起点、回跳之后不再丢一整圈。
- **读方预拷贝(A-16)**:读方每块把本代刚写好的数据拷一份到自己的缓冲(约 2 MiB / 每条已连接的轨,首次绑定时在非实时线程一次分配);写方回绕或回跳之后,上一圈尾段直接从这份拷贝读,不再受「新一圈是不是已经写到同一个环槽」「写方下一笔写在哪」的影响。所以圈长落在环长(48 kHz 下约 10.9 s)整数倍前后一个提前量之内的循环,以及宿主一口气补渲染之后紧接着的那一圈,不再每圈丢约一个提前量。新一代一发布到读方附近(宿主原地重算、位置连续的换代、往前定位、停调恢复),或者读方自己跳变、漏读、静止重读,这份拷贝就作废,不交旧版本;环路径的判据(在途保护、别名判据)一字未改。
- **推迟锚定**:当前窗的起点只在能论证它不早于写方本代真正起点时才设;不能论证时(例如读方跳变之前写方已经连续换了几代)推迟到写头确认属于新一代之后再锚。
- **w=0 清窗**:段头写头归 0(几何改写)时清掉当前窗与上一代窗,不拿旧位置的数据顶替。
- **段头 `channels` 比对**:音频线程每块按值比对段头 `channels` 与绑定时的快照,只比对、不寻址;不一致说明写方已按新布局重写这条环、`[M]` 还没换绑,整块不读,也不计入失准。
- **同步宿主不变**:读写在同一块里一起跳变的宿主(先 Input 后 Output)与旧实现逐块同读。
- **残余**:读方不知道写方恢复点,所以被提前处理的轨从静音段恢复时,句首最多少一个写方块(约 21 ms);循环结尾仍可能少「读方最后一次看到上一圈之后写方又写的那一截」,通常不超过一个写方块,与圈长无关;其余只在读写真正并发的宿主上出现的窄窗口(R1–R6、预拷贝的 RP1–RP8)写在 `ShmRingMixSource.h` 头注里。面向用户的说法与缓解见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-11。

措辞与细节见冻结文档 IPC_CONTRACT §2「读」和变更文档 `contract-changes/20261009-a5-reader-generation-handover.md`、`contract-changes/20261014-a16-reader-precopy.md`;已知用户可感知的残余见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-11。

## 6. 失准检测与语义(绝不静默出错)

- **缺口**:区间未被覆盖(write_head 落后或 epoch 不符)→ 该轨该块静音 + 失准计数 + UI 警告(ADR-002)。
- **重叠**:取时间线正确者(ADR-002)。
- **采样率不一致**:Input slot 写入自己的 sample_rate,Output 发现不一致 → 该轨禁用 + UI 错误(ipc §5)。
- **心跳陈旧**:>2000ms 视为陈旧(UI 显示),slot 接管需 ≥5000ms 且 pid 存活探测失败(双阈值,J10)。
- **已注入但持续读不到**(A-6):一条轨在 Output 的混音集合里、写方也在推进,而读方从一代开始就一直读不到它(读方领先写头一类:人声轨没送进装 Output 的总线、宿主先处理总线后处理人声轨、Aux 位置没做延迟补偿),这一代永远「读到过数据」不了。旧口径把「读到过数据之前」的失败一律当作还没上线、不计失准,于是这样的轨无声而不报警。现在同一段连续读失败超过宽限(读方时间线 200 ms 与「2 × 实测提前量上界 + 一块」取大)后,写方推进的每一块计一笔缺口,所以亮起时间随宿主提前量变化:提前量小时通常约 0.5 秒内,提前量越大越晚(约为提前量的 2 倍)。换代、起播、短于宽限的交接前后读不到仍不计。
- **正向在场才撤下**:失准会把该轨踢出注入集,读方不再读它、缺口自然停止增长;若只看「一段时间没有新缺口」,持续读不到的轨会周期性假恢复。所以撤下失准多一个条件:该轨自最近一笔缺口以来确实读出过数据(兜底:在注入集里满 1 秒而 Output 一次都没读它,例如面板上关掉了这一轨,退回原条件)。
- **挂起 ≠ 掉线**(A-8):写头冻结(宿主暂停调用该 Input,或用户旁路它)与「掉线」是两件事。走带在跑且心跳新鲜、采样率一致、已绑定、未失准、不在看门狗 hold 里时,冻结的轨**保持在线**(`connected_mask` 位与注入集都保留),只在 `scvb.conn` 的 `suspended` 里表示「此刻没有数据」,宿主恢复调用的同一块就读到;走带停住时仍按约 0.5 秒清位,心跳陈旧、失准、看门狗闩住也照旧清位。写方从一代开始就冻着的轨既不算挂起也不算失准(既有口径)。
- **注入轨全部停滞时回原声**(U3(b)-ii):保持注入之后,注入轨**全部**停滞(都是保持在线的挂起轨、本段没有一条读到数据)且总线输入听得见时,Output 经既有的 80 ms 等功率交叉把总线交回原声,有注入轨重新读到数据后再交叉回混音;总线输入数字静音时不回原声(那时原声就是静音,拨回直通只会让写方恢复时多走一遍淡入)。边界:总线上另有没装 Input 的声源时,总线因那一路「听得见」同样回原声,下一句开头仍有约 80 ms 淡入(同修前,见 KI-11)。旁路超过约 5.5 秒再解除旁路不再走交接握手,原声直接停、混音 80 ms 淡入。

措辞与细节见冻结文档 SCVB_CONTRACT §2.3、IPC_CONTRACT §2「读」与 §4,以及变更文档 `contract-changes/20261011-a6-misalign-positive-presence.md`、`20261011-a8-suspend-not-offline.md`。

## 7. 版本系统:内部曲线是真身,参数是打印头(ADR-005)

分析结果 = 每版本每轨的时间线分段曲线(pan/vol),存 Output state。123 个自动化参数是可自动化面;其中 30 条(15 轨×pan/vol)兼为「打印头」(J63):输出开关 ON 时引擎按播放位置把当前版本曲线值经 gesture 写到参数(供 DAW write 录制);OFF 时参数忠实跟随 host。`version_active` 为非自动化 state(防 write 自录);版本复制 = state 内曲线+配置复制,零 gesture、零自动化污染。

## 8. 线程模型表

| 线程 | 允许做什么 | 禁止做什么 |
|---|---|---|
| **音频线程 [A]**(processBlock) | 预分配缓冲上的定长运算、`juce::ScopedNoDenormals`、无锁 SPSC 环(acquire/release)、`std::atomic`、发布 playhead 快照(SPSC) | 堆分配/释放、任何锁、文件/网络/日志 I/O、抛/捕获异常、MessageManager / beginChangeGesture 系列、阻塞等待 |
| **消息线程 [M]**(25Hz Timer) | gesture 三段式(beginChangeGesture/setValueNotifyingHost/endChangeGesture)、落 state、消费命令环、绑定读环时一次性分配读方私有副本(约 2 MiB / 每条已连接的轨,A-16) | — |
| **UI(WebView)** | 轮询 ctrl 广播区(25Hz)显示、写操作经命令环发回 | 直接写音频数据 |

依据:ADR-006(gesture 只在消息线程)、ADR-011(跨线程原子 + lock-free)、CLAUDE.md §8。

## 9. 已知限制(v1)

- Output 不向 host 报告额外 latency;对齐靠时间线寻址,不用 PDC(ADR-002)。
- 同一时间只能有一个生效的 Output 实例(**同组内**语义;不同组各自独立,J66);第二个实例进只读观察 + UI 警告(ADR-002)。
- 轨道上限 15(InputSlot ×15,J59);参数预算封顶:Ableton Live 128 上限余 4,绝对禁止再加自动化参数(ADR-004)。
- 原始音频不存,只存特征(ADR-007);8MB 以上转 sidecar(ADR-007)—— **v1 默认不自动转存**(单点开关关闭,特征流一律内嵌;读带 sidecar 的老工程照常)。

## 10. 相关文件速查表(源码路径 ↔ 文档 ↔ ADR 编号)

| 源码路径 | 对应文档 | ADR 编号 |
|---|---|---|
| `src/core/CMakeLists.txt` / `ScvbCore.h/.cpp` | ARCHITECTURE(§2) | ADR-001/012 |
| `src/core/DesignBox.h`(由 web/shared/design-box.js 生成) | SCVB_CONTRACT | ADR-011 |
| `src/core/analysis/`(AutoAssign / CoverageMap / EnergyVad / FeatureExtractor / FrameStore / Hungarian / IVadBackend / Loudness / LoudnessMode / PanCurve / Reanalysis / Segmentation) | ANALYSIS | ADR-007/008/009/010 |
| `src/core/dsp/`(KWeighting / PanMath / ParamSmoother) | ANALYSIS | ADR-009/010 |
| `src/core/engine/`(AuthorityMode / CurveEvaluator / DspArbiter / PlayheadShot / VersionStore) | ARCHITECTURE(§7) | ADR-004/005/006 |
| `src/core/input/`(InputSession / OutputStage) | ARCHITECTURE(§1) | ADR-002 |
| `src/core/ipc/`(AudioRing / CtrlPlane / FeatRing / GroupProbe / IpcDiag / ISegmentBackend / PlatformSegmentBackend / Registry / SegmentBackendInProcess / SegmentBackendPosix / SegmentBackendWin32 / SegmentLayout) | IPC_CONTRACT | ADR-002 |
| `src/core/output/`(BusXfade / IMixSource / MixMath / OutputSession / ShmRingMixSource) | ARCHITECTURE(§1) | ADR-002 |
| `src/core/state/`(FeaturesCodec / InputStateCodec / OutputStateCodec / SegmentEdit / SidecarStore / StateCodec / StateMigration) | STATE_SCHEMA | ADR-005/007 |

`src/core/` 下七个子目录(`analysis / dsp / engine / input / ipc / output / state`)全部覆盖如上;`src/input/`、`src/output/` 与 `src/monitor/` 为三个插件的 Processor/Editor,`web/` 为 WebView UI,`web-preview/` 为浏览器 mock 预览。
