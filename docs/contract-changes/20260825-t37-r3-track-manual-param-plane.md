# 契约变更说明 —— 20260825-t37-r3-track-manual-param-plane

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] **docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)—— §1.16 `setTrackManual` 的写入面与 gesture 口径**

**参数集合未变**:123 参数的 id / 顺序 / 值域 / 默认值一个字节都没动,`tests/golden/params_v0.tsv` 不受影响。
变的是「`setTrackManual` 这个桥函数会不会去写参数面」这一条行为约定。

## 变更内容

`setTrackManual(ch, panOrVol, value)` 在写入常值段(曲线真身)之后,**追加**把同一个值写进
当前激活版本对应的 `v{v}_t{t:02d}_pan` / `_vol` 参数,并用
`beginChangeGesture()` / `endChangeGesture()` 包住。

与现行契约文字的冲突,逐条列明:

| 契约原文 | 冲突点 |
|---|---|
| §1.16「与段编辑同属**曲线真身操作**:**零 gesture**、入撤销栈」 | 现在会写参数面,且带 gesture |
| §1.13「轨 `pan`/`vol` 手动值 …… **不经参数通道**;…… **UI 不直写**」 | 「UI 不直写」仍成立(UI 没变,仍只调 `setTrackManual`);但 native 侧确实经参数通道落了值 |

## 为什么必须改(不改就等于这个功能不存在)

用户 v4 实测:**手动改音量/pan 先是有效,稍后完全无效**。链条:

1. 取值仲裁 `DspArbiter::processBlock` 对**冻结**维度读的是 `rawPan`/`rawVol`(host 参数),
   **不读曲线**(`DspArbiter.cpp:111-119`);
2. 打印器对冻结车道也只是把参数自己的当前值重写成平直线(#68),**不采样曲线**
   (`AutomationPrinter.cpp:262-273`);
3. 而 UI 在「手动写回成功」之后会**自动把该维度的 freeze 位置 1**
   (`tab-tracks.js`「用户裁定 2026-08-20:拖动 = 接管手动」)。

于是:第一次手动改能生效(此时 freeze 还是 0,曲线权威)→ UI 随即置 1 →
**之后每一次手动改都只写进了一个没人读的地方**。而旋钮照样跟手,因为读回值取自段表
(`rowFromStore` 刻意不按 freeze 分叉)—— 所以现象是「看起来改了、听起来没改」。

契约自身其实指向这个修法:§1.16 把本函数定义为「**冻结(freeze 对应位=1)时的手动静态值**」,
`PARAMETERS.md` 把冻结维度定义为「引擎不驱动、**host/手动**权威」。手动值要当权威,
就必须落在冻结维度真正会去读的那个平面上。

## 三条附带裁定

### 裁定① 撤销只回滚曲线,不回滚参数

`setTrackManual` 的撤销栈事务(`commitCrvsTransaction`)只快照/还原 CRVS 段表。撤销之后
参数面仍停在手动值上,于是**冻结维度上撤销在听感上无效**(要等下一次分析或手动改才会变)。

不把参数写进 undo 事务是有意的:参数面是宿主的自动化面,插件自己的 UndoManager 去回滚它
会与宿主的撤销栈打架(§0.9 已明确「自动化参数不入插件 UndoManager」)。
**代价记在这里,不静默**;若将来要让撤销也回滚参数,应走宿主 gesture 而非插件 undo。

### 裁定② `setValueNotifyingHost` 必须包 `beginChangeGesture` / `endChangeGesture`

裸写在宿主看来是一次没有起止的孤立写入:Cubase 这类宿主要么把它记成一个孤立自动化点、
要么在自动化 Read 档下当场把值顶回去(**那样这条修复根本不生效**)。包上 gesture 才是一次
完整的「用户编辑」。这与 §1.16 的「零 gesture」字面冲突 —— 以本裁定为准。

注:「零 gesture」原本的用意是「不要为手动值制造一串连续的自动化写入」。现行实现仍然满足
那个用意:UI 侧是**松手才发一次**(`MANUAL_COMMIT_MS` 300ms 防抖),一次编辑 = 一对 begin/end。

### 裁定③ §1.13 的「UI 不直写」不变

UI 仍然只调 `setTrackManual`,从不对 `v{v}_t{t:02d}_pan/_vol` 调 `setParam` ——
§1.13 防回环的那条纪律原样成立。变的只是 native 侧在处理 `setTrackManual` 时会落一次参数。

## 兼容性影响

- **参数面**:参数集合/布局/值域未动,`params_v0.tsv` golden 不变,ABI 不变。
- **宿主自动化**:冻结维度上手动改会在宿主自动化车道留下一次带 gesture 的写入 —— 这正是期望行为
  (用户在冻结轨上拖旋钮,本就该被宿主录到)。
- **工程 state**:无新字段。曲线真身与参数面本来就都在持久化范围内。
- **旧构建**:桥函数签名与回执形状未变(`{ok, replacedSegments, replacedLocked}`),旧 web 不受影响。

## 待办(合入前)

- [ ] `docs/SCVB_CONTRACT.md` §1.16 的「语义」与「撤销」两行按本文件改写(去掉「零 gesture」、
      补「同时落当前激活版本的 pan/vol 参数(带 gesture)」与裁定①的撤销代价说明);
      §1.13 的「不在本通道」行补一句「native 侧 `setTrackManual` 会落参数,UI 侧纪律不变」。
      —— **待用户批准后再改契约本体**(§9.0:冻结文档本体由统筹在批准后统一回填)。

## 审批

- [ ] **用户批准:待批准**(PR #87 已挂 `status/frozen-contract`)
- [x] 复审依据:PR #87 自动复审 R3 —— 「代码不必回退,但必须补变更文档走 §5,并把两个附带裁定
      写进去」;裁定②的 gesture 缺失同轮被点名为「Cubase 上可能落孤立自动化点**或**这条修复
      根本不生效」,已在本 PR 一并修掉(不只是文档)。
