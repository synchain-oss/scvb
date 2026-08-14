# S2 自动化写入 spike 设计与实现

> 卡 = T03(07-execution-plan §T03);真源 = 03 §1/§3/§4/§8、research/08 §10、ADR-006/014、params-v0 v2.1 §一。
> 本文件含「DAW 支持等级表(初版)」;周日上机后回填定版(判据清单见 S2-daw-checklist.md)。

<!-- 会话表头(周日上机填写) -->

| 字段            | 值                                  |
| --------------- | ----------------------------------- |
| 日期 / 执行人   | (待填)                              |
| SCVB commit SHA | (待填)                              |
| 构建配置        | RelWithDebInfo / MSVC / JUCE 8.0.8  |
| 机器            | (CPU / 核数 / RAM / OS build)       |
| DAW             | Cubase 15(主测)/ REAPER 7(兜底)    |

## 1. 目标与范围

验证 123 参数骨架([J59/J65])+ gesture 打印在 DAW 上的 write/latch 录制与回读。spike 插件无 DSP、无分析、无 IPC;唯一被测对象 = 参数布局 + AutomationPrinter 在宿主自动化车道的落点。

## 2. 插件设计

### 2.1 123 参数完整布局(冻结)

- 代码:spikes/s2/S2OutputProcessor.cpp 的 scvb::s2::makeS2Layout()(J16,由 T15 的 PR 删除)。
- 编译期:static_assert(kNumAutomatable == 123)(kNumAutomatable = 3 + 2*15*4)。
- 运行期:构造后 jassert 核对 getParameters() 总数 123、首 ID = width、末 ID = v2_t15_freeze。

索引映射公式(冻结,params-v0 v2.1 §一):

~~~
index(width) = 0; index(ms_balance) = 1; index(lead_select) = 2
index(v, t, k) = 3 + (v-1)*60 + (t-1)*4 + k,   k in {0=pan, 1=vol, 2=width, 3=freeze}
last = index(2, 15, freeze) = 3 + 60 + 56 + 3 = 122; total 123 (index 0..122)
~~~

| index | ParamID | 显示名 | 类型 / 范围 | 默认 | 说明 |
| --- | --- | --- | --- | --- | --- |
| 0 | width | Width | Float 0..150 % | 100 | 全局宽度 |
| 1 | ms_balance | MS Balance | Float -100..+100 | 0 | 总线 M/S 音量比 |
| 2 | lead_select | Lead Select | Int 0..15(step 1) | 0 | 0=遵循分析;1-15=强制该轨居中 |
| 3..6 | v1_t01_pan/vol/width/freeze | V1 T01 … | pan Float -100..100 / vol Float -24..12 dB / width Float 0..100 % / freeze Int 0..3 | 0 / 0 / 100 / 0 | 版本 1 轨道 1 |
| 59..62 | v1_t15_* | V1 T15 … | 同上 | 同上 | 版本 1 轨道 15 |
| 63..66 | v2_t01_* | V2 T01 … | 同上 | 同上 | 版本 2 轨道 1 |
| 119..122 | v2_t15_pan/vol/width/freeze | V2 T15 … | 同上 | 同上 | 版本 2 轨道 15(末位 122) |

- 全部 juce::ParameterID{id, versionHint=1};isAutomatable()==true;interval=0.0f(连续,除 lead_select/freeze 离散步进);skew 一律 1.0(J03)。
- ParameterGroup:Version {v} → Track {t:02d};全局三件在根组(03 §1.9)。
- 宿主可见 124 = 123 + wrapper 合成 bypass(§2.4);Live 128 上限余 4(封顶)。

### 2.2 AutomationPrinter(打印器)

- 挂 Processor(非 Editor,GUI 关闭也打印);juce::Timer 50Hz,写入全在消息线程(ADR-006)。
- 打印面 = 30 条车道(活跃版本 v1 的 15 轨 × pan/vol;spike 不实现版本切换,恒打 v1)。width/freeze/全局三件恒 host 权威、不入车道(J57/J58/J65)。
- gesture 三段式,pass 级长 gesture(03 §3.3 方案 A):进打印 beginChangeGesture() ×30 → 每 tick setValueNotifyingHost() → 出打印 endAllGestures()。出口 = stop / 关 Output / 离开窗口 / 析构(RAII 幂等)。
- deadband 去重:kDeadbandPan=0.5(pan 值域)、kDeadbandVolDb=0.1(dB);lookahead kLookaheadMs=20(补偿 Timer 抖动)。这两个默认值 = 03 待定项⑦,由周日实测校准。
- 打印窗口 = 固定 [0, 30s];区间外零 gesture(R5 → S2-A0)。
- 无 host echo 防回环逻辑(spike 无 DSP,listener 不注册,回吐无引擎可喂;正式版由 03 §3.5 承担)。

