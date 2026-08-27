# E2E 端到端联调(T37)—— 04 §7 十六步 UX 旅程 + 真机执行清单

> 状态:修补性改动(无冻结契约变更)。基线 = `origin/feature/v1`(0e975bf1);依赖 T29–T36/T36b 全部已合。
> 真源:`masterPlan/plan/04-capture-workflow.md` §7(16 步旅程,验收即测「插件反馈」列)/ §8(六类验收)。
> 分支:`feat/T37-e2e`;PR base = `feature/v1`。
>
> 本文把「能在无 DAW 环境下离线验证的部分」现在就跑并记录结果;「必须真机 DAW 的步」标
> 「待用户上机」并收敛到 §3 的**可勾选执行清单**。

---

## 0. 验证通道与离线覆盖结论

D3 三通道的本次落地:

| 通道 | 载体 | 本次结果 |
|---|---|---|
| **通道 1**(本地构建 + 装机) | `scripts/build.ps1 -Install` | 见 §4.1(干净构建 → 装机到 VST3,本机为系统级) |
| **通道 2**(浏览器 mock 预览) | `web-preview/`(`serve.ps1` + `shot.mjs`) | 见 §4.2(web smoke 全绿 + 17 张截图) |
| **通道 3**(CI artifact) | `gh run download` 最新 `build-and-validate` | 见 §4.3(层级 + 双 bundle 完整性) |

**离线覆盖结论**:16 步里,「连接态 / Input 七态 / 四个 tab 状态 / tour 后状态 / 打印守卫 /
重采集布防 / 分段时间线」等**视觉与状态机语义**可经 web-preview mock 覆盖(步 1–12、15 的
绝大部分);**必须真机 DAW** 的步集中在「真实走带 / 采集记账 / write 落盘 / 回读一致 /
fingerprint 过期」——步 3(电平与失准计数)、6/7(真实 coverage 生长与「样本不足」)、
13/14(write/Latch 落盘与回读)、16(改 EQ 后 fingerprint 过期)以及 §8 六类验收里所有依赖
宿主时间线与自动化的格子。这些步与格已全部收进 §3 清单。

---

## 1. 十六步旅程逐步结果

> 「实际验证结果」列:✅ = 离线已验(附截图/断言);🖥️ = 待用户上机(DAW)。
> 截图路径均为 `web-preview/.shots/` 下的**本地产物(已 gitignore,不入库)**;复现命令:
> `pwsh web-preview/serve.ps1` + `node web-preview/shot.mjs --<选项>`(见 §4.2)。

