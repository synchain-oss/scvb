# 契约变更说明 —— 20260914-sl413-seg-mode-reserved

> **状态:待批(随实现 PR 挂 `status/frozen-contract`)。** 定谳:统筹 2026-09-14
> (**SL-413**,用户裁 ②「把『分段方式』从 UI/规格摘掉」)。本文档是该变更的契约面记录,
> 与实现放在**同一个 PR** 里。

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md(自动化参数)—— **只动文本,不动参数面**:`analysis.segmentation.mode`
  那一行的注释标注为 v1 保留位;§四「命名与兼容规则」里「读到高版本 → 拒载并提示升级」那一条
  补上 [SL-412] 的接线实况。**123 参数表逐字未改**,ParamID / index / 顺序 / versionHint /
  skew 一个字节未动(三项本来就不是自动化参数,是 state-only)。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**:与跨进程共享内存无关。
- [x] docs/STATE_SCHEMA.md(state schema)—— **只动文本,零布局 / 零 abi / 零迁移**:
  §一 `analysis.segmentation.mode` 标 v1 保留位、§三 `CFGS` 那一档与 §三 新增一条说明、
  §三「迁移函数框架」与 CRVS minor 两条的「UI 通路待接线」口径改实(`CFGS` 布局、abi=4、
  `kMigrators` 三条、golden,**一个字节未动**)。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**:`newerState` 的 code / 载荷形状
  (`{localAbi:u32, projectAbi:u32}`)/ UI 落点(红横幅④)**早就在 §5.1 九码表里定义好了**
  (真源 03 §6.2 / J40),§2.9 的 envelope(`code` / `ch?` / `detail` / `active`)也一字不改。
  [SL-412] 补的是**生产者**(native 侧此前零调用方),不是契约面 —— 载荷字段**只增不改**
  这条铁律在本 PR 上一次都没有被用到。
- [ ] tests/golden/(golden 快照)—— **不动**:`abi1..abi4.bin` 四份逐字节未改。
  ⚠ 金样锁的是**容器头**(magic / abi / flags / chunkCount / TLV 框),它看不见 `CFGS` 载荷,
  所以「`mode` 现在是保留位」这件事**不会**、也**不该**让任何一份金样红。

## 变更内容

**一句话**:`analysis.segmentation.mode`(`valley` / `vad_only`)**从规格与 UI 里摘掉,追认为
v1 保留位** —— 落盘字段保留、恒写 `0`=valley、`vad_only` 留待接线那张卡启用;
同时把 [SL-412] 的 Output 侧升级提示通路接线,并把契约文本里两处「待接线」的口径改实。

### A. `segmentation.mode` → v1 保留位(本卡正题)

| 面 | 改前 | 改后 |
| --- | --- | --- |
| 规格(02 §0.3,真源在 masterPlan,由统筹改) | `vad_only` / `valley` 两档并列 | **v1 保留位**:UI 不露出、引擎不消费(恒按 `valley` 走 S1),`vad_only` 留待接线时启用 |
| `docs/STATE_SCHEMA.md` §一 | 只写 `[SL-411] 三项随工程落盘` | 追加「`mode` 为 **v1 保留位**:UI 不露出、引擎不消费、恒写 0=valley」 |
| `docs/STATE_SCHEMA.md` §三 | `CFGS` 行只列 segmentation 三项 | 那一档标注保留位;并新增一条说明「这句『恒写 0』是生产路径的实况,不是编解码的不变量」 |
| `docs/PARAMETERS.md` | `segmentation: {mode, sensitivity, min_segment_ms}` 无定义句 | `mode` 加三行注释(保留位 / 恒写 0 / 布局与 abi 不动 / 读侧原样往返) |
| `docs/USER_GUIDE.zh-CN.md` + `docs/USER_GUIDE.md` | 「分段方式 `mode` 今天只回到原位 —— 引擎侧尚无消费方」 | 改成事实:「界面上**从来没有**这个下拉(不是这一版藏起来的),引擎也不消费它,落盘只保证它不丢」 |
| `web/output/index.html` / `web/output/tab-wave.js` / `web/shared/i18n.js` | — | **零改动**(见下面「用户界面这一半为什么没有改动」) |
| `CFGS` 布局 / 容器 abi / golden / 迁移链 | — | **零改动**(abi 仍为 4) |

