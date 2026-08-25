# 契约变更说明 —— 20260825-export-suggestions

> 提出方 = **T41**(建议表 + CSV 导出;**U12 已答复「进 v1 主线」**);来历真源 =
> `11-risks.md §4.2.3` 通路 B 的 **B1(数值表)/ B2(CSV 导出)**,冻结口径 = `07-execution-plan.md` T41 卡。
> 本 PR **只提出**桥面变更、不改任何冻结文档本体;桥函数落地(保存对话框 + C++ 常量表 +
> 契约 §7 manifest)转 **native 侧**。行集构造与 CSV 序列化的**纯计算**部分本 PR 已两侧交付。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。建议表只**读** 123 个参数里的
      `v{v}_t{tt}_width`,不新增、不改名、不改 index。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。不跨进程,不进任何共享段。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。**本卡不新增任何 state 字段**:
      建议表的视图态是纯本地 UI 态(见下「为什么不落 state」),导出本身无状态。
- [ ] tests/golden/(golden 快照)—— **不动**。
- [x] **docs/SCVB_CONTRACT.md(桥面函数全集)** —— Output 侧**新增一个函数**
      `exportSuggestions(scope)`。这是本文件唯一提出的变更。

## 变更内容

### 新增 Output 函数 `exportSuggestions(scope)`

| 项 | 定义 |
|---|---|
| 名字 | `exportSuggestions(scope)` |
| 参数 | `scope`:`{versions?: "active" \| "all", tracksMask?: u16, startS?: f64, endS?: f64}`;整体可省(= 全默认) |
| 默认 | `versions:"active"`(当前激活版本)、`tracksMask:0x7FFF`(全 15 轨,**bit15 保留 0**,契约 §9.2)、时间窗不限 |
| 返回 | `{ok:true, rows:int, path:string}` 或 `{ok:false, reason}` |
| 拒绝态 | `badArg`(versions 不在两值内 / `endS ≤ startS`)、`cancelled`(用户关掉保存对话框)、`noData`(scope 内零段)、`ioError`(写盘失败) |
| 语义 | 从 **CRVS 曲线真身**取段表 → 按冻结列定义生成行集 → 序列化 CSV → **弹保存对话框**由用户选路径 → 落盘。**不写 state、不发 gesture、不动参数、不入撤销栈** |
| 线程/频率 | `[M]` 消息线程;用户点一次调一次。`processBlock` 与本函数无关 |
| 撤销 | 否(只读导出) |

**为什么必须是 native 而不是 web 自己下载**:① JUCE WebView 里 `<a download>` 与
`Blob` 保存都不通,只有 C++ 侧能弹系统保存对话框;② 跨版本导出(`versions:"all"`)UI
拿不到 —— §2.8 `scvb.segments` 一次只下发一个版本、§2.2 `scvb.params` 只带激活版本的 60 个
参数,**全版本的真相只在 C++ 的 CRVS 里**。

### 拒绝态 `cancelled` / `noData` / `ioError` 相对 §5.6 是新增值

契约 §5.6 现有八值(`badArg` / `busy` / `noLoop` / `notAdjacent` / `ringFull` /
`outputOffline` / `unassigned` / `noTimeline`)里没有「用户取消了一个文件对话框」这件事 ——
它是本函数带进来的第一个**用户可取消的阻塞式操作**。转正时 §5.6 的取值集合随本函数
从 8 值扩到 11 值(纯新增,不改既有值语义)。

### 冻结列定义(13 列,顺序不可改、不可增删)

```
track_index, track_label, source_channels, version, version_name,
segment_index, t0_sec, t1_sec, pan, vol_db, width, origin, locked
```

逐字 = 07 T41 卡 / 11 §4.2.3 B2。**`source_channels` 与 `width` 是 [J57] 随立体声进 v1 加的两列,
不可省** —— 一个 pan 点描述不了一条立体声轨,不带 width 的建议表对 stereo 轨**不可执行**。

| 列 | 类型 / 取值 | 说明 |
|---|---|---|
| `track_index` | 1..15 | 轨号 |
| `track_label` | 字符串 | `channels[ch].label`;**用户数据**,按 RFC 4180 转义 |
| `source_channels` | `1` \| `2` | 只读检测值(契约 §1.15 明文不可写) |
| `version` | 1..2 | |
| `version_name` | 字符串 | `versions[v].name`,同样按 RFC 4180 转义 |
| `segment_index` | 0 基 | 与 §2.8 段表下标同源 |
| `t0_sec` / `t1_sec` | 3 位小数 | 与 05 §2.3a 的 `mm:ss.mmm` 同精度 |
| `pan` | 1 位小数 | -100..+100 |
| `vol_db` | 1 位小数 | -24..+12 |
| `width` | 1 位小数 **或空** | stereo 轨有值;**mono 轨留空**——不是 0,0 是「收成 mono」的有效值([J57]),语义冲突 |
| `origin` | `auto` \| `user_edited` \| `user_created` | 与 T19 state 编码逐字一致 |
| `locked` | `true` \| `false` | 同上 |

**文件形制**:UTF-8 **带 BOM**(否则 Excel 打开中文轨名乱码 —— 本功能最常见的用户投诉面)、
换行 **CRLF**(含最后一行)、含表头行、字段按 RFC 4180 转义(含 `,` `"` CR LF 的字段整体
加双引号、内部双引号翻倍)。负零一律归一成正零(否则同一个零会写出两种字面值)。

