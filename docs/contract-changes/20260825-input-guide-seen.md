# 契约变更说明 —— 20260825-input-guide-seen

> 提出方 = T48(**[J80]** Input 首启轻量引导);裁决真源 = `masterPlan/plan/adjudications.md` 的 **J80** 行
> 与 `05-ui-spec.md` §3 文末 J80 节。
> 本 PR **只提出**变更、不改任何冻结文档本体;落地(state codec + 桥 setter + C++ 常量表 +
> 契约 §3/§4/§7)转 **DS 侧(native)**。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。本字段是 state,非自动化;123 个参数的 ParamID/index/顺序一字未改。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。不跨进程,不进任何共享段(Input 的 registry/ctrl 段布局零改动)。
- [x] docs/STATE_SCHEMA.md(state schema)—— **Input** state 的 `ui` 子树新增一个字段。
- [x] docs/SCVB_CONTRACT.md —— Input 侧新增 **1 个 native function**(§3 的 7 → 8)、
      §3.1 快照与 §4.1 `scvb.state` 的 `ui` 子树各多一个字段、§7 manifest 的 `input.functions` 加名。
- [ ] tests/golden/(golden 快照)—— **不动**。

> 说明:本 PR 的 diff **不含** `docs/STATE_SCHEMA.md` 与 `docs/SCVB_CONTRACT.md`。
> 与 `20260825-master-chart-mode.md` 同样的处置:先立字据,不让一个新 state 字段与一个新桥函数
> 无声地长在 web 里;等 native 落地时那个 PR 直接引用本文件即可。

## 变更内容

### 一、Input state `ui` 子树新增 `guide_seen`

| 项 | 定义 |
|---|---|
| 路径 | `ui.guide_seen`(**Input** state) |
| 类型 | bool |
| 默认值 | **`false`**(首装 = 没看过) |
| 语义 | Input 首启轻量引导([J80]:独立语言卡 + 5 步 mini tour)的**已读位**。置位后首启链不再自动弹;header「?」重看入口**不看本位**,已置位也能再开(与 Output 侧 `tour_seen` 的「重看引导」同款,契约 §1.33 语义行) |
| 自动化 | **否**。不占参数面,不可被 DAW 录制 |
| 持久化 | **是**,随工程走(与 Output 侧 `ui.guide_seen` / `ui.tour_seen` 同族) |
| 运行时态 | 否 |

**拼写为什么是 `guide_seen` 而不是新造一个名**:Output 侧 §1.32 的同名字段就是「首启引导已读位」,
Input 侧要表达的是同一件事(只是那一侧的引导内容不同)。§0.2 命名纪律要求直接镜像宪法字段的键
逐字沿用宪法拼写;`guide_seen` 已在 `params-v0` 的 Output `ui` 组里登记,Input 组照抄同一拼写,
两侧语义一致、判据代码可共用(`web/shared/lang-start.js` 的 `shouldShowLangStart` 两侧共用一件,
正是靠这个拼写一致)。**不新造 `input_guide_seen` 之类的名字** —— 那会让同一语义有两个落点(§0.1 第 4 条)。

### 二、`requestInitialState()`(§3.1)快照新增 `guide_seen_global`

| 项 | 定义 |
|---|---|
| 路径 | 快照顶层 `guide_seen_global` |
| 类型 | bool,**只读**,不属工程 state |
| 语义 | J50a 系统级全局默认判定位,**Input 侧自己的一份**(与 Output 侧 §1.1 的同名键同族、同机制:复用 `commitUiScale` 已有的全局配置通道) |

首启判据(两侧同构,J50a):**工程 `ui.guide_seen === false` 且 全局默认 `guide_seen_global === false`** 才弹。
「已看过」承诺跨工程成立:置位时连全局位一起写,新工程 `guide_seen=false` 但全局位已置 → 不再自动弹。

> **Input 与 Output 的全局位是同一个文件里的同一个键,还是各存一份?** 建议**各存一份**
> (例如全局配置里 `input.guide_seen_global` / `output.guide_seen_global` 两条)。两侧引导讲的是
> 两个界面、两套内容,看过 Output 的 44 步导览不等于看过 Input 的 5 步 mini tour;共用一个位会让
> 先装 Output 的用户永远看不到 Input 的引导 —— 而 J80 立这张卡的**全部理由**就是「Input 是用户见到的
> 第一个界面却零引导」。此点请 DS 侧在落地时确认并回写契约 §3.1 语义行。

### 三、`scvb.state`(§4.1)载荷的 `ui` 子树同步

`ui:{scale, language}` → `ui:{scale, language, guide_seen}`。
理由同 Output 侧 §1.1 语义行:**§3.1 快照的 state 子树字段集与 §4.1 事件的字段集不得各自漂移**。

