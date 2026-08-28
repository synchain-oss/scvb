# 契约变更说明 —— 20260827-sl209-analyze-undoable

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] **docs/SCVB_CONTRACT.md —— §1.6 `analyze()` 的「撤销」行**(如需,连带 §0.9 撤销表)

**参数集合、IPC 布局、state schema、桥函数签名与事件载荷一个字节都没动。** 变的只是
「分析产物进不进插件撤销栈」这一条行为约定。

## 变更内容

| | 改前 | 改后 |
|---|---|---|
| §1.6「撤销」行 | 否(分析产物变更不入撤销栈) | **是**。一次分析(全量 / 选区 / 单轨重新识别)= **一条**撤销步:提交前把整个 CRVS 快照压进**既有** UndoManager(与段编辑同栈)。撤销 = 恢复分析前的段表;重做 = 重放分析结果。撤销/重做各经 `scvb.segments`(`reason:"undo"` / `"redo"`)回推全量段表(§2.8,枚举与行为均不变) |

`docs/SCVB_CONTRACT.md` §0.9 的撤销表若逐条列了「哪些操作入栈」,需同批把 `analyze` 登记进左列
(用户 2026-08-28 批准后**已在同一 PR 内回填**,见下「审批」栏;提交变更文档时尚未获批,故当时按卡片指令「写变更文档停下报告」先只交文档。)

## 为什么要改(用户新需求)

分析是本产品**最重的一次性写入**:一次点击就重排整条(或选区内)人声的声像与音量。此前它是
工具里唯一「不可撤销」的破坏性操作 —— 用户对结果不满意时,唯一的回退手段是「再分析一次并期待
不同结果」(而分析是确定性的,期待落空),或者手工把每一段改回去。段编辑、手动接管、复制版本
这些**小得多**的操作反而都可撤销,轻重倒置。

## 实现与「分析不覆盖 user 段」如何共存

**共存是自动成立的,不需要另算「实际被替换面」。**

`commitCrvsTransaction` 快照的是**整个 `CrvsData`**(两个版本 × 15 轨),undo 原样还原 ——
那份快照里本来就含着这一轮会被保留的用户段(ADR-008)与范围外 auto 段(§4.4 局部重分析)。
所以撤销恢复的是「**分析前的那一刻**」,与这一轮实际替换了哪几段无关:

- 用户段在分析前就在表里 → 撤销后仍在(harness 用例 2 钉死);
- 范围外 auto 段分析时本就没动 → 撤销后一致(用例 3 钉死);
- 局部重分析改的面更小,快照口径不变。

一次分析压**一条**步:`beginNewTransaction("Analyze")` 起新事务,整轮合并循环在同一个 mutator
里跑完(实现上把原 `finishAnalysis` 的内联循环抽成 `applyAnalysisSegments`,只为把它整个塞进
mutator,逻辑逐字未改)。

## 旧注里的两条顾虑,逐条回应

原实现在 `finishAnalysis` 里写着「**不走** `commitCrvsTransaction`」,给了两条理由:

1. **「它会把分析产物压进 undo」** —— 那正是本卡要的结果,不再是反对理由。
2. **「Ctrl+Z 会以 `reason:"undo"` 重发段表,和分析完成的 `reason:"analyze"` 打架,UI 的分析态
   更难对齐」** —— 不成立。两者本就是**两次不同的事件**:`analyze` 在分析回落时发一次,`undo`
   在用户按 Ctrl+Z 时发一次,`reason` 各自如实描述自己那一次,正是 §2.8 的枚举语义。UI 的分析态
   由 `analysis_run`(§2.1)驱动,不看段表 `reason`;看 `reason` 的两处(Tab4「参数已改、结果陈旧」
   基线同步、Tab3 分析 diff 摘要条)只认 `analyze`,撤销时本来就**不该**触发它们。

## 兼容性影响

- **撤销栈**:多了一类事务。栈本身是插件内的运行时结构,不持久化,无迁移问题。
- **内存**:每次分析多留两份 `CrvsData` 快照(与 `editSegment` / `setTrackManual` 同量级);
  上限由本卡新定的 64 MiB 预算 + 最少 16 步兜住(见下「撤销预算的定参与推导」)。