**时间窗语义**:`startS`/`endS` 只做**筛选**,命中的段 `t0_sec`/`t1_sec` **不裁剪** ——
截半段会给出一个没人建议过的区间。

## 本 PR 已经落地的部分(不碰冻结面)

1. **C++ 纯计算层**:`src/core/export/SuggestionExport.{h,cpp}`(`scvb::suggest`)——
   `buildRows()` + `toCsv()`,纯 `scvb_core`、不链接 JUCE、不碰文件系统,离线可单测
   (`tests/core/test_suggestion_export.cpp`,9 个用例含 15 轨 × 2 版本 × 40 段的行数断言)。
   **native 侧转正时只需补「保存对话框 + 落盘 + 从 APVTS/CRVS 装 `ExportInput`」这一层**,
   格式相关的东西一行都不用再写。
2. **web 视图与同款序列化**:`web/output/tab-suggestions.js` —— 表格与 CSV 的
   13 个显示串**同出 `rowCells(row)`**(07 T41 验收「数值与 UI 显示值逐行相等,不是各算一遍」
   靠这条结构保证)。两侧的表头常量由 `web-preview/tests/smoke-t41-suggestions.mjs`
   读源码逐字对拍,漂了当场红。
3. **桥面**:名字停在 `web/shared/bridge.js` 的 `PENDING_FUNCS`(T43 `setMasterChartMode`
   立的先例,**不进** parity 比对面)。于是当下两种运行形态都成立:
   - **预览 / mock**:mock 后端已按上表形制实现 `exportSuggestions` —— 它**自己**从 mock
     的 state 真相数行数(不是把 UI 算好的数字收回来),预览页里导出全流程可走;
   - **真 JUCE 宿主(native 未落地)**:桥上根本不挂这个名字 → 导出钮 disabled + 说明文案,
     **不写、也不假装写了**任何文件。

## 为什么不落 state、不加第五个 tab

- **不加 tab**:契约 §1.31 `setActiveTab` 是 `master/tracks/wave/settings` 四值**冻结**枚举。
  建议表消费的正是 Tab3 的段数据(§2.8),故做成 **Tab3 的第二视图**(形制照 [J75] T43 给
  Tab1 加「分布 ↔ 轨迹」),桥面零改动。
- **不落 state**:视图态是查阅态而非长期偏好,重开面板回泳道档即可 —— 不值得为它再动一次
  冻结 state schema(与 T43 的 `ui.master_chart_mode` 是不同性质的东西,那个是用户会期望
  记住的图表偏好)。Tab3 那枚段检查器开关是同款处置的成例。

## 转正时要同批改的五处

缺一处 `node scripts/check-bridge-parity.mjs` 就会红,这是好事:

1. `docs/SCVB_CONTRACT.md` —— §1 正文新增条目(建议编号 §1.35)+ §5.6 拒绝态扩到 11 值 +
   §7 manifest 的 `output.functions` 加名(**计数断言 34 → 35**;若 `setMasterChartMode`
   先转正则为 35 → 36,`scripts/check-bridge-parity.mjs` 的 `EXPECTED` 表同改);
2. `src/output/OutputBridgeApi.h` —— 加 `kFnExportSuggestions` 常量;
3. `web/shared/bridge.js` —— 把名字从 `PENDING_FUNCS` **挪进** `BRIDGE_FUNCTIONS.output`
   (留在 `PENDING_FUNCS` 里等于让它绕过 parity 门禁,那才是真正的洞);
4. Output JUCE 层 —— 保存对话框 + 用 `scvb::suggest::buildRows/toCsv` 落盘;
   `ExportInput` 的三样输入(`CrvsData`、每轨 `{label, source_channels}`、每版本每轨 width)
   都已在 Output 侧手边,不需要新数据面;
5. `docs/SCVB_CONTRACT.md` §9.2 —— 若 native 评审对 `scope` 形状或三个新 reason 有异议,
   按 §9.0 流程改名(冻结前改名零成本)。

**不需要**改宪法:`params-v0.md` 的参数表与 state 形状一字未动,`ipc-contract-v0.md` 同理。
`abi` **不递增**(桥面是纯进程内 JS↔C++ 接口,既不改段布局也不改 state chunk,契约 §9.0 第 3 条明文)。

## 兼容性影响

- **既有工程**:无影响。不读不写工程数据以外的东西,不新增/不改任何持久化字段。
- **既有 DAW 自动化**:无影响。参数面 123 个一字未动;导出**只读**参数值,零 gesture。
- **新旧版本互通**:无破坏。旧版本不认识这个桥函数 = 桥上没这个名字 = UI 侧导出钮 disabled,
  与今天的真 JUCE 宿主是同一形态。
- **实时线程**:无影响。`[M]` 消息线程执行,`processBlock` 不参与。
- **用户数据**:CSV 里含轨名(用户数据)与建议值,**不含**任何路径、GUID 或机器信息。

## 审批

- PR 挂 **`status/frozen-contract`** 标签(仓库若尚无该 label,则在 PR 描述首行以文字标注),
  由用户明确批准后合入(仓 `CLAUDE.md` §5 / 06 §3.7)。
- 本 PR 的 diff **不触碰**四份冻结文档本体,故 branch-gate 的「冻结契约 path guard」不会被触发;
  本文件是**提前**立的变更说明 —— native 侧改 `docs/SCVB_CONTRACT.md` 时那个 PR 直接引用本文件即可。
