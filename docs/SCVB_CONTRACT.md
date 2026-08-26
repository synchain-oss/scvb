# SCVB_CONTRACT —— SCVB JS↔C++ 桥契约(冻结契约)

> **版本**:1.0(已冻结)
> **状态**:已冻结(2026-08-16;DeepSeek native 可实现性评审通过 + 用户批准);此后任何改动按 §9 走「只增不改」变更流程
> **真源**:函数名/签名形态/事件名 = `masterPlan/plan/05-ui-spec.md` §1.4(含 §2.3a payload、§2.0/§2.1/§2.3/§2.4/§3 组件表、§6.2/§6.3 数据面);载荷字段语义与线程/节流/防回环细则 = `masterPlan/plan/01-architecture.md` §6.1/§6.4(ctrl 段 §4.4、跨组探测 §4.5);参数面/state 字段 = `docs/constitution/params-v0.md`(**v2.3**);IPC 段与枚举 = `docs/constitution/ipc-contract-v0.md`(**v1.6**);裁定规则 = `masterPlan/plan/07-execution-plan.md` T25

---

## 0. 总则

### 0.1 契约地位

1. 本文件是 JS↔C++ 桥面的**唯一真源**。T26 起的 `src/input/InputBridgeApi.h`、`src/output/OutputBridgeApi.h`(C++ 常量表)、`web/shared/bridge.js`(JuceBackend + MockBackend)与 `web-preview/` 的 mock 后端一律以本文件为准;T29/T30/T31-T36 的第一真源是本文件,不是 01/05(07 T25)。
2. 本契约**自包含**:实施 agent 不需回读 05/01 即可实现。括注中的 `05 §x` / `01 §x` / `J{nn}` 仅为溯源标记。
3. 本文件所载函数/事件的**名字与签名在冻结后只增不改**(01 §6.1 对标 `BRIDGE_CONTRACT.md` 的同款纪律)。允许:新增函数/事件、在既有 payload 中新增**可选**字段。禁止:改名、改参数顺序、改既有字段语义、删除函数/事件/字段、收窄取值域。**触碰禁止面的改动 = 契约破坏性变更**(`contractVersion` 主版本号 +1),须同批更新 mock 后端、C++ 常量表与 `check-bridge-parity.mjs`;**它不触发 ipc/params 的 `abi` 升级**——`abi` 只由 ipc-contract-v0 §5(共享内存段布局)与 params-v0 §四(state chunk)定义的改动触发(§9.0 第 3 条)。
4. 本契约中**不得出现语义重复的双入口**(07 T25 硬规则)。已归并与禁止复活的旧名见 §8。

### 0.2 命名纪律

1. 事件名统一 `scvb.*` 前缀,全小写驼峰段(如 `scvb.captureProgress`);函数名 lowerCamelCase。
2. **键名拼写规则(本契约裁定,消除 01/05 拼写漂移)**:
   - **直接镜像宪法字段的键**(params-v0 §二/§三 的 state 树、ipc-contract 的段字段)一律**逐字沿用宪法拼写**(snake_case),例:`group_id`、`capture_enabled`、`source_channels`、`participate_in_auto_pan`、`lead_lock`、`lead_vol_exempt`、`pair_id`、`vol_db`(state 真身)、`config_seq`、`groups_online`、`loudness_mode`、`center_slot_policy`、`guide_seen`、`tour_seen`。
   - **纯传输/计算结构**(函数参数、函数返回、事件的计算字段与段编辑 payload)一律 **lowerCamelCase**,逐字照 05 §1.4/§2.3a,例:`tracksMask`、`startS`、`endS`、`segIdx`、`volDb`、`slotState`、`heartbeatFresh`、`misalignCount`、`outputReadOnly`、`peakDb`、`timeS`、`isPlaying`、`inRange`、`passId`、`minDb`、`maxDb`。
   - **Input 快照拼写已按规则①统一**(统筹裁定 **A-30**):`requestInitialState` 返回的 `channel_id`/`group_id`/`ui:{scale, language}` 与 Input `scvb.state`(§4.1)**同拼写**,同一 state 字段全契约只有一种键名;01 §6.2 的 camelCase 快照为旧文,记 §8.3。
3. 时间单位:桥面一律**秒(f64)**,字段后缀 `S`(`startS`/`endS`/`t0S`/`t1S`/`tS`/`timeS`)。state 真身以样本记(params-v0 `t0_samples`/`t1_samples`),**样本↔秒换算在 C++ 侧完成,UI 永不见样本**。
4. 通道编号 `ch` = **1..15**(J59);组号 `g` = **1..8**(J66,UI 显示 A-H,默认 1=A);版本号 `v` = **1..2**(J59)。
5. `tracksMask` = **u16 位图**,bit0=ch1 … bit14=ch15,bit15 保留为 0。
6. `groups_online` = **u8 位图**,bit0=组 A … bit7=组 H。

### 0.3 线程模型(01 §6.1/§6.4)

1. **全部 native function 在消息线程 [M] 处理**;音频线程 [A] 永不参与桥面。
2. **长耗时任务转工作线程 [W]**:native function 只负责**启动并立即 resolve**(返回受理回执),进度与结果一律走事件(`scvb.segments` / `scvb.state`)。适用:`analyze`、`setVadParams`/`setSegmentation` 的松手档流水线。**不新设 `analysisProgress`/`analysisDone` 事件**(§8 禁止复活)。
3. **拉取式不进事件流**:`requestWaveform` 为 **request/response** 语义——**一次调用一次 resolve**,**绝不作为推送事件**(01 §6.4)。是否把降采样放在 [W] 完成后回 [M] 兑现,由 native 侧决定(桥面不规定同步/异步,见 §1.27;01 §6.4 只要求「按需拉取、不进事件流」)。
4. gesture 三段式(`beginParamGesture`/`setParam`/`endParamGesture`)一律在 [M] 转发到宿主(ADR-006);音频线程禁止 `beginChangeGesture` 系列(CLAUDE.md §8)。
5. 跨组在线探测(`scvb.groups` 数据面)在 [M] 持 `lifecycleMutex` 执行,只读、1Hz,绝不映射异组 audio/feat/ctrl 段(01 §4.5)。

### 0.4 节流与 diff-then-emit(01 §6.1/§6.4)

1. 事件推送统一走 **diff-then-emit 模式**(承 01 §6.1 的 Bridge 25Hz Timer 机制,「SCVB 事件类别更多,**每类独立节流**」):**值未变不发**;**每一类事件的实际频率以 §2/§4 各条目标注的数值为准**(逐字照 05 §1.4:`scvb.params` 25Hz、`scvb.meters`/`scvb.playhead` 30Hz、`scvb.conn` ~4Hz、`scvb.captureProgress` 播放中 2Hz、`scvb.groups` 1Hz)。**基准 Timer 频率不在桥面契约内**:native 侧须选一个能整除上述各类别频率的基准(或按类别用多个 Timer),桥面只约束「每类的推送频率上限 + 值未变不发」。**注**:01 §6.1 的「25Hz 单 Timer」与 05 §1.4 的 30Hz meters/playhead 无公因子节拍——**统筹裁定 A-28 定案:各事件频率以 05 §1.4 逐类标注为准;基准 Timer/分频方式为 native 实现细节,不入桥面契约。若 native 评审认为 30Hz 实现代价过高,按 §9.0 流程回改 05 的对应档**。
2. 电平/失准等高频数据带阈值(电平 0.3 dB 阈值,镜像 Bridge)。
3. `scvb.groups`、`scvb.config` 为「按频率探测/轮询、**变化才发**」;`mBridgeReady` 后的**首帧必发**按事件类别分三档,保证 UI 不停在空态:
   - **状态类**(`scvb.state` / `scvb.params` / `scvb.conn` / `scvb.config` / `scvb.groups` / `scvb.meters` / `scvb.playhead` / `scvb.segments`)—— **首帧各必发一次**(`scvb.segments` 以 `reason:"snapshot"` 发全部轨全量段表,§2.8);
   - **采集类**(`scvb.captureProgress`)—— **只在播放中发**(§2.7),首启非播放时不发,空态由 `scvb.state` 承载;
   - **条件类**(`scvb.error`)—— **只在条件成立时发**(§2.9/§4.5),**不发空 error**。
4. 波形按视口拉取,LRU 缓存归 UI 侧(05 §6.3),C++ 不为波形维护推送状态。

### 0.5 防回环(01 §6.4,ADR-006)

1. **发起端唯一原则**:UI 仅在**用户直接操作**时上行 `setParam`;引擎打印期间(`output_enabled=ON`)UI 收到的 `scvb.params` **只更新显示**,绝不回写。
2. `scvb.params` 携带 `hostEcho` 标志:true = 本批值来自宿主回吐/引擎打印(ARMED/PRINT 下 UI 灰显),UI 不得据此发起任何上行调用。
3. host echo 在引擎侧忽略(ADR-006);`setParam` 的下一次 `scvb.params` 不得因自身回声再次触发 UI 上行。
4. 配置类写入唯一真源在 Output state(ADR-004):Input UI 是远程视图,任何配置写入只经 ctrl 命令环(§6),Input 端不得自建真源副本。

### 0.6 `mBridgeReady` 门控(01 §6.1)

1. C++ 侧在前端调用 `requestInitialState()` 之前**不推送任何事件**(`mBridgeReady=false`)。
2. `requestInitialState()` 返回后置 `mBridgeReady=true`,随后按 §0.4 推送;UI 在拿到首帧快照前不得渲染真实数据态。
3. 编辑器关闭/重建时 `mBridgeReady` 复位;重建后 UI 必须重新调用 `requestInitialState()`。

### 0.7 mock parity 要求(05 §1.5)

1. `web/shared/bridge.js` 的 **MockBackend 必须逐一实现**本契约全部函数与事件(假数据版),**导出名集合与 JuceBackend 完全一致**。
2. 三方名字集合比对由 `scripts/check-bridge-parity.mjs` 执行:**契约 manifest(§7) ↔ mock 后端 ↔ C++ 常量表**,任一侧多名/少名即退出码 1。当前(T25 时点)后两侧尚不存在,脚本打印 SKIP 并只做契约侧自检,口径见脚本头注释。
3. gesture 参数面(§1.12-§1.14)不引入新函数,但 **mock 后端必须有对应参数状态**(含每轨 `freeze`),否则 parity 视为不达标(05 §1.4 J65)。

### 0.8 通用类型与返回值约定

1. 所有 native function 返回 **JSON 对象**(无自然返回值者返回 `{"ok": true}`);绝不返回裸值、绝不返回 `undefined`。
2. 参数越界/类型不符:C++ 侧**夹取或拒绝**并返回 `{"ok": false, "reason": "badArg"}`,**绝不崩溃、绝不静默改写无关字段**;同时经 `scvb.error` 只在有对应用户可见警告面时上报(§5.1 九码,不为参数错误新增 code)。
3. 三种标准拒绝态(§5.6):`{"rejected": "printing"}`、`{"conflict": true}`、`{"observer": true}`。
4. 数值类型标注:`u8/u16/u32/u64` = 无符号整数,`f32/f64` = 浮点,`int` = 有符号整数,`bool`,`string`(UTF-8)。
5. **「返回」行登记的是可枚举的完整并集**:每个函数条目的「返回」行列出该函数**全部可能的返回形状**(成功形状 + 全部拒绝态形状),「拒绝态」行只解释各形状的**触发条件**、不再重复形状;§7 manifest 的 `returns` 字符串与「返回」行**同一并集**,写法 `A | B | C`。mock 后端、C++ 常量表与 parity 脚本三方以此对齐。

### 0.9 撤销性总则(05 §1.3,J34/J44)

| 入撤销栈(插件自有 UndoManager,03 §5.3) | 不入撤销栈 |
|---|---|
| `setPanCurve`、`editSegment`(全部 5 个 op)、`setTrackManual`、`copyVersion`、**`setVersionName`**([J82]) | `setCaptureEnabled`、`setOutputEnabled`、`setGroupId`、`setRange`、`setVersionActive`、`setChannelConfig`、`setVadParams`、`setSegmentation`、`setTransitionRamp`、`setAnalysisConfig`、`analyze`/`previewAnalyze`/`cancelAnalyze`、`recaptureArm`、`clearCoverage`、`confirmPrintGuard`、UI 类(`setUiScale`/`commitUiScale`/`setLang`/`setActiveTab`/`setGuideSeen`/`setTourSeen`/`setMasterChartMode`) |

UI 在 WebView 内捕获 `Ctrl+Z` / `Ctrl+Shift+Z` 映射到 `undo()` / `redo()` 并 `preventDefault`(防止冒泡到宿主撤销);焦点在文本输入框时不拦截(05 §1.3)。

---

## 1. Output —— native functions(UI → C++)

共 **36** 个。全部在 [M] 处理(§0.3)。

### 1.1 `requestInitialState()`

| 项 | 定义 |
|---|---|
| 参数 | 无 |
| 返回 | 全量快照对象(键为 state 镜像,拼写照 params-v0):<br>`{ session_guid:string, group_id:1..8, config_seq:u32,`<br>`  global:{capture_enabled:bool, output_enabled:bool, version_active:1..2, range:{mode, start_s:f64, end_s:f64}},`<br>`  analysis:{vad:{threshold_db:f32, hysteresis_db:f32, hangover_ms:int, padding_pre_ms:int, padding_post_ms:int}, segmentation:{mode:string, sensitivity:f32, min_segment_ms:int}, transition_ramp_ms:f32, loudness_mode, center_slot_policy},`<br>`  channels:[15 × {enabled:bool, label:string, source_channels:1\|2, participate_in_auto_pan:bool, priority:0..10, lead_lock:bool, lead_vol_exempt:bool, pair_id:0\|1..7}],`<br>`  versions:[2 × {name:string, empty:bool, pan_curve:{points:[{angle:f32, gain_db:f32, shape:"bell"\|"shelf"\|"cut", q:f32, side:"out"\|"left"\|"right"}]}}],`<br>`  features:{embedded:bool, bytes:u64},`<br>`  ui:{scale:f32, language:"zh"\|"en"\|"fr", active_tab, master_chart_mode:"distribution"\|"trajectory", guide_seen:bool, tour_seen:bool, lang_chosen?:bool},`<br>`  guide_seen_global:bool, tour_seen_global:bool, lang_chosen_global?:bool,`<br>`  print_guard:{pending:bool},`<br>`  recapture:{armed:bool, tracksMask:u16, startS:f64, endS:f64, autoStop:bool},`<br>`  analysis_run:{running:bool, progress?:f32},`<br>`  version:{plugin:string, abi:u32},`<br>`  conn:<同 §2.3 scvb.conn 载荷> }` |
| 语义 | 首帧全量快照,并置 `mBridgeReady=true`(§0.6)。**本返回的 state 子树字段集 = §2.1 `scvb.state`(`full:true`)的字段集 + 快照专属的 `session_guid` / `version` / `guide_seen_global` / `tour_seen_global` / `lang_chosen_global` / `conn`,两者不得各自漂移**(含 `print_guard`/`recapture`/`analysis_run` 三个运行时态:重开编辑器时 UI 靠本返回即可恢复守卫/布防/分析显示,不必等首帧事件)。**本机 abi 的唯一落点是 `version.abi`**(无顶层 `abi` 键,消除同一语义两个落点;**取值 = ipc 段布局 abi(`RegistryHeader.abi` 同源);state chunk abi 只经 `scvb.error.newerState.detail.{localAbi,projectAbi}` 暴露,不落本字段**)。**段表与曲线真身(`versions[].curves_per_track`)不在本快照内**——见 §2.8 契约边界(唯一来源 = `mBridgeReady` 后首帧 `scvb.segments`);`versions[].pan_curve` 因是**整表提交的小结构**(≤16 点)随本快照与 `scvb.state` 下推,不进 `scvb.segments`。`channels` 定长 15,下标 0 对应 ch1。`versions[v].empty=true` 表示该版本无曲线数据(05 §2.1 ③ 空版本 chip 角标)。`features.bytes` = 特征数据字节数(Tab4 存储状态显示,04 §5.4/ADR-007);**逐帧特征本体不下推**,波形一律走 `requestWaveform`。`guide_seen_global`/`tour_seen_global` 为系统级全局默认判定位(J50a),只读、不属工程 state。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M] 同步;每个编辑器生命周期至少调用一次 |
| 真源 | 05 §1.4;字段集 params-v0 §二;`empty`/`bytes`/`versions[].pan_curve` 下行落点为 T25 定名(§9.2) |

### 1.2 `setCaptureEnabled(on)`

| 项 | 定义 |
|---|---|
| 参数 | `on: bool` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"noTimeline"}` |
| 语义 | 写 state `global.capture_enabled`。ON = 对 `{enabled 轨} × {global.range}` 布防;实际写特征段只在「播放中且在 range 内」发生(01 §5.1、04)。变更经 `scvb.state` 回推。 |
| 拒绝态 | `noTimeline` 场景下 UI 侧 disabled(05 §2.0 横幅⑥);C++ 侧收到调用时返回 `{ok:false, reason:"noTimeline"}` 并不改 state |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.1 ① |

### 1.3 `setOutputEnabled(on)`

| 项 | 定义 |
|---|---|
| 参数 | `on: bool`(**两态**:ON=引擎驱动参数 write,OFF=follow host;ADR-005 / J08 维持 bool) |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"noTimeline"}` |
| 语义 | 写 state `global.output_enabled`。ON 且「播放中 ∧ 在 range 内」= PRINT 态(打印头写 gesture);ON 且停止/区间外 = ARMED;OFF = FOLLOW(03 §2.2 三态)。加载守卫未确认时**行为止于 ARMED**(04 §5.3,守卫态见 §2.1 `print_guard`,确认入口 = §1.34 `confirmPrintGuard()`)。 |
| 拒绝态 | 同 1.2 的 `noTimeline` 分支 |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.1 ① |

### 1.4 `setGroupId(g)`(J66)

