# 契约变更说明 —— 20260921-sl464-channel-id-zero-semantics

> **状态:待批(随本 PR 挂 `status/frozen-contract`)。** 用户 2026-09-21 已批准本次契约文字变更。
> 本文档记的是 **[SL-464]**:把 `docs/SCVB_CONTRACT.md` 里 `channel_id` 的取值语义**追上已经合并的
> 实现**([SL-446] / #273)。**实现侧不动一个字节** —— 本 PR 零 C++ / 零 JS。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。`channel_id: 0..15 # 0=未分配(J01)`
      (`:95`)说的是**存档 / 配置值**:`getStateInformation()` 取的是 `savedChannelId_`
      (`src/input/InputProcessor.cpp:441`,带 [SL-446 第 5 轮] 头注),[SL-446] 起它与
      「实际持有」分开记,语义一字未变。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。不碰段名、不碰布局、不碰 abi。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。§二 Input state 的
      `channel_id: 0..15 # 0=未分配(J01)`(`:88`)同上,是存档值,不是桥面那个字段。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— **三处语义文字改实**;函数名 / 签名形态 / 事件名 /
      **载荷字段与其取值域零变化**:§3.1 `requestInitialState` 的「语义」格(`:598`)、
      §4.1 `scvb.state` 的「字段纪律」格(`:677`)、§5.2 六值表的 `unassigned` 行(`:738`)。
- [ ] tests/golden/(golden 快照)—— **不动**。无布局、无 abi、无编码变化。

> **guard 的覆盖面与「本 PR 会不会触发它」是两件事。**
> 覆盖面:`.github/workflows/branch-gate.yml:169` 的冻结契约 path guard 正则
> `^(docs/PARAMETERS\.md|docs/IPC_CONTRACT\.md|docs/STATE_SCHEMA\.md|docs/SCVB_CONTRACT\.md|tests/golden/)`
> **确实**含 `docs/SCVB_CONTRACT.md`。
> **但本 PR 不触发它**:该 workflow 只挂 `on: pull_request: branches: [dev]`
> (`branch-gate.yml:11-14`),而本 PR 的 base 是 `feature/v1` ⇒ **整个 workflow 不启动**,
> `:169` 一次都不执行。`CLAUDE.md:79` 写的是同一件事:子 PR
> 「**没有** `branch-gate` —— 后者仅挂 pull_request→`dev`,所以 **DCO 与冻结契约
> path guard 在子 PR 上一次都不跑**,要到 feature→dev 收口 PR 才第一次生效。
> 改冻结契约的子 PR 别指望机器拦你。」
> guard 将在 `feature/v1 → dev` 收口 PR 上第一次生效 —— 届时本文档相对 `dev` 是
> `added`,能满足它的 `^docs/contract-changes/[0-9]{8}-.+\.md$` 检查。
>
> ⇒ **义务不变**(CLAUDE.md §5 是无条件的:四份冻结文档 + `tests/golden/` 的任何改动
> 都要变更文档 + `status/frozen-contract` 标签,本 PR 两样都做了),
> **但这一版没有任何机检兜底**,全靠人工裁定 —— 这与下文
> `contractVersion` 那一节的处境直接相关。

## 变更内容

**一句话**:`channel_id=0` 此前被契约定义成**等价于「未分配」**;#273 合并后它有了第二个成因
(claim 请求被拒 / 段不可用),契约据此改成「**`channel_id` 报的是配置号,只在 claim 请求被拒或
段不可用时为 `0`;是不是『未分配』一律看 `claim`,不得由 `channel_id=0` 反推**」。

### 这个组合在 base 上为什么不可能出现

顶层 `channel_id`(§3.1 首帧快照与 §4.1 `scvb.state` 两个出口)的取值来源在 #273 里换过一次:

| | base(`44099f3^`) | 现在(`44099f3`) |
| --- | --- | --- |
| 快照来源 | `s.channelId = channelId_`(**配置 / 请求号**,`InputProcessor.cpp:614`) | `s.channelId = session_.boundChannel()`(**实际持有**,`InputProcessor.cpp:695`)+ 另开一路 `s.configuredChannelId = channelId_`(`:700`) |
| 两个出口 | `buildInputSnapshot(snap.channelId, …)` / `buildStatePayload(snap.channelId, …)` | 两处都经 `bridge::displayChannelId(snap.claimState, snap.channelId, snap.configuredChannelId)`(`InputEditor.cpp:121` / `:156`) |

`bridge::displayChannelId()`(`src/input/InputBridgeLogic.cpp:53`)是:

```cpp
return (claimState == kActive || claimState == kUnassigned) ? configuredChannelId : channelId;
```

base 上配置镜像在 CAS 之前就被写成了请求号,所以**无论 claim 成不成功,顶层 `channel_id` 都是那个
非零的请求号** ⇒ 「`channel_id=0` 且 `claim` 不是 `unassigned`」在 base 上不可达。#273 之后,
`kConflict` / `kAbiMismatch` / `kUnavailable` 三态走 `channelId`(= `boundChannel()`)⇒ 新组合出现。
**这是 #273 引入的,必须补进契约。**

### 「只有两个出口」这个全称句的依据

本条款写了「**唯一的例外**」,而那句的成立依赖「顶层 `channel_id` 只有两个出口、且两个都经
`displayChannelId()`」。据以确认的两条命令与命中数:

```
$ grep -rn "buildInputSnapshot(\|buildStatePayload(" --include=*.cpp --include=*.h src/      # 9 命中
$ grep -rn "displayChannelId" --include=*.cpp --include=*.h --include=*.js --include=*.mjs \
        src/ web/ web-preview/                                                               # 7 命中
```

- 第一条 9 命中 = **2 个调用点**(`InputEditor.cpp:120` `buildInputSnapshot(` / `:155`
  `buildStatePayload(`)+ 4 处定义与声明(`InputBridgeLogic.cpp:150`/`:281`、`.h:104`/`:144`)
  + 3 处 **Monitor 侧同名成员**(`MonitorEditor.cpp:140`/`:315`、`.h:42`)—— 那是
  `MonitorEditor::buildStatePayload()`,另一张桥面、不承载 Input 的 `channel_id`,不在本条款范围内。
- 第二条 7 命中 = **2 个调用点**(`InputEditor.cpp:121` / `:156`)+ 定义 `InputBridgeLogic.cpp:34`
  + 声明 `.h:49` + 3 处注释(`InputEditor.cpp:118`、`:144`、`web/input/app.js:864`)。

⇒ 两个出口一一对应、无第三个出口。

### `channel_id=0` 且 `claim ≠ "unassigned"` 的**完整**集合(比立卡时点名的多一个)

`claim` 六值由 5 个 C++ 枚举 + `maskBit` + `srMismatch` 合成(`InputBridgeLogic.cpp:11-27`):

| `InputClaimState` | `claim` 字符串 | 该态下顶层 `channel_id` |
| --- | --- | --- |
| `kUnassigned` | `unassigned` | 配置号(**可以非 0** —— `releaseResources()` 之后会话已释放、配置原样留着) |
| `kActive` | `srMismatch` / `active` / `idle`(按 `srMismatch`、`maskBit` 分流) | 配置号(该态下与实际持有**本就相等**),**必非 0** |
| `kConflict` | `conflict` | **`0`** ← 新 |
| `kAbiMismatch` | `abiMismatch` | **`0`** ← 新 |
| `kUnavailable`(I0 段未打开 / 映射失败) | **`idle`** | **`0`** ← 新,**立卡时未点名的那一个** |

所以新组合是 **`conflict` / `abiMismatch` / `idle`(段不可用那一支)** 三个,不是两个。
`srMismatch` 与 `active` 只在 `kActive` 上成立 ⇒ 不在集合里。

### 「失败态恒为 0」是结构性保证 —— 写进契约后由谁撑着

这三态下 `claimedChannel_` 恒为 `0`,是 **`InputSession::openAndClaim()`**
(`src/core/input/InputSession.cpp`)的结构性保证:每条失败分支要么从未 `store` 过
`claimedChannel_`、要么显式 `store(0)`;`displayChannelId()` 头注(`InputBridgeLogic.cpp:50-52`)
明写「**不是巧合**,别再按『只挑 kConflict』的思路加回去」。

⚠ **本 PR 把这条内部约定升格成了对外契约条款。** 将来若有人让某条失败路径留下非 `0` 的
bound channel,**破的是 §3.1 的条款,而不只是一个函数的内部约定** —— §3.1 语义行里已点名
`InputSession::openAndClaim()` 并指回本文档,改那里的人能反查到。

### 反向那一半:`unassigned` 并不蕴含 `channel_id=0`

`displayChannelId()` 让 `kUnassigned` 走 `configuredChannelId`,而 `kUnassigned` 恰恰是「一个 slot
也没持有」的态。现成用例:`tests/host/test_host_harness.cpp:1055-1075` —— `setChannelId(6)` 成功
之后 `releaseResources()`,`CHECK(snap.channelId == 0)`(实际持有清 0)、
`CHECK(snap.configuredChannelId == 6)`、`CHECK(displayChannelId(...) == 6)`。

**这一半是既存的**,不是 #273 引入:base 上顶层恒用配置号,同一场景下同样是
`claim="unassigned"` + `channel_id=6`。本 PR 顺手把它一起写对,因为它与新组合是同一句话的两个
方向 —— 只改一个方向会让 §5.2 那行继续骗人。

### 口径区分:三个量,以及它们**什么时候**分叉

契约面把 `channel_id` 这个名字用在了三个不同的量上,读者在三份文档之间来回读时会撞上歧义:

| 量 | 落点 | 上桥面吗 |
| --- | --- | --- |
| **实际持有** | `session_.boundChannel()`(= `claimedChannel_`) | 不直接上;只经「失败态为 `0`」间接体现 |
| **配置镜像** | `channelId_`(= `session_.channelId()`) | **是** —— 桥面 `channel_id` 在 `active` / `unassigned` 两态报的就是它 |
| **工程存档** | `savedChannelId_`(`InputProcessor.cpp:441` 存 / `:508` 载入 / `:607` 用户主动改配置时同步) | 否;落 `docs/STATE_SCHEMA.md` §二 |

**能分辨的具体情形**(核过 `InputSession::prepare()` 的补偿式回滚三条路):

| 路径 | `channelId_` 最终值 | 机制 | 桥面 `channel_id` / 工程存档 |
| --- | --- | --- | --- |
| **没有可回滚的旧通道**(`previousChannel == 0`,首次绑定就撞车) | 请求号 | **不进** `if (previousChannel != 0)` 块(`InputSession.cpp:94`)—— `channelId_` 从头到尾就是请求号,**没有任何「落回」动作** | `0` / **被拒的那个号** ⇒ **分叉** |
| **回滚失败**(`previousChannel != 0`,重抢旧槽也没抢到) | 请求号 | 进块;`channelId_` 先被临时借用成 `previousChannel` 以复用 `openAndClaim()`,再经 `InputSession.cpp:113` `channelId_ = requestedChannel` **复原** | `0` / **被拒的那个号** ⇒ **分叉** |
| **回滚成功**(`previousChannel != 0`,重抢到旧槽) | **旧号** | 进块;`InputSession.cpp:104-105` 置 `state_ = kActive` 并提前 `return failure`,**不执行** `:113` | 旧号 / 旧号 ⇒ **不分叉** |

前两路的**结果相同**(桥面 `0`、存档记被拒的号,`savedChannelId_` 由 `InputProcessor.cpp:607` 在 `prepare()` 之后同步),但 **机制不同** —— 只有「回滚失败」那一路经过 `:113`;`previousChannel == 0` 时那一整块根本不执行。**把两路合成一句「都落回 `:113`」会给其中一条路径安上一个它不会执行的机制**。

三条路不能合成一句话说 —— §3.1 那句口径区分只写**结果**(分不分叉取决于回滚成不成功)、**不写行号与机制**,就是为了不把一个只在其中一条路上执行的机制写成契约。

### 三处逐条改法

| # | 文件:行 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | `SCVB_CONTRACT.md:598`(§3.1 语义格首句) | 「`channel_id=0` = **未分配**(不 claim 任何 slot,J01);Output 侧完全不可见该实例,不计入 `N/15` 计数,**不是错误态**」 | 「`channel_id` 报的是**配置(选中)的通道号**,只在 claim 请求被拒或段不可用时为 `0`」+ `openAndClaim()` 结构性保证的指路 + 两个成因(① `unassigned` 引导态/主动释放,**不是错误态**;② `conflict`/`abiMismatch`/`idle` 段不可用支,**claim 失败态**)+ **「一律以 `claim` 为准,不得由 `channel_id=0` 反推」** + 反向不成立那句 + **三个量的口径区分与分叉情形** |
| ② | `SCVB_CONTRACT.md:677`(§4.1 字段纪律格) | 只写「`claim` 六态定义见 §5.2」,对 `channel_id` 一字未提 | 补一句显式取值语义并点明与 §3.1 是同一个字段(A-30 统一拼写)。**两处那句逐字相同**(`grep -cF` 命中数 = 2),订正一处时用它 grep 得到另一处 —— 写第二份而不是靠继承:A-30 的纪律就是两处逐字一致,只写一处将来必漂 |
| ③ | `SCVB_CONTRACT.md:738`(§5.2 `unassigned` 行) | 「`channel_id=0`,未 claim 任何 slot(**引导态,非错误**;…)」 | **只去掉假的那半**(`unassigned ⟹ channel_id=0`),换成「**本态下 `channel_id` 未必为 `0`**:它镜像的是配置号,`releaseResources()` 之后可以为非 `0`;反过来 `channel_id=0` 也不蕴含本态」+ 指回 §3.1 |

> ① 的两成因共有那一半(未占用任何 `InputSlot` ⇒ Output 侧完全不可见、不计入 `N/15`)**原样保留**。
>
> ③ 这一格是本次最容易改过头的:那句 `channel_id=0` 钉着两件事,**一件假一件仍真**。仍真的那半
> —— 未 claim 任何 slot / 引导态非错误 / 不计入 `N/15` / Output 侧完全不可见 —— **一个字没动**。
> 也**不能**改成「对 `channel_id` 完全不做断言」:§5.2 是一张能被单独读的表,只读它的人会拿不到
> 「未必为 `0`」这个分辨点,而那恰恰是本次要防的误读。

## 兼容性影响

- **载荷形状 / 枚举 / abi / golden:零变化**。§7 manifest 的
  `"claimState": ["unassigned","idle","active","conflict","abiMismatch","srMismatch"]`(`:908`)
  一字未动,`node scripts/check-bridge-parity.mjs` 照常绿(它只解析 §7 那个 json 围栏块)。
  ipc abi 不升、state 容器 abi 不升、段名不升 v2、`tests/golden/` 一份未动。
- **本 PR 的行为面:零变化**。不动 `src/` 与 `web/`。
- **#273 带来的一处用户可见行为改变,在此照实登记**(不是本 PR 引入,但契约读者会问):
  `conflict` 态下 Header pill 从「等待 Output / 已连接」那一档变成了灰色「未选择通道」——
  base 上该态 `channel_id` = 请求号,走不到 `web/input/app.js:646` 那道 `channelId === 0` 的闸;
  现在为 `0`,被它拦下。**判为更对**(那一刻确实一个通道也没握住),且 §5.2 `conflict` 行的
  「UI 落点」列本来只写了「卡片抖动 + `ch.occupied` toast」、没写 pill ⇒ 无文字矛盾,本 PR 不改它。
- **页面端两处 `channel_id` 消费面复核过,都不做反向推导**:
  - `web/input/app.js:440` `priorityBlockReason()`:`channel_id===0 ⇒ "unassigned"`。桥面
    `channel_id=0` 的态**全都非活跃** ⇒ C++ 侧 `bridge::priorityRejection()` 不是走
    `kUnassigned` 就是走 `kNotActive`,两条都回 `reason:"unassigned"`,两侧同结论。
  - `web/input/app.js:646` `pillState()`:`abiMismatch` / `srMismatch` 两条红 pill 排在
    `channelId === 0` 那道闸**之前**,所以 §5.2 `abiMismatch` 行的「红 pill『版本不匹配』」
    仍然成立。
- **Bridge 仓零影响**:`channel_id` / `channelId` / `scvb.state` 在那边零命中(统筹 2026-09-21 核实)。

### `contractVersion` 保持 `1.0` —— 用户已裁「豁免」(SL-468)

按 §0.1 规则 3(`:15`),「**改既有字段语义**」在禁止面上,而 `channel_id` 在三个 claim 态上的
取值语义**确实变了** —— **该面已由 #273 触及**。本 PR 只做文字追认。是否升主版本号曾转 **[SL-468]** 交用户裁定,**现已裁定**(见下)。

> ~~**理由①(原文):触发点在 #273,不在本卡 —— 一张纯文字追认卡没法回溯地替上一个 PR
> 补版本号。**~~ **已撤回**(#275 复审【红旗】,2026-09-21):`contractVersion` 描述的是
> **契约当前状态**、不是**归因**,所以「没法回溯地替上一个 PR 补」这个说法不成立 ——
> 版本号**现在就能升**,该不该升是另一回事。**留此痕而不删净**:一条被有效反驳后撤回的
> 论证,正是后人最该看见的那种。

保持 `1.0` 的理由因此只剩两条,且**第二条是决定性的**:

1. **改动面。** 升 2.0 要按 §9.0 第 3 条连带更新 mock 后端 + C++ 常量表 +
   `check-bridge-parity.mjs`,本卡会从 3 个 `.md` 涨成跨 `web/` + `src/` + `scripts/`。
2. **授权范围(决定性)。** 用户批的是「SL-464 契约文字追认,前提没风险」;
   **主版本号 +1 是对外的破坏性声明,超出该批准**。统筹无权代为决定。

### 用户裁定(2026-09-21):取**豁免**

**`contractVersion` 保持 `1.0`,不升主版本号。**

**豁免的是 §0.1 规则 3** ——「触碰禁止面(含『改既有字段语义』)= 契约破坏性变更,
`contractVersion` 主版本号 +1」这一条。

**理由**(裁定时摆在用户面前的两条,第二条是决定性的):

1. 升 `2.0` 要按 §9.0 第 3 条连带更新 mock 后端 / C++ 常量表 / `check-bridge-parity.mjs`,
   本卡从 3 个 `.md` 涨成跨 `web/` + `src/` + `scripts/`;
2. 主版本号 +1 是**对外的破坏性声明**,而本次只是文字追认 —— 用户据此选择豁免。

⚠ **被豁免的事实本身不变**:禁止面**已由 #273 触及**,`channel_id` 在 `conflict` /
`abiMismatch` / `idle` 三态上的取值语义确实变了。豁免的是「**因此要升版本号**」这条
**后果**,**不是**「没碰禁止面」这个**事实** —— 两者别混读。

本条即 #275 复审【红旗】要求的「合并记录里写明『升 2.0』或『豁免 §0.1 规则 3 + 理由』
二选一」的落定记录(采纳其收口建议;二选一是用户的选择)。
⚠ 顺带记一笔:这一版**没有任何机检兜底** —— `branch-gate` 在 base=`feature/v1` 的子 PR 上
不跑(见上文「guard 的覆盖面与『本 PR 会不会触发它』是两件事」),本条全靠人工裁定落定。

**§9.0 第 4 条(「每次改动必须同步更新 §7 manifest 与本节版本行」)不触发**:§7 manifest 与
冻结表面本次零改动,manifest 与版本行都无可同步之处,`check-bridge-parity.mjs` 照常绿。
文件头 `:3` 的 `> **版本**:1.0(已冻结)` 因此不动。

⚠ **成例 [J90] 只覆盖「零语义改动的纯文字对齐」**(`20260828-j90-contract-text-align.md:113-117`
自己写明「函数名 / 签名 / 返回字段 / 事件名 / 载荷字段零改动」「本次零表面改动」),与本次前提
不同 —— 它**只能**用来支持上面那句「§9.0 第 4 条不触发」,**不能**用来论证「禁止面没被碰」。

⚠ **另记一条措辞不一致**(已进 [SL-468]):§0.1 规则 3(`:15`)的禁止面**含**「改既有字段语义」,
而 §9.0 第 3 条(`:989`)只枚举「任何改名/删除/收窄」。本次取值域仍是 `0..15`、**没有收窄**
⇒ **按 §9.0 第 3 条读不触发,按 §0.1 规则 3 读触发**,两条读出来结论相反。这一条**未被本次裁定覆盖**(用户裁的是「本卡升不升版本号」,不是「两条条款该怎么统一」),**仍留在 [SL-468] 卡上**作后续事项。

## 回扫:还有没有别的「某值 ⟹ 某态」反向定义被同一改动打破

**本 PR 落笔前**在 `docs/` 全目录跑的回扫(命令逐字为 `grep -rnF -- '<串>' docs/ | wc -l`):

| 串 | 命中 | 处置 |
| --- | --- | --- |
| `channel_id=0` | 4 | 改 2(`SCVB_CONTRACT.md:598` / `:738`);不改 2(见下) |
| `channel_id = 0` | 0 | — |
| `channel_id:0` | 2 | 不改:`SCVB_CONTRACT.md:597` / `:676` 是**载荷形状**行(`channel_id:0..15`),只写值域不做语义反推 |
| `channel_id: 0` | 3 | 不改:`PARAMETERS.md:95` / `STATE_SCHEMA.md:88` / `constitution/params-v0.md:94` —— **存档/配置值**,语义未变 |
| `channelId=0` | 0 | — |
| `channelId = 0` | 0 | — |
| `channelId == 0` | 0 | — |
| `= 未分配` | 1 | = `params-v0.md:94`,同上 |
| `未分配` | 10 | 上述之外:`constitution/params-v0.md:117`(J01 原文,讲首次插入默认 0,仍成立)、`SCVB_CONTRACT.md:608`(`setChannelId(n)` **入参** 0=未分配/主动释放,是**写侧**,未变)、`spikes/E2E-journey.md:39`(讲实例以 `channel_id=0` 启动,仍成立)、`design/SCVB 设计稿.dc.html` 三处(设计稿) |
| `unassigned` | 9 | 上述之外:`SCVB_CONTRACT.md:633`/`:635`(§3.4 `remoteSetPriority` 的 reason,见下「登记」)、`:784`(§5.6 十一值闭集,只列名)、`:908`(§7 manifest 枚举)、`:1041`(§9 补白表)、`contract-changes/20260825-export-suggestions.md:42`(旧变更文档引述)、`design/*.dc.html`(设计稿) |

⚠ 这张表的「零命中」取决于**模式的字符变体**与 `grep` 的**目录/后缀集**,两者都不在 `wc -l`
的输出里,所以命令原样写在上面。另确认过本文件全篇的括号 / 逗号 / 分号是 **ASCII** 半角
(`docs/SCVB_CONTRACT.md` 全篇的 `U+FF08/FF09/FF0C/FF1B/FF1A` 计数均为 0 —— **只量过这一个文件**,仓里别的 md(`CLAUDE.md` / `docs/constitution/*.md` / `CHANGELOG.md` 等)是有全角的),所以不存在「全角变体没扫到」这一路。

## 照实登记:本次**不改**的两处发现(已各自立卡)

1. **[SL-467] —— §3.4 `remoteSetPriority` 的拒绝态行不完整,但 #273 没让它变假**
   (`SCVB_CONTRACT.md:635`)。契约只写了「`channel_id=0` → `reason:"unassigned"`」;实现
   (`bridge::priorityRejection()`,`InputBridgeLogic.cpp:57`)对**非活跃**实例
   (`channel_id≠0` 但 `claim` ∈ {`conflict`,`abiMismatch`,`idle`})同样回 `"unassigned"`
   (`kNotActive` 分支)。这是 PR#54 R3 起的**既存**缺口,与本卡无关;按新口径读那句话仍然
   **为真**(桥面 `channel_id=0` 的态全都非活跃 ⇒ 两条分支都回同一个 reason),故不动。
   另记一笔供开卡时先看:对一个明明选了通道、只是撞了车的实例回「未分配」,`reason` 这个
   **串本身**可能才是问题(用户可见文案口径),开卡时先查 UI 侧怎么消费它。
2. **[SL-466] —— §5.2 `idle` 行的定义句不覆盖 `kUnavailable`**(`:739`)。现文写「已选 channel,
   slot 已声明但 Output 尚未健康读取」,而 `kUnavailable`(段未打开 / 映射失败)既没有已声明的
   slot、UI 落点也不是它写的「pill『等待 Output』」(`channel_id=0` 会先被
   `web/input/app.js:646` 那道闸拦到灰 pill「未选择通道」)。两个时间点要分开记:
   **定义句不覆盖 `kUnavailable` 是既存的**(`kUnavailable → idle` 的映射早于 #273);
   **这一支 `channel_id` 变成 `0` 是 #273 新引入的** —— 是后者把前者从「读起来别扭」变成
   「UI 落点写错了」。本 PR 只在 §3.1/§4.1 把这一支**点名**,不动 `idle` 行本身:改它要先裁
   「`idle` 该不该承载 `kUnavailable`」,那是契约值域问题、还要连 §7 manifest 的六值枚举一起算,
   面比本卡大。

## 变更文件

- `docs/contract-changes/20260921-sl464-channel-id-zero-semantics.md`(**新增**,本文档)
- `docs/SCVB_CONTRACT.md`(① ② ③ 三处语义文字改实)
- `CHANGELOG.md`(本卡的预写条目;**另含** #273 那条预写条目的回搬 —— 见 PR 描述)

## 审批

- **用户批准**:2026-09-21(契约文字变更;前置「没风险」由统筹核实:Bridge 仓零命中、
  SCVB 自己仅有的两处 `channel_id` 消费面不做反向推导)。
- **统筹裁定**(2026-09-21):不给新 A/J 号(最近六张契约变更卡 SL-395/398/411/413/414/415/416
  一个都没给,§9.1 的 A-28~A-32 是起草期封闭留档);§4.1 写第二份显式语义句且与 §3.1 逐字相同;
  §5.2 `unassigned` 行的越界批准但只去掉假的那半、行内留分辨点;§5.2 `idle` 行本卡不动
  (转 SL-466);§3.4 不动(转 SL-467);`contractVersion` 保持 `1.0`(曾转 SL-468;**用户 2026-09-21 已裁「豁免」**)。
- 随本 PR 挂 `status/frozen-contract`,由用户明确批准后合入(CLAUDE.md §5 / 06 §3.7)。

## 关联

- 实现来源:[SL-446] / **PR #273**(`44099f3`)—— `bridge::displayChannelId()` 与
  `BridgeTickSnapshot::configuredChannelId` 的引入。
- 转出的卡:**[SL-466]**(§5.2 `idle` 行)、**[SL-467]**(§3.4 拒绝态口径)、
  **[SL-468]** —— `contractVersion` 是否升 2.0:**已裁**(用户 2026-09-21 取豁免,保持 `1.0`);卡上**仍留一件后续** —— §0.1 规则 3 与 §9.0 第 3 条措辞不一致,未被本次裁定覆盖。
- 判据(**既有,本 PR 一格未加也未改**):`tests/host/test_host_harness.cpp:1055-1075`
  (`releaseResources()` 后配置值留存 + `displayChannelId()` 分流)、
  `tests/core/test_input_bridge_ipc.cpp:804` / `:821`(两个出口确实经 `displayChannelId()` 接线)。
