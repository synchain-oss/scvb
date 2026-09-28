# 契约变更说明 —— 20260928-j140-undo-coverage

> **状态:实质已批(用户 2026-09-28,J140)。** 用户原话:「13做,2也做吧,如果daw有自动化的话,插件这边撤销了,daw那边会强行纠正回来的对吧(根据自动化),这样应该其实没事,不会出现冲突。你检查一下。因为如果daw那边没有写入自动化的话,这边相当于就没办法撤销了。」
> 来由:SL-536(用户 v5.6.19 实测 B48 附带,J120「撤销对轨道页面里面的大部分开关都没用……这些优化一下吧」)。调查结论是「零缺陷,全部是契约设计不入撤销栈」,用户据此裁定三类都改成可撤销,其中第 ② 类以「先核实 DAW 侧前提」为条件 —— 核实结果见下「② 的前提核实」,结论:**成立**,按裁定实施。本文档与实现放在同一个 PR 里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:没有增删参数,ParamID / index / 顺序 / skew / versionHint 一个字节都没碰。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:撤销栈不落盘(与既有 CRVS 撤销同);载入工程时照旧整栈清空。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— 只改**「撤销」行与撤销性总则**,函数名 / 签名 / 事件名 / 载荷字段一个都不改(`contractVersion` 不升:§9.0 第 3 条只在桥面改名 / 删除 / 收窄时升):
  - §0.9 总则:左列加 `setChannelConfig`、`beginParamGesture`/`setParam`/`endParamGesture`,`setTrackManual` 由「仅未冻结的手动接管通道」改为两条通道都入;右列删掉 `setChannelConfig`;表下新增五条一体适用的规则(单栈 / 经宿主参数通路写回 / PRINT·ARMED 与用户编辑同规矩 / 一步的粒度与合并 / 记绝对值)。
  - §1.12-§1.14「撤销」行:否 → 是(一次 gesture = 一步)。
  - §1.15「撤销」行:否 → 是(只记 patch 里给了的字段)。
  - §1.16「撤销」行:按通道分叉(冻结否 / 接管仅回滚曲线)→ 两条通道都入,接管那一步连参数面与冻结位一起回滚;「线程/频率」行确认条理由那一句随之订正(「不入撤销栈」不再成立,确认条的理由收回到「不替换任何段」)。
  - §1.25/§1.26「语义」行:「覆盖左列的四类操作」→「全部」(该句自 [J82] 起就已过期,`web/output/tab-master.js` 撤销可用性注释里登记过);回推顺序补一句(参数面 / state 先于段表同拍)。同一句 #315 同日另行订正为「全部入栈操作」(`20260928-rc-misc-contract-corrections.md` ②,其中「现为 …」那份枚举是 [J140] 之前的左列),合并后的定稿 =「覆盖 §0.9 左列的**全部**入栈操作」+ 本卡的回推顺序;左列枚举以 §0.9 表为准。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

**一句话**:轨道页与 Tab1 上凡是用户能改的开关 / 旋钮,改动都进插件自己的撤销栈(Ctrl+Z / Ctrl+Shift+Z 与 header 两钮),与段编辑、分析同一条栈、按时间倒序弹。

| 类 | 落点(用户可见控件) | 桥面入口 | 一步 = | 合并 |
| --- | --- | --- | --- | --- |
| ① | A1-A7:启用 / 名称 / 优先级 ± / 主唱锁定 / 音量豁免 / 参与自动声像 / 配对 | `setChannelConfig` | 一次调用(只记给了的字段) | 优先级、配对的单字段连按 ≤300ms 并;开关与名称不并 |
| ② | 冻结 P / V 两枚开关、每轨 W 旋钮、Tab1 的 WIDTH / MS BALANCE / LEAD SELECT | gesture 三段式 | 一次 gesture(起点 ≠ 末值才压) | 连续量 / 步进量同一参数 ≤300ms 并;冻结开关不并 |
| ② | 冻结态的 PAN 旋钮 / 音量卡箍 | `setTrackManual`(冻结通道) | 一次调用 | 不并(UI 已按 300ms 防抖) |
| ③ | 未冻结时首次拖音量卡箍(接管) | `setTrackManual`(接管通道)+ UI 跟进的冻结 gesture | 段表 + 参数面 + 冻结位,一步 | — |

