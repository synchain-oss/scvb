# 契约变更说明 —— 20260831-j95-3a-release-apply

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] **docs/SCVB_CONTRACT.md —— §1.18 `setVadParams` / §1.19 `setSegmentation` 的「撤销」行,以及 §0.9 撤销表的归位**

**参数集合、IPC 布局、state schema、桥函数签名与事件载荷一个字节都没动。** §2.8 的 `reason`
十值枚举**本来就含 `vad` / `segmentation`**(连同 `manifest` 的 `segmentsReason` 与
`scripts/check-bridge-parity.mjs` 的白名单),所以事件面**无需任何改动** —— 本卡是把
**已经承诺的文字做实**,不是新增语义。

## 变更内容

| | 改前 | 改后 |
|---|---|---|
| §1.18「撤销」行 | 否(阈值本身不入栈;其触发的段改写属分析产物) | **是**。阈值本身仍不入栈;它在松手档触发的那一次**重分段 = 一条**撤销步(沿 [J89]:与 `analyze` 同栈、同口径)。撤销 = 恢复重分段前的段表;重做 = 重放重分段结果。撤销/重做各经 `scvb.segments`(`reason:"undo"` / `"redo"`)回推全量段表(§2.8,枚举与行为均不变) |
| §1.19「撤销」行 | 否 | **同上**(§1.19 的两段式与抑制条件本就「同 §1.18」) |
| §0.9 撤销表 | `setVadParams` / `setSegmentation` 在**右列**(不入栈) | 移到**左列**并带限定语:「`setVadParams`/`setSegmentation`(**仅其松手档触发的重分段**,[J95③a])」 |

§0.9 这一处是上面两行的**一致性后果**,不是独立政策:严格说来入栈的不是这两个**函数**
(阈值本身仍然不入栈,和其他 config 写一样),而是它们触发的那一次重分段。之所以仍然移到
左列而不是留在右列加注:左列是「**Ctrl+Z 会撤到什么**」的检索面,而用户按下 Ctrl+Z 时撤掉的
确实就是这一下 —— 留在右列会让人在左列找不到它。限定语把「函数 vs 它触发的产物」这层区别说清。

## 为什么要改

**这不是新政策,是 [J89] 之后的一处内部不一致修回。**

§1.18 现有那行的括注原文是「其触发的段改写**属分析产物**」—— 它把这一格的定性挂在
「分析产物」上。而 [J89](SL-209,2026-08-28 用户批准)已经把**分析产物改判为可撤销**。
于是同一份产物,经「点分析」进来可撤销、经「拖滑杆松手」进来不可撤销 —— 同一个东西两套
规矩,而用户并不知道自己刚才那一下算哪一种。

用户 2026-08-31 裁定(J95③a):两条都改「是」。

## 实现:复用 analyze 那条路,不另造

松手防抖跑的就是**同一条流水线**,因此三件事自动成立,不需要为本卡另写合并逻辑:

- **一条撤销步** —— 沿用 `commitCrvsTransaction(undo, crvsData_, …)`:它快照的是**整个
  `CrvsData`**(2 版本 × 15 轨),整轮合并循环在同一个 mutator 里跑完;
- **用户段逐字节不动** —— `applyAnalysisSegments` 在 `clearManual=false` 时的保留判据就是
  「`locked` 或 `origin != Auto`」,与 §1.18 承诺的「仅改写 `origin=auto` 且未 `locked` 的段」
  ([J34])逐字同义。**防抖这条路恒传 `clearManual=false`**;
- **不碰特征采集** —— 流水线是纯函数、只吃 `startAnalysis` 那一次拷出来的特征快照,全程不写
  `FrameStore`(唯一的写是 vadP 后验回灌,那是采集面元数据、且**留在事务之外**,与 SL-240 同口径)。

抑制条件按 §1.18 现有文字照做(**只有** PRINT 态或分析进行中,[J47]):布防时不排、到点再核一次。

## 连带做实:§1.18/§1.19 承诺的「含 diff 摘要」

两节的语义行都写着完成后回发的 `scvb.segments`「**含 diff 摘要**」。核下来 native 的
`buildSegmentsPayload` 里那个 `diff` 块是**硬编码的桩**(`kept:0` / `changed:[]` /
`added:0` / `removed:0`),从来没算过 —— 而 Tab3 的 A-02 摘要条正是对
`reason ∈ {vad, segmentation, analyze}` 读它。

