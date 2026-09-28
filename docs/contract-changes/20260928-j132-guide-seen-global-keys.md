# 契约变更说明 —— 20260928-j132-guide-seen-global-keys

> **状态:已批(随本 PR 挂 `status/frozen-contract`)。** 用户 2026-09-28 裁定 **J132**(卡 **SL-167b**):
> 「guide_seen/tour_seen 分键 vs 单键」**以实现为准改契约文字**,行为不变。用户原话:「298 180 167b 94 218
> 按你说的做」—— 其中 167b 那一项「按你说的做」指的就是「以实现为准改契约文字」这个提案。
> **实现侧不动一个字节**:本变更零 C++。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [x] docs/STATE_SCHEMA.md(state schema)—— **一句描述改实**:§二 Input `ui.guide_seen` 小节的
      「全局默认位」条。字段、类型、默认值、编码落点、abi **零变化**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— **一句描述改实**:§3.1 `requestInitialState` 的「语义」格
      末尾那句「Input 与 Output 的全局位各存一份」的括注。函数名 / 签名 / 事件名 / **载荷字段零变化**。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`:这是描述文字改实,不是改名 / 删除 / 收窄(§9.0 第 3 条不命中),不需要豁免。

## 变更内容

**一句话**:契约说两侧的全局已读位「`input.*` / `output.*` 分键,`UiDefaultsStore` 命名空间本就按侧分」;
实现里既没有 `input.*` / `output.*` 这种键,`UiDefaultsStore` 也只有一个命名空间 `scvb::uidefaults`、一个落盘文件。
现在两处改成实现真正的样子:**同一个文件 `ui-defaults.settings`,两个键 —— Output `guide_seen_global`、
Input `guide_seen_global_input`;`tour_seen_global` 只有 Output 一个键。**

### ⚠ 卡面与裁定里的「单键」说法要订正(本 PR 没有照它写)

SL-167b 的卡面写「实现是单键(仅 ui_scale 真分侧)」,J132 的备注据此写「契约改为单键口径」。
**这是 SL-258(#167)之前的事实**:#167 已经给 Input 另起了 `guide_seen_global_input` 键,
`tests/core/test_ui_defaults_store.cpp` 的 `[SL258]` 用例钉着「两侧互不串扰」。所以今天的实现是
**guide_seen 分两个键、tour_seen 一个键(Output 专属)**,不是单键。

照字面写成「单键」会让契约与实现从「写法不对」变成「结论相反」。J132 的实质是「以实现为准」,
本 PR 按实现写;「各存一份」这个**结论本来就是对的**,错的只是括注里描述「怎么分」的那半句。

核对命令(本 PR 开工时在 `origin/feature/v1` `52f5d6d` 上跑):

```
$ grep -n "kKeyGuideSeen\|kKeyTourSeen" src/plugin-common/UiDefaultsStore.cpp
16:constexpr const char* kKeyGuideSeen = "guide_seen_global";
17:constexpr const char* kKeyTourSeen = "tour_seen_global";
31:constexpr const char* kKeyGuideSeenInput = "guide_seen_global_input";
...
$ grep -n "guideSeenGlobalInput()" src/input/InputEditor.cpp
122:        ... scvb::uidefaults::guideSeenGlobalInput(), ...
```

### 两处逐条

| # | 文件 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | `SCVB_CONTRACT.md` §3.1 语义格 | 「各存一份(`input.*` / `output.*` 分键,`UiDefaultsStore` 命名空间本就按侧分)」 | 「各存一份:两侧共用 `UiDefaultsStore` 的同一个落盘文件,靠键名分开 —— Output `guide_seen_global`、Input `guide_seen_global_input`」;补一句 `tour_seen_global` 只有 Output 一个键 |
| ② | `STATE_SCHEMA.md` §二「全局默认位」条 | 「各存一份(`UiDefaultsStore` 命名空间本就按侧分)」 | 「各存一份:同一个落盘文件里的两个键 —— Output `guide_seen_global`、Input `guide_seen_global_input`」 |

## 没改、但同样过时的一处(不在本裁定范围)

`docs/constitution/params-v0.md` 的「两侧全局位各存一份」条也写着「`input.*` / `output.*` 分键」与
「命名空间本来就按侧分(`scvb::output::uidefaults` / `scvb::input::uidefaults`)」。这是宪法只读副本,
与 masterPlan 原件逐字节比对(`scripts/check-constitution-sync.ps1`),改它要走修宪流程,J132 只批了
SCVB_CONTRACT / STATE_SCHEMA 两份。**留给统筹决定是否另起修宪。**
`docs/contract-changes/20260825-input-guide-seen.md` 里的同款说法是当时的提案记录,不改。

## 验证

- `web-preview/tests/smoke-tab4-settings.mjs` ⑥ 节新增一格:实现里确有两个键字面量;
  两份契约都写出 `guide_seen_global_input`,且不再出现旧括注的写法。
- 删除式:把 ① 或 ② 改回旧文字 ⇒ 该格红;复原 ⇒ 绿(读数见 PR 描述)。

## 兼容性

零行为变化:不动任何键名、任何落盘位置、任何首启判据。老用户已勾的「不再显示」不受影响。

## 审批

- 用户 2026-09-28 裁定 J132(masterPlan `plan/adjudications.md`),原话见文首。
- 与裁定备注「单键口径」的出入见上面 ⚠ 一节,已在 PR 描述里向统筹报告。