实现落点:`src/output/ParamUndo.h`(`ParamWriteAction` / `CoalescibleUndoAction`)、`ScvbOutputAudioProcessor` 的 `uiBeginParamGesture` / `uiSetParam` / `uiEndParamGesture`(桥面 `OutputEditor::handle*ParamGesture` 转过来)、`bridgeApplyChannelConfig`(`ChannelConfigAction`)、`setTrackManual`(冻结通道一步 / 接管通道的追加动作与冻结位占位)、`pushUndoStep`(合并);UI 侧 `settlePendingEdits()` 起把 Tab2 width 旋钮与 Tab1 两条滑轨的「按住拖动中」也纳入中止(与 SL-450 同一条规矩),`app.js` 给两个 tab 一份包过 `setChannelConfig` / `endParamGesture` 回执的桥,用来置亮撤销钮(这两类没有段表事件可认)。

**③ 为什么不让 native 自己在接管时置冻结位**:那会改变 `setTrackManual` 的写入面(host 套件里有多格用例按「未冻结 ⇒ 每次都走接管通道」连续调用,会全部改义),而 UI 本来就在接管后置位。所以 native 只做「认出这次跟进、并进同一步」:接管时压一个旧值 == 新值的冻结位占位,UI 的 `endParamGesture` 在「同一个 freeze id、栈顶仍是接管那一步(事务名带全局流水号逐字核对)、起点 = 接管时的冻结值、末值 = 起点 | 该维度位」四条全中时把新值填进占位;任何一条不中则按普通参数步另起一步(host 用例 UNDO-T 的两条对照臂就是这两种不中)。

## ② 的前提核实(用户要求「你检查一下」)

用户的判断:「DAW 有自动化时,插件撤销了 DAW 会按自动化纠正回来,不冲突;DAW 没有自动化时,插件撤销是唯一的撤销途径。」**成立**,依据如下,边界一并写明。