| 步 | DAW 操作(§7 用户操作列) | 预期插件反馈(§7 插件反馈列) | 实际验证结果 |
|---|---|---|---|
| 1 | 每条人声轨最后一格插 **SCVB Input** | 实例以 `channel_id=0`(未分配)启动,**不 claim 任何 slot**;UI 首开高亮通道轮带并强制引导「选择本轨的通道编号」;用户选择后才 claim,冲突时红字「Channel N 已被占用」 | ✅ 离线:Input 七态经 web-preview 验证(未分配→引导、`occupied`→`conflict` 红字、`connected`→`active`)。截图 `t37-input-{connected,no-output,occupied,…}.png`。`claim` 语义的 core 断言 = `test_input_session.cpp` / `test_ipc_lifecycle.cpp`(channel_id=0 不 claim、冲突不写命令环)。🖥️ 真机补:「轨道名 label 自动填入」是 JUCE patch 项(kChannelUIDKey 透传,**待验证**)→ §3 清单 L-2 |
| 2 | 总线第一格插 **SCVB Output** | 检测各 Input 心跳 → 每轨连接灯亮;轨道名自动填入 label | ✅ 离线:Output `fifteen-tracks` fixture(15 轨全连)。截图 `t37-output-master-fifteen.png`。🖥️ 真机补:label 透传与心跳点亮在真实宿主内核对 → §3 清单 L-2 |
| 3 | 播放几秒验证接线 | Output 每轨电平表跳动;无时间线数据 → 黄警告「未收到 Track N 音频」;失准计数应为 0 | ✅ 离线:电平表/失准横幅视觉 —— `fifteen-tracks`(健康)vs `misaligned`(琥珀横幅 + 轨 ⚠)。截图 `t37-output-master-{fifteen,misaligned}.png`。🖥️ 真机补:「播放几秒 → 电平真实跳动」与「失准计数 = 0」依赖真实走带 → §3 清单 L-3 |
| 4 | 「每轨设置」:优先级/主唱锁/配对/label | 即改即存 Output state;Input UI 同步(ctrl 广播,config_seq) | ✅ 离线:Tab2 轨道卡(优先级/锁/配对/label 控件)截图 `t37-output-tracks.png`;config_seq 透传的 core 断言 = `test_input_bridge_ipc.cpp`「configSeq」。🖥️ 真机补:Input↔Output 跨实例实时同步 → §3 清单 L-4 |
| 5 | 选 range:默认「全曲跟随(follow)」;或「用循环区」/手动 | daw_loop 下拖 DAW 循环区实时跟随;无 loop 能力时「用循环区」disabled + tooltip | ✅ 离线:Tab1 range 三值枚举(follow 默认档)截图 `t37-output-master-fifteen.png`;`&loop=none` 降级 → `second-output`(daw_loop 代表档)+ `&loop=none` 变体。🖥️ 真机补:「拖宿主循环区 → 插件 range 实时跟随」需真实宿主 → §3 清单 L-5 |
| 6 | **采集开关 ON** → 播放 range | 绿点脉冲「采集中」;各轨覆盖进度条实时生长;离开 range → 琥珀 +「已离开采集范围」 | 🖥️ 待用户上机:采集布防/coverage 生长依赖真实走带(采集 ON ∧ 播放 ∧ playhead 有效三条件)。离线只验「采集态视觉与布防门控」的 mock 面(见步 15 `recapture-armed`)。→ §3 清单 L-6 |
| 7 | 播完,**采集开关 OFF**(或自动停) | 每轨覆盖率汇总;有效唱段 <1.5s 的轨黄标「样本不足」;FrameStore 转只读 | 🖥️ 待用户上机:覆盖率汇总/样本不足/只读态依赖真实采集结果。→ §3 清单 L-7 |
| 8 | 点 **分析** | <1s 出结果;时间线显示分段边界+段响度+各轨 pan/vol 曲线;洞区间显示「未采集」空槽 | ✅ 离线(视觉):Tab3 波形 + 分段(15 泳道 + 段检查器)截图 `t37-output-wave.png`。分析流水线(VAD/分段/响度/分配)的 core 断言 = `test_vad.cpp`/`test_segmentation.cpp`/`test_assign.cpp`/`test_balance.cpp`。🖥️ 真机补:「<1s 出结果」实测耗时 → §3 清单 L-8 |
| 9 | 拖 VAD 阈值/灵敏度滑杆,松手等自动应用(抑制时点「应用到分段」) | 拖动中分段预览实时重算(<50ms,不写曲线);松手 300ms 防抖后自动跑流水线(仅 origin=auto 未锁段);仅 PRINT/分析中才抑制;用户段任何路径不动 | ✅ 离线(逻辑):`smoke-tab3-interactions.mjs`(纯函数 + mock 端到端,重分析保护用户段等)。视觉:Tab3 截图 `t37-output-wave.png`。🖥️ 真机补:拖动全程零曲线写入的交互手感 + 300ms 防抖时序 → §3 清单 L-9 |
| 10 | 试听:**输出开关 OFF→ON**(DAW 自动化不 arm) | OFF→ON 一次性非模态确认(双后果文案);确认后引擎接管 DSP 立即听到平衡;A/B 开关切听 | ✅ 离线:`print-guard` 场景(打印守卫)+ write 确认文案。截图 `t37-output-master-printguard.png`。🖥️ 真机补:真实 DSP 听感 + 「不 arm 仅试听不落盘」在宿主 Read 模式下的行为 → §3 清单 L-10 |
| 11 | 微调:拖段边界/改段 pan/vol/编辑 pan 曲线/调 Width | 改动即入当前版本曲线,`origin=user_edited`;试听实时反映 | ✅ 离线(逻辑):`smoke-tab3-interactions.mjs`(段编辑 origin/locked);`smoke-curve-editor.mjs`(曲线编辑纯函数)。视觉:`t37-output-wave.png`。🖥️ 真机补:拖动手感 + 实时试听 → §3 清单 L-11 |
| 12 | 满意后复制 V1→V2,在 V2 上做更宽版本 | 版本复制 = state 曲线深拷贝,零 gesture;随时切 `version_active` 对比 | ✅ 离线(逻辑):版本复制/切换的 core 断言 = `test_version.cpp`/`test_version_params.cpp`;Tab1 版本 chip 视觉见 `t37-output-master-fifteen.png`。🖥️ 真机补:复制过程零 gesture(不产生自动化点)→ §3 清单 L-12 |
| 13 | **write 落盘**:总线轨自动化设 **Latch(推荐)或 Write;不用 Touch**;输出 ON,播放 range | PRINT 进入不弹窗(覆盖确认已在步 10 完成);打印中圆点 + footer「引擎驱动 V2 · 33–49 小节」;播放中参数被 DAW 录制(稀疏节点);离开 range 自动闭合 gesture | 🖥️ **待用户上机**(write 模式必须 DAW)。→ §3 清单 L-13 / 六类验收「宿主矩阵」 |
| 14 | 写完停止;检查 DAW 自动化;**输出开关 OFF** | 停止(PRINT→ARMED)footer 短提示「本次打印覆盖…建议切回跟随宿主试听核对」;OFF 后 DSP 跟随 DAW 自动化回放,听感与试听一致(S2 验收) | 🖥️ **待用户上机**。→ §3 清单 L-14 |
| 15 | 局部重做:勾第 3 轨、拖工作选区 →「重采集选区」→ 播放 → 重分析 | §4.2/4.3 流程(布防用工作选区,全局 Range 不变);完成后 diff「已更新 3 个区间;其余区间与其他版本未改动」 | ✅ 离线(视觉):`recapture-armed` 场景(工作选区布防态)截图 `t37-output-wave-recapture.png`;重分析逐字节不变性 core 断言 = `analysis_reanalysis_test.cpp`。🖥️ 真机补:真实重采集覆盖写入 + 其余 14 轨逐字节不变(§8 局部重做)→ §3 清单 L-15 |
| 16 | 后续某天改了某轨 EQ | 播放时 fingerprint 不匹配 → 该轨该区间 ⚠ 斜纹「数据可能过期」→ 回到步 15 | 🖥️ **待用户上机**(需真实上游改动 + 播放)。SL-177 起整条通路已实装并有离线断言:fingerprint 打包往返(`test_ipc_lifecycle.cpp`)、两端指纹一致性与滞回/10% 判定(`test_feature_fingerprint.cpp`)、Input 命令环 → Output `stale` 端到端(`test_output_session.cpp`「SL-177 端到端」)、三处提示真渲染(`web-preview/tests/smoke-output-stale-page.mjs`)。真机剩下的是「真实 EQ 改动 + 播放」这一跳。→ §3 清单 L-16 |