### 四、新增 Input native function `setGuideSeen(seen, alsoGlobal = true)`

签名与语义**逐字照 Output 侧 §1.32**(契约 §7 的同名同签名项按侧各自登记,`requestInitialState` /
`setGroupId` / `setUiScale` / `commitUiScale` / `setLang` 已有先例):

| 项 | 定义 |
|---|---|
| 参数 | `seen: bool`,`alsoGlobal: bool = true` |
| 返回 | `{ok:true}` |
| 语义 | Input 首启引导已读的**唯一写入口**:写 state `ui.guide_seen`;`alsoGlobal=true` 时同步写系统级全局默认位(**不回写 state**)。**mini tour 完成与 Skip 都置位**(J50a 镜像,与 Output 侧 `setTourSeen` 同款);header「?」重看引导**不调用本函数**(`guide_seen` 已置位也可重看) |
| 拒绝态 | 无 |
| 撤销 | 否(UI 偏好不进 undo 栈) |
| 线程/频率 | `[M]`;用户走完/跳过引导时各一次 |

转正时要同批改的**五处**(缺一处 `scripts/check-bridge-parity.mjs` 就会红,这是好事):

1. `docs/SCVB_CONTRACT.md` —— §3 正文新增 §3.8 条目、§3.1 与 §4.1 的 `ui` 子树加字段、
   §7 manifest 的 `input.functions` 加名(**计数断言 7 → 8**,`scripts/check-bridge-parity.mjs`
   的 `EXPECTED` 表同改);§3 节首「共 **7** 个」一行同改;
2. `src/input/InputBridgeApi.h` —— 加 `kFnSetGuideSeen` 常量(名字与 Output 侧同名常量并存,各在各的头文件里);
3. `web/shared/bridge.js` —— 把名字从 `PENDING_FUNCS.input` **挪进** `BRIDGE_FUNCTIONS.input`
   (留在 `PENDING_FUNCS` 里等于让它绕过 parity 门禁,那才是真正的洞);
4. `docs/STATE_SCHEMA.md` —— 并入上面的「字段定义」(Input state 一节);
5. **宪法 `params-v0.md` 的 Input state `ui` 组** —— 即 `§三 Input state` 的
   `ui: {scale, language}` 一行加上 `guide_seen`。**先例就在 Output 组那一行**:
   `guide_seen` / `tour_seen` 当初随 J50 / J62 进来时都是在宪法里登记的;宪法侧的 state 形状
   不跟着改,这一行就会与 `STATE_SCHEMA.md` 讲两个故事。

   ⚠️ **改的是原件,不是 repo 里这份**:`docs/constitution/*` 是 `masterPlan/constitution/*` 的
   **只读副本**(`scripts/check-constitution-sync.ps1` 按 sha256 盯着,是 gates 第 1 关的一部分)。
   只改 repo 副本 = 门禁当场红。正确做法:走**宪法升版**流程改
   `masterPlan/constitution/params-v0.md` 原件,再同步副本 —— 这比契约变更更重,
   须在转正 PR 里单独说明并取得批准。

### 落地参照:T37 真机 round-2(`feat/fix-t37-field-round2`)已在 Output 侧做过同一件事

那条分支修的是 Output 侧「引导每次重开都重弹」的真机 bug,拆出来正好是本变更 native 半边的**两块**,
Input 侧照抄结构即可,不必重新设计:

1. **工程位要真的落盘** —— `src/core/state/OutputStateCodec.h` 在 CFGS 布局尾部追加了
   `u32 uiGuideSeen` / `u32 uiTourSeen`(**向后兼容的追加段**:老工程按旧长度解码、两位取默认 0)。
   此前这两位只活在运行时结构里,重开工程即回到「首启」。
   **Input 侧对应改**:`src/core/state/InputStateCodec.h` 的 `struct InputState`
   (当前 = `channelId` / `groupId` / `uiScale` / `uiLanguage`)尾部追加 `u32 uiGuideSeen`,同款追加段语义。
2. **全局默认位要真的有个地方存** —— 新增 `src/output/UiDefaultsStore.{h,cpp}`
   (`scvb::output::uidefaults`,每次读写现开一份 `juce::PropertiesFile`、不驻留进程内状态),
   因为此前 §1.1 快照里的 `guide_seen_global` / `tour_seen_global` 是**硬编码 false**。
   **Input 侧对应改**:同族的 `scvb::input::uidefaults`(或同一实现加 input 前缀的键)。
   注意它的命名空间本来就是**按侧分**的 —— 这与上文「两侧全局位各存一份」的建议是同一个结论,
   落地时保持一致即可。