1. **插件撤销对宿主而言就是一次用户编辑**。撤销 / 重做经 `ParamWriteAction` 注入的 writer 写回,writer 是 `beginChangeGesture` → `setValueNotifyingHost` → `endChangeGesture`(`ScvbOutputAudioProcessor::paramWriter`),与 UI 拖旋钮走的 `handleBeginParamGesture` / `handleSetParam` / `handleEndParamGesture` 同一条通路。JUCE 8 的 VST3 wrapper(`juce_audio_plugin_client_VST3.cpp`,`JuceVST3EditController`)把这三下分别转成 `beginEdit` / `setParamNormalized + performEdit` / `endEdit` —— wrapper 本身就是挂在 processor 上的一个 `AudioProcessorListener`,host 用例用同一层的 `HostSpy` 断「撤销与重做各到达一对 begin/end、且值是经通知通路到的」(UNDO-P / UNDO-F / UNDO-PRINT)。插件**没有**任何绕过宿主的静默写,参数值始终只有 APVTS 一份,不存在「插件以为是 A、宿主以为是 B」的状态。
2. **Read 档且有自动化**:宿主在播放 / 定位时按车道数据写参数,撤销后的值被顶回 —— 与用户拖完旋钮被顶回是同一个结局(用户的判断)。宿主的这次写经 wrapper 的 `setValueAndNotifyIfChanged`(置 `inParameterChangedCallback`,不回 `performEdit`,没有回环);它**不**进插件撤销栈(撤销步只由桥面入口压,UNDO-PRINT 断「宿主顶回来之后仍可重做」);对打印车道参数(每轨 pan/vol)在 ARMED / PRINT 下照常记 `hostEcho`,UI 的「宿主正在写」如实亮起。UI 收到 `scvb.params` 只更新显示、从不回写(§0.5,`tab-master.js` 头注),所以不会与宿主拉扯。撤销本身带打印器自写位,**不**冒充宿主回吐(UNDO-PRINT 断 `hostEchoCount` 不变)。
3. **Write / Touch / Latch 档且在走带**:撤销会被宿主录成一段自动化 —— 与用户在同一时刻拧旋钮完全相同。这是 gesture 通路的直接后果,按裁定接受(写进了 §0.9 第 2 条,用户在这些档下按 Ctrl+Z 会得到一笔自动化写入)。
4. **没有自动化**:值写进去就留着,插件撤销是唯一的回退途径(用户的判断)。
5. **PRINT / ARMED(插件自己的打印态)**:PRINT 区间里打印器替每条 pan/vol 车道常开着 gesture;撤销 / 重做(与冻结通道的手动写入)在这种车道上只写值、不再自己 begin/end,免得嵌套、或替打印器把宿主那边的 gesture 提前关掉(#311 复审【重要】,host 用例 `HOST SL-536:PRINT 中冻结通道写入与撤销 / 重做不与打印器的 gesture 嵌套` 钉住)。这几类用户编辑在 PRINT / ARMED 下本来就不拒绝(§1.12-§1.16 拒绝态行:「这是宿主可录的用户操作面」),撤销同规矩受理。冻结维度在 PRINT 下由打印器按参数当前值重写平直线(#68/J78),撤销改了参数值,打印器随后按新值写 —— 与用户在 PRINT 下拧冻结旋钮同一个结局;接管那一步撤销后冻结位回 0,该维度回到引擎曲线,打印器按(已还原的)曲线写。
6. **与宿主撤销栈的关系**(用户没问、但属于「会不会冲突」):两条栈互不感知。宿主若把插件参数改动记进自己的撤销历史(记不记因宿主与其设置而异,本卡未逐个宿主实测),也会把插件的撤销当成一次新的参数改动记下;交替按两边的撤销是「后写者生效」,不会损坏数据 —— 每一步写的都是一个合法的绝对值、都经宿主通路。边界:插件一步记的是「改前值 → 改后值」的**绝对值**,若其间宿主(自动化或宿主撤销)改过同一参数,插件撤销写回的仍是记下的改前值。同一口径也覆盖 Input 远程改优先级(§3.4,不入插件撤销栈):它若落在两次优先级 ± 的合并窗之间,撤销那一步会把远程那次一并回到连按之前的值(窗口 300ms,影响很小)。另:宿主若把插件整份 state 纳入自己的撤销点(部分宿主有此类选项),宿主撤销会经 `setStateInformation` 重载 state,插件撤销栈照既有规矩整栈清空(既有行为,本卡未改)。
7. **没找到会造成真实冲突的反例**。唯一需要额外处理的是「UI 拖动还按着时按 Ctrl+Z」:gesture 未收尾,撤销先弹掉上一步,松手那一下再压一步把撤销覆盖掉 —— 这是插件内部的次序问题而非宿主冲突,已按 SL-450 的既有规矩(按住中 ⇒ 中止并回到抓握值)补到 Tab2 width 旋钮与 Tab1 两条滑轨。

## 兼容性影响

- 既有工程:无影响(撤销栈不落盘)。
- 既有 DAW 自动化:无影响。撤销写参数的方式与用户编辑相同(见上第 1-3 条);唯一新增的宿主可见行为是「在 Write / Touch / Latch 档走带时按 Ctrl+Z 会录下一笔自动化」,与用户编辑同形。
- 用户可感知:此前 Ctrl+Z 对这些控件「按了像没反应」(实际弹掉的是更早的某次段编辑),现在撤的就是刚才那一下。CHANGELOG 旧条目「冻结中调整不进插件的撤销栈……请用宿主自己的撤销」与 USER_GUIDE 两份的同一段已按本裁定改写。
- 载入只带 PRMS 的轨道 / 参数预设(没有 CFGS):此前不清插件撤销栈 —— 那时栈里只有段表类步,预设不动段表;本卡起栈里有了写 PRMS 的参数步,留着它撤销会把预设里的值撤回载入前,所以改为与载入工程同口径整栈清空(段表类步一并清掉,JUCE `UndoManager` 不能按类筛)。落点在 `setStateInformation` 的 PRMS 读回处,CFGS 缺失 / 解不开的两处早退之前(#311 第 7 轮复审【重要】;host 用例 UNDO-L)。
- 新旧版本互通:桥面名字 / 载荷零变化,mock 后端(`web-preview/mock/juce-bridge-mock.js`)的 `undo`/`redo` 仍是「栈空」桩,不受影响。

## 审批

- 实质:用户 2026-09-28 J140 批准(见文首原话)。
- PR 挂 `status/frozen-contract` 标签。