---

## 2. Processor 级回归(deepseek 两条,core/桥级锚点)

> 真机路径在 `ScvbInputAudioProcessor`(依赖 WebView2,不可离线编入 Catch2)。已在
> `tests/core/test_input_bridge_ipc.cpp` 用 `InputSession` + `CtrlPlane` + `InputStateCodec`
> 逐段复刻同一编排,补齐两条离线断言(deepseek 要求):

| 回归 | 锚点断言 | 结果 |
|---|---|---|
| **Input 载入组≠1 工程 → remoteSetPriority 落对组** | 解码 `group=2` 的 Input state → `ctrl_.changeGroup(2)`(等价 `setStateInformation` 对齐)→ `enqueue(ch, kSetPriority, 7)` → 断言 g2 环可见、g1 环无残留 | ✅ 单测 `T37 Processor 回归(deepseek):载入组≠1 工程 → remoteSetPriority 落对组` |
| **srMismatch 读对组** | g1 写 SR=44100 → `changeGroup(2)` → g2 写 SR=48000 → `readGlobalInfo().output_sample_rate == 48000`(不读 g1 残留) | ✅ 单测 `T37 Processor 回归(deepseek):srMismatch 读对组 —— changeGroup 后 readGlobalInfo 读新组 SR` |

两断言进入 `scvb_tests`(ctest 全量)。无法离线的 WebView2/宿主接线面已并入 §3 清单。