**为什么是「摘掉」而不是「接线」**:bot 建议(deepseek 第 1 轮建议③)给的两条出路是「接线
(`vad_only` ⇒ 跳过谷切分)+ core 格 + 删除式」或「从规格/UI 摘掉」。用户裁 ② 选了后者。
接线要动 `SegmentationParams` 与 S1 的调用面、会让所有既有工程重分析后段数变化,而 v1 已进入
收官阶段;而这一档**今天本来就到不了任何一条生产路径上**(UI 没有控件、引擎不读),
所以「追认成保留位」是零行为变更地把规格改说实话,不是把功能删掉。

### B. [SL-412] Output 侧升级提示接线(同批,不属契约面)

`CLAUDE.md` §7.3 / STATE_SCHEMA 的「读到高版本 → **拒载并提示升级**」里,「拒载」那一半一直是
好的(`OutputProcessor::setStateInformation` 的 `RejectedNewer` 分支置 `stateAbiMismatch_` +
`preservedStateBlob_` 原样回写),**「提示」那一半没接线**:`hasStateAbiMismatch()` /
`stateAbiSeen()` 零调用方、`scvb.error` 的 `newerState` 码**没有生产者**,而消费端
(`web/output/app.js` 的红横幅④)与 mock 夹具早就就绪。旧构建打开一份更高 abi 的工程只落一行
`DBG` —— 而 `DBG` 在 Release 里是空语句,**用户看到的是一份没有任何解释的空工程**。

本 PR 补上生产者:`OutputEditor::emitNewerStateError()`,在 `emitTick` 里按 §2.9 发
`scvb.error{code:"newerState", detail:{localAbi, projectAbi}, active}`。判定与记账抽成纯函数
`scvb::output::planNewerStateEmit`(`BridgeArgs.h`):条件成立发 `active:true`、条件解除发
`active:false` 撤横幅、**同一份拒载态不重复发**、**webview 不可见时不发也不记账**。

**Input 侧同一通路仍未接线**,不在本次改动面内(尺寸与理由见 PR 描述,等统筹裁定)。

## 兼容性影响

- **零布局、零 abi、零迁移**:`CFGS` 尾部 28 字节里 `segMode` 那一个 u32 照旧在
  (`OutputStateCodec` 一个字节未改),容器 abi 仍为 4,`kMigrators` 仍是三条,`tests/golden/state/`
  四份金样逐字节未改。**新版本读旧工程 / 旧版本读新工程的行为与本 PR 之前逐字相同。**
- **用户可见面:什么都不会变。** 这一条是**把规格追认成事实**,不是新增或移除一项行为 ——
  界面上没有那个下拉已经很久了(实际上从来没有)。此前 CHANGELOG 与用户指南里那句
  「分段方式今天只回到原位」读起来像一个「你找得到、按了却没用」的控件,现在改成实话。
- ⚠ **「恒写 0」是生产路径的实况,不是编解码的不变量**:`segModeOrdinal` / `segModeString`
  仍按**原样往返** —— 一份手写成 `segMode = 1` 的 abi=4 blob 读进来仍是 `vad_only`、
  再存回去仍是 `1`(`tests/core/test_output_session.cpp` 的 `[SL-411]` 四格逐字断言着这条往返,
  本 PR 一条都没有改)。契约 §7.3「不得静默丢数据」压过「统一成 0」:把读侧改成强写 0
  等于**替用户改数据**,而那正是这一节存在的理由。这一段是**有意**写进两份冻结文档的,
  别把它当成一句可以「顺手收干净」的残留。
- **参数面:零影响** —— 三项都不是自动化参数;123/124 口径、ParamID / index / 顺序 /
  versionHint 全部不动。
- **桥面:零影响** —— `setSegmentation` 的 mode 白名单(`isSegmentationMode`)与
  `scvb.state` 的 `analysis.segmentation` 载荷行**一个字都没改**。UI 侧 `DEFAULT_SEGMENTATION`
  里那句 `mode: "valley"` **必须留着**:契约 §1.19 要求三个字段**整包**下发,少一个整个
  `setSegmentation` 直接 badArg、三个字段一个都进不去(与 [SL-382] 保留灵敏度滑杆那条同一笔账)。