只接流水线不算 diff 的话,用户会从「拖了没效果」变成「有效果但摘要说改了 0 段」,而
**本卡的立卡理由恰恰就是「承诺没兑现」** —— 那等于换一种方式再违约一次。故本卡一并算,
**连带把 `analyze` 那边同样的零摘要一起修好**(它是同一个桩)。

这**不是契约文字变更**:字段早已在 §2.8 载荷里定义,只是没填。

### 判同口径(写进用例注释)

`changed[]` 的条目形状由 UI 唯一决定(`renderDiffItems`):`{ch, segIdx, panFrom, panTo,
volDbFrom, volDbTo}` —— 它**只渲染 pan/volDb 的前后值**。所以:

- **配对**:old / new 两张段表按**时间重叠最大**的那一条配对。不用 `(t0,t1)` 全等 ——
  重分段本来就会挪边界,全等配对会把几乎所有段判成「删一条 + 加一条」,摘要就没信息量了;
  重叠配对表达的是「这一段音频」,与用户看波形时的直觉一致。
- **changed**:配对上、且 pan 或 volDb **按显示精度(1 位小数)**不同 —— 判据与用户屏幕上
  看到的数字对齐,亚显示精度的浮点抖动不算改动。`segIdx` 取**新表**的下标(UI 显示 `segIdx+1`
  = 「第 N 段」,指的就是现在这张表的第几段)。
- **added / removed**:新表里没配上的条数 / 旧表里没配上的条数。
- **kept**:⚠ **不是**「没改动的条数」。§2.8 的字段纪律逐字写着「`diff.kept` = 本次
  **手动编辑/锁定段已保留**计数(05 词条 `wave.diffKept` 的 `{k}`)」,词条原文是
  「{k} 处手动编辑/锁定段已保留」。所以它数的是新表里 `locked || origin != Auto` 的段数 ——
  回答的是「你手改过的那些段这次动没动」。**这一格我第一版实现成了「配对上且未改动」,
  读契约字段纪律时才发现是两回事**,已改正并单独立了一条用例守着(把它改回错的写法即红)。
  顺带一提 preview 的 mock 一直是对的(它用的就是 `manualKept`)—— 这次是 native 要向它看齐。
  **复审后再收一次口**:还要按**本轮分析范围**(半开区间相交)与**本轮真参与分析的轨**
  过滤 —— `previewAnalysis` 的 `manualKept` 自 [SL-193] 起就是这么算的,不过滤就是把
  SL-193 修掉的「A-07 与 A-02 两个 {k} 对不上」从另一侧再造一遍。
- `origin` / `locked` **不进 changed 判据**:UI 不渲染它们;而且按 [J34] 这条路根本不动
  用户段,它们不可能变。

## 影响面:web 消费者两处必须同步改(否则契约改了等于没改)

§0.9 与 §1.18/§1.19 的这三行不是纯文字 —— web 侧有两处**按 reason 分支**的消费者,
不跟着改就是「契约改一列、行为原地不动」:

| 位置 | 改前 | 为什么必须改 |
|---|---|---|
| `web/output/tab-master.js` 的 `UNDOABLE_REASONS` | `{edit, trackManual, copyVersion, analyze}` | §0.9 左列已含这两个函数(其松手档重分段),而白名单不含 `vad`/`segmentation` ⇒ 重分段完成发的事件走 `historyAfterSegments` 的 `return cur` ⇒ **undo 钮不置亮**,而键盘 Ctrl+Z 那条路不看按钮属性 —— 两个入口行为分叉。该文件 `:273-275` 的注释**逐字预告过**这次漏改:「契约改一列,这里就得改一行」 |
| `web/output/tab-settings.js` 的「结果陈旧」基线 | `if (seg.reason === "analyze")` | 松手档跑的是**同一条完整流水线**、用的是**当前 `loudness_mode`**,产出与「点分析」同质。只认 `analyze` 的话,拖完滑杆松手后基线不同步、派生 stale 不归零 ——「参数已改、结果陈旧」会一直挂着,而结果其实已经是新的。这条不一致是**本卡新引入**的(此前这条路根本不存在) |

两处都按「与 `analyze` 逐字同款」处理。特别地,undo 侧沿用 `analyze` 那条既有取舍
(**只置亮 undo、不清 redo**):段表事件里没有「这一轮到底压没压步」的证据,而
`finishAnalysis` 对恒等的那一轮确实不压步 —— 照着清 redo 会造出**不可自愈的假灰**
(灰掉的钮点不动,拿不到能救回它的 `ok:false`/`ok:true` 回执)。

## 兼容性影响

