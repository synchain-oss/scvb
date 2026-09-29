# 契约变更说明 —— 20260929-j166-first-enable-gate

> **状态:实质已批(用户裁定 J166),随本 PR 挂 `status/frozen-contract`。** 用户 v0.9.0-rc.1 实测时的原话:
> 「没点知道了开始，自动化还是写入了。能不能不点开始就不开这个开关？」裁定:03 输出首次打到「写入自动化」时,
> 确认条点「知道了,开始」**之前不写**宿主自动化(试听照常),「撤销(回溯到跟随宿主)」照旧;与重开工程的
> 加载守卫同一口径。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:输出开关是 state 不是自动化参数,参数面零变化。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:`print_guard` 仍是运行时态,不入 state chunk,容器 abi 不变。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— 三处**只增**:
  1. §1.3 `setOutputEnabled(on)` → `setOutputEnabled(on, opts?)`:新增**可选**第二参 `opts?: {requireConfirm?: bool = false}`;
     §7 manifest 的 `params` 由 `["on"]` 改为 `["on", "opts"]`;
  2. §1.1 快照与 §2.1 `scvb.state` 的 `print_guard` 新增**可选**字段 `reason?: "restore" | "firstEnable"`;
  3. §1.34 `confirmPrintGuard()` 的语义格补一句:它同时是首次开输出守卫的确认入口(不新增函数)。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`:没有改名、删除、改参数顺序或收窄取值域;§0.1 第 3 条允许「在既有 payload 中新增可选字段」。
§1.3 的第二参是**尾部可选参数**,缺席 / `null` 时行为与改前逐字相同 —— 改前唯一的调用形态 `setOutputEnabled(on)` 不受影响。
§0.1 的允许面没有逐字列出「给既有函数加尾部可选参数」这一形态;统筹裁定 **J166a** 按「只增」处理,`contractVersion` 与 §0.1 都不改(见文末「审批」)。
函数名、事件名集合零变化(`node scripts/check-bridge-parity.mjs` 通过)。

## 变更内容

### §1.3 改前(照录)

> | 参数 | `on: bool`(**两态**:ON=引擎驱动参数 write,OFF=follow host;ADR-005 / J08 维持 bool) |
> | 返回 | `{ok:true}` 或 `{observer:true}` 或 `{ok:false, reason:"noTimeline"}` 或 `{ok:false, reason:"badArg"}`(`on` 不是严格布尔) |
> | 语义 | 写 state `global.output_enabled`。ON 且「播放中 ∧ 在 range 内」= PRINT 态(打印头写 gesture);……加载守卫未确认时**行为止于 ARMED**……

「ON 且播放中 ∧ 在 range 内 = PRINT」这一句就是「首次开输出立即写」的出处:UI 首次 OFF→ON 只调它,
确认条的「知道了,开始」只把条收起,引擎当拍就按这一句进 PRINT。

### §1.3 改后(要点)

- 参数:加 `opts?: {requireConfirm?: bool = false}`;
- 返回:`badArg` 的条件加上「`opts` 在席却不是对象、`opts.requireConfirm` 在席却不是严格布尔」(传歪了宁可整次拒掉,
  不按「没要确认」照开 —— 那个方向会直接写);
- 语义:`on=true ∧ opts.requireConfirm=true` 时,**同一次调用里**把 `print_guard` 置为 `{pending:true, reason:"firstEnable"}`
  (已有加载守卫 `reason:"restore"` 待确认时保持不变);确认前与加载守卫同一条判据:行为止于 ARMED(引擎驱动 DSP、
  零 gesture、零写入,试听不受影响);确认入口同为 §1.34。`on=false` 一律解除守卫(两种来由都一样)。
  「开」与「置守卫」必须是同一次调用 —— 拆成两次桥调用,中间那一拍 25Hz 求值就可能进 PRINT。

### §2.1 `print_guard.reason`

闭集 `"restore"`(加载守卫)/ `"firstEnable"`(首次开输出);仅 `pending=true` 时下发且仅此时有意义;
缺席或不认识的值一律按 `"restore"` 处理(§0.1 容忍纪律)。UI 据它把守卫分给两个既有界面:
`restore` → 05 §2.0 横幅⑦「输出开关处于写入自动化状态(随工程恢复)」+「继续写入自动化」;
`firstEnable` → 05 §2.1 write 确认条「知道了,开始」/「撤销(回溯到跟随宿主)」。

**为什么需要这个字段**:插件窗口关掉再打开,页面是新建的,不知道刚才点过开关。没有来由的话,首次开输出的守卫在
新页面上只能按加载守卫显示 —— 亮出说「(随工程恢复)」的横幅⑦,来由说错了。有了来由,新页面照样显示确认条。

## 实现对照(本 PR)

- `src/output/OutputProcessor.h`:`OutputRuntimeState::printGuardPending`(`atomic<bool>`)换成
  `printGuard`(`atomic<PrintGuardReason>`,None / Restore / FirstEnable)—— 「待确认 + 来由」装在一个 atomic 里,
  读方不会撕成两次读;`printGuardPending()` 保留,等价于「不是 None」。
- `OutputProcessor::setOutputEnabled(on, requireConfirm = false)`:`on ∧ requireConfirm` 时 `compare_exchange` None → FirstEnable。
- `OutputProcessor::timerCallback` 三态求值:判据**不分来由**,仍是「待确认 ⇒ 止于 ARMED」那一行。
- `setStateInformation`(恢复 ON ⇒ Restore / 恢复 OFF ⇒ None)、`confirmPrintGuard`(⇒ None)、
  `applyOutputEnabled(false)`(⇒ None):写点不变,只是写的值换成了枚举。
- `src/output/BridgeArgs.h` `parseSetOutputEnabledArgs`:第二参解析;`OutputEditor::handleSetOutputEnabled` 走它。
- `OutputEditor::buildStateSubtree`:待确认时 `print_guard` 多带 `reason`。
- web:`tab-master.js` 首次 OFF→ON 带 `{requireConfirm:true}`;「知道了,开始」调 `confirmPrintGuard`。
  - **「首次」** = 本窗口(本页会话)里**还没确认过** ∧ 此刻没有守卫待确认。「确认过」(`session.writeConfirmSeen`)
    只在 §1.34 `confirmPrintGuard` **成功回执之后**记,全仓两处:确认条「知道了,开始」(`tab-master.js`)与横幅⑦
    「继续写入自动化」(`app.js`);确认条上过屏、点了「撤销」、调用失败(桥抛错,`call()` 回 null)都**不**记。
    于是没点「开始」就把输出关掉(开关直接关 / 开 01 采集连带关 / 宿主重灌出 OFF)再打开,仍带 `requireConfirm`、
    仍置守卫、确认条再出、点之前不写;确认过之后在同一窗口关再开,不带 `requireConfirm`、开了就写。页面会话位
    不入 state,关窗再开重新计。
  - **确认条显隐** = (意图位 ∨(首次开输出守卫待确认 ∧ `!writeConfirmDismissed`))∧ 输出开 ∧ 守卫待确认(不分来由)。
    `writeConfirmDismissed` 在点「开始 / 撤销」或把开关关掉时置真、下一次首次开输出时清掉:从点下到回推之间守卫位
    还是旧的,不让条多挂一拍。条上那句「点开始之前不写」只在守卫挂着时成立 —— 点横幅⑦确认之后条跟着收起;
    此后(本窗口已确认过)经 01 采集把输出连带关掉再打开,条也不会凭残留的意图位重新上屏。
  - `app.js` 横幅⑦只认 `reason` 不是 `firstEnable` 的守卫。mock 同形(含 §1.2 连带关输出时解除守卫)。

## 判据

- `tests/host/test_host_harness.cpp`「HOST 首次开输出(J166):点开始之前播放零写入,点开始后恢复打印」:真 processor +
  宿主替身 listener,播放中 ∧ 在区间内,`setOutputEnabled(true, true)` 之后 gesture begin / 参数写入次数 = 0、模式 ARMED;
  中途切版本仍为 0;`confirmPrintGuard()` 之后 > 0、模式 PRINT。
- 同文件「HOST 首次开输出(J166):守卫的置位、解除与来由」:撤销解除、非首次不设、Restore 不被改写、确认条挂着时存盘重灌
  ⇒ Restore 仍待确认、J92a 连带关输出解除、上桥字面量。
- `tests/core/test_bridge_args.cpp`「parseSetOutputEnabledArgs」:第二参的形态与默认。
- `web-preview/tests/smoke-output-stale-page.mjs` ⑥b:钮的接线与确认条的显隐(页面级,真渲染)。f 钉「没点开始 ⇒
  关输出(开关直接关 / 开采集连带关 / 宿主重灌出 OFF)再开仍带 requireConfirm、守卫待确认、条再出、播放中页脚不进写入」;
  b2 与 d 末尾钉「点过开始 ⇒ 关再开直接写、不出条」;g / g 尾钉「守卫没挂 ⇒ 条不上屏 / 跟着收起」与「横幅⑦也算确认过」;
  h 钉「确认调用失败不算确认过」。`smoke-tab1-interactions` 源码格钉「确认过」恰好两个置位点、都只认成功回执。
- 反向注入与读数见 PR 描述。

## 兼容性影响

- 既有工程:零影响(state、参数、IPC 都没动)。
- 既有 DAW 自动化:首次开输出后、点「知道了,开始」之前不再写(这正是本变更要的);点了之后与改前相同。
- 新旧互通:桥面是进程内接口,web 资源随插件二进制一起编译,不存在新 web 配旧 native 的组合。

## 用户文档

`docs/USER_GUIDE.md` / `docs/USER_GUIDE.zh-CN.md`「输出」一节补上「点『知道了,开始』之前只试听、不写」,以及
「没点就关再开仍要确认、确认过一次之后本窗口关再开直接写」;「把结果写进 DAW」一节第 1 步补「先点『知道了,开始』
(重开工程时点横幅『继续写入自动化』),否则只试听不写」;加载守卫那一条里「关掉守卫后再打开」的说法随之改准。
i18n 三语的确认条正文改为先说「点开始后才写」。

## 没改、说法相同的地方(不在本仓)

计划侧 05-ui-spec §2.1 输出开关行(「write 确认……『知道了,开始』/『撤销』两钮」)、§1.4 函数表 `setOutputEnabled` /
`confirmPrintGuard()` 行、§2.0 横幅⑦,由统筹决定是否跟改。

## 审批

- 用户裁定 J166(masterPlan `plan/adjudications.md`)。
- 统筹裁定 **J166a**(同上,J166 实施细则):给既有桥函数加尾部可选参数(`setOutputEnabled(on, opts?)`)**算「只增」**,
  `contractVersion` 不变、§0.1 不改 —— 旧调用方不传 `opts` 行为不变、传错回 `badArg`,与「只增」的兼容性要求一致。
  若以后要在 §0.1 补一句「尾部可选参数属于只增」,另走变更文档。
- 本 PR 挂 `status/frozen-contract`。
