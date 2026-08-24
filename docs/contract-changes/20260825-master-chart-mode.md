# 契约变更说明 —— 20260825-master-chart-mode

> 提出方 = T43(**[J75]** Tab1 分布图双视图 + 轨道配色);裁决真源 = `05-ui-spec.md` 文末「J75 增补规格」A 节。
> 本 PR **只提出**变更、不改任何冻结文档本体;落地(state codec + 桥 setter + C++ 常量表 + 契约 §7 manifest)转 **DS 侧(native)**。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。本字段是 state,非自动化;123 个参数的 ParamID/index/顺序一字未改。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。不跨进程,不进任何共享段。
- [x] docs/STATE_SCHEMA.md(state schema)—— Output state 的 `ui` 子树新增一个字段。
- [ ] tests/golden/(golden 快照)—— **不动**。

> 说明:本 PR 的 diff **不含** `docs/STATE_SCHEMA.md`。该文件当前仍是 T39a 的占位骨架
> (`## 一、Output state(占位)`),正式内容尚未蒸馏;等 T39a 填充或 native 落地时,
> 把下面「字段定义」一节原样并入其 `ui` 子树即可。此处先立字据,是为了不让一个新 state 字段
> 无声地长在 web 里。

## 变更内容

Output state `ui` 子树新增 **`master_chart_mode`**:Tab1「声像 / 音量分布」卡片的视图态。

### 字段定义

| 项 | 定义 |
|---|---|
| 路径 | `ui.master_chart_mode` |
| 类型 | 字符串枚举 `"distribution" \| "trajectory"` |
| 默认值 | **`"distribution"`**(05 J75 A 逐字:「默认 `distribution`」) |
| 语义 | `distribution` = 既有的声像/音量分布图;`trajectory` = 新增的 pan 轨迹图(x = 工程时间线,y = pan 角度域) |
| 自动化 | **否**。不占参数面,不可被 DAW 录制 |
| 持久化 | **是**,随工程走(与 `ui.active_tab` 同族:重开面板恢复上次视图) |
| 运行时态 | 否(不同于 `print_guard` / `recapture` / `analysis_run` 三件) |

### 迁移语义

- **读到没有该键的旧工程**(v1 之前存的 chunk):按默认档 `"distribution"` 处理,**不报错、不提示**。
  这是纯显示偏好,缺省与「用户从没切过视图」是同一件事,不存在丢数据。
- **读到未知取值**(手改工程文件、跨版本):同样回落 `"distribution"`。UI 侧的
  `chartModeOf()`(`web/output/tab-master.js`)已按此实现,并有冒烟断言
  (`web-preview/tests/smoke-t43-chart.mjs` ③:缺子树 / 缺字段 / 未知值三条都落默认档)。
- **不需要写迁移函数**:字段是纯增量、有确定性默认值,不改变任何既有字段的编码或含义,
  故 `abi` **不必递增**。若 native 侧选择在同一批里做别的 schema 改动而递增了 `abi`,
  本字段随那次迁移一并带上即可(「读到低版本 → 迁移函数升格」时把本键补成默认值)。
- 反向兼容(**新工程被旧版本读到**):旧版本的 `setStateInformation` 不认识这个键,按其既有
  的「忽略未知键」路径丢弃 —— 用户看到的是视图回到分布档,无其它影响。

### 建议的桥写入口(最小增量)

契约 `ui` 同族 setter 的现成形制(`§1.28 setUiScale` / `§1.30 setLang` / `§1.31 setActiveTab`)
里,**`setActiveTab` 是最贴的一个**:同为「枚举取值 → 写 `ui` 子树 → 经 `scvb.state` 回推」的
纯 UI 偏好,无撤销、无拒绝态以外的副作用。照它逐字对齐:

| 项 | 定义 |
|---|---|
| 名字 | `setMasterChartMode(mode)` |
| 参数 | `mode: "distribution" \| "trajectory"` |
| 返回 | `{ok:true}` 或 `{ok:false, reason:"badArg"}` |
| 语义 | 写 state `ui.master_chart_mode`;经 `scvb.state`(§2.1)回推。**不触发**任何分析/打印/参数写入 |
| 拒绝态 | 未知值 → `{ok:false, reason:"badArg"}` |
| 撤销 | 否(与 `setActiveTab` 同 —— UI 偏好不进 undo 栈) |
| 线程/频率 | `[M]`;用户点一次调一次 |

转正时要同批改的四处(缺一处 `scripts/check-bridge-parity.mjs` 就会红,这是好事):

1. `docs/SCVB_CONTRACT.md` —— §1 正文新增条目 + §7 manifest 的 `output.functions` 加名
   (计数断言 **34 → 35**,`scripts/check-bridge-parity.mjs` 的 `EXPECTED` 表同改);
2. `src/output/OutputBridgeApi.h` —— 加 `kFnSetMasterChartMode` 常量;
3. `web/shared/bridge.js` —— 把名字从 `PENDING_FUNCS` **挪进** `BRIDGE_FUNCTIONS.output`
   (留在 `PENDING_FUNCS` 里等于让它绕过 parity 门禁,那才是真正的洞);
4. `docs/STATE_SCHEMA.md` —— 并入上面的「字段定义」。

### 本 PR 的 web 侧落地(不碰冻结面)

- 视图态的**读**:`chartModeOf(state)` 直接读 `ui.master_chart_mode`,缺失即默认档 ——
  下行方向不需要任何契约变更,今天就能工作。
- 视图态的**写**:走 `bridge.js` 新立的 `PENDING_FUNCS` 表(一张**不在** parity 比对面里的
  「待转正」名表,能力探测后才挂到桥上)。于是当下两种运行形态都成立:
  - **预览 / mock**:mock 后端已按上表形制实现 `setMasterChartMode`,调用落地 →
    `scvb.state` 回推 → UI 把本地乐观值交还给 state。「切换态持久化往返」在 mock 下当场可验
    (`smoke-t43-chart.mjs` ⑤ 断言了往返与 `badArg`)。
  - **真 JUCE 宿主(native 未落地)**:桥上根本不挂这个名字,调用直接返回 `null`,视图态停在
    UI 本地乐观值 —— 重开面板回默认档,**不写、也不假装写了**任何 state。
- 代码里的 TODO 锚点:`web/output/tab-master.js` 的 `setChartMode()` 头注、
  `web/shared/bridge.js` 的 `PENDING_FUNCS` 头注,两处都指回本文件。

## 兼容性影响

- **既有工程**:无影响。旧 chunk 没有该键 → 默认档 → 与本次改动之前的界面行为逐字相同。
- **既有 DAW 自动化**:无影响。参数面 123 个一字未动,本字段不可自动化、不进参数通道。
- **新旧版本互通**:无破坏。新版写的工程被旧版读到只是丢一个显示偏好;旧版工程被新版读到走默认档。
- **abi**:按上文,本字段自身**不要求** `abi+1`。
- **实时线程**:无影响。`[M]` 消息线程写入,`processBlock` 不读不写。

## 审批

- PR 挂 **`status/frozen-contract`** 标签(仓库若尚无该 label,则在 PR 描述首行以文字标注),
  由用户明确批准后合入(仓 `CLAUDE.md` §5 / 06 §3.7)。
- 本 PR 的 diff **不触碰**四份冻结文档本体,故 branch-gate 的「冻结契约 path guard」不会被触发;
  本文件是**提前**立的变更说明 —— 等 native 侧改 `docs/STATE_SCHEMA.md` 与 `docs/SCVB_CONTRACT.md`
  时,那个 PR 直接引用本文件即可,不必再写一份。