- **state / 参数面 / IPC**:未触碰。
- **UI**:撤销/重做按钮的可用性判据(`tab-master.js` 的 `historyAfterSegments`,由回执与
  §2.8 `reason` 推)是**白名单**语义 —— 它认的是 `UNDOABLE_REASONS` 这张表里有没有这个
  `reason`,**不是**「有没有发过段表事件」。所以 §0.9 改判**必须同批改 web 一侧**:
  `web/output/tab-master.js` 的 `UNDOABLE_REASONS` 加 `"analyze"`,
  `web-preview/tests/smoke-undo-redo.mjs` 把 `analyze` 从「右列不入栈」组挪到左列组
  (期望 `{undo:true, redo:false}`)。
  ⚠ 本文件初版在这里写的是「天然把 `analyze` 算进去」—— **那句是错的**,已更正:
  不改白名单的后果是分析完成后 undo 钮不置亮(用户此前空点过一次撤销把它置灰之后,
  它会一直灰着),而 `app.js` 的 Ctrl+Z 不看按钮属性照样能撤 —— 鼠标与键盘两个入口行为分叉;
  反向也错:新事务入栈会清空 redo 栈,redo 钮却仍亮着(点一下拿 `ok:false` 才自愈)。
- **撤销深度(本卡显式定参,见下节)**:`CrvsTransactionAction::getSizeInUnits()` 从「恒 1」
  改成按段数记字节后,`juce::UndoManager` 的**默认**预算(30000 units / 最少 30 步)会让真实
  工程的撤销深度塌到 30 步,且内存并没有被真的按字节封住。本卡在 `OutputAuthority` 显式定参,
  推导见 `SegmentEditService.h` 的 `kCrvsUndoBudgetBytes` 注释。
- **已知取舍(不回退的两处 UI 摘要)**:撤销分析之后,Tab4「参数已改、结果陈旧」基线与 Tab3 的
  分析 diff 摘要条**不会跟着回退**(它们只认 `reason:"analyze"`,撤销发的是 `reason:"undo"`)。
  于是段表已经回到分析前,摘要条还在描述那一轮被撤销的分析。**判定为可接受**:这两处描述的是
  「最后一次分析这个动作发生过什么」,不是「当前段表长什么样」;要让它们回退就得给撤销/重做也
  带上「这一步跨越了哪一次分析」的信息,那是 §2.8 载荷的扩张,超出本卡范围。记录在此,不静默。
- **旧构建**:桥函数签名与事件形状未变,新旧 web 都不受影响。

## 撤销预算的定参与推导(#152 复审【重要】①②)

分析事务与段编辑走同一个 `CrvsTransactionAction`,而本卡把它的 `getSizeInUnits()` 从「恒 1」
改成按段数记字节(1 unit ≡ 1 字节)。这一改与 `juce::UndoManager` 的**默认**参数
(`maxNumberOfUnitsToKeep = 30000`,`minimumTransactionsToKeep = 30`)组合出副作用,故显式定参。

**先把 JUCE 的裁剪语义核准**(`juce_UndoManager.cpp`,8.0.8 逐行核对):

```cpp
while (nextIndex > 0 && totalUnitsStored > maxNumUnitsToKeep
                     && transactions.size() > minimumTransactionsToKeep)
    { 丢掉栈底那条 }
```

三个条件是**与**,`minimumTransactionsToKeep` 优先于 units 上限。由此:

- ✅ **单条超限事务永远不会被丢**(栈里事务数没超过 min 时循环根本不进)。所以复审里
  「超限动作被丢 / 整个撤销历史被清空 ⇒ 分析撤销静默失效」这个判断**与源码不符**,已按源码更正:
  「一次分析 = 一条撤销步」在任何段数下都成立。
- ⚠ 真正的后果是**深度**与**内存**:默认参数下,两份快照总段数 ≳ 938 就一口吃光 30000 units,
  于是撤销深度恒 = `minimumTransactionsToKeep` = 30 步;而这 30 条大快照照样全留着
  (密集工程一条 ≈ 3.8 MB ⇒ ≈ 115 MB),内存**并没有**被按字节封住。