- **[SL-412] 那一半的行为面**:唯一的用户可见变化是「旧构建打开高 abi 工程 ⇒ 红横幅④ 上说清
  楚是版本问题」。拒载 / 原样回写 / 段表与曲线的处置**一个字都没改**。CRVS minor 那一支
  (同 abi、CRVS minor 更高)走的是另一个位 `crvsNotRestored_`,**本 PR 没有接线**,
  已在 `docs/STATE_SCHEMA.md` §三 单列一句,免得被读成「升级提示都接了」。

### 用户界面这一半为什么没有改动(照实登记)

裁定原话是「把『分段方式』从 UI/规格摘掉」,派工也按同族判例 #251 的形态给了做法
(「DOM 留、加 `hidden`、注释写清裁定与日期」)。**实现侧没有对象**:`web/output/index.html` 里
`valley` / `vad_only` / `分段方式` 三个字面**全 0 命中**,`git log -S "wave-seg-mode" -- web/`
追不到任何一版;**全仓唯一的 `<select>` 是设置页的缩放档位**(`settings-scale-select`)。
也就是说 `mode` 只存在于「§1.19 整包载荷的默认值」里(`tab-wave.js` 的
`DEFAULT_SEGMENTATION.mode = "valley"`)与 mock 的枚举表里,**没有任何 DOM 消费它**。
同族判例 #251 对「若界面有分段模式选择器一并隐藏」那句报过**同一句**结论(PR 描述里写着
「无对象」)。所以本 PR **不加控件、也不藏控件**;改为在页面级冒烟里落一条**负空间判据**
(`smoke-seg-restore-page.mjs` 的 `[SL-413]` 一节):分段工具条组里「真在渲染的滑杆恰 1 根 /
`<select>` 0 个 / `data-gb` 含 `mode` 的节点 0 个」—— 把裁定的承诺钉住,而不是写一条**恒绿**的
「那个下拉不产生布局盒」(本仓判例:恒真的格比没有格更坏,它还在报绿)。

## 判据与机检(每条都实跑,删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| 纯函数 | `tests/core/test_bridge_args.cpp` `BRIDGEARGS-SL412`(7 格) | `planNewerStateEmit` 的四态:首次发 `active:true` / 同一份不重复发 / 换工程 abi 要重发 / 条件解除发 `active:false`;外加「屏上本来没有就不发空撤销帧」与**不可见时不发也不记账**(三支一起断) |
| host 真路径 | `tests/host/test_host_harness.cpp` `HOST SL412` | 真 `setStateInformation` 喂 abi=5 的合成 blob ⇒ `hasStateAbiMismatch()==true` 且 `stateAbiSeen()==5`;`getStateInformation` **原样回写**宿主那串字节;再喂 abi=4 ⇒ 拒载态复位 |
| 调用点(离线不可达的那一跳) | `web-preview/tests/smoke-tab2-interactions.mjs` `[SL-412]` 六条源码钉子 | `emitTick` 真的调了 `emitNewerStateError()` / 判定走纯函数 / 载荷按 §2.9 信封发(**不带 `ch`**)/ `detail` 两个字段名逐字对 §5.1 / 闩锁按 plan 回填 / 闩锁是「两位」不是单 bool |
| 页面级(消费端) | `web-preview/tests/smoke-group-lock-page.mjs` `[SL-412]` 一节 | mock 推 `newerState{localAbi:4, projectAbi:5}` ⇒ 红横幅④ 可见、文案**逐字**等于词条 `banner.versionMismatch` 填上那两个数;再推 `active:false` ⇒ 横幅撤下 |
| 页面级(负空间) | `web-preview/tests/smoke-seg-restore-page.mjs` `[SL-413]` 一节 | 分段工具条组里可见滑杆恰 1 根 / `<select>` 0 个 / `data-gb` 含 `mode` 的节点 0 个 |
| 词条面 | `node scripts/check-i18n.mjs` | 569 key × 3 语(本 PR **零新增词条** —— 横幅④ 的文案 `banner.versionMismatch` 早就在三语里了,所以**不触发**字体覆盖检查) |
| 真源对拍 | `node scripts/gen-hard-rules.mjs --check` | 硬约束九条 6 个落地面逐字节一致(本 PR 只动「分段」那一条 bullet,与硬约束无关) |