| 项 | 定义 |
|---|---|
| 参数 | `g: 1..8`(UI 显示 A-H) |
| 返回 | `{ok:true}` 或 `{observer:true}`(新组 OutputSlot 已被占 → 本实例进只读观察) |
| 语义 | 写 state `group_id`;触发 01 §4.2 改组释放-重连:释放旧组 OutputSlot → Unmap 旧组全部段 → 新组 claim → 判定主/只读。成功后 `scvb.state.group_id` 与 `scvb.conn` 一并刷新。**与 Input 侧 §3.3 同名同签名**,返回值不同(Output=`observer`,Input=`conflict`)。 |
| 拒绝态 | PRINT 态由 UI 侧整组 disabled(05 §2.1 ⓪ tooltip「打印中不可切组」);C++ 侧不新增 `rejected` 码 |
| 撤销 | 否 |
| 线程/频率 | [M] 持 `lifecycleMutex`;用户确认后触发 |
| 真源 | 05 §1.4 / §2.1 ⓪;生命周期 01 §4.2 |

### 1.5 `previewAnalyze(scope)`

| 项 | 定义 |
|---|---|
| 参数 | `scope`:`{tracksMask:u16, startS?:f64, endS?:f64}` 或字符串 `"all"`(同 `analyze`) |
| 返回 | `{intervals:int, tracks:int, manualKept:int}` |
| 语义 | **纯只读 dry-run**:只算 `range ∩ coverage` 与 `origin≠auto` 段相交,毫秒级返回;**不执行任何流水线、不写 state、不发事件**。供「点击前影响预览」行在 scope/范围/勾选变化时节流刷新(05 §2.1 ①/§2.3)。 |
| 拒绝态 | 无(空集合返回 `{0,0,0}`) |
| 撤销 | 否 |
| 线程/频率 | [M] 同步,目标 <10ms;UI 侧节流调用 |
| 真源 | 05 §1.4(01 草案 `getAnalyzePreview` 已归并,§8) |

### 1.6 `analyze(scope, opts?)`

| 项 | 定义 |
|---|---|
| 参数 | `scope`:`{tracksMask:u16, startS?:f64, endS?:f64}` 或 `"all"`;`opts?`:`{clearManual?:bool=false}` |
| 返回 | `{ok:bool, affected:{intervals:int, tracks:int, manualKept:int}}` 或 `{ok:false, reason:"busy"}` —— **受理回执 + 影响面**,不是最终结果 |
| 语义 | 离线分析(秒级)。native function 只负责启动:[M] 提交 [W] job 后即 resolve;**结果一律经 `scvb.segments` 回推**,运行态经 `scvb.state.analysis_run` 下推(01 §6.4)。`clearManual:true` = 「重新识别(含手动段)」:仅把目标段 `origin` 重置为 `auto` 后重算;**`locked=true` 段不受影响,须先逐段解锁**(04 §4.4,J34);该分支须 UI 二次确认。默认(`clearManual:false`)只覆盖 `origin=auto` 段(ADR-008 v1.1)。 |
| 拒绝态 | `range ∩ coverage = ∅` → `{ok:false, affected:{0,0,0}}`(UI 侧同条件下按钮 disabled);已有分析在跑 → `{ok:false, reason:"busy"}` |
| 撤销 | 否(分析产物变更不入撤销栈) |
| 线程/频率 | [M] 启动 → [W] 执行;用户操作触发 |
| 真源 | 05 §1.4 / §2.3;线程语义 01 §6.4 |

### 1.7 `cancelAnalyze()`

| 项 | 定义 |
|---|---|
| 参数 | 无 |
| 返回 | `{ok:bool}`(false = 当前无进行中的分析) |
| 语义 | 取消进行中的 [W] 分析任务;已写入的段保持,取消后经 `scvb.state.analysis_run.running=false` 与(若有部分结果)`scvb.segments` 通知 UI。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 |

### 1.8 `setRange(mode, startS, endS)`

| 项 | 定义 |
|---|---|
| 参数 | `mode`: **`"follow" \| "daw_loop" \| "manual"`**(三值,默认 `follow`);`startS: f64`;`endS: f64` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` 或 `{ok:false, reason:"noLoop"}` |
| 语义 | 写 state `global.range`(J04)。`follow` 时**忽略 `startS`/`endS`**(调用方可传 0,C++ 不解释),**无哨兵约定**——v0 的「manual+[0,曲末] / start=end=0」已废除。`manual` 要求 `startS < endS`,否则返回 `{ok:false, reason:"badArg"}` 且不改 state。`daw_loop` 时 C++ 跟随宿主循环区;宿主 `kCycleValid` 瞬态缺失时**保持最后有效值**、不回退档位、不清空起止(04 §2.2 条 4)。宿主 loop 端点仅提供 PPQ 时,秒值按 `PPQ × 60 / bpm` 线性近似(best-effort,变拍工程不保证精确;评审附注 2026-08-16)。Tab3「设为范围」按钮即以 `mode="manual"` 调用本函数。 |
| 拒绝态 | `{ok:false, reason:"badArg"}`(manual 且 `startS >= endS`);宿主从不提供 loop 字段时 `daw_loop` 返回 `{ok:false, reason:"noLoop"}`(UI 侧该档位 disabled 但**绝不隐藏**) |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.1 ② |

### 1.9 `setVersionActive(v)`

| 项 | 定义 |
|---|---|
| 参数 | `v: 1..2`(J59) |
| 返回 | `{ok:true}` 或 `{rejected:"printing"}` |
| 语义 | 写 state `global.version_active`(**非自动化参数**,ADR-005:切换零 gesture、零自动化污染)。切换后 C++ **全量重发** `scvb.params`(`full:true`,`hostEcho:false`)以刷新新激活版本的 60 个每轨参数,**并全量重发 `scvb.segments`(`reason:"versionActive"`,含全部轨新版本段表)+ `scvb.state`(含新版本 `versions[].pan_curve`)**——换出的是另一版本的曲线真身,泳道段叠加层与 pan 曲线编辑器必须随之刷新(§2.8)。ARMED 态切换由 UI 弹轻确认(纯 UI 行为)。 |
| 拒绝态 | **PRINT 态(输出 ON + 播放 + 在 range 内)C++ 侧硬拒绝**,回 `{rejected:"printing"}`(03 §2.2 硬规则);UI 侧同时 chip disabled + tooltip |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.1 ③ |

### 1.10 `setVersionName(v, name)`

| 项 | 定义 |
|---|---|
| 参数 | `v: 1..2`;`name: string`(**≤16 字符**,超长由 C++ 截断;空串/纯空白回落默认 `"V{v}"`) |
| 返回 | `{ok:true, name:string}`(回显实际落盘名,供 UI 显示截断/回落结果) |
| 语义 | 写 state `versions[v].name`(J05)。**`copyVersion` 不复制 name**(§1.11)。 |
| 拒绝态 | 无 C++ 侧拒绝码;PRINT 态由 UI 侧 chip(含双击重命名)整组 disabled |
| 撤销 | **是**([J82]:07 §T18「`juce::UndoManager` 接入(重命名可撤销)」为准;改名进插件 UndoManager 事务,与 `copyVersion` 同族) |
| 线程/频率 | [M];用户提交(Enter/失焦)触发 |
| 真源 | 05 §1.4 / §2.1 ③ |

### 1.11 `copyVersion(src, dst)`

| 项 | 定义 |
|---|---|
| 参数 | `src: 1..2`;`dst: 1..2`(`src != dst`) |
| 返回 | `{ok:true}` 或 `{rejected:"printing"}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 深拷贝 `versions[src]` 的 `curves_per_track[15]`(含每段 `origin`/`locked` 标记)与 `pan_curve` 到 `dst`;**不复制 `name`**(保留目标名);**零 gesture**(ADR-005)。完成后经 `scvb.segments`(`reason:"copyVersion"`)回推段表、经 `scvb.state` 回推 `versions[dst].pan_curve` 与 `empty` 标志。 |
| 拒绝态 | PRINT 态 → `{rejected:"printing"}`(03 §5.3 前置校验);`src == dst` → `{ok:false, reason:"badArg"}` |
| 撤销 | **是**(05 §1.3;UI 确认框末句「可撤销(Ctrl+Z)」) |
| 线程/频率 | [M];用户确认后触发 |
| 真源 | 05 §1.4 / §2.1 ③ |

### 1.12 `beginParamGesture(id)`

### 1.13 `setParam(id, value)`

### 1.14 `endParamGesture(id)`

三者为**同一 gesture 通道**的三段式,合并定义:

| 项 | 定义 |
|---|---|
| 参数 | `id: string` = ParamID;`value`(仅 `setParam`)= **参数真实工程值**(与 params-v0「范围」列同单位,**非归一化**;C++ 侧负责 normalize) |
| 可驱动 ParamID 全集 | 全局三件:`width`(0..150 %,默认 100)、`ms_balance`(-100..+100,默认 0)、`lead_select`(int 0..15,step 1,默认 0)<br>每轨(**当前激活版本** `v = global.version_active`):`v{v}_t{t:02d}_width`(0..100 %,默认 100)、`v{v}_t{t:02d}_freeze`(int **0..3**,step 1,bit0=pan / bit1=vol,默认 0)<br>`t` = `01`..`15`(两位零填充) |
| **不在**本通道 | 轨 `pan` / `vol` 手动值 —— 走 `setTrackManual`(§1.16);`v{v}_t{t:02d}_pan` / `v{v}_t{t:02d}_vol` 由引擎打印头驱动,**UI 不直写**(UI 从不对这两个 ParamID 调 `setParam`,§0.5 防回环纪律原样成立)。**注**:native 侧处理 `setTrackManual` 时**会**落一次这两个参数(带 gesture,见 §1.16 与变更文档 `20260825-t37-r3-track-manual-param-plane.md`)。**[J85] 起 native 的写入面按通道分叉**:未冻结的手动接管通道写「曲线真身 + 参数面」,**冻结通道只写参数面、不碰曲线**(变更文档 `20260826-j85-freeze-param-plane.md`)—— 变的一直是 native 写入面,UI 侧纪律不变 |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}`(`id` 不在上表时,**不得静默忽略**) |
| 语义 | 三段式转发到 [M] gesture(ADR-006):`beginParamGesture` → 若干次 `setParam` → `endParamGesture`;可被 DAW 录制。写入的一律是**当前激活版本**对应参数。`freeze` 的两枚 UI 开关**写同一参数**的两个位(J65);mock 后端须有对应参数状态(§0.7)。 |
| 拒绝态 | 无(PRINT/ARMED 下照常允许——这是宿主可录的用户操作面) |
| 撤销 | 否(自动化参数不入插件 UndoManager) |
| 线程/频率 | [M];拖动期间 `setParam` 由 UI 侧节流(建议 ≤50Hz),`begin`/`end` 各一次 |
| 真源 | 05 §1.4(v2.1/J65 全集);01 §6.3「仅全局三件」为 J65 之前的旧文,不采纳(§8.3) |

### 1.15 `setChannelConfig(ch, patch)`

| 项 | 定义 |
|---|---|
| 参数 | `ch: 1..15`;`patch`(**全部字段可选,只写给出的字段**):<br>`{ enabled?:bool, label?:string(≤24 字符), priority?:0..10(int), lead_lock?:bool, lead_vol_exempt?:bool, participate_in_auto_pan?:bool, pair_id?:0\|1..7 }` |
| 返回 | `{ok:true}` 或 `{observer:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 `channels[ch]` state —— **配置类唯一真源写入点**(ADR-004)。`participate_in_auto_pan` 默认值:**未显式设置一律 true**(J83 取代 J60 的按源声道推导 —— `source_channels` 来自轨道总线布局而非素材声道数,mono 素材放在 stereo 轨上就报 2;排除权在轨道页每轨的开关)。`pair_id=0` = 无配对,1..7 = 配对组(15 轨最多 7 对,J59)。写入后 `config_seq+1` 并经 `scvb.state` 回推 + ctrl 广播区刷新(Input 远程视图经 `scvb.config` 看到)。**`config_seq` 是 ctrl 广播区的整体版本号、不是本函数的调用计数**——广播区任一字段变化都会 bump,口径见 §4.3 字段纪律。 |
| **不可写字段** | `source_channels`(1\|2)为 **Input 实测检测值**,只读、经 `scvb.state.channels[].source_channels` 下推;**`auto_pan`/`auto_vol` 已删除**(J65,改由每轨 `freeze` 自动化参数承载)。patch 含上述键 → `{ok:false, reason:"badArg"}` |
| 拒绝态 | 只读观察态(`outputReadOnly=true`)下全 UI 写控件 disabled;C++ 侧收到写入返回 `{observer:true}` 且不改 state |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.2;字段集 params-v0 §二 `channels[15]` |

### 1.16 `setTrackManual(ch, panOrVol, value)`

| 项 | 定义 |
|---|---|
| 参数 | `ch: 1..15`;`panOrVol: "pan" \| "vol"`;`value: f32`(pan:-100..+100;vol:-24..+12 dB) |
| 返回 | `{ok:true, replacedSegments:int, replacedLocked:int}` 或 `{observer:true}` 或 `{ok:false, reason:"badArg"}`(`ch`/`panOrVol` 非法,或 `value` **非有限**——NaN/±Inf 一律拒绝,不得静默夹取:冻结维度上这个数就是 DSP 的音频目标值)。**`replacedSegments`/`replacedLocked` 只对手动接管通道有意义;冻结通道恒 0**(确实没替换任何段,如实统计,[J85] 变更文档裁定②) |
| 语义 | 轨 `pan`/`vol` 的手动静态值。**[J85] 按调用时该维度的 `freeze` 位分成两条通道,写入面不同**(变更文档 `20260826-j85-freeze-param-plane.md`,PR #106):<br>**① 冻结通道**(该维度 `freeze` 对应位 = **1**)—— 静态值**只**写**当前激活版本**对应的 `v{v}_t{t:02d}_pan` / `_vol` 参数,**曲线真身一个字节都不动**、不产生任何段。**为什么不许写曲线**:整表烘焙成全时限常值之后,解冻回读曲线读到的仍是那条常值段,而重分析按 ADR-008 v1.1 不覆盖 `origin=user` 段 —— 于是**一次冻结即永久锁死**,pan 再也回不到引擎分析曲线上(v5.3 A2 实测)。冻结按定义是**可逆的临时接管**,「解冻即回引擎分析曲线继续运动」是它的语义本身。<br>**② 手动接管通道**(该维度 `freeze` 对应位 = **0**)—— 用户主动「设为手动」。**编码 = 04 §1.5 方案 A**:向**当前激活版本**该轨曲线写入**覆盖全时间线的单段常值**,段标 `origin=user_edited`(J34),**不新增任何 state 字段**;写入常值段之后**追加**把同一个值写进同一对参数。重分析按 ADR-008 v1.1 不覆盖这条常值段(**只有本通道**会产生它);**会连 `locked` 段一并替换** —— J34 的 locked 保护只约束重分析。<br>**两条通道都落参数面、都包 `beginChangeGesture()` / `endChangeGesture()`**(变更文档 `20260825-t37-r3-track-manual-param-plane.md`,PR #87)。**为什么必须落参数面**:`DspArbiter` 对**冻结**维度读的是 host 参数而**不读曲线**,打印器对冻结车道也只把参数当前值重写成平直线(#68/J78)—— 冻结维度上没有任何读者会去看曲线。**为什么必须包 gesture**:裸 `setValueNotifyingHost` 在宿主看来是一次没有起止的孤立写入,Cubase 这类宿主要么记成孤立自动化点、要么在 Read 档下当场把值顶回去(那样这条写入根本不生效)。原文「零 gesture」以本条为准作废 —— 其原意「不要为手动值制造一串连续写入」仍然满足:UI 侧松手才发一次(`MANUAL_COMMIT_MS` 300ms 防抖),一次编辑 = 一对 begin/end。<br>**两条通道都按 §2.8 回推**:写入后经 `scvb.segments`(`reason:"trackManual"`)回推该轨**当前**段表 —— 冻结通道段表没变也照发(reason 枚举闭合不受影响;UI 用这一帧作为「已落地」信号清本地乐观值,不发会让乐观值挂死)。回推**之前**同步补一帧 `scvb.params`,两帧同拍到达 —— 冻结通道的新值只在参数面上,晚一拍会让 UI 先丢乐观值再读到旧参数值(旋钮回弹)。`value` 越界按 §1.16 的 `value` 域夹取,**非有限值不夹取、直接 badArg**。版本切换会换出另一版本的手动值。 |
| 拒绝态 | 只读观察态 → `{observer:true}` |
| 撤销 | **按通道分叉([J85])**。<br>**冻结通道:否** —— 它不产生任何 CRVS 变更,**一步都不往插件 UndoManager 里压**。⚠ 代价记在此处,不静默:用户在冻结态调了 pan 再按 Ctrl+Z,弹掉的是**上一笔不相干的 CRVS 事务**,看到的变化与刚做的操作无关。不压「空事务」是刻意的 —— 那只会让 Ctrl+Z 变成「按一下没反应、再按一下跳掉一笔旧编辑」。冻结通道唯一改的是自动化参数,而 §0.9 已明确「自动化参数不入插件 UndoManager」。<br>**手动接管通道:是**(仅回滚曲线真身)。⚠ 撤销事务(`commitCrvsTransaction`)只快照/还原 CRVS 段表,**参数面不回滚**。这是有意的:参数面是宿主的自动化面,插件自己的 UndoManager 去回滚它会与宿主撤销栈打架。<br>若将来要让冻结中调整也可撤销,唯一正路是走**宿主 gesture**,不是插件 undo |
| 线程/频率 | [M];每轨首次调用前 UI 须弹一次性行内确认(**无 `origin=auto` 前置条件**,每轨每会话一次;05 §2.2 R3)。⚠ **已知文案不一致(待裁定)**:`tracks.manualOverwriteConfirm`「将以固定值替换该轨的全部分段结果,可撤销」对**冻结通道**两句都不成立(不替换任何段、不入撤销栈)—— 是否改为「冻结维度不弹」或按通道分叉文案,涉及 05 §2.2 R3「无条件弹」的语义 + i18n 三语同改,记在变更文档 `20260826-j85-freeze-param-plane.md` 裁定② |
| 真源 | 05 §1.4 / §2.2;编码 04 §1.5 |

### 1.17 `setPanCurve(points)`

| 项 | 定义 |
|---|---|
| 参数 | `points`: 数组(**整表提交**,≤16 点),元素 `{ angle:f32(-100..100), gain_db:f32(建议显示域 ±12), shape:"bell"\|"shelf"\|"cut", q:f32(>0; shape="cut" 时承载 slope: 6\|12\|18\|24 dB/oct,默认 12;bell/shelf 时即 Q), side:"out"\|"left"\|"right"(默认 "out") }` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写**当前激活版本**的 `pan_curve.points[]`(params-v0 v2.0:`pan_curve` 属 `versions[2]`,J07/J59)。整表语义 = 给定数组即全量替换,**不做点级增删接口**(`setPanCurvePoint`/`removePanCurvePoint` 已归并,§8)。**写入后经 `scvb.state` 回推 `versions[active].pan_curve`**(下行落点见 §1.1/§2.1)——曲线编辑器渲染既有点集的唯一数据源。`side` 默认 `out`,方向由 `sign(angle)` 决定;`shape∈{shelf,cut} ∧ side="out" ∧ |angle|<5` 的组合在 UI 侧自动改选 left/right,数据层若仍出现按 02 §7.1 确定性回退(`angle≥0`→right,`angle<0`→left),**不报错**。 |
| 拒绝态 | 点数 >16 或字段越界 → `{ok:false, reason:"badArg"}` |
| 撤销 | **是** |
| 线程/频率 | [M];`pointerup`/工具条变更后提交,**节流 1 次/gesture,不逐帧下发** |
| 真源 | 05 §1.4 / §6.2 |

### 1.18 `setVadParams(p)`

| 项 | 定义 |
|---|---|
| 参数 | `p`(全部字段必填):`{ threshold_db:f32, hysteresis_db:f32, hangover_ms:int, padding_pre_ms:int(默认 120), padding_post_ms:int(默认 200) }`(J23) |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `analysis.vad`。**两段式(04 §1.2 逐字)**:<br>**拖动档** —— 每次调用只触发**即时重判决**并回发 VAD/边界**预览**(非破坏:不写 `versions` 曲线;目标 <50ms);<br>**松手档** —— UI 停止调用后由 **C++ 侧 300ms 防抖**自动跑完整流水线,**仅改写 `origin=auto` 且未 `locked` 的段**(J34),用户段逐字节不动;完成后回发 `scvb.segments`(`reason:"vad"`,含 diff 摘要)。<br>抑制条件**只有** PRINT 态或分析进行中(J47):抑制时不自动应用,UI 退回显式「应用到分段(重分析)」按钮 = `analyze(scope)`。 |
| 拒绝态 | 字段缺失/越界 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否(阈值本身不入栈;其触发的段改写属分析产物) |
| 线程/频率 | [M] 预览同步;松手档 [M] 防抖计时 → [W] 流水线 |
| 真源 | 05 §1.4 / §2.3;两段式 04 §1.2 |

### 1.19 `setSegmentation(p)`

| 项 | 定义 |
|---|---|
| 参数 | `p`:`{ mode:string, sensitivity:f32, min_segment_ms:int }` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `analysis.segmentation`;**两段式与抑制条件同 §1.18**(拖动=预览,松手 300ms 防抖跑流水线,仅 `origin=auto` 且未 `locked` 段);完成后 `scvb.segments`(`reason:"segmentation"`)。 |
| 拒绝态 | 同 §1.18 |
| 撤销 | 否 |
| 线程/频率 | 同 §1.18 |
| 真源 | 05 §1.4 / §2.3 |

### 1.20 `setTransitionRamp(ms)`

| 项 | 定义 |
|---|---|
| 参数 | `ms: f32`,**范围 20..300**(02 §0.3 真源),默认 80 |
| 返回 | `{ok:true}` |
| 语义 | 写 state `analysis.transition_ramp_ms`(ADR-010 段间过渡)。越界由 C++ 夹取到 [20,300] 并在 `scvb.state` 回推夹取后的值。 |
| 拒绝态 | 无(夹取) |
| 撤销 | 否 |
| 线程/频率 | [M];拖动节流 |
| 真源 | 05 §1.4 / §2.1 ④ |

### 1.21 `setAnalysisConfig(patch)`(J69;函数名见 05 §2.4 数据来源列,**两组枚举字符串值为 T25 定名,待 DeepSeek 评审确认**)

| 项 | 定义 |
|---|---|
| 参数 | `patch`(字段可选,只写给出的字段):<br>`{ loudness_mode?: "kw_integrated" \| "rms" \| "peak_dbfs", center_slot_policy?: "priority_queue" \| "lead_exclusive" \| "even_spread" }` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `analysis.loudness_mode` / `analysis.center_slot_policy`(03 文档层新增字段,**零修宪**)。**零 gesture**。义项与默认值以 05 §2.4 / J69 为准:`loudness_mode` 默认 `kw_integrated`(K 加权段积分;词条正名不用 "LUFS-S"),`center_slot_policy` 默认 `priority_queue`(按优先级排队)。改后**不自动重分析**,UI 显示「改后需重分析」注记。三档数学定稿以 02 §4.3 / §5.6 为准。 |
| 拒绝态 | 枚举值不在表内 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否(配置类) |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §2.4(J69 授权立项)。**函数名 `setAnalysisConfig` 在 05 §2.4「第二响度指标」块的数据来源列已有字面出处**(`setAnalysisConfig({loudness_mode})`,括注「由 01/03 细化时定名并进 T25 冻结契约」)——改名须同步回改 05 §2.4;T25 补白的是 `patch` 形状与**两组枚举字符串值 + 默认档**(见 §9.2) |

### 1.22 `editSegment(ch, op, payload)`

| 项 | 定义 |
|---|---|
| 参数 | `ch: 1..15`;`op ∈ "move_boundary" \| "split" \| "merge" \| "set_values" \| "set_locked"`;`payload` 逐 op 见 §5.4 |
| 返回 | `{ok:true}` 或 `{observer:true}` 或 `{ok:false, reason:"badArg"}` 或 `{ok:false, reason:"notAdjacent"}`(成功时结果段表经 `scvb.segments`(`reason:"edit"`)回推;`split`/`merge` 后的段下标以事件为准) |
| 语义 | 段级编辑,作用于**当前激活版本** `versions[active].curves_per_track[ch].segments[]`(曲线真身)。边界/值改动 → 该段 `origin=user_edited` **且自动 `locked=true`**(J34/J44,05 §2.3);`set_locked` 单独切换 `locked`。重分析不覆盖 `origin≠auto` 段(ADR-008 v1.1)。**零 gesture、零自动化污染**(与版本复制同级别的纯 state 操作,ADR-005)。PRINT 态编辑**允许**(改的是曲线不是参数),当前正在发射的段值在段边界处才切换(段间 ramp,ADR-010)。 |
| 拒绝态 | `segIdx` 越界 / `payload` 字段缺失 → `{ok:false, reason:"badArg"}`;`merge` 两段不相邻 → `{ok:false, reason:"notAdjacent"}`;只读观察态 → `{observer:true}` |
| 撤销 | **是**(全部 5 个 op) |
| 线程/频率 | [M];边界拖拽**释放才发**(拖动中纯 UI 本地重绘) |
| 真源 | 05 §1.4 / §2.3a |

### 1.23 `recaptureArm(tracksMask, startS, endS, autoStop?)`

| 项 | 定义 |
|---|---|
| 参数 | `tracksMask: u16`;`startS: f64`;`endS: f64`;`autoStop?: bool`(默认 **false**,T25 补白) |
| 返回 | `{ armed:bool, tracksMask:u16, startS:f64, endS:f64, reason?: "noTracks"\|"noSelection"\|"readOnly"\|"noTimeline" }`(`armed=false` 时必带 `reason`) |
| 语义 | 局部重采集布防(轨×区间失效单元,ADR-007)。`autoStop=true` = 「播完自动停」:区间播毕自动 OFF(04 §4.2)。布防态同时进 `scvb.state.recapture`,重开面板/切 tab 后可恢复显示。**UI 以返回值而非乐观假设点亮布防 badge**。布防**只门控采集**:输出引擎恒按 `global.range ∩ coverage` 工作,与本次选区无关(03 §2.1,04 §4.2)。`tracksMask=0` → `reason:"noTracks"`;`startS>=endS` → `reason:"noSelection"`。传 `tracksMask=0 ∧ startS=endS=0` 视为**撤销布防**并返回 `{armed:false}` 无 `reason`。 |
| 拒绝态 | `armed:false` + `reason`(见上);只读观察态 → `reason:"readOnly"` |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §2.3;04 §4.2 |

### 1.24 `clearCoverage(tracksMask, startS, endS)`

| 项 | 定义 |
|---|---|
| 参数 | `tracksMask: u16`;`startS: f64`;`endS: f64` |
| 返回 | `{ok:true, clearedS:f64}`(实际清除的总时长秒数,供 UI 反馈)或 `{observer:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 显式清除选中轨×区间的采集特征数据(04 §1.1「被用户显式清除」的唯一入口),UI 侧须二次确认。清除后经 `scvb.captureProgress`/`scvb.state` 回推覆盖率变化;波形侧由 UI 重新 `requestWaveform`。 |
| 拒绝态 | `tracksMask=0` 或 `startS>=endS` → `{ok:false, reason:"badArg"}`;只读观察态 → `{observer:true}` |
| 撤销 | 否(**不入撤销栈**:采集数据不属曲线真身) |
| 线程/频率 | [M];用户确认后触发 |
| 真源 | 05 §1.4 / §2.3 |

### 1.25 `undo()` / 1.26 `redo()`

| 项 | 定义 |
|---|---|
| 参数 | 无 |
| 返回 | `{ok:bool}`(false = 撤销/重做栈为空) |
| 语义 | 插件自有 UndoManager(03 §5.3),覆盖 §0.9 左列的四类操作。执行后受影响面回推:段表经 `scvb.segments`(`undo()` → `reason:"undo"`,`redo()` → `reason:"redo"`,§2.8),`pan_curve` 与其余 state 面经 `scvb.state`。**不触碰宿主撤销栈**;UI 侧须 `preventDefault` 阻止冒泡。 |
| 拒绝态 | 无 |
| 撤销 | 不适用 |
| 线程/频率 | [M];键盘触发 |
| 真源 | 05 §1.3 / §1.4 |

### 1.27 `requestWaveform(ch, startS, endS, cols)`

| 项 | 定义 |
|---|---|
| 参数 | `ch: 1..15`;`startS: f64`;`endS: f64`(`startS < endS`);`cols: int`(1..4096,视口像素列数) |
| 返回 | `{ minDb:f32[cols], maxDb:f32[cols], vad:u8[cols], covered:u8[cols], stale:u8[cols], passId:u32[cols], valleys:f64[] }` 或 `{ok:false, reason:"badArg"}`(无数组字段) |
| 语义 | 拉取式下采样(canvas 分块渲染)。C++ 从特征流(`kw_ms`/`peak`)降采样出每列 min/max dB 包络、VAD 判决位、覆盖位、stale 位与 passId(不同采集轮次底色微差)。**`valleys[]`(T25 新增字段,见 §9.2)** = 该区间内的**能量谷时间点列表(秒,升序)**,供边界拖拽吸附(05 §6.3 明文「由 C++ 在 `requestWaveform` 附带谷点列表」但未定字段名)。未覆盖列:`covered=0` 且 `minDb=maxDb=-INF 哨兵 -160`。 |
| 拒绝态 | 参数越界(`ch`/`cols` 越界、`startS >= endS`) |
| 撤销 | 否 |
| 线程/频率 | **[M] 受理,结果以 Promise 异步 resolve**(降采样可在 [W] 完成后回 [M] 兑现——每次调用要从特征环降采样 6 个 `cols` 长数组 + 谷点检测,视口约 1000 列 × 每可见轨一次,不宜在消息线程同步做完);**一次调用一次 resolve,绝不进事件流**(01 §6.4)。UI 侧按视口变化拉取,静止 120ms 后取新块,块内 LRU 缓存归 UI(05 §6.3)。**定案(DeepSeek native 评审,2026-08-16):异步**——JUCE 8 `withNativeFunction` 的 Promise completion 可任意线程回调,降采样放 [W] 完成后回 [M] 兑现;native 须保证每次调用**恰好一次** completion(含 `badArg`),UI promise 永不悬挂 |
| 真源 | 05 §1.4 / §6.3;01 §6.4 |

### 1.28 `setUiScale(f)` / 1.29 `commitUiScale()`

| 项 | 定义 |
|---|---|
| 参数 | `setUiScale(f: f32)` = 缩放**因子**(Output 档位 `0.5/0.65/0.8/1/1.25/1.5/2`);`commitUiScale()` 无参 |
| 返回 | `setUiScale`:`{ok:true}` 或 `{ok:false, reason:"badArg"}`;`commitUiScale`:`{ok:true}` |
| 语义 | `setUiScale` = 立即预览(C++ `setSize(round(W*f), round(H*f))`,`setResizable(false,false)`);`commitUiScale` = 10 秒防呆确认通过后**落盘系统级全局默认**。取消/超时/关窗回退,不污染新实例。档位与设计盒常量唯一真源 = `web/shared/design-box.js`(Output 1180×780 / Input 460×560),构建期生成 `src/core/DesignBox.h`,**C++ 不得二次硬编码**。 |
| 拒绝态 | `f` 不在档位表 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.2 / §1.4;01 §6.1。**01 §6.2 写作 `setUiScale(pct)` 为异名**,本契约取 05 的因子口径(§8.3) |

### 1.30 `setLang(code)`

| 项 | 定义 |
|---|---|
| 参数 | `code: "zh" \| "en" \| "fr"` |
| 返回 | `{ok:true}` |
| 语义 | 写 state `ui.language`;UI 侧同步 `document.documentElement.lang` 与全部 `aria-label`。未知 code 由 C++ 回退 `"zh"` 并在 `scvb.state` 回推实际值(05 §5 `dict(lang)` 未知回退 zh)。**副作用([J81],变更文档 `20260825-t37-r3-misalign-semantics.md` 追加节)**:同时置 `ui.lang_chosen = true` 与系统级全局默认位 `lang_chosen_global`(**不回写 state**),二者语义与 `guide_seen`/`guide_seen_global` 完全同构,用于判定首启语言选择卡是否还要弹。**不新增桥函数** —— web 启动时的语言回填走 `setLang(..., {push:false})` **不经桥**,所以「桥的 `setLang` 被调用过」正好等价于「用户显式选过语言」。`ui.language` 本身不能兼任该标记:它默认 `"en"`,「从没选过」与「用户就是选了英文」不可区分。 |
| 拒绝态 | 无(回退) |
| 撤销 | 否 |
| 线程/频率 | [M] |
| 真源 | 05 §1.4 / §5 |

### 1.31 `setActiveTab(tab)`

| 项 | 定义 |
|---|---|
| 参数 | `tab: "master" \| "tracks" \| "wave" \| "settings"`(**T25 定名**,对齐 05 §0.1 的 `tab-master.js`/`tab-tracks.js`/`tab-wave.js`/`tab-settings.js`) |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `ui.active_tab`(重开面板恢复上次 tab)。**tour 期间的跨 tab 翻页为纯 UI 本地态,不调用本函数**;tour 结束时按结束位置同步一次(05 §2.6)。 |
| 拒绝态 | 未知值 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否 |
| 线程/频率 | [M] |
| 真源 | 05 §1.4 / §2.6 |

### 1.32 `setGuideSeen(seen, alsoGlobal = true)`(J50/J50a)

| 项 | 定义 |
|---|---|
| 参数 | `seen: bool`;`alsoGlobal: bool`(**默认 true**) |
| 返回 | `{ok:true}` |
| 语义 | 首启九条红字引导页「不再显示」勾选的**唯一写入口**:写 state `ui.guide_seen`(params-v0 v1.2 [J50]);`alsoGlobal=true` 时**同步写系统级全局默认文件**(复用 `commitUiScale` 已有的全局配置通道)。**全局默认仅作首启弹出判定,不回写 state**(新工程 `guide_seen=false` 时先读全局默认决定是否弹出,「不再显示」承诺跨工程成立)。`requestInitialState()` 的 ui 快照含 `guide_seen` 与全局判定位 `guide_seen_global`。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M];勾选时一次 |
| 真源 | 05 §1.4(R4)/ §2.0;01 §6.3 |

### 1.33 `setTourSeen(seen, alsoGlobal = true)`(J62)

| 项 | 定义 |
|---|---|
| 参数 | `seen: bool`;`alsoGlobal: bool`(**默认 true**) |
| 返回 | `{ok:true}` |
| 语义 | 交互式引导 tour **完成**或询问步点「暂不」的**唯一写入口**(两者均置 true):写 state `ui.tour_seen`(params-v0 v2.0 [J62],**独立于 `guide_seen`**);`alsoGlobal=true` 时同步写系统级全局默认(J50a 镜像,复用 `commitUiScale` 通道,不回写 state)。`requestInitialState()` 的 ui 快照含 `tour_seen` 与全局判定位 `tour_seen_global`。**tour 本体为纯 UI 层,除本函数外不新增任何桥函数**;设置页「重看引导」重启 tour 亦不调用本函数(`tour_seen` 已置位也可重看)。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M];完成/婉拒时一次 |
| 真源 | 05 §1.4 / §2.6;01 §6.3 |

### 1.34 `confirmPrintGuard()`(统筹裁定 A-29)

| 项 | 定义 |
|---|---|
| 参数 | 无 |
| 返回 | `{ok:true}`(**幂等**:`pending` 已为 false 时仍返回 `{ok:true}`) |
| 语义 | 加载守卫(04 §5.3)「继续引擎驱动」按钮的**唯一确认入口**:置 `print_guard.pending=false`(运行时态,不入 state chunk、不随工程持久化),本工程会话内横幅⑦不再出现;确认前引擎行为止于 ARMED(§1.3),确认后若满足 PRINT 三与条件(输出 ON ∧ 播放 ∧ 在 range 内)即恢复正常 PRINT。**零 gesture**;变更经 `scvb.state.print_guard` 回推。成例:R4 `setGuideSeen`(用户可见承诺须有唯一写入口)。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M];横幅⑦按钮触发,每工程会话至多一次有效 |
| 真源 | 04 §5.3 / 05 §2.0 横幅⑦ / §2.5 `print-guard` 验收(语义已定、函数缺位);**函数名为 T25 授权增量(统筹裁定 A-29),待 DeepSeek 评审 + 用户批准;若否决,回退方案 = 复用 `setOutputEnabled(true)` 作确认信号并回改本节** |

### 1.35 `setMasterChartMode(mode)`([J75] T43)

| 项 | 定义 |
|---|---|
| 参数 | `mode: "distribution" \| "trajectory"` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `ui.master_chart_mode`(Tab1「声像 / 音量分布」卡片的视图态;随工程持久化,重开面板恢复上次视图)。`distribution` = 既有声像/音量分布图;`trajectory` = 新增 pan 轨迹图(x = 工程时间线,y = pan 角度域)。**不触发**任何分析/打印/参数写入;经 `scvb.state` 回推。 |
| 拒绝态 | 未知值 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否(与 `setActiveTab` 同 —— UI 偏好不进 undo 栈) |
| 线程/频率 | [M];用户点一次调一次 |
| 真源 | 05 J75 A(T43 双视图);变更文档 `docs/contract-changes/20260825-master-chart-mode.md` |

### 1.36 `exportSuggestions(scope)`([J81] T41;变更文档 `20260825-export-suggestions.md`)

| 项 | 定义 |
|---|---|
| 参数 | `scope`:`{versions?: "active" \| "all", tracksMask?: u16, startS?: f64, endS?: f64}`;**整体可省**(= 全默认) |
| 默认 | `versions:"active"`(当前激活版本)、`tracksMask:0x7FFF`(全 15 轨,**bit15 保留 0**,§9.2)、时间窗不限 |
| 返回 | `{ok:true, rows:int, path:string}` 或 `{ok:false, reason:"badArg"\|"cancelled"\|"noData"\|"ioError"}` |
| 语义 | 从 **CRVS 曲线真身**取段表 → 按冻结 13 列定义生成行集 → 序列化 CSV → **弹系统保存对话框**由用户选路径 → 落盘。**不写 state、不发 gesture、不动参数、不入撤销栈**。**为什么必须是 native**:① JUCE WebView 里 `<a download>` 与 `Blob` 保存都不通,只有 C++ 侧能弹系统保存对话框;② 跨版本导出(`versions:"all"`)UI 拿不到 —— §2.8 `scvb.segments` 一次只下发一个版本、§2.2 `scvb.params` 只带激活版本的 60 个参数,**全版本的真相只在 C++ 的 CRVS 里**。纯计算层 = `src/core/export/SuggestionExport.{h,cpp}`(`scvb::suggest::buildRows/toCsv`,不链接 JUCE、离线可单测) |
| 冻结列(13,顺序不可改、不可增删) | `track_index, track_label, source_channels, version, version_name, segment_index, t0_sec, t1_sec, pan, vol_db, width, origin, locked` |
| 文件形制 | UTF-8 **带 BOM**(否则 Excel 打开中文轨名乱码)、换行 **CRLF**(含最后一行)、含表头行、字段按 RFC 4180 转义(含 `,` `"` CR LF 的字段整体加双引号、内部双引号翻倍)。负零一律归一成正零 |
| 列口径要点 | `width` 列 stereo 轨有值、**mono 轨留空**——不是 0,0 是「收成 mono」的有效值([J57]),语义冲突;`t0_sec`/`t1_sec` 3 位小数;`pan`/`vol_db` 1 位小数;`origin`/`locked` 与 T19 state 编码逐字一致。⚠ native 装 `ExportInput` 时 `widthPercent` **必须逐格装满**:未装的格是哨兵 `kWidthUnknown`(负值)→ 该行 `width` 留空;零值初始化会让「漏装」静默产出 `0.0`,而 0 在 stereo 轨上是有效建议,等于替用户做了一个语义相反的决定 |
| 时间窗语义 | `startS`/`endS` 只做**筛选**,命中的段 `t0_sec`/`t1_sec` **不裁剪** —— 截半段会给出一个没人建议过的区间 |
| 拒绝态 | `badArg`(versions 不在两值内 / `endS ≤ startS`)、`cancelled`(用户关掉保存对话框)、`noData`(scope 内零段)、`ioError`(写盘失败) |
| 撤销 | 否(只读导出) |
| 线程/频率 | `[M]`;用户点一次调一次。`processBlock` 与本函数无关 |
| 真源 | 07 T41 卡 / 11 §4.2.3 通路 B(B1 数值表 / B2 CSV 导出);U12「进 v1 主线」;变更文档 `docs/contract-changes/20260825-export-suggestions.md`(PR #91) |

---

## 2. Output —— events(C++ → UI)

共 **9** 个。全部经 `backend.addEventListener(name)` 订阅;推送纪律见 §0.4/§0.6。

### 2.1 `scvb.state`

| 项 | 定义 |
|---|---|
| 频率 | **变化时**(diff-then-emit;`mBridgeReady` 后首帧必发一次全量) |
| 载荷 | `{ full:bool, config_seq:u32, group_id:1..8,`<br>`  global:{capture_enabled:bool, output_enabled:bool, version_active:1..2, range:{mode:"follow"\|"daw_loop"\|"manual", start_s:f64, end_s:f64}},`<br>`  analysis:{vad:{threshold_db, hysteresis_db, hangover_ms, padding_pre_ms, padding_post_ms}, segmentation:{mode, sensitivity, min_segment_ms}, transition_ramp_ms:f32, loudness_mode, center_slot_policy},`<br>`  channels:[15 × {enabled, label, source_channels:1\|2, participate_in_auto_pan, priority, lead_lock, lead_vol_exempt, pair_id}],`<br>`  versions:[2 × {name:string, empty:bool, pan_curve:{points:[{angle:f32, gain_db:f32, shape, q:f32, side}]}}],`<br>`  features:{embedded:bool, bytes:u64},`<br>`  ui:{scale, language, active_tab, master_chart_mode, guide_seen, tour_seen, lang_chosen?:bool},`<br>`  print_guard:{pending:bool},`<br>`  recapture:{armed:bool, tracksMask:u16, startS:f64, endS:f64, autoStop:bool},`<br>`  analysis_run:{running:bool, progress?:f32} }` |
| 字段纪律 | `full:true` = 全量快照;`full:false` = 增量(只含变化子树,UI 做深合并)。**`print_guard` 与 `recapture` 为运行时态**:不入 state chunk、不随工程持久化(04 §4.2/§5.3)。`ui.guide_seen`/`ui.tour_seen` 为持久化字段(params-v0 v1.2 [J50] / v2.0 [J62];编码落点 03 §6.1 PRMS)。`analysis_run` 为运行时态(01 §6.4「进度经 `scvb.state` 下行」的落点,**T25 定名**,§9.2)。**段表(`versions[].curves_per_track[].segments[]`)与逐帧特征不进本事件**(前者走 `scvb.segments`,后者走 `requestWaveform`);**但 `versions[].pan_curve` 进本事件**——它是整表提交的小结构(≤16 点,params-v0 J07),是 05 §6.2 pan 曲线编辑器渲染既有点集的唯一下行落点(**T25 定名的下行落点**,§9.2),`setPanCurve`/`setVersionActive`/`copyVersion`/`undo`/`redo` 后一律经本事件回推。本事件的字段集与 §1.1 快照的 state 子树**必须一致**(§1.1 语义行)。 |
| UI 消费 | 全 UI 的单向渲染源;`print_guard.pending` → 05 §2.0 横幅⑦;`recapture.armed` → 三处布防 badge;`analysis_run.running` → 分析按钮菊花 / 「正在应用…」态 / J47 抑制判定 |
| 真源 | 05 §1.4;字段集 params-v0 §二 |

### 2.2 `scvb.params`

| 项 | 定义 |
|---|---|
| 频率 | **25 Hz**(节流;**值未变不发**) |
| 载荷 | `{ values:{ "<ParamID>": <f32 工程值>, ... }, hostEcho:bool, full:bool, versionActive:1..2 }` |
| 字段纪律 | `values`(容器键为 **T25 定名**,§9.2)= **稀疏 diff**(只含本帧变化的 id,05 §1.4「`{id→value}` 稀疏 diff」);参数总数 **123**(声明;宿主可见 124,J59/J65),本事件覆盖面 = 全局三件 + **当前激活版本 60 个**(15 轨 × pan/vol/width/freeze)= 最多 63 个 id。**非激活版本参数不进本事件**;切版本后 C++ **全量重发**(`full:true`,`versionActive` 为新版本)。`hostEcho:true` = 本批来自宿主回吐/引擎打印(ARMED/PRINT),UI 灰显且**绝不回写**(§0.5)。工程值单位与 params-v0「范围」列一致(非归一化)。 |
| UI 消费 | Tab2 pan/width/vol/冻结的 follow 态、Tab1 Width/MS Balance/Lead Select、声像/音量分布图的唯一数据源 |
| 真源 | 05 §1.4(03 §3.5 移交);`full`/`versionActive` 为 T25 定名(§9.2) |

### 2.3 `scvb.conn`

| 项 | 定义 |
|---|---|
| 频率 | **~4 Hz**(diff-then-emit) |
| 载荷 | `{ channels:[15 × { slotState:0\|1\|2, heartbeatAgeMs:u32, heartbeatFresh:bool, capturing:bool, misalignCount:u32, srMismatch:bool, suspended:bool }], outputReadOnly:bool, generation:u32 }` |
| 字段纪律 | `slotState` 逐字照 ipc §1:`0=空闲 1=已声明 2=活跃`;**`heartbeatAgeMs`** = 自该 slot 最后一次心跳起的毫秒数(ipc §0 steady clock 口径;`slotState=0`/从未心跳时发 `0xFFFFFFFF` 哨兵表示「无数据」,**T25 定名**,§9.2);`heartbeatFresh` = **`heartbeatAgeMs ≤ 2000` 的派生布尔**(ipc §0 显示口径,J10;保留以便 UI 直接判色);`misalignCount` = 该轨**本次失准发作**内的缺口数 —— 该轨连续 1s(`kMisalignRecoverMs`)无新缺口即**归零**(变更记录:`docs/contract-changes/20260825-t37-r3-misalign-semantics.md`;原为「累计失准计数」,与本字段唯一的 UI 消费面自相矛盾 —— web 判据 `misalignCount > 0` 是无状态的「当前是否失准」,而累计值只增不减,抖一次就把失准横幅永久钉死、恢复健康也撤不下来)。**进程寿命累计值未丢**,改由 ctrl 段 `OutputGlobalInfo.gap_count[15]`(J09 全局小节)暴露;native 侧 `gapCount()`(累计)与 `misalignCount()`(本次发作)是两个独立入口;`srMismatch` = 该轨 Input 采样率与 Output 不一致(ipc §5,该轨禁用);**`suspended`** = 该轨**写方停着**(宿主在无信号段挂起 Input / 用户 bypass / 轨未激活;判据 = 写头冻结 ≥ `kSuspendStallMs` ∧ 走带在跑 ∧ 本段停滞期内确有饿读)。与 `misalignCount` **是两件事**:后者只数**真失准**(走带推进中的时间线缺口),本字段是**中性状态**,UI 用灰蓝提示而非 ⚠ —— 乐句间隙里宿主挂起 Input 属正常现象,用户自己 bypass 的更不需要警报(变更记录:`docs/contract-changes/20260825-v5-misalign-stall-semantics.md` 第二次修订);`outputReadOnly` = 本实例为**同组内**第二个 Output,进只读观察(ADR-002,J66 同组语义);**`generation`** = 本组 `RegistryHeader.generation`(ipc §1,每次覆盖式重新初始化 +1),**全组单值故置顶层而非每轨**(**T25 定名**,§9.2)。**`groups_online` 不在本事件**——独立事件 §2.4(01 §6.3 的 `groupsOnline` 并入 `scvb.conn` 为旧落点,§8.3)。 |
| UI 消费 | Header 连接摘要 pill(`N/15` 只统计 `slotState=2 ∧ heartbeatFresh` 的 slot,J01)、Tab2/Tab3 状态灯、失准 ⚠ 计数、只读横幅②、**Tab4 诊断区四项**(每轨 `heartbeatAgeMs` / `misalignCount` + 全组 `generation` + `config_seq`(取 `scvb.state` 顶层),05 §2.4)。**注**:`misalignCount` 改为「本次发作」语义后,诊断区若要展示**累计**失准,数据源应改取 ctrl 段 `OutputGlobalInfo.gap_count`;若展示的是「当前是否失准」,则沿用本字段即可 |
| 真源 | 05 §1.4 / §2.0 / **§2.4 诊断区**(四字段清单);ipc §0/§1/§5 |

### 2.4 `scvb.groups`(J70)

| 项 | 定义 |
|---|---|
| 频率 | **1 Hz**(探测频率;**变化才发**,首帧必发一次) |
| 载荷 | `{ groups_online: u8 }` —— 位图,bit0=组 A … bit7=组 H,置位 = 该组 registry 头有**心跳新鲜**(≤2000ms)的 OutputSlot |
| 数据面 | [M] 线程 1Hz **逐组判定**(01 §4.5 协议逐字):**`G == 本实例 group_id` 时直接读已映射的本组 `OutputSlot`**(`state==2 ∧ 心跳 ≤2000ms`),**不重复 `OpenFileMapping`**;**异组**才 `OpenFileMappingW(FILE_MAP_READ,…, "Local\\SynchainSCVB.v1.g{G}.registry")` → 只映射 `RegistryHeader` + `OutputSlot` → 判 `magic=='SCVB' ∧ abi==1 ∧ state==2 ∧ 心跳≤2000ms` → Unmap/Close(不保留句柄)。**绝不映射异组 audio/feat/ctrl 段,绝不写异组任何字节,绝不对异组 slot 做 CAS/claim**。探测失败(段不存在/映射失败/abi 不符)一律记 `false`,**绝不重试、绝不报错**——绿点缺失是可接受降级。 |
| 语义边界 | **纯展示位**:不参与 per-channel 健康判定、不参与 J12 直通仲裁、不影响 `connected_mask`、不影响只读观察判定(异组 Output 依旧不构成 O3,J66) |
| UI 消费 | **唯一消费者 = 组胶囊在线绿点**(Output §2.1 ⓪ / Input §3 组选择器)与 group-mismatch 引导副行 |
| 两侧一致 | **Input/Output 同名同载荷**(§4.4) |
| 真源 | 05 §1.4(落点与字段名真源)/ 01 §4.5(数据面) |

### 2.5 `scvb.meters`

| 项 | 定义 |
|---|---|
| 频率 | **30 Hz**(05 §1.4 逐字;**A-28**:逐类频率为准,基准 Timer 不入契约) |
| 载荷 | `{ tracks:[15 × {db:f32, peakDb:f32}], bus:{l:{db:f32, peakDb:f32}, r:{db:f32, peakDb:f32}} }` —— 容器键 `tracks`/`bus`/`l`/`r` 为 **T25 定名**(§9.2),值形状逐字照 05 |
| 字段纪律 | 每轨为**后置 gain** 电平;stereo 源按**双通道求和**口径(J57,BS.1770 多通道求和);地板 **-60 dB**(未连接/静音轨发 -60)。弹道常数在 UI 侧实现且与 Bridge 完全一致:fast-follow 回落 120 dB/s、peak-hold 2200ms 后 20 dB/s。 |
| UI 消费 | Tab2「音量 / 电平」合成件液柱与峰值线;总线电平 |
| 真源 | 05 §1.4 / §2.2 / §4 |

### 2.6 `scvb.playhead`

| 项 | 定义 |
|---|---|
| 频率 | **30 Hz**(05 §1.4 逐字;同 §2.5,**A-28**:逐类频率为准,基准 Timer 不入契约) |
| 载荷 | `{ timeS:f64, isPlaying:bool, loopStartS?:f64, loopEndS?:f64, inRange:bool }` |
| 字段纪律 | `loopStartS`/`loopEndS` 仅在宿主提供且有效(`kCycleValid`)时出现;缺失即字段不存在(**不发哨兵值**)。宿主仅提供 PPQ 时按 §1.8 同款 best-effort 近似换算。`inRange` = 播放头是否落在 `global.range` 内(`mode="follow"` 时恒 true——follow 无界,05 §2.1 ②)。UI 侧 rAF 插值平滑。 |
| UI 消费 | 播放头竖线、采集中/出范围提示、PRINT 态判定(`output_enabled ∧ isPlaying ∧ inRange`) |
| 真源 | 05 §1.4 / §2.1 |

### 2.7 `scvb.captureProgress`

| 项 | 定义 |
|---|---|
| 频率 | **播放中 2 Hz**(非播放不发) |
| 载荷 | `{ channels:[ { ch:1..15, addedRanges:[{startS:f64, endS:f64}], coveragePct:f32 } ] }` |
| 字段纪律 | **增量**:**`addedRanges`**(**T25 定名**,§9.2)= 自上一帧新增覆盖的区间(合并后)——它是宪法字段 `features.per_channel[].coverage_ranges[]`(params-v0 §二)的**增量投影**,与 state 真身**语义不同故不同名**(真身是全量区间表,本字段是本帧增量),按 §0.2 规则②取 lowerCamelCase;05 §1.4 该行原文为「每轨 `coverage_ranges` 增量」。`coveragePct` = 该轨在 `global.range`(follow 态取全时间线已分析域)内的覆盖百分比 0..100(**T25 定名**,§9.2,供 Tab3 轨头「覆盖率」显示)。仅包含**本帧有变化**的轨。 |
| UI 消费 | 泳道底部 2px 覆盖条实时延伸、Tab3 轨头覆盖率 |
| 真源 | 05 §1.4 / §2.3 |

### 2.8 `scvb.segments`

| 项 | 定义 |
|---|---|
| 频率 | **`mBridgeReady` 后首帧一次(`snapshot`)+ 分析/阈值变化/段编辑/手动常值写入/撤销/重做/版本切换/版本复制后**(事件驱动,非周期) |
| 载荷 | `{ version:1..2, reason:"analyze"\|"vad"\|"segmentation"\|"edit"\|"trackManual"\|"undo"\|"redo"\|"versionActive"\|"copyVersion"\|"snapshot",`<br>`  channels:[ { ch:1..15,`<br>`    segments:[ {segIdx:int, t0S:f64, t1S:f64, pan:f32(-100..100), volDb:f32(-24..+12), origin:"auto"\|"user_edited"\|"user_created", locked:bool, loudnessLufs:f32, openEnded:bool} ],`<br>`    stale:bool } ],`<br>`  diff:{ kept:int, changed:[ {ch:1..15, segIdx:int, panFrom:f32, panTo:f32, volDbFrom:f32, volDbTo:f32} ], added:int, removed:int } }` |
| `reason` 十值与触发面 | `analyze`←`analyze()`(§1.6);`vad`←`setVadParams` 松手档(§1.18);`segmentation`←`setSegmentation` 松手档(§1.19);`edit`←`editSegment` 五 op(§1.22);`trackManual`←`setTrackManual`(§1.16;**[J85] 冻结通道段表未变也照发** —— 枚举闭合说的是「改动段表的函数必须落在某个 reason 上」,不等于「没改动就不许发」;UI 用这一帧作为「已落地」信号清本地乐观值,不发会让乐观值挂死。该帧**之前**同步补一帧 `scvb.params`,两帧同拍);`undo`←`undo()`、`redo`←`redo()`(§1.25/§1.26);`versionActive`←`setVersionActive`(§1.9,换出另一版本段表);`copyVersion`←`copyVersion`(§1.11);**`snapshot`←`mBridgeReady` 后首帧必发**(§0.4 第 3 条,全部轨全量段表)。**枚举闭合**:任何改动段表的函数都必须落在本表某个 `reason` 上,新增函数须同批扩本枚举与 §7 manifest 的 `enums.segmentsReason`(**T25 定名**,§9.2)。 |
| 字段纪律 | `channels` 只含**受影响轨**(`reason:"snapshot"` 与 `reason:"versionActive"` 时为全部轨)。段表为该轨在 `version` 版本下的**完整段列表**(便于 UI 直接替换,不做段级增量)。`segIdx` = 段在该表内的 0 基下标,**每次事件后重新编号**,UI 的选中态须按事件重绑。`t0S`/`t1S` 为秒(state 真身为样本,换算在 C++ 侧,§0.2)。`loudnessLufs` = 段内积分响度(ADR-009,段检查器只读显示)。`diff.kept` = 本次「手动编辑/锁定段已保留」计数(05 词条 `wave.diffKept` 的 `{k}`)。`stale` = 该轨存在过期采集区间(fingerprint watchdog 语义预留)。**`openEnded`** = 该段在 state 真身里是「无末端」段(`setTrackManual` 的单段全时限常值,CRVS 的 `t1` = `1<<40` 哨兵,真末端由宿主时间线提供)。此时上桥的 **`t1S` 是一个保守下界**(工程级已知时间线末端;保证 `t1S > t0S`,段不坍缩、可点可切),**不是真末端** —— UI 判定「包含 / 相交 / 重叠」时须把这类段的右端当作 +∞,否则播放头走过该下界后会判成「不在任何段内」。哨兵**绝不原样上桥**(2^40 采样 ÷ 48k ≈ 265 天,曾被前端当成工程时长)。 |
| UI 消费 | 泳道 pan/vol 阶梯曲线叠加层、分段边界、段检查器、diff 变更列表(一次性反馈组件) |
| 契约边界 | **段数据的唯一来源 = `mBridgeReady` 后首帧的本事件(`reason:"snapshot"`,含全部轨全量段表)+ 后续增量事件**;**`requestInitialState` 返回不含曲线真身**(§1.1 只给 `versions[]` 元数据与 `pan_curve`);**不新增拉取式段 API**(`listSegments` / `scvb.segmentList` 不存在,§8)。<br>**与真源字面的偏离(记 §8.3)**:05 §2.3a 与 01 §6.3 R2 写作「数据 = `requestInitialState` 全量快照 + `scvb.segments` 增量事件」,本契约把首帧全量段表明确路由到 `reason:"snapshot"` 事件,避免快照与事件两个入口各携一份段表(§0.1 第 4 条)。 |
| 真源 | 05 §1.4 / §2.3a;J44/J67 |

### 2.9 `scvb.error`

| 项 | 定义 |
|---|---|
| 频率 | **即时**(条件成立/消失各发一次;持续性条件由 UI 按条件维持横幅) |
| 载荷 | `{ code:<§5.1 九码之一>, ch?:1..15, detail:object, active?:bool }` |
| 字段纪律 | `ch` 仅在轨级错误(`srMismatch`/`channelConflict`/`lowSample`)出现;`detail` 逐码定义见 §5.1;`active` 缺省视为 `true`,`false` = 该条件已解除(用于持续性横幅的撤下)。**UI 不静默**:未知 code 一律原样显示 code 字符串并入诊断区(ADR-002/ipc §5)。 |
| 真源 | 05 §1.4 / §2.0 |

---

## 3. Input —— native functions(UI → C++)

共 **8** 个。全部在 [M] 处理。

### 3.1 `requestInitialState()`

| 项 | 定义 |
|---|---|
| 参数 | 无 |
| 返回 | `{ channel_id:0..15, group_id:1..8, role:"input",`<br>`  conn:{outputOnline:bool, maskBit:bool, capturing:bool, passthrough:bool, passthroughPending:bool, occupiedMask:u16},`<br>`  config:{label:string, priority:0..10, lead_lock:bool, pair_id:0\|1..7, freeze:0..3, source_channels:1\|2, participate_in_auto_pan:bool, config_seq:u32, channelLabels:[15 × string]},`<br>`  ui:{scale:f32, language:"zh"\|"en"\|"fr", guide_seen:bool},`<br>`  guide_seen_global:bool, version:{plugin:string, abi:u32} }` |
| 语义 | 首帧全量快照并置 `mBridgeReady=true`。`channel_id=0` = **未分配**(不 claim 任何 slot,J01);Output 侧完全不可见该实例,不计入 `N/15` 计数,**不是错误态**。`config` 为本组 ctrl 广播区中本 channel 的只读快照(Output 离线时字段可为默认值,UI 侧隐藏远程摘要行)。`config.channelLabels` 供 4×4 通道网格首帧渲染(15 张卡的 Output 侧 label,A-32,见 §4.3)。`conn.occupiedMask` 见 §4.2 —— 供 4×4 通道网格在点击前标出已被占用的卡(05 §3)。**键名拼写与 §4.1 `scvb.state` 逐字一致**(A-30,§0.2 规则①)。**`ui.guide_seen`**([J81]/J80/T48)= Input 首启轻量引导已读位,随工程持久化;**`guide_seen_global`** = 系统级全局默认判定位(J50a),**只读、不属工程 state**。首启判据两侧同构:**工程 `ui.guide_seen === false` 且 全局 `guide_seen_global === false`** 才弹。**Input 与 Output 的全局位各存一份**(`input.*` / `output.*` 分键,`UiDefaultsStore` 命名空间本就按侧分)——两侧引导讲的是两个界面、两套内容,共用一个位会让先装 Output 的用户永远看不到 Input 的引导,而那正是 J80 立 T48 的全部理由。 |
| 拒绝态 | 无 |
| 撤销 | 否 |
| 线程/频率 | [M] 同步 |
| 真源 | 01 §6.2(快照字段)/ 05 §1.4 / §3;**键名拼写取 params-v0 §三 Input state 真源**(`channel_id`/`group_id`/`ui:{scale, language}`),与 §4.1 `scvb.state` 同拼写(A-30);01 §6.2 的 camelCase 快照(`channelId`/`groupId`/`uiScale`/`lang`)为旧文,记 §8.3 |

### 3.2 `setChannelId(n)`

| 项 | 定义 |
|---|---|
| 参数 | `n: 0..15`(**0 = 未分配/主动释放**,J01/J59) |
| 返回 | `{ok:true}` 或 `{conflict:true}` |
| 语义 | claim 本组 registry 的 `InputSlot[n-1]`:CAS `state 0→1`;已被占且心跳新鲜 → `{conflict:true}`(UI:卡片抖动 + 红 toast `ch.occupied`「通道 {n} 已被占用(组 {g})」,`{n}`=请求值、`{g}`=当前组);**陈旧占用可覆盖成功**(≥5000ms 且 pid 存活探测失败,J10 双条件)。`n=0` = 释放当前 slot,本轨回到直通(J12);Output 侧该轨回落「未连接」。claim 结果经 `scvb.state.claim` 回推。 |
| 拒绝态 | `{conflict:true}`;abi 不符的对端由 registry 侧拒连,本实例 claim 态置 `abiMismatch`(§5.2) |
| 撤销 | 否 |
| 线程/频率 | [M] 持 `lifecycleMutex`;用户操作触发 |
| 真源 | 05 §1.4 / §3;ipc §1;01 §4.1 |

### 3.3 `setGroupId(g)`(J66)

| 项 | 定义 |
|---|---|
| 参数 | `g: 1..8`(UI 显示 A-H) |
| 返回 | `{ok:true}` 或 `{conflict:true}`(新组同 channel 已被占 → 01 §4.1 I2) |
| 语义 | 写 state `group_id`;触发 01 §4.1 改组释放-重连:释放旧组 slot → 新组对同 channel 重走 claim;**期间走直通,人声不消失**。经 §3 释放确认条触发。**与 Output 侧 §1.4 同名同签名**,返回值不同。 |
| 拒绝态 | `{conflict:true}` |
| 撤销 | 否 |
| 线程/频率 | [M] 持 `lifecycleMutex`;用户确认后触发 |
| 真源 | 05 §1.4 / §3;01 §4.1 |

### 3.4 `remoteSetPriority(n)`

| 项 | 定义 |
|---|---|
| 参数 | `n: 0..10`(int) |
| 返回 | `{queued:bool, reason?: "ringFull" \| "outputOffline" \| "unassigned"}`(**T25 定名**,§9.2) |
| 语义 | 经 ctrl 命令环投递一条记录 `{seq:u32, channel:u32, op:kSetPriority(=1), value:u64(=n)}`(§6);Output [M] 消费后落 state(唯一真源,ADR-004)并经 `config_seq` 变化回执。**Input UI 只做本地乐观显示,以 `scvb.config` 回执为准**。满环时写方覆盖最旧记录 + 溢出计数,返回 `{queued:false, reason:"ringFull"}`,UI 提示「设置未送达,请重试」(01 §4.4-c)。 |
| 拒绝态 | Output 离线 → `{queued:false, reason:"outputOffline"}`(UI 侧 stepper 同时 disabled + tooltip「需 Output 在线」);`channel_id=0` → `reason:"unassigned"` |
| 撤销 | 否 |
| 线程/频率 | [M];用户操作触发 |
| 真源 | 05 §1.4 / §3;ipc §4;01 §4.4-c |

### 3.5 `setUiScale(f)` / 3.6 `commitUiScale()` / 3.7 `setLang(code)`

| 项 | 定义 |
|---|---|
| 参数 | 同 §1.28-§1.30,**唯一差异**:Input 档位 = `0.33/0.5/0.75/1/1.25/1.5/1.75/2/2.5/3`(设计盒 460×560,同 Bridge) |
| 返回 | 同 §1.28-§1.30:`setUiScale` = `{ok:true}` 或 `{ok:false, reason:"badArg"}`;`commitUiScale`/`setLang` = `{ok:true}` |
| 语义 | 同 Output 侧(10 秒防呆确认、全局默认落盘、未知 lang 回退 zh) |
| 真源 | 05 §1.2 / §1.4 / §3 |

### 3.8 `setGuideSeen(seen, alsoGlobal = true)`([J81] J80/T48;变更文档 `20260825-input-guide-seen.md`)

签名与语义**逐字照 Output 侧 §1.32**(契约 §7 的同名同签名项按侧各自登记,`requestInitialState` / `setGroupId` / `setUiScale` / `commitUiScale` / `setLang` 已有先例)。

| 项 | 定义 |
|---|---|
| 参数 | `seen: bool`;`alsoGlobal: bool`(**默认 true**) |
| 返回 | `{ok:true}` |
| 语义 | Input 首启引导已读的**唯一写入口**:写 state `ui.guide_seen`;`alsoGlobal=true` 时同步写系统级全局默认位(**不回写 state**)。**mini tour 完成与 Skip 都置位**(J50a 镜像,与 Output 侧 `setTourSeen` 同款);header「?」重看引导**不调用本函数**(`guide_seen` 已置位也可重看,与 §1.33 语义行同款)。拼写逐字沿用宪法的 `guide_seen`,**不新造 `input_guide_seen`** —— 同一语义两个落点是 §0.1 第 4 条要禁的,两侧拼写一致才能让判据代码共用(`web/shared/lang-start.js` 的 `shouldShowLangStart` 两侧共用一件) |
| 拒绝态 | 无 |
| 撤销 | 否(UI 偏好不进 undo 栈) |
| 线程/频率 | `[M]`;用户走完/跳过引导时各一次 |
| 真源 | 05 §3 文末 J80 节;裁决 J80;变更文档 `docs/contract-changes/20260825-input-guide-seen.md`(PR #84) |

---

## 4. Input —— events(C++ → UI)

共 **5** 个。

> **脚注(相对 05 §1.4 的授权增量,见 §8.4)**:05 §1.4「Input — events」表只列四个(`scvb.state` / `scvb.conn` / `scvb.config` / `scvb.groups`),第五个 `scvb.error` 依 **01 §6.2** 的事件存在性与 T25 裁定 **A-8** 补入(05 §3 的 `ch.occupied` 冲突反馈与 `srMismatch` 红 pill 需要即时错误通道);其**载荷形状统一取 05**(§8.3)。

### 4.1 `scvb.state`

| 项 | 定义 |
|---|---|
| 频率 | 变化时(diff-then-emit;首帧必发) |
| 载荷 | `{ channel_id:0..15, group_id:1..8, claim:"unassigned"\|"idle"\|"active"\|"conflict"\|"abiMismatch"\|"srMismatch", abi:u32, abi_remote?:u32, ui:{scale:f32, language:"zh"\|"en"\|"fr", guide_seen:bool} }` |
| 字段纪律 | `claim` 六态定义见 §5.2。`abi` = 本机 abi(**T25 定名**,§9.2;**取值 = ipc 段布局 abi,`RegistryHeader.abi` 同源;state chunk abi 不落本字段,只经 `scvb.error.newerState.detail` 暴露**),`abi_remote` = 探测到的对端 abi(**T25 定名**,§9.2)——05 §3 明文「两端 abi 占位符取 `scvb.state`」,供 `banner.abiMismatch`「(Output abi {a} / Input abi {b})」渲染;探测不到时 `abi_remote` 字段不存在。`ui:{scale, language, guide_seen}` 为 `setUiScale`/`setLang`/`setGuideSeen` 写入后的回推路径(**T25 定名**,§9.2;与 §3.1 快照同拼写,A-30 统一)。**§3.1 快照的 state 子树字段集与本事件的字段集不得各自漂移**(与 Output 侧 §1.1 语义行同纪律)。**`capturing` 不在本事件**——本实例 `InputSlot.flags` bit0 的唯一桥面落点是 §4.2 `scvb.conn.capturing`(逐字照 05 §1.4 Input events 表;消除同一位的双载,§0.1 第 4 条)。05 §3「采集指示」行的数据来源列写作 `scvb.state`,属 05 内部偏差,记 §8.3。 |
| UI 消费 | 组胶囊/通道卡选中态、claim 错误态红 pill 与卡内横幅、`banner.abiMismatch` 两端 abi 数字 |
| 真源 | 05 §1.4 / §3;01 §6.2;与 §3.1 快照同拼写(A-30 统一) |

### 4.2 `scvb.conn`

| 项 | 定义 |
|---|---|
| 频率 | **~4 Hz**(diff-then-emit) |
| 载荷 | `{ outputOnline:bool, maskBit:bool, capturing:bool, passthrough:bool, passthroughPending:bool, occupiedMask:u16 }` |
| 字段纪律 | **全部为本组语义**(J66:`outputOnline` = **本组** OutputSlot 心跳新鲜 ≤2000ms;`maskBit` = 本组 `connected_mask` 中本 channel 位;状态文案带组号)。`passthrough` = 本实例当前音频路径:**true=直通(按原路径出声,未经平衡),false=静音转发(已接管)**(J12/J32)。`passthroughPending`(**T25 定名**,§9.2)= 处于「静音→直通」的 **5 秒滞回窗口**内(J32:滞回**只作用于静音→直通**方向;「直通→静音」在确认健康后立即 80ms ramp),UI 据此显示过渡提示「连接不稳定,即将切换直通」。`capturing` = 本实例 `InputSlot.flags` bit0(采集中绿点)——**本位在桥面的唯一落点**(§4.1 字段纪律)。**`occupiedMask`**(**T25 定名**,§9.2)= **u16 位图**,bit0=ch1 … bit14=ch15,bit15 保留为 0;置位 = 本组该 `InputSlot` 已被**心跳新鲜**(≤2000ms,ipc §1/§0)的实例占用(`state≥1`),**含本实例自己占的位**——UI 以 `scvb.state.channel_id` 自行区分「本实例已选中」与「他人占用」,陈旧占用(J10 双条件)不置位以保持「陈旧可覆盖」语义。数据面 = [M] 读**本组** registry 的 15 个 `InputSlot`(已映射,不新增跨组访问)。 |
| UI 消费 | Header pill 四态 + 音频路径副文案 + 滞回过渡提示 + 采集中绿点 + **4×4 通道网格的占用灰点**(05 §3:已被其他实例占用的卡显示右上角灰点,点击前即可见) |
| 真源 | 05 §1.4 / §3(**占用态明文「来自 `scvb.conn`/registry 快照」**,Channel 选择器行);01 §6.2;ipc §1 |

### 4.3 `scvb.config`

| 项 | 定义 |
|---|---|
| 频率 | **25 Hz 轮询,节流到变化才发**(ctrl 广播区 `config_seq` 变化检测) |
| 载荷 | `{ label:string, priority:0..10, lead_lock:bool, pair_id:0\|1..7, freeze:0..3, source_channels:1\|2, participate_in_auto_pan:bool, config_seq:u32, channelLabels:[15 × string] }` |
| 字段纪律 | **本组** ctrl 广播区中**本 channel** 的只读快照;**全部只读**(配置真源在 Output,ADR-004)。`freeze` = 该轨 `v{active}_t{ch:02d}_freeze` 自动化参数的**只读镜像**(J65;0..3,bit0=pan/bit1=vol)——01 §6.2 写作 `auto` 快照为 J65 之前的旧文(§8.3)。`lead_lock` 即 01 §6.2 的 `lead`(本契约取宪法拼写,§0.2)。<br>**写侧纪律(`config_seq` 语义)**:`config_seq` 是 **ctrl 广播区的整体版本号**,不是 `setChannelConfig` 的调用计数——Output [M] 在**广播区任一字段**发生变化时一律 `config_seq+1`,**含 `freeze`(自动化参数镜像,由 gesture/DAW 自动化/切版本驱动,不经 `setChannelConfig`)、`source_channels`(Input 实测值)与 `participate_in_auto_pan` 的默认值推导**。否则本事件的变化检测会漏掉这三类,Input 的冻结图标将永久停在陈旧值(05 §3 远程只读摘要行)。本条是 ipc §1 `OutputSlot.config_seq`「Output 配置版本号,Input UI 变化检测」的细化口径,**不改段布局、不修宪**。<br>**`channelLabels`**(T25 定名,§9.2;裁定 **A-32**)= 本组 15 通道 label 的只读镜像(索引 = ch-1,空串 = 未设);数据 = ipc §4 广播区 `channels[15]` 快照既有内容,**零修宪**。**仅 label 投影**——其余 14 通道的 priority/lead/pair 等**不下推**(Input 页无消费面)。任一 label 变化经既有 `config_seq+1` 触发本事件重发。 |
| UI 消费 | 远程只读摘要行(lead/pair/冻结)、优先级 stepper 回执、**通道网格 16 张卡的 Output 侧 label**(`channelLabels`,05 §3) |
| 真源 | 05 §1.4 / §3;01 §6.2;ipc §4 |

### 4.4 `scvb.groups`(J70)

与 Output 侧 §2.4 **同名同载荷同频率**(`{groups_online:u8}`,1Hz,变化才发,数据面 01 §4.5)。Input 侧唯一消费者 = 组胶囊在线绿点与 group-mismatch 引导副行(`group.noOutput`)。

### 4.5 `scvb.error`

| 项 | 定义 |
|---|---|
| 频率 | 即时 |
| 载荷 | `{ code:<§5.1 九码之一>, ch?:1..15, detail:object, active?:bool }` —— **与 Output 侧 §2.9 同形状**(01 §6.2 的 `{code, message}` 为异名,统一取 05 的 `detail`,§8.3) |
| Input 侧实际会发的 code | `channelConflict`(claim 冲突)、`srMismatch`(采样率不一致)。**abi 不匹配不占 error code**——走 claim 态 `abiMismatch` + 卡内 `banner.abiMismatch` 横幅(05 §3 定论:Output 侧不另设 abi 横幅,以 Input 侧为准) |
| 真源 | **01 §6.2(事件存在性)**;载荷形状取 05 §1.4 Output `scvb.error` 的 `{code, ch?, detail}`(§8.3);Input 侧实际 code 与 UI 落点取 05 §3。**05 §1.4 的「Input — events」表未列本事件**,保留依据 = 01 §6.2 + T25 裁定 **A-8**(§4 节首脚注 / §8.3 / §8.4) |

---

## 5. 共享枚举与错误降级语义

### 5.1 `scvb.error.code` —— 九码(与 05 §2.0 警告面一一对应)

| code | 触发条件 | `ch` | `detail` | UI 落点 | 真源 |
|---|---|---|---|---|---|
| `srMismatch` | Input slot 的 `sample_rate` 与 Output 不一致 → 该轨禁用,不做重采样 | 必填 | `{inputSr:u32, outputSr:u32}` | 红横幅③「轨 N 采样率不一致,已禁用」+ Tab2 该轨红灯整行 disabled;Input 侧红 pill「采样率不一致」 | ipc §5 |
| `secondOutput` | **同组内**已有主 Output,本实例进只读观察 | — | `{groupId:1..8}` | 红横幅②`banner.secondOutput`「组 {X} 已有主 Output,本实例只读观察」+ **全 UI 写控件 disabled** | ADR-002 / J66 / J71② |
| `channelConflict` | claim CAS 失败且占用方心跳新鲜 | 必填 | `{groupId:1..8}` | Input 卡片抖动 + 红 toast `ch.occupied`「通道 {n} 已被占用(组 {g})」 | ipc §1 |
| `newerState` | 工程 state chunk 的 abi 高于本机 → **拒载** | — | `{localAbi:u32, projectAbi:u32}` | 红横幅④「此工程由更新版本的 SCVB 保存,请升级插件(本机 abi {a} / 工程 abi {b})」 | 03 §6.2 RejectedNewer / J40 |
| `sidecarMissing` | 外部特征文件缺失或校验失败 | — | `{path?:string}` | 琥珀横幅⑤「采集数据缺失/过期,请重新采集」 | 04 §5.3 |
| `noTimeline` | 宿主未提供时间线(无 `timeInSamples`) | — | `{}` | 琥珀横幅⑥「宿主未提供时间线」+ 采集/输出开关 disabled | 04 §2.6 |
| `projectCopy` | 检测到工程副本(sessionGUID 不匹配)→ 已建独立采集数据副本 | — | `{sessionGuid:string}` | toast①「检测到工程副本,已创建独立采集数据副本」 | 04 §5.6 |
| `sidecarSwitched` | 采集数据超 **8MB** 自动转存外部文件 | — | `{bytes:u64}` | toast②「采集数据已超过 8MB,已转存外部文件——发给他人需重新采集」 | 04 §5.4 / ADR-007 |
| `lowSample` | 该轨采集后**有效唱段 <1.5s** | 必填 | `{voicedS:f32}` | Tab2 状态灯旁 + Tab3 轨头黄标「样本不足,结果可能不稳」 | 04 §7 步7 |

**降级纪律**:①UI **不静默**任何 code——未知 code 原样显示并入 Tab4 诊断区;②持续性条件(横幅①-⑥)不可手动关闭,条件消失(`active:false`)才撤下;一次性提示(toast)可关闭;③参数错误(`reason:"badArg"`)**不占用**本枚举,由函数返回值承载。

### 5.2 Input claim 态 —— 六值

| 值 | 含义 | UI 落点 |
|---|---|---|
| `unassigned` | `channel_id=0`,未 claim 任何 slot(**引导态,非错误**;不计入 Output 的 `N/15`,Output 侧完全不可见) | pill「未选择通道」+ 直通副文案 |
| `idle` | 已选 channel,slot 已声明但 Output 尚未健康读取本轨 | pill「等待 Output」/「Output 未运行」+ 直通副文案 |
| `active` | slot 活跃且被本组 Output 健康读取(`connected_mask` 本位=1) | pill「已连接」+「已接管:本轨静音转发」 |
| `conflict` | claim 失败:同组同 channel 被心跳新鲜的实例占用 | 卡片抖动 + `ch.occupied` toast |
| `abiMismatch` | 两端 SCVB abi 不匹配 → **拒连**(不进 registry 有效位) | 红 pill「版本不匹配」+ 卡内 `banner.abiMismatch`(两端 abi 取 §4.1 `abi`/`abi_remote`)+ 直通副文案照常 |
| `srMismatch` | 采样率不一致 | 红 pill「采样率不一致」+ 直通副文案 |

### 5.3 `range.mode` —— 三值(J04)

| 值 | 语义 | `startS`/`endS` |
|---|---|---|
| `follow`(**默认**) | 全曲跟随:播放到哪采到哪,无预设终点;`inRange` 恒 true;「已离开采集范围」提示不出现 | **忽略**(无哨兵约定) |
| `daw_loop` | 跟随宿主循环区;`kCycleValid` 瞬态缺失时保持最后有效值 + 行内提示,不回退档位 | 由宿主提供,C++ 覆写 |
| `manual` | 用户手动区间,要求 `startS < endS` | 必填有效 |

**已废除**:v0 的「全曲 = manual+[0,曲末] / `start=end=0` 哨兵」约定。

### 5.4 `editSegment` op 与 payload —— 五值(05 §2.3a 逐字)

| op | payload | 语义与后置状态 |
|---|---|---|
| `move_boundary` | `{segIdx:int, edge:"t0"\|"t1", tS:f64}` | 移动该段一侧边界到 `tS`(秒);相邻段随之收缩;吸附能量谷由 UI 侧用 `requestWaveform.valleys[]` 完成(按住 Alt 关吸附),**释放才发**。后置:该段 `origin=user_edited` 且 `locked=true` |
| `split` | `{segIdx:int, tS:f64}` | 在 `tS` 处分割;**两子段继承原值**(pan/volDb)。后置:两子段 `origin=user_edited`、`locked=true` |
| `merge` | `{segIdxA:int, segIdxB:int}` | 合并**相邻**两段;**合并值由 C++ 按时长加权**(02/04 定细则)。不相邻 → `{ok:false, reason:"notAdjacent"}` |
| `set_values` | `{segIdx:int, pan?:f32(-100..100), volDb?:f32(-24..+12)}` | 改段值(两字段均可选,至少给一个)。后置:`origin=user_edited`、`locked=true` |
| `set_locked` | `{segIdx:int, locked:bool}` | 单纯切换 `locked`;**不改 `origin`**。解锁后该段允许被重分析覆盖 |

**共同后置**:任何值/边界编辑 → `origin=user_edited`(J34/J44),编辑后默认 `locked=true`;重分析不覆盖 `origin≠auto` 段(ADR-008 v1.1);`analyze(scope,{clearManual:true})` 只重置 `origin`,**`locked` 段免疫,须先逐段解锁**(04 §4.4)。全部 op 入撤销栈。

### 5.5 `recaptureArm` 的 `reason` —— 四值

| 值 | 触发 | UI |
|---|---|---|
| `noTracks` | `tracksMask=0`(未勾选轨) | 行内说明,不点亮 badge |
| `noSelection` | 无有效选区(`startS>=endS`) | 同上 |
| `readOnly` | 本实例为只读观察态 | 同上 |
| `noTimeline` | 宿主未提供时间线 | 同上 |

### 5.6 拒绝语义 —— 三种

| 返回 | 出现在 | 语义 |
|---|---|---|
| `{rejected:"printing"}` | `setVersionActive`、`copyVersion` | PRINT 态(输出 ON + 播放 + 在 range 内)C++ 侧**硬拒绝**(03 §2.2/§5.3);UI 同时 disabled + tooltip,二者互为双保险 |
| `{conflict:true}` | Input `setChannelId`、Input `setGroupId` | claim 冲突:目标 slot 被心跳新鲜的实例占用 |
| `{observer:true}` | Output `setGroupId`(新组已有主 Output);只读观察态下的一切写函数 | 本实例进/处于只读观察模式,写入未生效 |

其余失败一律 `{ok:false, reason:<string>}`,`reason` 取值**十一值闭集**:`badArg` / `busy` / `noTimeline` / `noLoop` / `notAdjacent` / `ringFull` / `outputOffline` / `unassigned` / **`cancelled`** / **`noData`** / **`ioError`**。后三个由 §1.36 `exportSuggestions` 带入([J81],纯新增,不改既有八值语义)——`cancelled` 是本契约第一个**用户可取消的阻塞式操作**(文件对话框),此前八值里没有「用户取消了一个对话框」这件事。**本集合由 T25 按逐函数拒绝态汇总收敛**;其中 `busy`(05 §1.4 `analyze` 已有分析在跑)/ `noLoop`(05 §2.1 ②、04 §2.2 条 4)/ `notAdjacent`(05 §2.3a `merge` 要求相邻)三个字符串为 T25 定名,已登记 §9.2;`ringFull`/`outputOffline`/`unassigned` 随 `remoteSetPriority` 回执一并登记。每个函数条目的「返回」行按 §0.8 第 5 条写明本函数实际可能出现的取值,不得使用本表以外的 `reason`。

---

## 6. ctrl 命令环 op 核对表(J36 镜像,与 01 §4.4-c 双向一致)

**记录形状(ipc v1.2 [J46] 定型)**:

```c
enum class CtrlOp : u32 { kNone = 0, kSetPriority = 1, kFpReport = 2 };
struct CtrlRecord { u32 seq; u32 channel; CtrlOp op; u64 value; };
```

**环的形状**:ctrl 段 **per-组各一份**(段名 `Local\SynchainSCVB.v1.g{G}.ctrl`,G=1..8;ipc v1.5 [J66],01 §4.4 小节标题同款口径「per-组各一份 [J66]」)——下述 15 条 SPSC 环与广播区(§4.3)均为**本组**范围,**跨组互不可见**。每 slot 一条独立 SPSC(共 **15** 条,ipc §4/J59);生产者 = 该 Input 的 [M] **唯一**,消费者 = Output [M](通道 C6);**音频线程绝不直接写**(04 §4.5)。满环语义:写方覆盖最旧记录 + 溢出计数,Input UI 提示「设置未送达,请重试」。

| CtrlOp | 数值 | UI 入口(05 §1.4) | `value` 打包 | v1 状态 |
|---|---|---|---|---|
| `kNone` | 0 | — | — | 保留值,不投递 |
| `kSetPriority` | **1** | Input 单页优先级 stepper → **`remoteSetPriority(n)`**(§3.4) | `value = n`(0..10,u64) | **v1 op,双向一致** |
| `kFpReport` | **2** | **无 UI 入口** —— Input [M] 25Hz 排水后自动上报 fingerprint(04 §4.5) | `value = (u64(tile_idx) << 48) \| (hash & 0x0000FFFFFFFFFFFF)`;tile_idx 高 16 位(≈18.2 小时时间线上限),fingerprint 截断为低 48 位;**单条记录原子**,不引入跨记录配对 | **v1 op**;**本契约不为其设桥函数**(07 T25 卡「新增 `fp_report` 的上行入口」的措辞与 05 §1.4 / 01 §4.4-c 冲突,取后者,见裁定记录 A-12) |
| 字符串型(`set_label` 等) | — | 无(Input 页 label 只读显示) | 记录仅有标量 `value` 字段,字符串无法过环 | **不在 v1**;将来须走 abi+1 增补变长区(ipc §4)。01 草案的 `remoteSetChannelConfig({field,value})` 已裁剪废除(§8) |

**纪律**:新增 op **必须两处同步**(本表 ↔ 01 §4.4-c);禁止两端各自发明;跨轨上游延迟汇总 op 明确**不进 v1**(J46,入 abi+1 增补清单)。

---

## 7. 机器可读清单(manifest)

以下 JSON 块由 `scripts/check-bridge-parity.mjs` 解析,**必须与 §1-§6 正文逐项一致**(名字、参数名与顺序、枚举值集合)。脚本对本块做**双向**断言:manifest 每一项必须在**对应侧**正文有条目,正文每个函数/事件条目也必须被 manifest 收录;并对六个计数(Output **36**/9、Input **8**/5、**Monitor 5/4**)与跨侧同名函数的 `params` 一致性做硬断言。
**`returns` 登记口径**:按 §0.8 第 5 条写**完整并集**(成功形状 + 全部拒绝态形状,`A | B` 分隔),与正文「返回」行逐字对应;`{ok:false, reason:"…"}` 简写为 `{ok:false,reason:"…"}`。

```json
{
  "contractVersion": "1.0",
  "output": {
    "functions": [
      {"name": "requestInitialState", "params": [], "returns": "OutputSnapshot"},
      {"name": "setCaptureEnabled", "params": ["on"], "returns": "{ok} | {ok:false,reason:\"noTimeline\"}"},
      {"name": "setOutputEnabled", "params": ["on"], "returns": "{ok} | {ok:false,reason:\"noTimeline\"}"},
      {"name": "setGroupId", "params": ["g"], "returns": "{ok} | {observer:true}"},
      {"name": "previewAnalyze", "params": ["scope"], "returns": "{intervals,tracks,manualKept}"},
      {"name": "analyze", "params": ["scope", "opts"], "returns": "{ok,affected:{intervals,tracks,manualKept}} | {ok:false,reason:\"busy\"}"},
      {"name": "cancelAnalyze", "params": [], "returns": "{ok}"},
      {"name": "setRange", "params": ["mode", "startS", "endS"], "returns": "{ok} | {ok:false,reason:\"badArg\"} | {ok:false,reason:\"noLoop\"}"},
      {"name": "setVersionActive", "params": ["v"], "returns": "{ok} | {rejected:\"printing\"}"},
      {"name": "setVersionName", "params": ["v", "name"], "returns": "{ok,name}"},
      {"name": "copyVersion", "params": ["src", "dst"], "returns": "{ok} | {rejected:\"printing\"} | {ok:false,reason:\"badArg\"}"},
      {"name": "beginParamGesture", "params": ["id"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setParam", "params": ["id", "value"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "endParamGesture", "params": ["id"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setChannelConfig", "params": ["ch", "patch"], "returns": "{ok} | {observer:true} | {ok:false,reason:\"badArg\"}"},
      {"name": "setTrackManual", "params": ["ch", "panOrVol", "value"], "returns": "{ok,replacedSegments,replacedLocked} | {observer:true}"},
      {"name": "setPanCurve", "params": ["points"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setVadParams", "params": ["p"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setSegmentation", "params": ["p"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setTransitionRamp", "params": ["ms"], "returns": "{ok}"},
      {"name": "setAnalysisConfig", "params": ["patch"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "editSegment", "params": ["ch", "op", "payload"], "returns": "{ok} | {observer:true} | {ok:false,reason:\"badArg\"} | {ok:false,reason:\"notAdjacent\"}"},
      {"name": "recaptureArm", "params": ["tracksMask", "startS", "endS", "autoStop"], "returns": "{armed,tracksMask,startS,endS,reason?}"},
      {"name": "clearCoverage", "params": ["tracksMask", "startS", "endS"], "returns": "{ok,clearedS} | {observer:true} | {ok:false,reason:\"badArg\"}"},
      {"name": "undo", "params": [], "returns": "{ok}"},
      {"name": "redo", "params": [], "returns": "{ok}"},
      {"name": "requestWaveform", "params": ["ch", "startS", "endS", "cols"], "returns": "{minDb,maxDb,vad,covered,stale,passId,valleys} | {ok:false,reason:\"badArg\"}"},
      {"name": "setUiScale", "params": ["f"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "commitUiScale", "params": [], "returns": "{ok}"},
      {"name": "setLang", "params": ["code"], "returns": "{ok}"},
      {"name": "setActiveTab", "params": ["tab"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setGuideSeen", "params": ["seen", "alsoGlobal"], "returns": "{ok}"},
      {"name": "setTourSeen", "params": ["seen", "alsoGlobal"], "returns": "{ok}"},
      {"name": "confirmPrintGuard", "params": [], "returns": "{ok}"},
      {"name": "setMasterChartMode", "params": ["mode"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "exportSuggestions", "params": ["scope"], "returns": "{ok,rows,path} | {ok:false,reason:\"badArg\"} | {ok:false,reason:\"cancelled\"} | {ok:false,reason:\"noData\"} | {ok:false,reason:\"ioError\"}"}
    ],
    "events": [
      "scvb.state",
      "scvb.params",
      "scvb.conn",
      "scvb.groups",
      "scvb.meters",
      "scvb.playhead",
      "scvb.captureProgress",
      "scvb.segments",
      "scvb.error"
    ]
  },
  "input": {
    "functions": [
      {"name": "requestInitialState", "params": [], "returns": "InputSnapshot"},
      {"name": "setChannelId", "params": ["n"], "returns": "{ok} | {conflict:true}"},
      {"name": "setGroupId", "params": ["g"], "returns": "{ok} | {conflict:true}"},
      {"name": "remoteSetPriority", "params": ["n"], "returns": "{queued,reason?}"},
      {"name": "setUiScale", "params": ["f"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "commitUiScale", "params": [], "returns": "{ok}"},
      {"name": "setLang", "params": ["code"], "returns": "{ok}"},
      {"name": "setGuideSeen", "params": ["seen", "alsoGlobal"], "returns": "{ok}"}
    ],
    "events": [
      "scvb.state",
      "scvb.conn",
      "scvb.config",
      "scvb.groups",
      "scvb.error"
    ]
  },
  "monitor": {
    "functions": [
      {"name": "requestInitialState", "params": [], "returns": "MonitorSnapshot"},
      {"name": "setObservedGroup", "params": ["g"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "setUiScale", "params": ["f"], "returns": "{ok} | {ok:false,reason:\"badArg\"}"},
      {"name": "commitUiScale", "params": [], "returns": "{ok}"},
      {"name": "setLang", "params": ["code"], "returns": "{ok}"}
    ],
    "events": [
      "scvb.state",
      "scvb.groups",
      "scvb.viz",
      "scvb.playhead"
    ]
  },
  "enums": {
    "rangeMode": ["follow", "daw_loop", "manual"],
    "editSegmentOp": ["move_boundary", "split", "merge", "set_values", "set_locked"],
    "segmentsReason": ["analyze", "vad", "segmentation", "edit", "trackManual", "undo", "redo", "versionActive", "copyVersion", "snapshot"],
    "errorCode": ["srMismatch", "secondOutput", "channelConflict", "newerState", "sidecarMissing", "noTimeline", "projectCopy", "sidecarSwitched", "lowSample"],
    "claimState": ["unassigned", "idle", "active", "conflict", "abiMismatch", "srMismatch"],
    "ctrlOp": {"kSetPriority": 1, "kFpReport": 2},
    "analysisLoudnessMode": ["kw_integrated", "rms", "peak_dbfs"],
    "analysisCenterSlotPolicy": ["priority_queue", "lead_exclusive", "even_spread"]
  }
}
```

**计数自检**:Output 函数 **36** / 事件 **9**;Input 函数 **7→8** / 事件 **5**;**Monitor 函数 5 / 事件 4**([J81] 转正,见 §10)。

---

## 8. 归并映射与禁止复活名单(附录)

### 8.1 归并映射(旧名 → 本契约唯一入口)

| 旧名(出处) | 归并去向 | 依据 |
|---|---|---|
| `getAnalyzePreview`(01 草案) | `previewAnalyze(scope)` | 05 §1.4 R1 |
| `setCurveSegmentValue`(01 草案) | `editSegment(ch,'set_values',{segIdx,pan?,volDb?})` | 05 §1.4 R1 |
| `setSegmentBoundary`(01 草案) | `editSegment(ch,'move_boundary',{segIdx,edge,tS})` | 05 §1.4 R1 |
| `splitSegment`(01 草案) | `editSegment(ch,'split',{segIdx,tS})` | 05 §1.4 R1 |
| `mergeSegments`(01 草案) | `editSegment(ch,'merge',{segIdxA,segIdxB})` | 05 §1.4 R1 |
| `setSegmentLocked`(01 草案) | `editSegment(ch,'set_locked',{segIdx,locked})` | 05 §1.4 R1 |
| `setPanCurvePoint` / `removePanCurvePoint`(01 草案) | `setPanCurve(points)` 整表提交 | 05 §1.4 R1 |
| `setSegmentValue`(07 T25 卡旧稿) | `editSegment(ch,'set_values',…)` | 07 T25 R2 更正 |
| `listSegments`(01 §6.3 R2 曾补 / 07 旧稿) | **无替代函数**:段数据 = **首帧 `scvb.segments`(`reason:"snapshot"`,全部轨全量段表)+ 后续增量事件**;`requestInitialState` 只返回 `versions[]` 元数据(含 `pan_curve`),**不含 `curves_per_track`**(§1.1/§2.8) | 05 §2.3a;J67 |
| `remoteSetChannelConfig({field,value})`(01 草案) | `remoteSetPriority(n)`(v1 唯一远程写 op) | 05 §1.4;01 §4.4-c |
| `paramBeginGesture` / `paramSetValue` / `paramEndGesture`(01 草案) | `beginParamGesture` / `setParam` / `endParamGesture` | 05 §1.4 R1 |
| `setTrackAutoPanParticipation(ch,bool)`(07 T25 卡) | `setChannelConfig(ch,{participate_in_auto_pan})` | 05 §1.4;裁定记录 A-3 |
| `sourceChannels` 作为可写入口(07 T25 卡) | **只读**字段 `scvb.state.channels[].source_channels` | 05 §1.4;params-v0 §二 |
| 事件 `analysisProgress` / `analysisDone` | `scvb.state.analysis_run` + `scvb.segments` | 01 §6.4 R1 |
| 事件 `paramChanged` | `scvb.params` | 01 §6.3 R1 |
| 事件 `misalign` | `scvb.conn.channels[].misalignCount` | 01 §6.3 R1 |
| 事件 `curves` | `scvb.segments` | 01 §6.3 R1 |
| 事件 `waveformTile` | `requestWaveform` 请求/响应(不进事件流) | 01 §6.4 |
| 事件 `scvb.segmentList` | `scvb.segments` | J67 |

### 8.2 禁止复活名单(machine-checked)

以下名字**一律不得**出现在 `web/shared/bridge.js` 的导出名集合、`src/*/{Input,Output}BridgeApi.h` 的常量表、或本契约 manifest 中;`scripts/check-bridge-parity.mjs` 命中即退出码 1:

`getAnalyzePreview`、`setCurveSegmentValue`、`setSegmentBoundary`、`splitSegment`、`mergeSegments`、`setSegmentLocked`、`setPanCurvePoint`、`removePanCurvePoint`、`setSegmentValue`、`listSegments`、`remoteSetChannelConfig`、`paramBeginGesture`、`paramSetValue`、`paramEndGesture`、`setTrackAutoPanParticipation`、`analysisProgress`、`analysisDone`、`paramChanged`、`misalign`、`curves`、`waveformTile`、`scvb.segmentList`

### 8.3 已知旧文(不采纳,保留溯源)

| 旧文 | 本契约口径 |
|---|---|
| 01 §6.3 gesture 行「仅全局三件 width/ms_balance/lead_select」 | J65 之前的旧文;gesture 面 = 全局三件 + 每轨 `width` + 每轨 `freeze`(05 §1.4 v2.1) |
| 01 §6.2 把 `groupsOnline` 并入 `scvb.config`、01 §6.3 并入 `scvb.conn` | 旧落点;独立事件 `scvb.groups`,载荷键 `groups_online`(05 §1.4 J70;01 自述「字段名与落点以 05 §1.4 为准」) |
| 01 §6.2 `setUiScale(pct)` | 取 05 的**因子**口径 `setUiScale(f)` |
| 01 §6.2 `scvb.config` 的 `auto` 快照 | J65 后为 `freeze`(每轨自动化参数只读镜像) |
| 01 §6.2 `scvb.error {code, message}` | 统一取 05 的 `{code, ch?, detail}`(同名事件不得双载荷形状) |
| 07 T25 卡「Input 侧新增 `fp_report` 的上行入口」 | `kFpReport` 是 [M] 自动上报,**无 UI 入口、无桥函数**(05 §1.4 核对表 / 01 §4.4-c) |
| 07 T25 卡 `setTrackAutoPanParticipation` / `sourceChannels` | 见 §8.1 归并映射 |
| 05 §1.4「Input — events」表只列四个事件(缺 `scvb.error`) | 第五个事件依 01 §6.2 + 裁定 A-8 补入(§4 节首脚注、§8.4);载荷形状取 05 |
| 01 §6.2 Input `scvb.state` 含 `capturing`;05 §3「采集指示」行数据来源列写 `scvb.state` | `capturing` 的唯一落点 = `scvb.conn`(05 §1.4 Input events 载荷表逐字);消除同一 `InputSlot.flags` bit0 的双载(§4.1/§4.2,裁定 A-21) |
| 05 §2.3a / 01 §6.3 R2「数据 = `requestInitialState` 全量快照 + `scvb.segments` 增量事件」 | 首帧全量段表改由 `scvb.segments`(`reason:"snapshot"`)承载,快照不含 `curves_per_track`(§1.1/§2.8,裁定 A-22) |
| 01 §6.1「25Hz Timer diff-then-emit」(单一基准 Timer 读法) | 各事件类别频率以 05 §1.4 逐类标注为准(meters/playhead 30Hz 与 25Hz 无公因子);基准 Timer 不入桥面契约,**已裁定 A-28** |
| 01 §6.2 Input 快照 `channelId`/`groupId`/`uiScale`/`lang`(camelCase) | 统一取宪法拼写 `channel_id`/`group_id`/`ui:{scale, language}`(**A-30**;§0.2 规则①)——同一 state 字段全契约一种键名,mock 与 JuceBackend 不再各实现两套键 |
| 01 §4.3-i「用 `slot.pid` 反查进程名提示『被 <进程名> 中的另一工程占用』」 | **v1 不实现**:冲突反馈 = `ch.occupied` 词条(05 §5.1,J71② 定稿);`channelConflict` 的 `detail` 仅 `{groupId}`。将来需要时按 §0.1 第 3 条增**可选**字段,零破坏(**A-31**) |

### 8.4 相对 05 §1.4 的授权增量(共 **4** 项)

T25 卡验收要求「对 05 §1.4 的函数/事件全集**零差异**」。本契约的函数/事件**名字集合**相对 05 §1.4 只有以下四项增量,除此之外零差异(逐项比对由 `check-bridge-parity.mjs` 的 `EXPECTED` 冻结期望表机器断言):

| 增量项 | 授权来源 | 说明 |
|---|---|---|
| Output 函数 `setAnalysisConfig(patch)` | **05 §2.4**(J69 两设置块的数据来源列逐字写有 `setAnalysisConfig({loudness_mode})`,括注「由 01/03 细化时定名并进 T25 冻结契约」) | 函数名有 05 字面出处,只是不在 §1.4 的函数表内;T25 补白 `patch` 形状与两组枚举值(§1.21/§9.2) |
| Input 事件 `scvb.error` | **01 §6.2**(Input 事件列有 `scvb.error`)+ 裁定记录 **A-8**;需求面 05 §3(`ch.occupied` 冲突反馈、`srMismatch` 红 pill 需即时错误通道) | 05 §1.4 的 Input events 表未列;载荷形状统一取 05 的 `{code, ch?, detail}`(§4.5/§8.3) |
| Output 函数 `confirmPrintGuard()` | **04 §5.3 / 05 §2.0 横幅⑦**(语义与 §2.5 验收已定,05 §1.4 函数表缺位);统筹裁定 **A-29**,成例 = R4 `setGuideSeen` | **T25 新增名**(不同于前两项,05 全文无字面出处);对 05 §1.4 的回写要求列入 PR 描述 |
| Output 函数 `setMasterChartMode(mode)` | **05 J75 A**(T43 Tab1 分布图双视图的视图态);变更文档 `docs/contract-changes/20260825-master-chart-mode.md` | 纯 UI 偏好 state 写入口(照 §1.31 `setActiveTab`);取值 `"distribution"`/`"trajectory"`,默认 `"distribution"`,未知值 `badArg`;不占参数面、不进 undo 栈 |

---

## 9. 版本与变更流程

### 9.0 变更纪律

1. 本契约 **1.0 已于 2026-08-16 冻结**(DeepSeek native 可实现性评审通过 + 用户明确批准,PR #33)。
2. 冻结后任何改动一律走仓库 `CLAUDE.md` §5 的冻结契约变更规范:① 先获用户明确批准;② 写变更文档到 `docs/contract-changes/<YYYYMMDD>-<slug>.md`;③ PR 挂 `status/frozen-contract` 标签。
3. 「只增不改」的允许面见 §0.1 第 3 条。任何改名/删除/收窄一律视为**契约破坏性变更**(`contractVersion` 主版本号 +1),须同批更新 mock 后端、C++ 常量表与 `check-bridge-parity.mjs`。**该变更不触发 ipc/params 的 `abi` 升级**——`abi` 仅由 ipc-contract-v0 §5(共享内存段布局改动 → abi+1 且段名 v2,新旧不互认)与 params-v0 §四(state chunk 拒载/迁移判定)定义的改动触发。桥面是纯进程内 JS↔C++ 接口,改名既不改段布局也不改 state chunk 结构,**绝不可为一次桥面重命名把 ipc abi 升到 2**。
4. 每次改动必须同步更新 §7 manifest 与本节版本行,并跑 `node scripts/check-bridge-parity.mjs` 退出码 0。

### 9.1 统筹裁定记录(O-1/O-2/O-3/O-4/P-1,2026-08-16 已闭环)

下表为五项争议的**原文留档**(起草期的两侧出处与影响面照旧保留,供评审复核裁定是否成立);每行开头加粗标注裁定结果与编号。

| # | 事项 | 两侧出处 | 影响面 |
|---|---|---|---|
| **O-1** | **【裁定 A-30:Input 快照统一 snake_case,按 §0.2 规则①逐字沿用宪法拼写;回填 §0.2/§3.1/§4.1/§8.3】** —— 争议原文:Input `requestInitialState` 快照用 `channelId`/`groupId`/`uiScale`/`lang`(camelCase),而 Input `scvb.state` 用 `channel_id`/`group_id`/`ui.scale`/`ui.language`(snake_case + 子树)——**同一 state 字段两组三对拼写**(channel / group / ui) | 01 §6.2 快照逐字 camelCase(T25 简报第 11 条照录);05 §1.4 Input events 逐字 snake_case;`ui` 子树与 `uiScale`/`lang` 均为 T25 依写入路径补的回推落点(§9.2) | mock 与 JuceBackend 须两种键都实现;**统一方向须一次覆盖 channel / group / ui 三组**,否则 mock 要实现三套键。两侧均为逐字真源,起草者不自行改判 |
| **O-2** | **【裁定 A-31:v1 不做 `holderPid`/进程名提示,`detail` 仅 `{groupId}`;回填 §5.1/§8.3】** —— 争议原文:claim 冲突/只读的用户可见文案不一致:01 §4.3-i 要求用 `slot.pid` 反查**进程名**提示「被 <进程名> 中的另一工程占用」,而 05 §5.1 只定义 `ch.occupied`「通道 {n} 已被占用(组 {g})」;`setChannelId`/`setGroupId` 返回仅 `{conflict:true}`,无字段承载 pid/进程名 | 01 §4.3-i;05 §3 / §5.1 | 决定 `{conflict:true}` 是否增 `holderPid`(本契约 §5.1 已把 `holderPid` 标为可选待裁);涉及 05 是否补词条 |
| **O-3** | **【裁定 A-32:`scvb.config` 增 `channelLabels:[15 × string]`,取候选 (a) 的「只投影 label」变体;回填 §3.1/§4.3/§9.2】** —— 争议原文:**Input 通道网格「各通道 Output 侧 label」无数据面**:05 §3 Channel 选择器要求「卡片显示编号 + Output 侧 label(有则显示)」,即 15 张编号卡各显示一个 label;但 05 §1.4 的 Input 事件面只给**本 channel** 的配置快照(`scvb.config`),契约 §4.3 据此写死为「本组 ctrl 广播区中**本 channel** 的只读快照」——其余 14 张卡的 label 在契约内无合法数据源。(**占用灰点已闭环**:05 §3 明文「占用态来自 `scvb.conn`/registry 快照」,故 T25 依授权在 §4.2 补 `occupiedMask:u16`,见 §9.2;label 无同款明文授权,故留裁定) | 05 §3 Channel 选择器行(16 张卡 + label)↔ 05 §1.4「Input — events」`scvb.config`「本 channel」;ipc §4「Output → Input 广播区:当前 channels[15] 配置快照(label/priority/lead/pair)+ config_seq」——**ipc 侧本来就是 15 条** | 候选:**(a)** Input `scvb.conn` 或 `scvb.config` 增 `labels:[15 × string]`;**(b)** 把 §4.3 `scvb.config` 从「本 channel 快照」放宽为「本组 15 通道快照」(与 ipc §4 一致,数据本来就在广播区里,零修宪)。影响面:05 §3 验收清单 `?scenario=occupied` 七态之一的卡面渲染;T30 实施。**起草者不自行裁定**(05 内部张力),裁定后回填 §4.2/§4.3/§9.2 |
| **O-4** | **【裁定 A-28:取候选 (b)——频率以 05 §1.4 逐类为准,基准 Timer 不入桥面契约;回填 §0.4/§2.5/§2.6/§8.3】** —— 争议原文:**事件基准 Timer 频率**:01 §6.1 写「25Hz Timer diff-then-emit 节流」(SCVB 用法「原样复制模式;事件类别更多,每类独立节流」),05 §1.4 逐事件写 `scvb.meters`/`scvb.playhead` **30Hz**、`scvb.params` **25Hz**——25 与 30 无公因子节拍,单一 25Hz Timer 物理上驱动不出 30Hz | 01 §6.1 第 655 行;05 §1.4 事件表第 147-148 行 | 候选:**(a)** 基准提到 **50Hz**(meters/playhead 每 2 拍=25Hz、params 每 2 拍=25Hz)或 **60Hz**(meters/playhead 每 2 拍=30Hz、params 每 2 拍=30Hz),对应要动 05 的一档频率;**(b)** 允许多 Timer / 高基准(如 150Hz)分频,05 与 01 都不动。契约 §0.4 第 1 条现按「频率以 05 §1.4 逐类为准、基准不入桥面契约」实施,裁定后回填 §0.4/§2.5/§2.6 |
| **P-1** | **【裁定 A-29:取候选①,新增 Output 函数 `confirmPrintGuard()`;回填 §1.3/§1.34/§7/§8.4/§0.9】** —— 争议原文:**加载守卫「继续引擎驱动」按钮无桥函数入口**:04 §5.3 / 05 §2.0 横幅⑦ 要求「确认前仅 ARMED、确认后恢复 PRINT、本会话不再出现」,但 05 §1.4 与 01 §6.3 的函数全集**都没有**清除守卫的函数;若不定名,T31 无合法入口(与 R4 `setGuideSeen` 同类缺口) | 05 §2.0 横幅⑦ / §2.5 `print-guard` 验收;04 §5.3;05 §1.4 与 01 §6.3 函数全集 | 候选:①新增 `confirmPrintGuard()`;②复用 `setOutputEnabled(true)` 作确认信号(语义含混)。**起草者不自行定名**;本契约暂只保留 `scvb.state.print_guard.pending` 只读态 |

**五项均已回填正文**(A-28 → §0.4/§2.5/§2.6;A-29 → §1.3/§1.34/§7/§8.4/§0.9;A-30 → §0.2/§3.1/§4.1;A-31 → §5.1;A-32 → §3.1/§4.3/§9.2;旧文溯源一并记 §8.3)。DeepSeek 评审如对任一裁定有异议,按 §9.0 冻结前改名零成本流程重开。

### 9.2 T25 定名清单(依真源授权补白,待 DeepSeek 评审确认)

以下名字/取值在真源中**语义已定、名字未定**,由 T25 依授权定名;评审如有异议,改名走 §9.0 流程(冻结前改名零成本)。**本表是 T25 补白的完整清单**(§9.3 的 DeepSeek 评审勾选项之一),**共 32 项**;正文中任何标「T25 定名」的字符串都必须在本表出现。

| 名字 | 授权来源 | 说明 |
|---|---|---|
| `setAnalysisConfig(patch)` 的 `patch` 形状与**两组枚举字符串值 + 默认档** | 05 §2.4 / J69(明文「由 01/03 细化时定名并进 T25 冻结契约」) | **函数名 `setAnalysisConfig` 在 05 §2.4 数据来源列已有字面出处,不属 T25 发明——改名须同步回改 05 §2.4**;T25 补白的是 `loudness_mode`: `kw_integrated`/`rms`/`peak_dbfs` 与 `center_slot_policy`: `priority_queue`/`lead_exclusive`/`even_spread` 六个字符串值 + 默认 `kw_integrated`/`priority_queue`;义项与数学以 05 §2.4 与 02 定稿为准 |
| `requestWaveform` 返回的 `valleys[]` | 05 §6.3(明文「由 C++ 在 `requestWaveform` 附带谷点列表」,未定字段名) | 能量谷时间点列表(秒,升序),供边界拖拽吸附 |
| `scvb.state.analysis_run:{running,progress?}` | 01 §6.4(明文「进度/结果走 C14 回 [M],经 `scvb.segments`/`scvb.state` 下行」) | 分析按钮菊花 / 「正在应用…」/ J47 抑制判定的数据源 |
| `scvb.state.versions[].empty` | 05 §2.1 ③(空版本 chip 角标「空」,未定字段名) | 该版本无曲线数据 |
| `scvb.state.features.bytes` | 05 §2.4 存储状态显示「内嵌于工程(x.x MB)」 | 特征数据字节数 |
| `scvb.state.print_guard.pending` | 04 §5.3 / 05 §2.0 横幅⑦(数据源明文 `scvb.state.print_guard`) | 守卫待确认态;确认入口 = §1.34 `confirmPrintGuard()`(裁定 **A-29**,§8.4 授权增量③) |
| `scvb.params` 的 `full` / `versionActive` | 05 §1.4(「非激活版本按需拉取」+ 切版本需刷新) | 全量重发标志与当前版本号;取代未定义的「按需拉取」函数 |
| `scvb.captureProgress` 的 `coveragePct` | 05 §2.3 泳道轨头「覆盖率百分比」 | 该轨在当前 range 内覆盖百分比 |
| `scvb.conn`(Input)的 `passthroughPending` | 05 §3(滞回期 pill 过渡提示)/ J32 | 处于「静音→直通」5 秒滞回窗口 |
| `scvb.state`(Input)的 `abi_remote` | 05 §3(明文「两端 abi 占位符取 `scvb.state`」) | 对端 abi,供 `banner.abiMismatch` 渲染 |
| `remoteSetPriority` 返回 `{queued, reason?}` | 01 §4.4-c(满环须「UI 提示设置未送达」) | 无回执则该用户可见行为无法实现 |
| `setActiveTab` 的四值枚举 | 05 §0.1 目录树 `tab-{master,tracks,wave,settings}.js` | `"master"/"tracks"/"wave"/"settings"` |
| `setTrackManual` 返回 `{replacedSegments, replacedLocked}` | 05 §2.2(确认文案「含 {l} 个锁定段」需计数) | 一次性确认条的计数来源 |
| `recaptureArm` 的 `autoStop` 默认 `false` | 05 §1.4(可选参数,未给默认值) | 「播完自动停」默认不勾选 |
| `clearCoverage` 返回 `{ok, clearedS}` | 05 §2.3(确认后需反馈清除量) | 可选反馈,不影响主流程 |
| `scvb.error` 的 `active` 字段 | 05 §2.0(持续性横幅「条件消失才消失」) | 条件解除信号 |
| `scvb.conn`(Output)的 `heartbeatAgeMs`(每轨)与 `generation`(顶层) | 05 §2.4 诊断区逐字四项「每轨 **heartbeat 年龄** / 失准计数 / **generation** / config_seq」,数据来源列写 `scvb.conn`;ipc §0(steady clock 心跳)/ §1(`RegistryHeader.generation` 每次重新初始化 +1) | 无此二字段则 Tab4 诊断区四项只能实现两项;`heartbeatFresh` 降级为 `heartbeatAgeMs ≤ 2000` 的派生布尔(保留,便于 UI 判色);`generation` 为全组单值故置顶层 |
| `scvb.conn`(Input)的 `occupiedMask:u16` | 05 §3 Channel 选择器行明文「**占用态来自 `scvb.conn`/registry 快照**」+「已被其他实例占用的卡显示右上角灰点」;ipc §1 `InputSlot.state` + 心跳 | bit0=ch1…bit14=ch15,bit15 保留 0;置位 = 本组该 slot 被心跳新鲜的实例占用(含本实例)。**只解决占用灰点**;15 张卡的 label 由 `scvb.config.channelLabels` 承载(裁定 **A-32**,见本表下一行) |
| `scvb.config` 的 `channelLabels:[15 × string]` | 05 §3 Channel 选择器行「卡片显示编号 + Output 侧 label(有则显示)」;ipc §4 广播区本就是 `channels[15]` 快照 | 统筹裁定 **A-32** 采候选 (a) 变体:**只投影 label**,不放宽整个 config 面为 15 通道快照(候选 (b) 否决:Input 页对其余通道的 priority/lead/pair 无消费面)。索引 = ch-1,空串 = 未设;经既有 `config_seq+1` 触发重发(§4.3) |
| `scvb.state.versions[].pan_curve` 的**下行落点** | 05 §6.2(pan 角度域曲线编辑器须渲染既有点集)+ params-v0 §二 J07(`pan_curve` 属 `versions[2]`,字段逐字沿用) | `setPanCurve` 只有写入口;切版本/`copyVersion`/`undo`/`redo` 后 UI 需重新取点集。落在 §1.1 快照与 §2.1 `scvb.state`(整表 ≤16 点,不进 `scvb.segments`) |
| `scvb.segments` 的 `reason` **十值枚举**与 `version` 字段 | 05 §2.3a(段编辑/重分析后回推)+ 01 §6.4(「结果经 `scvb.segments` 下行」);触发面逐条对应 §1.6/§1.9/§1.11/§1.16/§1.18/§1.19/§1.22/§1.25/§1.26 与 §0.4 首帧 | `analyze\|vad\|segmentation\|edit\|trackManual\|undo\|redo\|versionActive\|copyVersion\|snapshot`;**枚举须闭合**(任何改段表的函数都有对应 reason),同步登记进 §7 manifest 的 `enums.segmentsReason` 供 parity 机器断言;`snapshot` 的触发场景 = `mBridgeReady` 后首帧必发(§0.4 第 3 条) |
| `scvb.segments` 段项的 `loudnessLufs` | 05 §2.3a 段检查器「段响度:只读 mono(段内积分响度 **LUFS**,ADR-009)」(未定字段名) | 段内积分响度,只读显示 |
| `scvb.segments` 每轨的 `stale` | 04 fingerprint watchdog(过期采集区间)+ 05 §6.3 波形 `stale[]` 列同款语义 | 该轨存在过期采集区间;与 `requestWaveform.stale[]` 同源不同粒度 |
| `scvb.segments` 的 `diff:{kept, changed:[{ch, segIdx, panFrom, panTo, volDbFrom, volDbTo}], added, removed}` | 05 §2.3「diff 变更列表:段号 + pan/vol 旧值→新值」+ 词条 `wave.diffKept`「{k} 处手动编辑/锁定段已保留」 | 一次性反馈组件的数据源;`kept` 即词条的 `{k}` |
| `scvb.captureProgress` 的 `addedRanges` | 05 §1.4 该行原文「每轨 **`coverage_ranges` 增量**」;宪法字段名 params-v0 §二 `features.per_channel[].coverage_ranges[]` | **刻意不同名**:本字段是「本帧新增区间」的增量投影,与 state 真身的全量区间表语义不同,同名会造成两个入口混淆(§0.1 第 4 条);按 §0.2 规则②取 lowerCamelCase |
| `scvb.params` 的 `values` 键 | 05 §1.4「`{id→value}` 稀疏 diff」(未定容器键名) | 稀疏 diff 容器 |
| `scvb.meters` 的结构键 `tracks` / `bus` / `l` / `r` | 05 §1.4「每轨 `{db, peakDb}` ×15 + 总线 L/R」(值形状已定,容器键未定) | 只是容器命名,值形状逐字照 05 |
| `scvb.state.recapture:{armed, tracksMask, startS, endS, autoStop}` 的**字段形状** | 05 §1.4(`recaptureArm` 返回 `{armed, tracksMask, startS, endS, reason?}` + 「布防态同时进 `scvb.state.recapture`」) | 落点在 05,形状由 T25 按返回值镜像补齐(不带 `reason`,只读态用 `autoStop` 补「播完自动停」) |
| Input `scvb.state` 的 `abi` 与 `ui:{scale, language}` | `abi`:05 §3「两端 abi 占位符取 `scvb.state`」(与 `abi_remote` 同源);`ui`:Input 侧 `setUiScale`/`setLang`(05 §1.4 Input functions)写入后须有回推路径 | 与 §3.1 快照**同拼写**(裁定 **A-30**:全契约统一取 `ui:{scale, language}`,§0.2 规则①) |
| `{ok:false, reason}` 的 **`busy` / `noLoop` / `notAdjacent`** 三值(§5.6 八值集的新增部分) | 逐函数拒绝态汇总:`busy`←05 §1.4 `analyze`(已有分析在跑);`noLoop`←05 §2.1 ②/04 §2.2 条 4(宿主能力级缺 loop);`notAdjacent`←05 §2.3a `merge`「合并**相邻**两段」 | 语义在真源、字符串由 T25 收敛;`badArg`/`ringFull`/`outputOffline`/`unassigned`/`noTimeline` 同表(§5.6) |
| `setChannelConfig` 的 `label ≤24 字符`上限 | **真源未给上限**(05 §1.4 只写 `label?`);T25 提议值 | **评审可放宽**;注意 §0.1 第 3 条冻结后**禁止收窄**,放宽须走 §9.0 流程。C++ 侧超长截断、不报错 |
| `tracksMask` 的「**bit15 保留为 0**」约定 | 05 §1.4(`tracksMask: u16` + 通道 1..15,J59);15 轨用不满 16 位 | T25 收敛的位约定,避免两侧对第 16 位各自解释 |

### 9.3 冻结前检查清单

- [x] DeepSeek native 侧确认(**2026-08-16 通过**):§1/§3 全部 34+7 函数可实现;§9.2 **32 项**定名无异议;`requestWaveform` 口径定案「异步」(§1.27);`heartbeatAgeMs`/`generation`/`occupiedMask`/`channelLabels` 四载荷字段可实现;`confirmPrintGuard`(§1.34)可行。附注:`requestWaveform.passId` 与 `scvb.segments.stale` 依赖特征段数据面最终落地(J35 run_id / fingerprint watchdog),桥面形状先冻结、首版可回退常量,值填充不触发契约变更。
- [x] §9.1 五项已由统筹会话裁定并回填(**A-28~A-32**,2026-08-16);DeepSeek 评审可按 §9.0 重开
- [x] `node scripts/check-bridge-parity.mjs` 退出码 0(2026-08-16 实测;当前 B/C 两侧 SKIP,三方比对待 T28/T29)
- [x] 用户批准(**2026-08-16**):状态行已改「已冻结」;变更记录 `docs/contract-changes/20260816-scvb-contract-v1-initial.md` 随本 PR 入库;PR 挂 `status/frozen-contract` 标签

---

## 10. Monitor —— native functions / events([J81] 转正;T45 #94 + T46 #90)

> **转正来历**:Monitor 桥面由 T45(#94,插件壳 + `src/monitor/MonitorBridgeApi.h`)与 T46(#90,真 UI + `web/monitor/monitor-bridge.js`)分两卡交付。#94 当时把 `manifest.monitor` 明确延后到 T46;T46 已合入且其 `monitor-bridge.js` 文件头逐字写着「**临时形制,待随 ipc v1.6 修宪转正进契约 §7**」—— 延后条件已满足,故随 J81 一并转正。名表**逐字**取自 `web/monitor/monitor-bridge.js` 的 `MONITOR_FUNCTIONS` / `MONITOR_EVENTS`,C++ 侧真源 `src/monitor/MonitorBridgeApi.h`。

**Monitor 是纯只读监视器**(ADR-001a 三铁律:0 自动化参数 / 音频直通逐样本按位相等 / 对任何共享段零写入)。**本表里没有任何一个写引擎状态的函数** —— `setUiScale` / `commitUiScale` / `setLang` 写的是**本实例自己的 UI 偏好**,不属于被观察组的状态,不违反只读。

### 10.1 native functions —— 共 **5** 个

除 `setObservedGroup` 外的四个由 `WebViewHost` 基类注册(故 `MonitorBridgeApi.h` 只声明 `kFnSetObservedGroup` 一个常量;parity 脚本按此口径,不得据「头文件里没有 `kFnRequestInitialState`」判缺名)。

| 名字 | 参数 | 返回 | 语义 |
|---|---|---|---|
| `requestInitialState()` | 无 | `MonitorSnapshot` | 首帧全量快照并置 `mBridgeReady=true`(§0.6)。载荷 = 组回显 + `ui:{scale, language}` + viz 三态与 fresh |
| `setObservedGroup(g)` | `g: 1..8` | `{ok:true}` 或 `{ok:false, reason:"badArg"}` | **换观察对象**,只读语义:释放旧句柄 → 换组 → 只读 attach,**不 claim、不建段、对被观察组零副作用**。⚠ **刻意不叫 `setGroupId`** —— §1.4 的 `setGroupId` 是 Output 的**改组**(断开本组全部连接、要弹确认条),与「换一个组的 viz 段来看」是两件事;共用名字迟早有人照 §1.4 的语义去实现它。组切换走**只读专用**路径,复用写方的 `changeGroup()` 会把「新组还没有写方」变成「我来建一个空段」,正好破坏第三条铁律 |
| `setUiScale(f)` | `f: f32` | `{ok:true}` 或 `{ok:false, reason:"badArg"}` | 10 秒防呆的预览档,形制同 §1.28 |
| `commitUiScale()` | 无 | `{ok:true}` | 「保持」落盘系统级全局默认,形制同 §1.29 |
| `setLang(code)` | `code: "zh"\|"en"\|"fr"` | `{ok:true}` | 形制同 §1.30 |

全部在 `[M]` 处理;全部**不入撤销栈**(Monitor 无 UndoManager)。

### 10.2 events —— 共 **4** 个

| 事件 | 频率 | 载荷 |
|---|---|---|
| `scvb.state` | 变化时 | 组 / 缩放 / 语言 / **viz 三态与 fresh**(段级状态的唯一真源) |
| `scvb.groups` | **1 Hz** | **逐字复用** §2.4 的既有形状:`{groups_online}` 位图,组胶囊绿点 |
| `scvb.viz` | **4 Hz** | viz 帧:每轨当前值每帧刷;降采样车道按 `lane_revision` 变化才带(形状见 `web/monitor/viz.js` 头注;数据面 = ipc §6) |
| `scvb.playhead` | **25 Hz** | **逐字复用** §2.6 的既有载荷形状(WebViewHost 定时器上限) |

后两个之所以逐字复用 Output 侧的形状而不另立一套:轨迹图的 `onPlayhead(ev)` 与组胶囊的消费代码因此**一行不改**。

**`scvb.state` 的 viz 面为什么必须由 native 给**:「Output 进程真没了」与「还在但不再发帧」在 UI 侧长得一模一样 —— 命名段是引用计数存活的,只要 Monitor 自己不松手,段就一直在、读也一直成功。T45 的做法是帧陈旧时松开映射再探一次,这件事 UI 做不了。

### 10.3 键名拼写

照 §0.2 规则① 与裁定 A-30:镜像宪法 state 字段的键一律 **snake_case** —— `group_id`、`ui:{scale, language}`、`groups_online`(camelCase 在 §8.3 明记为旧文)。

### 10.4 转正后的收尾(不阻塞本次)

`web/monitor/monitor-bridge.js` 按其文件头自陈,应退化成 `createBridge({role:"monitor"})` 的一层薄封装或直接删除 —— **列 T46 后续小项**,不在本 PR。在它退化之前,`bridge.js` 的 `BRIDGE_FUNCTIONS.monitor` / `BRIDGE_EVENTS.monitor` 是 parity 的**唯一比对面**,`monitor-bridge.js` 内的两张表已加注「以 `bridge.js` 为准」。
