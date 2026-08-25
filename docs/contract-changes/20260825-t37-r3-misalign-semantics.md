# 契约变更说明 —— 20260825-t37-r3-misalign-semantics

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] **docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)—— §2.3 `scvb.conn.channels[].misalignCount` 字段语义**

## 变更内容

`scvb.conn.channels[].misalignCount` 的语义由「该轨**累计**失准计数(gapCount)」改为
「该轨**本次失准发作**内的缺口数 —— 连续 `kMisalignRecoverMs`(1s)无新缺口即归零」。

字段名、类型(int)、位置(§2.3 载荷 `channels[]` 内)均不变;**只改语义**。

### 为什么必须改

原语义与它唯一的 UI 消费面自相矛盾:

- web 侧判据是**无状态**的 —— `app.js` 每帧从 `scvb.conn` 重算
  `misalignedTracks(conn) = channels.filter(c => c.misalignCount > 0).length`,
  `show(banner, mis > 0)`,健康即隐藏。这是「**当前是否失准**」的语义。
- 而 native 侧送上桥的 `ShmRingMixSource::gapCount_` 是**进程寿命累计值**:全代码只有两处
  `fetch_add`、**零处复位**,`bind`/`unbind`/`release`/`changeGroup` 都不清。

两者相接的后果:**只要抖过一次,「路由失准:N 轨检测到时间线缺口」横幅就永久钉死**,
音频链路恢复正常也撤不下来。这正是 T37 三轮真机报告的原话:「重开 Input、Input 显示
connected、音频链路正常,但 Output 的失准警告一直不消失」。

累计值不是不该存在,是**不该走这条通道**:它是诊断量,而 §2.3 这条是状态量。

### 累计值去哪了(没有丢)

进程寿命累计值原样保留,经既有的两条诊断通道暴露:

- `OutputSession::gapCount(ch)` → ctrl 段 `OutputGlobalInfo.gap_count[15]`(J09 全局小节,
  验证工具与 Input 侧只读);
- Tab4 诊断区若要显示「累计失准」,数据源改取上面这条,不再取 `scvb.conn`。

native 侧两个口径各有独立入口,互不覆盖:
`ScvbOutputAudioProcessor::gapCount()`(累计)与 `misalignCount()`(本次发作)。

## 兼容性影响

- **桥面 ABI**:无。字段名/类型/位置不变,`scripts/check-bridge-parity.mjs` 的名字集合比对不受影响。
- **IPC ABI**:无。`RegistryHeader`/`CtrlHeader` 等段布局未动,`abi` 不变。
- **参数面**:无。123 参数布局未动。
- **工程 state**:无。该字段是运行时事件载荷,不入 state chunk、不随工程持久化。
- **UI 行为变化(有意)**:
  - 「路由失准」横幅与轨道页逐行 ⚠ 现在**会自行撤下**(恢复健康满 1s 后),此前永久常驻;
  - **Tab4 诊断区「失准计数」一栏含义随之改变** —— 若该栏意在展示「累计」,需改接
    `OutputGlobalInfo.gap_count`;若意在展示「当前是否失准」,则本改动正是它需要的语义。
    本 PR 未改 Tab4 的取数(它目前也取 `scvb.conn`),留待 UI 侧按上述二选一定夺。
- **旧构建互操作**:无。该字段只在同一进程内的 C++→JS 方向流动,不跨版本对接。

## 配套的实现改动(同 PR)

- `OutputSession::misalignCountRecent(ch)`:`gapCount - misalignBaseline_`;
  `evaluateChannels` 判定恢复健康(`!misaligned`)时把 baseline 对齐到当前累计值。
- `resetChannelTracking()` 顺带修一处既有缺陷:原先把 `lastGapCount_` 填 0,而 `gapCount_`
  从不清零,导致改组后下一拍 `gc != lastGapCount_` 恒真、新组第一拍即被判失准 ——
  改为对齐到当前值。
- 回归断言:
  - `tests/core/test_output_session.cpp` —— 失准发作 → 恢复健康归零 → 再次失准重新报数;
  - `tests/host/test_host_harness.cpp` `HOST L-6b` —— 端到端:bypass 断流报警 → 恢复后自行清除。

---

# 追加变更 —— `ui.lang_chosen` / `lang_chosen_global`(v4 实测 P1-6)

## 变更内容

`scvb.state.ui` 新增**可选**布尔 `lang_chosen`;`requestInitialState()` 快照新增**可选**布尔
`lang_chosen_global`。二者语义与既有的 `guide_seen` / `guide_seen_global` 完全同构:
前者随工程走(PRMS),后者是系统级全局默认(UiDefaultsStore),用于判定「首启语言选择卡
是否还要弹」。

**不新增桥函数**:置位点选在既有的 §1.30 `setLang` 桥入口 —— web 启动时的语言回填走
`setLang(..., {push:false})` 不经桥,所以「桥的 setLang 被调用过」正好等价于「用户显式选过语言」。

## 为什么必须加

首启语言卡的抑制条件此前**只有** web 侧 `store.session.langChosen` —— 会话内标志,随 WebView
一起销毁;而卡片的显示门读的是 `ui.guide_seen` / `guide_seen_global`(**完全没有看语言**)。
于是只要用户没走完红字页,每次开窗都会重新问一遍语言(v4 实测 P1-6:用户要求「选过就记住」)。

`ui.language` 本身不能兼任这个标记:它默认 `"en"`,「从没选过」与「用户就是选了英文」不可区分;
而 CFGS 是定长解码,追加字段会让旧构建整块拒载(见 `OutputUiState.h` 头注),所以新位必须
落 PRMS 的 ValueTree —— 那里增删字段零成本、老工程读不到即 false。

## 兼容性影响

- 按 §0.1 规则 3,这是**加法**(既有 payload 新增可选字段),非破坏性,`contractVersion` 不升主版本;
- 旧 web 不读该字段 → 行为不变(仍每次问);旧工程无该属性 → 读回 false → 走一次首启,选完即持久;
- 参数面 / IPC ABI / state chunk 布局均未动。

## 同 PR 已完成

- [x] `docs/SCVB_CONTRACT.md` §1.1 快照表补 `lang_chosen_global?`、§1.1/§2.1 的 `ui` 字段表补 `lang_chosen?`
- [x] `web-preview/mock/juce-bridge-mock.js` 的 `setLang` 同步置两位;`web/shared/mock-data.js` 默认 state/快照补字段
      (mock 与真桥须同契约,CLAUDE.md §10)

## 审批

- [ ] **用户批准:待批准**(PR #87 已挂 `status/frozen-contract` 标签;本条不合入直到用户确认)
- [x] 复审依据:PR #87 自动复审两轮均判定「代码方向正确、不必回退,补变更文档 + 更新 §2.3
      文字 + 挂标签即可」(R2)。
