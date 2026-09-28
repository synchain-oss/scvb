# 契约变更说明 —— 20260928-j150-snapshot-host

> **状态:实质已批(随本 PR 挂 `status/frozen-contract`)。** 用户 2026-09-29 发布前 17 条拍板,
> 第 6 条「03 §4 宿主专属界面提示没做;04 §7『轨道名自动填入 label』没做」—— 用户原话「**6 两件都做**」,
> 统筹记为裁定 **J150**(masterPlan `plan/adjudications.md`):「①03 §4.2/§4.4:REAPER 进 PRINT 弹一次性
> 提示 + 常显『打印期间请保持插件窗口打开』;Live 打印结束提示 Re-Enable Automation(juce::PluginHostType
> 判宿主)…… IPC/契约变更实质已批,走变更文档」。本变更是 J150 **①** 那一半需要的桥面字段;
> ②(轨道名自动填 label)另走自己的 PR,与本文无关。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**:宿主标识只在 Output 自己的桥面上,不跨进程。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:宿主标识是运行期判定,不入 state chunk、不随工程保存。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— **§1.1 `requestInitialState()` 返回的快照加一个可选字段 `host`**;
      同条「语义」格的「快照专属键」清单并入 `host`,并补一段 `host` 的取值与用途。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`:这是 §0.1 第 3 条允许面里的「在既有 payload 中新增**可选**字段」,
不改名、不删、不改既有字段语义、不收窄任何取值域;§7 manifest 的 `requestInitialState` 返回写作
`OutputSnapshot`,不逐字段列举,**manifest 零改动**,`check-bridge-parity.mjs` 照旧通过。

## 变更内容

**一句话**:Output 的首帧快照多一个 `host?:"reaper"|"live"|"cubase"|"other"`,页面据此只在 REAPER / Live
上出宿主专属提示(横幅 ⑪⑫⑬)。

| 项 | 定义 |
| --- | --- |
| 落点 | §1.1 快照顶层,与 `session_guid` / `version` / `*_global` / `conn` 同属**快照专属键** —— 不进 §2.1 `scvb.state` 事件 |
| 取值 | 闭集 `"reaper"` / `"live"` / `"cubase"` / `"other"`。C++ 侧在 Output 实例**构造期**用 `juce::PluginHostType` 判定一次:`isReaper()` ⇒ reaper,`isAbletonLive()`(各版本)⇒ live,`isCubase()`(各版本)⇒ cubase,其余(含 Nuendo / Studio One / FL Studio)⇒ other。代码真源 `src/output/HostId.h`;发布点 `ScvbOutputAudioProcessor::hostId()` → `OutputEditor::buildSnapshot()` |
| 为什么只进快照 | 宿主在实例寿命内不变;编辑器每次重建都会重新调 `requestInitialState()`(§0.6),拿得到 |
| 为什么 `cubase` 单列 | Cubase 是主测宿主,页面级冒烟要拿它当「有名有姓、但不该出提示」的反例;并进 `other` 的话「Cubase 上不出」只是「other 上不出」的同义反复 |
| `?` 的含义 | native 本版恒发;`?` 是给 UI 的容忍纪律 —— **缺席或不认识的值一律按 `other` 处理**(不出任何提示)。今后加宿主只许放宽取值域 |
| UI 消费 | `web/output/host-hints.js`:reaper ∧ 输出开关 ON ⇒ ⑪「写入自动化期间请保持本插件窗口打开」;reaper ∧ 本会话进过 PRINT ⇒ ⑫ 一次性首选项提示;live ∧ 打印刚结束 ⇒ ⑬「点 Re-Enable Automation」。三条都是建议类横幅,带 ✕(沿 [SL-373] `showDismissible` 口径) |

### 改动面(契约文本两处)

| # | 位置 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | §1.1「返回」格 | 末两行 `version:{plugin, abi}`、`conn:…` | 两行之间加 `host?:"reaper"\|"live"\|"cubase"\|"other",` |
| ② | §1.1「语义」格 | 快照专属键清单 `session_guid / version / guide_seen_global / tour_seen_global / lang_chosen_global / conn` | 清单并入 `host`;格末补一段 `host` 的取值、判定时机、用途与容忍纪律(即上表) |

## 用词与规格的一处出入(报统筹)

J150 与 03 §4.2 引的横幅原文是「打印期间请保持插件窗口打开」。界面上**没有**照抄「打印」二字:
[J88] 用户把输出开关 ON 档改名为「写入自动化」,此后 zh 界面文案里不再出现「打印」(那是规格里的内部说法),
所以 ⑪ 写作「REAPER:写入自动化期间请保持本插件窗口打开——窗口关着时 REAPER 可能不写入自动化」。
同理 ⑫ 不说「车道」([J97] 用户裁定界面用词不用「车道」),写作「若写完后没有录到自动化」。
这两处是用词对齐,不改变提示的内容;若统筹认为应逐字照 J150 引文,改 `web/shared/i18n.js` 三条词条即可。

## 验证

- **C++**:`tests/host/test_host_harness.cpp` 的 `HOST J150` —— 注入九种 `PluginHostType::HostType`
  (Reaper / AbletonLiveGeneric / AbletonLive11 / SteinbergCubaseGeneric / SteinbergCubase10_5 /
  SteinbergNuendoGeneric / StudioOne / pluginval / UnknownHost)后构造的 Output 实例,`hostId()` 各报各的值;
  不注入时 harness 进程报 `other`;注入只影响之后构造的实例。
- **native 发布点**:`OutputEditor.cpp` 编不进任何 C++ 测试目标,由 `web-preview/tests/smoke-host-hints.mjs`
  的源码钉子锁住 `buildSnapshot` 里那一行 `put(o, "host", juce::var(processor_.hostId()));`。
- **页面**:`smoke-host-hints.mjs`(纯函数真值表 / mock `?host=` 接线 / 三语词条 / 模板与接线钉子)+
  `smoke-host-hints-page.mjs`(无头 Chrome:reaper / live / cubase / 缺省 四个宿主 × 进出 PRINT,
  以及「走带位置冻住时边沿只从 scvb.state 来」一格)。
- 删除式逐落点一格(读数见 PR 描述)。

## 兼容性影响

- **对旧工程 / DAW 自动化**:零影响 —— 不动参数面、不动 state、不发 gesture。
- **新旧版本互通**:桥面只在同一个插件二进制内部(页面与 native 同包发布),不存在跨版本对端;
  即便有,旧页面忽略未知键、新页面把缺席按 `other` 处理,两个方向都安全。
- **用户可见**:只在 REAPER / Ableton Live 上多出提示横幅,都可点 ✕ 关掉;Cubase 与其他宿主上界面零变化。

## 审批

- 实质:用户 2026-09-29「6 两件都做」(裁定 J150,「IPC/契约变更实质已批,走变更文档」)。
- 本 PR 挂 `status/frozen-contract`;用词出入见上一节,已在 PR 描述里向统筹报告。