⚠ **离线不可达、本 PR 没有改变的那一跳**:`OutputEditor` 要真 WebView2,编不进任何 C++ 测试
目标(`tests/CMakeLists.txt` 的 `scvb_monitor_tests` 头注写着这条边界),所以
「`emitTick` 真的调了发送面」在 C++ 侧没有可执行落点 —— 由上面那条**源码钉子** +
`HOST SL412` 的**输入侧** + 页面级的**消费侧**三面合起来兜,与 `HOST SL391` / `HOST SL411`
头注同一笔账。**这是缺口不是覆盖。**

## 变更文件

> 每条路径都经 `git ls-files --error-unmatch <path>` 核过(全部为已跟踪文件的改动,唯一新增
> 文件是本变更文档自己)。

- `docs/STATE_SCHEMA.md`(§一 `mode` 标保留位;§三 `CFGS` 行同步、新增一条说明保留位的
  两条边界「恒写 0 是生产实况 / 读侧原样往返」;§三「迁移函数框架」与 CRVS minor 两条的
  「UI 通路待接线」口径改实;文首「最后更新」链追加本条)
- `docs/PARAMETERS.md`(`mode` 三行注释;§四 命名与兼容规则那条补接线实况;文首「最后更新」)
- `docs/USER_GUIDE.zh-CN.md` + `docs/USER_GUIDE.md`(「分段方式今天只回到原位」改成事实)
- `CHANGELOG.md`(⚠️ 契约变更 与 修复 两个小节的**预写条目**,写在待合并注释块里 ——
  按本文件规矩①,合并前最后一次推送才搬进正文;另把 [SL-411] 那条里的「只回到原位」
  改成实话)
- `docs/contract-changes/20260914-sl413-seg-mode-reserved.md`(**本文件,新增**)
- `src/output/BridgeArgs.h`(`NewerStateEmitPlan` + `planNewerStateEmit`,纯函数)
- `src/output/OutputEditor.h`(`emitNewerStateError()` 声明 + 两位闩锁)
- `src/output/OutputEditor.cpp`(实现 + `emitTick` 里的调用点,替掉那句「T29 无触发面」)
- `tests/core/test_bridge_args.cpp`(`BRIDGEARGS-SL412` 七格)
- `tests/host/test_host_harness.cpp`(`HOST SL412`)
- `web-preview/tests/smoke-tab2-interactions.mjs`(`[SL-412]` 六条源码钉子)
- `web-preview/tests/smoke-group-lock-page.mjs`(页级 `[SL-412]` 一节 + 文件头「跑什么」同步)
- `web-preview/tests/smoke-seg-restore-page.mjs`(页级 `[SL-413]` 负空间一节 + 文件头同步)

**没有动的**(逐一核过,免得读的人以为漏了):`src/core/state/OutputStateCodec.{h,cpp}`、
`src/core/state/StateMigration.{h,cpp}`、`src/core/state/StateCodec.h`(`kCurrentAbi` 仍为 4)、
`src/output/OutputProcessor.{h,cpp}`、`tests/golden/state/*.bin`、
`tests/core/test_output_session.cpp`、`web/output/index.html`、`web/output/tab-wave.js`、
`web/shared/i18n.js`、`web/shared/mock-data.js`。

## 审批

待批:随实现 PR 挂 `status/frozen-contract`,由用户明确批准后合入(06 §3.7)。
定谳来源:统筹 2026-09-14 **SL-413**(用户裁 ②「把『分段方式』从 UI/规格摘掉」;
bot 建议来源 = #259 第 1 轮 deepseek 建议③)。[SL-412] 那一半的定谳来源 = #259 第 1 轮
claude 重要①(用户 2026-09-14 裁 ①,排产 v5.6.15)。
