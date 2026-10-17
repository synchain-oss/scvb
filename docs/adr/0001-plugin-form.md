# ADR-001 · 插件形态:三个插件目标 + 共享核心库(CMake 四主目标)

> 状态: 冻结
> 最后更新: 2026-10-16(对应 constitution/ADR.md v2.4)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-001 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

一个仓库,CMake **四个主目标**:`scvb_core`(静态库:DSP/分析/IPC/状态/版本引擎,可离线单测)、`SCVB Input`(VST3)、`SCVB Output`(VST3)、**`SCVB Monitor`(VST3,[J75]/J81 新增:纯只读监视器)**。JUCE 一个 target 一个插件,不做单插件双模式(用户会同时开十几个实例,双模式易误操作)。
- PLUGIN_CODE:Input=`Scvi`,Output=`Scvo`,**Monitor=`Scvm`**(Snb1 已被 Bridge 占用;厂商码统一 `Snch`)
- BUNDLE_ID:`com.synchain.scvb.{input,output,monitor}`
- PRODUCT_NAME:"SCVB Input" / "SCVB Output" / **"SCVB Monitor"**(显示名带 Synchain 由厂商列免)…最终名在 05/UI 文档定,插件码/bundle id 冻结
- 打包产物由 2 个 `.vst3` 变 **3 个**(`build.ps1 -Target` / `gates.ps1` gate 7/8 的 bundle 允许表与计数 / `build-vst3.yml` 计数同步 2 → 3)
- **发布格式与平台(v2.4/J186)**:Windows x64 发布 **VST3**(三个插件,与上同)。**macOS arm64(Apple Silicon,最低 macOS 11)新增 VST3 + Audio Unit(AU),首批 macOS 产物为 beta**:同一 CMake 目标加 `AU` 格式(`FORMATS VST3 AU`),**不新增主目标**,三插件目标 / PLUGIN_CODE / BUNDLE_ID 不变,AU 沿用同一 PLUGIN_CODE 与厂商码。macOS 产物未签名、未公证(用户自行 xattr);不支持 Intel / Rosetta / GarageBand;**Logic 未实测、不作支持承诺**;**不做 AAX**。**AU 三元组冻结**:`aufx Scvi Snch`(Input)/ `aufx Scvo Snch`(Output)/ `aufx Scvm Snch`(Monitor),首个含 AU 的公开发布后不可改;**AU 参数 ID 与 VST3 同源**:由 ParamID 字符串散列派生(`JUCE_FORCE_USE_LEGACY_PARAM_IDS=0`、`JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1`,两宏由根 CMakeLists 一处给出),与 123 参数面同一冻结点;回归判据 `tests/core/test_au_param_ids.cpp`。

### ADR-001a Monitor 三铁律(v2.1/J81 入宪正文)

**SCVB Monitor 是纯只读监视器**,以下三条为**宪法级**约束,实施不得以任何理由偏离;违反即回退。

| 铁律 | 落实 | 断言 |
|---|---|---|
| **0 自动化参数** | 无 `AudioProcessorValueTreeState`,不调 `createParameterLayout`;宿主自带 bypass 由 JUCE wrapper 合成,不占 123 的预算(它在 VST3 里带 `kCanAutomate` 与 `kIsBypass` 两个标志,宿主照样能自动化它;v2.3 / [J183] 订正原句「不占自动化位」) | `getParameters().isEmpty()` + `getBypassParameter()==nullptr` |
| **音频直通(逐样本按位相等)** | `processBlock` 对 buffer **什么都不做**;`isBusesLayoutSupported` 只接受进出一致的 mono/stereo,故无需补清尾声道 | `memcmp` **按位**比对(非近似):mono/stereo × {1,64,512} 块长 × 含 0/−0/非规格化/极值的样本,外加连续 200 块无累积 |
| **对任何共享段零写入** | registry 只经 `openExistingReadOnly` 探测;viz 只经 `VizPlane::attachReadOnly()`;**不 claim InputSlot/OutputSlot、不碰 ctrl 段** | 段不存在时 Monitor 跑完一轮**段仍不存在**(只读方绝不建段);写方发布一帧后 Monitor 跑 50 块音频 + 12 拍 [M],段内容 `memcmp` 一字未变,写方 `foreignThreadWrites()` 恒 0 |