**定参**(`src/output/SegmentEditService.h`,生产装配点 = `OutputAuthority` 的构造):

```cpp
inline constexpr int kCrvsUndoBudgetBytes     = 64 * 1024 * 1024; // 64 MiB
inline constexpr int kCrvsUndoMinTransactions = 16;
```

推导:一条事务 ≈ `(旧段数 + 新段数) × 32B`,稳态下 ≈ `64·N`(N = 2 版本 × 15 轨的总段数)。

| 工程档位 | N | 一步开销 | 64 MiB 买到的深度 | 默认预算下的深度 |
|---|---|---|---|---|
| 典型(200 段/轨/版本) | 6,000 | 384 KB | ≈ **174 步** | 30 步(内存无封顶) |
| 密集(2,000 段/轨/版本) | 60,000 | 3.84 MB | ≈ **17 步** | 30 步(≈ 115 MB 挂栈上) |

- 64 MiB 的量级依据:插件内存大头是 FrameStore 的特征页(一小时素材 × 15 轨为百 MB 级),
  撤销栈压在它下面一个档位 —— 既不成为内存主项,又留得下上百步编辑历史。
- 最少 16 步是**硬地板**(JUCE 的 min 优先于 units);取 16 而非更大,是为了让地板在密集档上
  不越过预算:16 × 3.84 MB ≈ 61 MB ≲ 64 MiB,与该档算出的 ≈17 步同量级,两个数不打架。
- 这是拿**深度**换一个**真的存在**的内存天花板:密集工程从「30 步 / 内存无上限」变成
  「≥16 步 / ≤64 MiB 量级」,典型工程则从 30 步涨到 ≈174 步。

**用例**(`SERVICE-12`,`tests/core/test_segment_edit_service.cpp`),三支各带反向验证:

1. 2000 段工程连压 64 条事务 → 64 次撤销全成功(实测深度 524)。
   ★ 反向:删掉 `configureCrvsUndoBudget(undo)` → 实测退回 **30 步**,红。
2. 生产装配点:`OutputAuthority` 出厂即带预算(同样 64/64)。
   ★ 反向:删掉构造里那行 → 实测退回 **30 步**,红。
3. 封顶咬合:连压 700 条后深度被裁到 524 < 700,且 ≥ 硬地板 —— 证明预算不是「调大到形同虚设」。

## 机器门禁

- `branch-gate` 冻结契约 path guard:本 PR **触碰** `docs/SCVB_CONTRACT.md`(§1.6 撤销行 + §0.9 撤销表,
  共两行),本文件即 guard 要求的、与改动同 PR 的变更文档 —— 满足 CLAUDE.md §5。
- `node scripts/check-bridge-parity.mjs`:名字集合与 `returns` 未变,照常绿。

## 审批

- [x] **用户批准:已批准(2026-08-28,用户,[J89] 入册)** —— 两项一并批准:
      ① §1.6「撤销」行改判(否 → 是);② §0.9 撤销表左列登记 `analyze`。
- [x] 契约本体已按批准回填(同 PR,§5「变更文档与改动放在同一个 PR 里」):
      `docs/SCVB_CONTRACT.md` §1.6「撤销」行 + §0.9 撤销表两处。
- [x] **③ §0.9 表内两处文字披露(随同批 J85/J89 语义补全,不含新语义)**:
      ① 左列 `setTrackManual` 补「仅未冻结的手动接管通道([J85])」—— [J85] 已裁定冻结通道
      不产生 CRVS 事务、不入撤销栈(见 `20260826-j85-freeze-param-plane.md` 裁定①),
      该表此前未跟进,属**已批语义的文字缺口**;
      ② `previewAnalyze` / `cancelAnalyze` 留在右列并注明「只读干跑 / 取消,不改段表」,
      防止读者把它们与 `analyze` 一并误移到左列。
      两处都**不引入新裁定**,只把既有裁定在这张表上写全 —— 单列在此供批准时一并过目。
- [x] 代码与用例已就位(harness `HOST SL-209` 三例);**批准前代码与契约文字不一致**,这一点
      在 `finishAnalysis` 的注释里也显式记着,不静默。