> **合并顺序**:本 PR 与 `feat/fix-t37-field-round2` 都动 `web/output/tour.js` 与
> `web/shared/`(前者提取共享画法,后者加 `onEnd` 一次性位),后合者按 J76 rebase。
> 两边的 web 侧修法是**同一套**:一次性门控不依赖异步回执 —— 用户一答完就在本地会话位上置位,
> `scvb.state` 回推只是最终一致的确认。本 PR 的 Input 侧 `store.session.guideClosed`
> 由 `tour-in.js` 的 `endTour` **同步**调 `onEnd` 落位,与那条分支的 `tourAnswered` 逐字同构。

### 迁移语义

- **读到没有该键的旧工程**:按默认 `false` 处理 —— 即「没看过」,首启链会弹一次。
  这与「用户从没打开过这个工程」是同一件事,不存在丢数据;若用户此前已在别的工程看过引导,
  全局默认位会替他挡住(这正是 J50a 全局位存在的理由)。
- **不需要写迁移函数**:字段是纯增量、有确定性默认值,不改变任何既有字段的编码或含义,故 `abi` **不必递增**。
- **反向兼容**(新工程被旧版本读到):旧版本 `setStateInformation` 不认识这个键,按其既有的
  「忽略未知键」路径丢弃 —— 用户看到的是引导又弹一次,无其它影响。

## 本 PR 的 web 侧落地(不碰冻结面)

- **读**:`shouldShowInputGuide(state, snapshot, closedThisSession)`(`web/input/tour-in.js`)与
  `shouldShowLangStart(...)`(`web/shared/lang-start.js`,与 Output 共用一件)直接读
  `ui.guide_seen` + `guide_seen_global`,缺失即 `undefined !== false` → **不弹**。
  下行方向不需要任何契约变更。
- **写**:走 `web/shared/bridge.js` 的 `PENDING_FUNCS.input`(一张**不在** parity 比对面里的
  「待转正」名表,能力探测后才挂到桥上)。于是当下两种运行形态都成立:
  - **预览 / mock**:mock 后端(`web-preview/mock/juce-bridge-mock.js` 的 Input 后端)已按上表形制
    实现 `setGuideSeen`,调用落地 → `scvb.state` 回推 → 再取快照往返不丢。
    `web-preview/tests/smoke-input-tour.mjs` ③ 断言了这一往返与「seen 后不再弹」。
  - **真 JUCE 宿主(native 未落地)**:上下行**都**还没有这件东西。下行 ——
    `src/input/InputBridgeLogic.cpp` 的 `buildInputSnapshot` / `buildStatePayload` 当前只发
    `ui:{scale, language}`,顶层也没有 `guide_seen_global`;按上一条的判据
    (`undefined !== false`),首启链在真宿主里**一次都不会自动弹**,自然也谈不上「跨会话重弹」,
    此刻唯一能看到 mini tour 的入口是 header 的「?」重看。上行 —— 桥上根本不挂
    `setGuideSeen`,调用直接返回 `null`。页内会话标记(`store.session.guideClosed` /
    `langChosen`,不入 state chunk)拦的是**预览 / mock 形态**下的重弹,真宿主里只是空转。
    这是「没写成」的如实表现,**不是**静默假装写成了 —— 首启链要真跑起来,须等本变更的
    native 落地(两个键下行 + `setGuideSeen` 上行),届时本条描述一并作废。
- 代码里的 TODO 锚点:`web/input/tour-in.js` 头注、`web/shared/bridge.js` 的 `PENDING_FUNCS`
  头注,两处都指回本文件。

## 兼容性影响

- **既有工程**:引导会弹一次(默认 `false`),弹完置位。参数、曲线、段表、采集数据零影响。
- **既有 DAW 自动化**:无影响。参数面 123 个一字未动,本字段不可自动化、不进参数通道。
- **新旧版本互通**:无破坏(见「迁移语义」两条)。
- **abi**:按上文,本字段自身**不要求** `abi+1`。
- **实时线程**:无影响。`[M]` 消息线程写入,`processBlock` 不读不写;引导本体是纯 UI overlay,
  除本函数外不发任何桥调用(`smoke-input-tour.mjs` ⑤ 有「唯一桥调用 = setGuideSeen」的源码级断言)。

## 审批

- PR 挂 **`status/frozen-contract`** 标签(仓库若尚无该 label,则在 PR 描述首行以文字标注),
  由用户明确批准后合入(仓 `CLAUDE.md` §5 / 06 §3.7)。
- 本 PR 的 diff **不触碰**四份冻结文档本体,故 branch-gate 的「冻结契约 path guard」不会被触发;
  本文件是**提前**立的变更说明。
