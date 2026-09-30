# 契约变更说明 —— 20260928-j160-constitution-guide-keys

> **状态:已批(随本 PR 挂 `status/frozen-contract`)。** 用户裁定 **J160**(发布前拍板清单第 13 项
> 「宪法副本 `params-v0.md:87` 过期口径」):**现在修** —— 走宪法修订流程,只改说明文字为 J132 口径。
> 用户原话:「8接收现状,9我晚点改,提醒我。13现在修就行。15晚点再说吧」—— 其中「13现在修就行」即本项。
> **实现侧不动一个字节**:本变更零 C++、零 JS、零测试。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**(§二 同一句已由 J132 改实,#301)。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**(§3.1 同一句已由 J132 改实,#301)。
- [ ] tests/golden/(golden 快照)—— **不动**。
- [x] **宪法** `docs/constitution/params-v0.md`(masterPlan `constitution/params-v0.md` 的仓内只读副本)
      v2.3 → **v2.4**:§二 Output `ui` 组「两侧全局位各存一份」一条的说明文字改实。字段、类型、默认值、
      编码落点、首启判据、state 容器 `abi`、自动化参数面(123 个)**零变化**。

上面五份冻结契约一份没碰,所以 branch-gate 的 path guard 不要求本文档;写它是 J160 的要求
(「修订记录 + 变更文档」)。

## 变更内容

**一句话**:J132(#301)把桥契约与状态契约里「两侧全局位各存一份」那句的括注改成了实现的实际写法,
当时宪法副本里同一句被明确留在范围外(见 `20260928-j132-guide-seen-global-keys.md`
「没改、但同样过时的一处」)。本变更按修宪流程把宪法里这一句也改掉,口径与 #301 一致:
**同一个落盘文件 `ui-defaults.settings`,两个键 —— Output `guide_seen_global`、Input
`guide_seen_global_input`;`tour_seen_global` 只有 Output 一个键。**

### 宪法文件里动了哪三处

原件与副本同改(副本 = 头注行 + 原件逐字节,所以副本行号 = 原件行号 + 1):

| # | 位置 | 改动 |
| --- | --- | --- |
| ① | 状态行(副本第 4 行) | `v2.3` → `v2.4`,补一句本次修订摘要;v2.3 的原描述整句保留 |
| ② | §二 `[J81]` 字段说明末条(副本第 87 行) | 见下面改前 / 改后 |
| ③ | 文末 | 追加 `## v2.4 修订(2026-09-28,J160 说明文字改实)` 节(修订节只追加,历史节一字未动) |

### ② 改前 / 改后(逐字)

改前:

```text
- **两侧全局位各存一份**(`input.*` / `output.*` 分键):两侧引导讲的是两个界面、两套内容;共用一个位会让先装 Output 的用户永远看不到 Input 的引导 —— 而 J80 立 T48 的**全部理由**就是「Input 是用户见到的第一个界面却零引导」。`UiDefaultsStore` 的命名空间本来就按侧分(`scvb::output::uidefaults` / `scvb::input::uidefaults`)。
```

改后:

```text
- **两侧全局位各存一份**(指 `guide_seen` 的全局位):两侧引导讲的是两个界面、两套内容;共用一个位会让先装 Output 的用户永远看不到 Input 的引导 —— 而 J80 立 T48 的**全部理由**就是「Input 是用户见到的第一个界面却零引导」。两侧共用 `UiDefaultsStore` 的**同一个**落盘文件(`ui-defaults.settings`),靠**键名**分开 —— Output 用 `guide_seen_global`、Input 用 `guide_seen_global_input`;`tour_seen_global` 只有 Output 一个键(Input 没有交互式导览)。([J160] 按 J132 口径改写,见文末 v2.4 修订节)
```

中间那句理由(「两侧引导讲的是……零引导」)逐字未动。括注由「怎么分」改成范围限定「指 `guide_seen`
的全局位」:紧上一条同时列了 `guide_seen_global` / `tour_seen_global` / `lang_chosen_global`,
不限定就会被读成三个全局位都按侧分 —— 而 `tour_seen_global` 只有一个键(本句已写明),
`lang_chosen_global` 不在 J160 的范围里,本变更对它不作任何断言。

### 与 J132 裁定备注的出入(沿用 #301 的处理)

J132 的裁定备注写「契约改为单键口径」。那是 SL-258(#167)给 Input 另起 `guide_seen_global_input`
之前的事实;#301 已按实现写成「两个键」,本变更与 #301 一致,不照「单键」字面写
(理由见 `20260928-j132-guide-seen-global-keys.md` 的 ⚠ 节)。

### 实现事实核对

在本 PR 的基线 `origin/feature/v1` `44ac4bf` 上跑:

```text
$ grep -n 'kKeyGuideSeen = \|kKeyGuideSeenInput = \|kKeyTourSeen = \|applicationName = \|filenameSuffix = \|^namespace scvb' src/plugin-common/UiDefaultsStore.cpp
10:namespace scvb::uidefaults
16:constexpr const char* kKeyGuideSeen = "guide_seen_global";
17:constexpr const char* kKeyTourSeen = "tour_seen_global";
31:constexpr const char* kKeyGuideSeenInput = "guide_seen_global_input";
70:    options.applicationName = "ui-defaults";
72:    options.filenameSuffix = "settings";
```

即:一个命名空间 `scvb::uidefaults`、一个落盘文件 `ui-defaults.settings`(`applicationName` +
`.` + `filenameSuffix`),guide 两个键、tour 一个键。改前那句写的 `input.*` / `output.*` 键与
`scvb::output::uidefaults` / `scvb::input::uidefaults` 两个命名空间在实现里都不存在。

## 修宪流程落地(`docs/constitution/ADR.md` 文末「修宪流程」第 4 条)

| 项 | 落点 |
| --- | --- |
| ① 改原文并升版本号 | masterPlan `constitution/params-v0.md`:上表三处(一个 commit,message 带 J160) |
| ② 同步只读副本 | 本 PR 的 `docs/constitution/params-v0.md`(头注行 + 原件逐字节) |
| ③ INDEX 口径行 | masterPlan `plan/INDEX.md` 口径行 `params v2.3` → `params v2.4`(与 ① 同一个 commit) |
| ④ 受影响的计划文档 / 任务卡 | 仓内:`20260928-j132-guide-seen-global-keys.md`「没改、但同样过时的一处」节补一行指路到本文档 |
| ⑤ 通知在跑的 agent | 由统筹处理 |

## 兼容性影响

零行为变化:不动任何键名、落盘位置、首启判据;宪法与副本都是文档。老用户已勾的「不再显示」不受影响。

## 验证

- `pwsh scripts/check-constitution-sync.ps1 -MasterPlanDir <改后的 masterPlan constitution/>` ⇒ 三份 PASS;
  对**未改**的原件跑 ⇒ `params-v0.md` FAIL、其余两份 PASS(副本确实变了,且只有与原件同改才绿)。
- 旧写法回扫(`grep -rn -I "scvb::output::uidefaults\|scvb::input::uidefaults" .`,排除 `.git` /
  `node_modules`):**本文档自身以外**命中 4 行,全部是**引述旧文字**的记录:宪法 v2.4 修订节 1 行(说明改了什么)、
  `20260825-input-guide-seen.md` 2 行(当时的提案记录,不改)、`20260928-j132-guide-seen-global-keys.md`
  1 行(J132 当时对本处的登记)。**宪法正文**(修订节之外)0 命中。
- **没有**给宪法副本加内容级断言:副本已由 `check-constitution-sync.ps1` 钉成与原件逐字节相等
  (本地 gates 的 gate 1;CI 上没有 masterPlan,不跑)。`web-preview/tests/smoke-tab4-settings.mjs`
  里 J132 那一格只扫 `SCVB_CONTRACT` / `STATE_SCHEMA`,它的旧写法正则对不上宪法这句的旧措辞
  (「命名空间**本来**就按侧分」、括注后跟的是右括号不是逗号),而修订节本身又引述了旧文字 ——
  直接把宪法加进那张表会得到一格恒绿的空判据,所以不加。

## 审批

- 用户裁定 **J160**(masterPlan `plan/adjudications.md`),原话见文首;所依口径 **J132** 同见该文件。
