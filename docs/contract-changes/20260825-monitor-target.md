# 契约变更说明 —— 20260825-monitor-target

> **状态:待用户批准。** 本文件是 scvb 仓侧的变更真源。合并 + 批准后,由**统筹**执行
> masterPlan `constitution/ADR.md` 的 **ADR-001 修订**(两插件 → 三插件),再把仓内只读副本
> `docs/constitution/ADR.md` 与派生的 `docs/adr/0001-plugin-form.md` 同批更新
> —— 那两份是 sha256 同步面 / 「不得单独改」面,**本 PR 一个字节都不动**。
>
> 卡:**T45**(`plan/07-execution-plan.md` §T45)/ 裁决:**J75** /
> 规格真源:`plan/05-ui-spec.md` 文末「J75 增补规格」节 C + `plan/01-architecture.md` 文末 J75 注记。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **本体不动**;Monitor 的「0 参数」只在此文件与 README 备注
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动(viz 段是 T44 的事,见 `20260825-viz-segment.md`)
- [ ] docs/STATE_SCHEMA.md(state schema)—— **零新增**,理由见下
- [ ] docs/SCVB_CONTRACT.md(桥契约)—— **本体不动**;Monitor 桥面(1 专属函数 + 4 事件)
      尚未进 §7 `manifest.monitor`,随 T46 与 ipc v1.6 修宪同批转正(理由见下「桥面」节)
- [ ] tests/golden/ —— 不动
- [x] **ADR-001「插件形态:两个插件目标 + 共享核心库」** —— 主目标数 2 → 3

## 变更内容

新增第三主 VST3 目标 **`SCVBMonitor` / "SCVB Monitor"**(`src/monitor/`),纯只读监视器。

| 项 | 值 |
|---|---|
| PLUGIN_CODE | `Scvm`(`Scvi`=Input / `Scvo`=Output / `Snb1`=Bridge 已占) |
| PLUGIN_MANUFACTURER_CODE | `Snch`(与两插件同厂商码) |
| BUNDLE_ID | `com.synchain.scvb.monitor` |
| PRODUCT_NAME | `SCVB Monitor` |
| FORMATS | VST3 |

### ADR-001 为什么需要改

`docs/adr/0001-plugin-form.md`(状态:冻结)写的是「CMake **三个目标**:`scvb_core` + `SCVB Input` +
`SCVB Output`」,J22 的澄清只把 `juce_add_binary_data` 资源目标与 `scvb_tests` 等**辅助**目标排除在外
—— 第三个 `.vst3` 是**主**目标,不在 J22 的豁免里。

授权来源已经齐备,缺的只是把它落到 ADR 本体:
- **J75 裁决**(2026-08-20,用户):「新增同仓第三 VST3 target **SCVB Monitor**」;
- **01-architecture 文末 J75 注记**:「仓内新增第三 VST3 target(T45):纯只读监视器,音频直通,
  0 自动化参数;不注册 InputSlot/OutputSlot、不 claim 任何组 —— 对既有注册表/心跳/接管/看门狗机制
  **零改动**」;
- **05 文末 J75 节 C**:定位/布局/数据面全在。

建议的 ADR-001 修订文字(供统筹修宪时取用):

> 一个仓库,CMake **四个主目标**:`scvb_core`(静态库)、`SCVB Input`(VST3)、`SCVB Output`(VST3)、
> **`SCVB Monitor`(VST3,[J75] 新增:纯只读监视器,0 自动化参数、音频直通、只读 attach,
> 不参与注册表/心跳/接管/看门狗)**。
> - PLUGIN_CODE:Input=`Scvi`,Output=`Scvo`,**Monitor=`Scvm`**(`Snb1` 已被 Bridge 占用;厂商码统一 `Snch`)
> - BUNDLE_ID:`com.synchain.scvb.{input,output,monitor}`

### PARAMETERS:Monitor = 0 自动化参数(**本体不动**)

Monitor **没有 `AudioProcessorValueTreeState`**,`getParameters()` 恒为空,`getBypassParameter()` 为
`nullptr`。宿主自带的 bypass 由 JUCE wrapper 提供,不占自动化位。

因此 `docs/PARAMETERS.md` §一的 **123 参数表逐字不动**,也不新增小节 —— 「Monitor 0 参数」这条事实
由本文件 + `README` 承载(与 §三「Input 插件:state(无自动化参数)」同精神)。
断言:`tests/core/test_monitor_processor.cpp` 的「0 自动化参数」用例。

### STATE_SCHEMA:零新增

Monitor 的 state 只有三项 —— `group_id`(看哪一组)、`ui.scale`、`ui.language`,与 Input state 的字段
**完全同形**。故直接复用既有 `scvb::state::InputState` 的 CFGS payload 布局(`channel_id` 恒 0
且不参与语义),容器仍是 T19 的 TLV。**没有新的 chunk、没有新的字段、没有新的 abi**,
`docs/STATE_SCHEMA.md` 一个字节不动。

### 桥面:T45 只落壳,契约 §7 manifest.monitor 归 T46

`src/monitor/MonitorBridgeApi.h` 列的是 T45 壳层的最小桥面:

- **函数 1 个(专属)**:`setObservedGroup(1..8)` —— 组选择。**刻意不叫 `setGroupId`**:
  契约 §1.4 的 `setGroupId` 是 Output 的改组(断开本组全部连接、要弹确认条),
  与「换一个组的 viz 段来看、不 claim、对被观察组零副作用」是两件事;共用名字迟早有人照 §1.4 去实现它。
  通用四函数(`requestInitialState` / `setUiScale` / `commitUiScale` / `setLang`)由 `WebViewHost` 基类注册。