### 2.3 固定包络(30s 阶梯)

| 车道 | 档位 | 周期 | 轨道错相 |
| --- | --- | --- | --- |
| pan(15 条) | -60 / -20 / +20 / +60(4 档) | 每 4s 一档 | 相位偏移 (t-1)×0.25 档 |
| vol(15 条) | -6 / -3 / 0 / +3 / +6 dB(5 档) | 每 3s 一档 | 相位偏移 (t-1)×0.5 档 |

30 条车道值互不相同,便于在宿主车道上一一辨认;阶梯形状同时服务「写录制判据」与「段边界必写点」观察。

### 2.4 wrapper 合成 bypass(待定项②)

spike 不声明 bypass 参数(避免扰动 123 布局)。JUCE VST3 wrapper 会在参数列表末尾自行合成一个;宿主可见总数 = 124。周日实测其确切索引位置(应为 123,即 123 之后)。

## 3. 构建与装机

~~~powershell
$env:JUCE_PATH = 'C:\Users\lenovo\deepseekHarness\juce'
$env:PATH = 'C:\Users\lenovo\deepseekHarness\tools\cmake-3.31.12-windows-x86_64\bin;' + $env:PATH
pwsh scripts/gates.ps1 -Quick -BuildDir build-T03
# 产物:build-T03\SCVBS2Output_artefacts\Release\VST3\SCVB S2 Output.vst3
~~~

安装到用户级 VST3 目录 %LOCALAPPDATA%\Programs\Common\VST3(Cubase 扫描即见)。

## 4. DAW 支持等级表(初版,上机前)

> 初版依据 research/08 §2.1 + 03 §4,周日实测后回填为定版。Tier 1=完全支持 / Tier 2=有限制 / Tier 3=不支持。

| DAW | 初版等级 | 已知坑 / 前置(03 §4) | S2 验证点 |
| --- | --- | --- | --- |
| REAPER 7 | Tier 1(附条件) | GUI 关闭不写自动化(R1,VST 确认,VST3 待验);Parameter automation notifications = process all notifications | S2-R1 |
| Cubase 13/14/15 | Tier 1(附条件) | 插件自发变化录不进(R2,无公开解);自动化藏 Ins 隐藏车道(R3) | S2-C2(最高优先)/ S2-C1 |
| Ableton Live 12 | Tier 1(附条件) | 128 参数上限;Re-Enable Automation 频繁亮起(R4/R9) | S2-L1 / S2-L2 |
| Studio One 7 | Tier 1(附条件) | 自动化模式须在插件窗口内设 Write/Latch | S2-S1 |
| FL Studio 21 | Tier 2(预判) | automation clip / Last tweaked 为中心;第三方插件参数不被识别的报告(R16) | S2-F1 |

> J15 停线口径:Cubase 录不进且三级 fallback 全败 → 不停整线,触发 Cubase 取舍决策(Cubase 降 Tier 2 +「导出自动化中间格式」升 v1.x);区间外被覆盖 → 停线(数据破坏级)。

## 5. 待验证项(回填)

| 项 | 内容 | 结论(周日) |
| --- | --- | --- |
| 03 待定项① | REAPER「关 GUI 不写」在 VST3 下是否复现 | (待填) |
| 03 待定项② | wrapper bypass 确切位置 / 行为 | (待填) |
| 03 待定项⑦ | deadband / lookahead 默认值校准 | (待填) |
| 04 异议 4 | Read 模式下「输出 ON 试听」是否意外落盘 | (待填) |

## 6. 结果回填(周日)

| 验收 | 判据 | P/F | 备注 |
| --- | --- | --- | --- |
| S2-C2(最高) | Cubase Write/Latch 录进车道 | | |
| S2-A0 | 区间外手工自动化完好 | | |
| S2-R1 | REAPER VST3 GUI 开/关 Write/Latch 落点 | | |
| S2-L1/L2 | Live Re-Enable + 124 参数 configure + 余 4 | | |
| S2-S1 | Studio One 窗口内 Write/Latch 落点 | | |
| S2-F1 | FL automation clip | | |
| 回读一致性 | 关 Output 回放数值与打印一致 | | |