推论(不另立条款,但实施须知):Monitor **不注册 InputSlot/OutputSlot、不 claim 任何组**,对既有注册表/心跳/接管/看门狗机制**零改动**;其 state 与 Input state **完全同形**(`group_id` / `ui.scale` / `ui.language`,`channel_id` 恒 0 且不参与语义),复用既有 `InputState` 的 CFGS payload 布局,**无新 chunk、无新字段、无新 abi**。组切换走**只读专用**路径(释放旧句柄 + 换组,**不 attach、不创建**)—— 复用写方的 `changeGroup()` 会让「新组还没有写方」变成「我来建一个空段」,正好破坏第三条铁律。

## 修订历史(摘自 constitution/ADR.md)

- **[J22→ADR-001 澄清]** 「三目标」指三主目标;juce_add_binary_data 资源目标与 scvb_tests 等辅助目标不违宪。
- **[J81h→ADR-001 改写]** 主目标 2 插件 → **3 插件**:新增 `SCVB Monitor`(VST3,`PLUGIN_CODE=Scvm`,`com.synchain.scvb.monitor`),纯只读监视器。授权来源已齐备(J75 用户裁决「新增同仓第三 VST3 target SCVB Monitor」+ 01 文末 J75 注记 + 05 文末 J75 节 C),缺的只是落到 ADR 本体 —— **J22 的豁免管不到它**:J22 只把 `juce_add_binary_data` 资源目标与 `scvb_tests` 等**辅助**目标排除在外,第三个 `.vst3` 是**主**目标。标题措辞取「三个插件目标 + 共享核心库(CMake 四主目标)」以保 J22「三目标 = 三主目标」的原语义不被推翻。来源:PR #94 `docs/contract-changes/20260825-monitor-target.md`。
- **[J81i→ADR-001a 新立]** Monitor **三铁律**(0 自动化参数 / 音频直通逐样本按位相等 / 对任何共享段零写入)入 ADR 正文,连同各自的落实方式与断言口径。理由:三条都是「一旦破了就再也发现不了」的性质(直通用近似比对会漏掉非规格化差异;零写入用注释承诺等于没有承诺),写进宪法才有回退依据。
- **[J183→ADR-001a 措辞订正]** 三铁律表「0 自动化参数」一格的「落实」列原写「宿主自带 bypass 由 JUCE wrapper 提供,不占自动化位」,
  改为「由 JUCE wrapper 合成,不占 123 的预算」,并注明宿主照样能自动化它。依据 JUCE 8.0.8(仓内 `.juce-version`)的 VST3 wrapper 源码
  `modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp`:插件的 `getBypassParameter()` 为 `nullptr` 时,wrapper 自建一个
  `AudioParameterBool`(ID `byps`)并追加到导出的参数表末尾(VST3 要求 bypass 必须导出);它的参数信息带 `kCanAutomate`
  (`AudioParameterBool` 默认可自动化)与 `kIsBypass`。所以宿主能自动化它,「不占自动化位」不对;它不占的是插件自己声明的参数的预算
  (Output 的 123 个 —— params-v0 §一「宿主可见 124」本来就把它算在内)。
- **[J186→ADR-001 增补]** 新增「发布格式与平台」一条:Windows x64 VST3 不变;**macOS arm64(Apple Silicon,最低 macOS 11)新增 VST3 + AU,首批 macOS 产物为 beta**。
  同一 CMake 目标加 `AU` 格式,不新增主目标;三插件目标 / PLUGIN_CODE / BUNDLE_ID 不变;macOS 产物不签名不公证,不支持 Intel / Rosetta / GarageBand,
  Logic 未实测、不作支持承诺,不做 AAX。同条冻结 AU 三元组(`aufx Scvi|Scvo|Scvm Snch`)与 AU 参数 ID 的派生方式(与 VST3 同源,两宏由根 CMakeLists 一处给出)。
  理由:与 J81 同类 —— 新增平台与新增插件格式,J22 的豁免只管辅助目标,管不到;按 J183 先例在 tag 前写进正文,免得代码、CI 与宪法互相矛盾。
  「beta」只描述首批 macOS 产物的发布属性,**不把 beta 固化为永久状态**。