- **事件 4 个**:`scvb.state`(组 / 缩放 / 语言 / viz 三态 / fresh)、`scvb.groups`(1Hz 跨组在线位图)、
  `scvb.viz`(4Hz 帧:降采样车道按 `lane_revision` 带、每轨当前值每帧刷)、
  `scvb.playhead`(25Hz,载荷形状逐字复用 Output 侧 §2.6)。

**键名拼写照契约 §0.2 规则① 与裁定 A-30**:镜像宪法 state 字段的键一律 snake_case ——
`group_id`、`ui:{scale, language}`、`groups_online`(camelCase 在 §8.3 明记为旧文)。

这套桥面**尚未**进 `docs/SCVB_CONTRACT.md §7 manifest`,也不在 `scripts/check-bridge-parity.mjs`
的抽取路径(该脚本只扫 `src/input` 与 `src/output` 两个显式路径);`web/shared/bridge.js` 的冻结名表
**一个字节未动**。

**T46 立项时一并办**:契约 §7 加 `manifest.monitor` → `bridge.js` 加名表 → parity 脚本加抽取路径 →
T46 的页面改用 `createBridge("monitor")`。理由:05 J75 节 C 把 Monitor 的真 UI 与完整桥面归 T46,
T45 若先把一个半成品桥面写进冻结契约,T46 就得走「只增不改」的弯路。

### 本卡**不带页面**

`web/monitor/` 只有一份 `README.md`(目录约定 + 设计盒真源 + native 接口真源指路)。
早期版本带过一个占位页,T46 的真实现(#90)落地后它只剩制造合并冲突的作用,已删除。
`MonitorEditor` 传 `resourceSource = {}` —— 与 `web/input`、`web/output` 的**现状同口径**
(仓内还没有 `juce_add_binary_data` 接线,三个插件的 web 资源都尚未编进二进制)。

### 设计盒 960×720 / 七档

真源 = `web/shared/design-box.js` 的 `DESIGN.monitor` → `scripts/gen-design-box.py` 生成
`src/core/DesignBox.h` 的 `kMonitorDesignW/H` + `kMonitorPresets`;`scripts/check-design-box.mjs`
(gate 3d)逐值对拍;`BridgeBase.h` 的 `designBoxWindowSize` 与 `BridgeBase.cpp` 的
`parseUiScaleArg` **各自都有 monitor 分支**。尺寸依据由 T46 依内容定稿(05 J75 节 C)。

## 三条铁律与其落实方式

| 铁律 | 落实 | 断言 |
|---|---|---|
| **0 自动化参数** | 无 APVTS,不调 `createParameterLayout` | `getParameters().isEmpty()` + `getBypassParameter()==nullptr` |
| **音频直通(逐样本按位相等)** | `processBlock` 对 buffer **什么都不做**;`isBusesLayoutSupported` 只接受进出一致的 mono/stereo,故无需补清尾声道 | `memcmp` 按位比对(非近似):mono/stereo × {1,64,512} 块长 × 含 0/-0/非规格化/极值的样本,外加连续 200 块无累积 |
| **对任何共享段零写入** | registry 只经 `probeGroupsOnline` 的 `openExistingReadOnly`;viz 只经 `VizPlane::attachReadOnly()`;不 claim InputSlot/OutputSlot、不碰 ctrl 段 | 段不存在时 Monitor 跑完一轮**段仍不存在**(只读方绝不建段);写方发布一帧后 Monitor 跑 50 块音频 + 12 拍 [M],段内容 `memcmp` 一字未变,写方的 `foreignThreadWrites()` 恒 0 |

## 顺带修掉的一个隐患(`VizPlane`)

T44 的 `VizPlane::changeGroup()` 按「上次角色」决定重开方式:一个**从未 attach 过**的只读方调它,
`readOnly_` 还是 `false`,于是走 `open()` —— 把「新组还没有写方」变成「我来建一个空段」,
正好破坏零写入铁律。本 PR 加 `setGroupReadOnly()`(释放旧句柄 + 换组,**不 attach、不创建**),
Monitor 的组切换一律走它;`changeGroup()` 的注释同步标明「写方专用」。

## 兼容性影响

- **既有工程 / 既有 DAW 自动化**:零影响。Monitor 是新插件,不改任何既有插件的参数、state、段。
- **新旧版本互通**:Monitor 只读 viz 段;Output 未在该组上线 → `kFailed` → 空态;abi 不符 →
  `kAbiMismatch` → 拒连横幅(J40)。两者可区分,都不崩。
- **打包**:产物从 2 个 `.vst3` 变 3 个。已同步 `scripts/build.ps1`(`-Target` 加 `Monitor`)、
  `scripts/gates.ps1`(gate 7/8 的 bundle 允许表与计数 2 → 3)、
  `.github/workflows/build-vst3.yml`(计数 2 → 3);产物上传是 `build/**/*.vst3` 通配,无需改。

## 审批

- [ ] 用户批准(挂 `status/frozen-contract` 标签)
- [ ] 合并后:统筹执行 masterPlan `constitution/ADR.md` 的 ADR-001 修订(两插件 → 三插件),
      并同批更新 `docs/constitution/ADR.md`(sha256 同步面)与 `docs/adr/0001-plugin-form.md`