- **撤销栈**:多了一类事务(名字与 analyze 区分,便于用户在栈里认出来)。栈是插件内运行时结构,
  **不持久化**,无迁移问题;预算沿用既有 `kCrvsUndoBudgetBytes` = 64 MiB / 最少 16 步。
- **既有工程 / 既有 DAW 自动化 / 新旧版本互通**:无影响。abi 不变,`CFGS` 布局未动,不加迁移函数。
- **行为可感知的变化**:拖完 VAD/分段滑杆松手之后,Ctrl+Z 现在会撤掉那一次重分段(此前
  Ctrl+Z 会跳过它、撤到更早的一步)。这正是本次裁定要的效果。
- **撤销步不再是「每跑一轮就压一条」,而是「段表真变了才压」**:判据是新旧段表**五个字段
  全等**(`segmentsIdentical`),不是「diff 是否全空」—— 后者只比 pan/volDb,「边界挪了而
  两个值没变」的重分段 diff 全空但段表真变了,拿它当判据会漏压、用户撤不回来。
  这条守卫把旧的「空转分析(15 轨零产出)不压步」判据整个包住(零产出 ⇒ 恒等 ⇒ 不压步,
  行为逐字不变),同时挡住松手补发重排跑出的那一次恒等重分段。

## 复审后的四处修正(第 3 轮,head `c461dfe`)

| # | 问题 | 改法 |
|---|---|---|
| ① | 触发档记在成员上,而**取消**那条路不经过 `finishAnalysis`(代号不符即整份丢弃)⇒「松手重分段 → 取消 → 再点分析」会让后面那次**点分析**以 `reason:"vad"` 发出 | reason **随作业走**(进 `PendingAnalysis`),代号一丢它跟着丢。host 加一例在飞行中取消、断下一次点分析仍是 `analyze` |
| ② | `armResegment` 排在 `if (changed)` 内,与 mock 分叉 | 提到 `if (changed)` **之外**。契约 §1.18 的措辞是「UI **停止调用**后」—— 防抖的是**调用流**不是变化流;mock 的 `debounceAnalysisPipeline` 也是每次调用无条件重排。漏掉的是最常见的一种手势:「拖到某值 → 停手挑一会儿 → 再松手」,尾包与上次逐字相同 ⇒ 不重排 ⇒ 松手时挂上的倒计时条等不到事件、2s 兜底静默撤掉 |
| ③ | `diff.kept` 未按范围 / 轨筛,与 A-07 预览行的 `manualKept` 对不上 | 范围维在 `diffTrackInto` 里按半开区间筛(判据与 `previewAnalysis` 逐字同款、用的是同一对量化边界);轨维由调用方按「本轮**真参与分析**的轨」(mask ∩ enabled ∩ 范围内有覆盖)筛。`added/removed/changed` 不受影响 |
| ④ | 到点 PRINT 复检读的是**上一拍**的 mode(`tickResegmentDebounce` 排在 printer 三态求值之前),40ms 竞态窗 | 把 `tickResegmentDebounce(now)` 挪到同一回调内三态求值**之后** |

同批处理的小项:`changed[]` 封顶 200 条(全量重分段可能跨过显示精度数以千计,而 UI 是
逐条拼 `<li>` 的无封顶列表;**`kept`/`added`/`removed` 仍是精确总数,被截断的只有明细表** ——
不另加「还有多少条」的字段,那属契约变更);配对由逐对枚举改为「按 t0 有序 + 单调游标」的
线性扫(本函数对 15 轨全程持 `lifecycleMutex_` 跑在消息线程上);撤销步事务名在松手档落
`"Resegment"`(与「点分析」的 `"Analyze"` 区分,兑现上面兼容性段的承诺);
`sameAtDisplayPrecision` 加 `isfinite` 守卫(MSVC 上 `lround(NaN)` 实测与 `lround(0)` 相等,
不加守卫会把 NaN 判成「与 0 相同」)。

### 与 mock 仍存在的两处分叉(登记,不在本卡改)

- **full-dump 的 diff 形状**:native 对 `{analyze, vad, segmentation}` 之外的 reason 一律填全 0;
  mock 对 `snapshot`/`versionActive` 填 `{kept: manualKept, added: total}`。UI 只在三个分析
  reason 时读 `diff`,当前无可见影响。
- **防抖的范围口径**:native 的 `tickResegmentDebounce` 走 `analyzeAllRange`(尊重
  `manual`/`daw_loop` 的用户范围,`AnalyzeScopeMath.h` 头注说明这是有意取舍);mock 恒 ±∞
  全时间线。两者在 `follow` 档下等价。

## 审批

挂 `status/frozen-contract` 标签;用户明确批准后合入。