---

## 3. DAW 真机执行清单(可勾选)

> 打印后照着打勾;产物目录建议 `C:\Users\lenovo\deepseekHarness\S1-2026-08-16\T37\`,
> 截图命名 `t37-<格子号>.png`。主测 DAW = Cubase 15(48000 / buffer 512),REAPER 7 兜底。

### 3.1 十六步中的真机步(与 §1 对齐)

- [ ] **L-2** 轨道名 label 自动填入(TrackProperties.name,kChannelUIDKey 透传,JUCE patch 项)
- [ ] **L-3** 播放几秒:电平真实跳动;失准计数 = 0(无时间线数据 → 黄警告「未收到 Track N 音频」)
- [ ] **L-4** 每轨设置即改即存;Input UI 经 ctrl 广播(config_seq)同步显示
- [ ] **L-5** `daw_loop` 模式下拖 DAW 循环区 → 插件 range 实时跟随;宿主无 loop 时「用循环区」disabled + tooltip
- [ ] **L-6** 采集 ON + 播放 range:绿点脉冲「采集中」;覆盖进度条实时生长;离开 range → 琥珀 +「已离开采集范围」
- [ ] **L-7** 采集 OFF:每轨覆盖率汇总;有效唱段 <1.5s 的轨黄标「样本不足」;FrameStore 转只读
- [ ] **L-8** 点分析:<1s 出结果;时间线分段边界 + 段响度 + pan/vol 曲线;洞区间「未采集」空槽
- [ ] **L-9** 拖 VAD 阈值:拖动中预览(<50ms)且**拖动全程零曲线写入**;松手 300ms 防抖自动应用(仅 origin=auto 未锁段);PRINT/分析中才抑制
- [ ] **L-10** 输出 OFF→ON:一次性确认文案(双后果);引擎接管 DSP 立即听到平衡;Read 模式试听不落盘(S2 验收)
- [ ] **L-11** 微调段边界/pan/vol/Width:`origin=user_edited`;试听实时反映
- [ ] **L-12** V1→V2 复制:state 深拷贝,零 gesture(复制过程不产生自动化点);随时切 `version_active` 对比
- [ ] **L-13** write 落盘:Latch(推荐)或 Write(**不用 Touch**);PRINT 进入不弹窗;footer「引擎驱动 V2 · 33–49 小节」(follow 档走 follow 变体);参数被 DAW 录制(稀疏节点);离开 range 自动闭合 gesture
- [ ] **L-14** 写完停止:PRINT→ARMED footer 短提示;输出 OFF 后 DSP 跟随 DAW 自动化回放,听感与试听一致(S2 验收)
- [ ] **L-15** 局部重做:勾第 3 轨 + 工作选区 → 重采集 → 重分析;diff「已更新 3 个区间;其余区间与其他版本未改动」;其余 14 轨与第 3 轨选区外曲线逐字节不变(§8 局部重做)
- [ ] **L-16** 改某轨 EQ → 播放 fingerprint 不匹配 → 该轨 ⚠「数据可能过期,建议重新采集」(SL-177 已实装:轨级提示 = 横幅 ⑧ + tab 导航琥珀点 + 泳道 ⚠;区间级斜纹待 `requestWaveform.stale[]` 接线)

### 3.2 04 §8 六类验收(真机部分;其余已由 Catch2 覆盖)

- [ ] **特征/coverage**:5 分钟单轨采集帧数 = 30000±1;内存 <1MB(真机只验帧数,内存由单测口径折算)
- [ ] **采集闭环**:4 小节循环 ×10 遍 → coverage 单区间且逐 hop 值等于最后一遍;OFF 后写入路径 assert 触发 0 次(release 静默丢弃)
- [ ] **分析闭环**:松手 300ms 防抖 <1s 且只有 origin=auto 未 locked 段被改写;抑制路径(PRINT/分析中)下零自动写入;有手动/锁定段时自动应用**照常发生**(diff 首行报保留数)
- [ ] **局部重做**:`analysis_reanalysis_test` 已离线绿(重做轨3 30–40s 后其余序列化逐字节不变)
- [ ] **持久化**:roundtrip save→load→save 两次 chunk 逐字节一致;>8MB 自动转 sidecar 且回读成功;删除 sidecar 后加载曲线完好 + 横幅提示(真机只验 >8MB/sidecar 场景)
- [ ] **宿主矩阵**:S1/S2 spike(Reaper/Cubase/Live/Studio One × 实时/离线)loop 字段表、采集对齐、write/latch 回读;pluginval strictness 5 全绿(非 GUI 已由 CI/gates 覆盖,全量含 GUI 本地收口)

### 3.3 #68 冻结平直线(write 模式真机验证项)

- [ ] 冻结某轨某维度(freeze 含该维位)→ write 模式走一遍该 range → **冻结维度落平直线**(手动静态值 = 全时间线单段常值,origin=user_edited)
- [ ] 该轨非冻结维度照常打印曲线;其余轨不受影响
- [ ] 落盘后回读:冻结维 = 常值平直线;与 §4.1/#68「write 时打印平直线」一致

### 3.4 D3 三通道真机收口

- [ ] 通道 1:`scripts/build.ps1 -Install` 装机后 DAW 能扫到「SCVB Input / SCVB Output」并加载(本次的装机记录见 §4.1)
- [ ] 通道 2:web-preview 六 fixture + Input 七态在浏览器可点(本次已跑,见 §4.2)
- [ ] 通道 3:CI artifact 解压到临时 VST3 目录后 DAW 能扫到(本次的下载/层级记录见 §4.3)

---

## 4. 离线验证记录(本次已跑)

### 4.1 通道 1:`scripts/build.ps1 -Install`(干净构建 → 装机)

干净构建目录 `build/`(此前不存在)→ `scripts/build.ps1 -Install -BuildDir build`:

- 依赖预检:cmake 3.31.12 / MSVC 2019 BuildTools / JUCE 8.0.8 全绿;
- 构建:5 个 target 全产出;**ctest 5/5 全绿**(scvb_tests / scvb_params_tests / scvb_plugin_common_tests / scvb_input_bridge_tests / scvb_ipc_tests);
- 产物(两个 bundle):
  - `build/src/input/SCVBInput_artefacts/Release/VST3/SCVB Input.vst3`
  - `build/src/output/SCVBOutput_artefacts/Release/VST3/SCVB Output.vst3`
- 装机:本机 `C:\Program Files\Common Files\VST3`(系统级)存在且可写 → 装机落 **系统级**:
  - `C:\Program Files\Common Files\VST3\SCVB Input.vst3`
  - `C:\Program Files\Common Files\VST3\SCVB Output.vst3`
  (脚本语义 = 系统级存在即装系统级,否则回退用户级 `%LOCALAPPDATA%\Programs\Common\VST3`;
  本机用户级目录此前已有旧 build,本次不覆盖)。

**修途发现并修复的 2 个 `build.ps1` 小 bug**(本分支一并提交):

1. 磁盘余量检查 `Get-PSDrive -Name (Split-Path -Qualifier $RepoRoot)` —— `Split-Path -Qualifier`
   返回带冒号的 `C:`,而 `Get-PSDrive -Name` 认不带冒号的 `C`,导致脚本在磁盘检查处必炸
   (`Cannot find drive ... 'C:'`)。已改 `... .TrimEnd(':')`。
2. 版本显示 `Select-String -Pattern 'VERSION'` 首行命中的是 `cmake_minimum_required(VERSION 3.22)`
   而非 `project(SCVB VERSION 0.1.0)` → 输出「版本: 3.22」。已改 pattern `project\(SCVB VERSION`。

修复后 `build.ps1 -Install` 全绿(exit 0),上表即修复后实录。

### 4.2 通道 2:web-preview mock 验证

**web smoke(全部 exit 0,与 gate 3e 同口径)**:`smoke-mock` / `smoke-ready-race` /
`smoke-curve-editor` / `smoke-input` / `smoke-tab1-interactions` / `smoke-tab2-interactions` /
`smoke-tab3-interactions` / `smoke-tab4-settings` / `smoke-tour` —— 9 套全绿。

**截图(17 张,`web-preview/.shots/`,已 gitignore)**:见 §1 表格引用。复现:

~~~powershell
pwsh web-preview/serve.ps1
node web-preview/shot.mjs --tab=master --fixture=fifteen-tracks --out=web-preview/.shots/t37-output-master-fifteen.png
node web-preview/shot.mjs --role=input --scenario=occupied --out=web-preview/.shots/t37-input-occupied.png
~~~

### 4.3 通道 3:CI artifact 下载安装验证

最新 `build-and-validate` 成功 run:`gh run download 32771369536`(event=push,branch=feature/v1,
标题「fix(output): 冻结维度 write 时打印平直线 (#68)」,即当前基线 0e975bf1)。

- artifact 名:`SCVB-VST3-win64-preview-feature-v1-0e975bf1e5e0ce958b82ccb6ba22d1f9131b974a`;
- 解压后层级(双 bundle 完整):
  - `...\src\input\SCVBInput_artefacts\Release\VST3\SCVB Input.vst3\Contents\Resources\moduleinfo.json`
  - `...\src\input\SCVBInput_artefacts\Release\VST3\SCVB Input.vst3\Contents\x86_64-win\SCVB Input.vst3`(3,763,200 B)
  - `...\src\output\SCVBOutput_artefacts\Release\VST3\SCVB Output.vst3\Contents\Resources\moduleinfo.json`
  - `...\src\output\SCVBOutput_artefacts\Release\VST3\SCVB Output.vst3\Contents\x86_64-win\SCVB Output.vst3`(3,956,224 B)
- moduleinfo.json:Name/Vendor `Synchain`/Version `0.1.0`/SDK `VST 3.7.12`/Sub Category `Fx` 均正确;
- 与本机 build 的 bundle 层级一致;DLL 字节数有 ±几 KB 差异(CI=windows-2022 VS2022 vs 本机 VS2019,工具链差异,预期内)。
- 双 bundle 各含 `Contents/Resources` + `Contents/x86_64-win`,完整性 ✅。

---

## 5. 变更清单(本分支修补性改动)

- `tests/core/test_input_bridge_ipc.cpp`:新增两条 T37 Processor 回归(core 锚点),含
  `readGlobalInfo` 换组读对组 + 载入组≠1 state 后命令环落对组。
- `scripts/build.ps1`:修 2 个 `-Install` 阻断/显示小 bug(见 §4.1)——
  磁盘余量检查的 `Get-PSDrive -Name` 驱动器名去冒号;版本显示改匹配 `project(SCVB VERSION)`。
- `docs/spikes/E2E-journey.md`:本文档。
