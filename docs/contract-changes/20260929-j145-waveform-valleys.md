# 契约变更说明 —— 20260929-j145-waveform-valleys

> **状态:实质已批(随本 PR 挂 `status/frozen-contract`)。** 用户 2026-09-29 发布前 17 条拍板,
> 裁定 **J145**(#1 边界拖拽「吸附能量谷」→ **现在做功能**)。用户原话:「1和2现在都做代码功能,
> 不要什么都想着以后做」。统筹备注:「Output requestWaveform 真填 valleys[](复用 Segmentation 谷点检测),
> web snapBoundary 已读」。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:谷点现算现给,不落盘。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.27 `requestWaveform` 的「语义」格补一段 **`valleys[]` 的取值口径**,
      并写明 `passId` / `stale` 在 v1 恒 0。函数名 / 参数 / 返回字段名 / 字段类型 / 字段顺序 **零变化**。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`(J105 的做法):没有改名、删除、改参数顺序、收窄取值域。`valleys` 的类型
仍是 `f64[]`(秒、升序),此前契约只说它是「该区间内的能量谷时间点列表」、没有定义什么算一个谷;
native 侧一直回空数组。本变更**第一次定义**它,并把实现如实写进去。

## 变更内容

**一句话**:`requestWaveform` 回包里的 `valleys[]` 从「恒为空数组」变成真的谷点表;契约写明谷点怎么算。

### 为什么要改(缺陷形态)

- 波形页边界手柄的 tooltip(`wave.boundaryHandleTip` / `wave.boundarySnapTip`,三语)一直许诺
  「拖动时吸附到能量谷,按住 Alt 关闭」;web 侧 `snapBoundary` 与拖拽路径早就接好了,读的就是
  `requestWaveform.valleys[]`。
- 但 `OutputEditor::handleRequestWaveform` 回的 `valleys` 是一个**从不填**的空数组 ⇒ 真机上**从没吸附过**。
- mock(`web/shared/mock-data.js` 的 `makeWaveformTile`)一直给谷点(乐句间隙中点),所以 web 侧的冒烟一直是绿的。

### §1.27 语义格新增的口径(逐条对应实现)

| 条目 | 口径 | 落点 |
| --- | --- | --- |
| 数据面 | 只在该轨**已覆盖**的 hop 上、按覆盖段逐段检测;未覆盖不当静音,不跨空洞连成一个谷 | `src/core/analysis/WaveValleys.cpp` |
| ℓ | 帧响度 ℓ(02 §0.2),与 S1 谷切分同一条式子(`kwDbq → dequantizeKwMs → frameLoudnessDb`) | 同上 |
| 谷底 | 5 hop 平滑后的局部极小平坦区,代表点取平坦区中心 —— 与 `detectValleys` **共用**同一份平滑与判定(`findMinimumRuns`,从 `detectValleys` 原样抽出) | `src/core/analysis/Segmentation.cpp` |
| depth | **地形 prominence**:向两侧各走到第一个更低的点为止的最大值,取两侧较小者减谷底 | `detectSnapValleys` |
| 门槛 | depth > minDepth,minDepth 与 02 §3.2 候选谷同一条(随当前 `analysis.segmentation.sensitivity`,3–12 dB,默认 6 dB) | `OutputProcessor::waveformOf` 传 `runtime_.segmentationSensitivity` |
| 时刻 | 谷底 hop 的**中心** `(hop+0.5)·hop_s` | `WaveValleys.cpp` |
| 上下文 | 请求窗两侧各带 3 s 参与检测,回包只含落在 `[startS, endS)` 内的 | 同上(`kSnapValleyContextHops`) |
| 条数上限 | `cols`;超出时留 prominence 最大的那些,再按时间升序 | 同上 |
| 扫描上限 | 该轨在「请求窗 + 上下文」内的已覆盖 hop 超过 360000(1 h)⇒ 空数组(代价与跨度无关,P0-A 那一族的纪律) | 同上(`kSnapValleyMaxScanHops`) |
| `passId` / `stale` | v1 **恒 0**(§9.3 附注允许的首版回退常量);UI 的「不同采集轮次底色微差」因此不出现 | `OutputProcessor::waveformResponse` |

### 与「复用 Segmentation 谷点检测」的出入(报统筹)

统筹备注写「复用 Segmentation 谷点检测」。本 PR 复用了它的**平滑、谷底判定、minDepth 门槛与 ℓ 口径**,
但**没有**照搬 `detectValleys` 的 depth:那个 depth 的侧峰边界是「相邻局部极小」(02 §3.2 步骤 2a,S1 递归要的
「无自指」)。两句之间若是**带噪的底噪**而不是数字静音,平滑后仍有零点几 dB 的起伏,每个起伏都是一个局部极小
⇒ 按那个口径算 depth 只有零点几 dB,整段间隙一个谷都过不了门槛 —— 吸附在这类素材上等于没有。
`tests/core/test_segmentation.cpp` 的「[J145] 带噪间隙」一格把这件事钉成对照:同一份 ℓ 喂 `detectValleys`
零候选、喂 `detectSnapValleys` 恰一个谷(在间隙最低点)。S1 谷切分的**行为未改**(`detectValleys` 只是把
谷底判定原样抽成了共用函数,SEG-* 与流水线用例全绿)。

### 为什么把回包拼装挪进 processor

`OutputEditor.cpp` 依赖 WebView2,不在 host 套件的 TU 清单里 —— 拼装留在那边,「谷点有没有真的进回包」这一跳
离线永远测不到,而此前坏的恰恰就是这一跳。现在编辑器只剩一行 `c(ScvbOutputAudioProcessor::waveformResponse(
processor_.waveformOf(...)))`,拼装在 `OutputProcessor.cpp`(host 套件编得进来)。六列的取值与此前逐位相同。

## 验证

- **core**(`scvb_tests`,`[J145]`):`detectSnapValleys` 五格(矩形谷落中心 / 浅谷随灵敏度 / 段端不算谷 /
  带噪间隙对照 / 两谷升序);`snapValleysSeconds` 五格(已知静音间隙 ⇒ 13.255 s / 窗外不回 + 窄窗靠上下文 /
  未覆盖空洞不是静音 / 条数上限留最深 / 扫描上限)。
- **host**(`scvb_host_tests`,`[J145]`):Input 真采「响 · 静 · 响」⇒ `waveformOf` 恰一个谷点、落在静音段内、
  离中心 ≤ 60 ms(实测晚 27 ms,K 加权滤波器余振),回包 `valleys` 与之逐个相同;4.4 dB 浅谷只在灵敏度 100 下出现
  (钉 `runtime_.segmentationSensitivity` 那一行接线)。
- **页面级**(`web-preview/tests/smoke-valley-snap-page.mjs`,mock):拖到谷点旁 3 px ⇒ `move_boundary.tS` = 谷点、
  手柄 `data-snap=1`;按住 Alt ⇒ tS = 指针处裸时刻、`data-snap=0`。
- **删除式**:每个落点一格,注入 ⇒ 设计接住它的那条断言红,复原 ⇒ 绿(读数见 PR 描述)。

## 已知局限(复审提出,本 PR 不改)

- **谷点按受理那一刻的灵敏度算,UI 的块缓存不按灵敏度分键。** web 的 `waveSource` 以 `(ch, startS, endS, cols)`
  为键缓存瓦片,失效只来自 `clearCoverage` 与采集增量。所以若灵敏度变了而视口没动,吸附会沿用旧门槛算的谷点,
  直到下一次平移 / 缩放 / 新采集把这一块换掉。**v1 界面上没有改灵敏度的入口**:灵敏度滑杆按用户 2026-09-10 的裁定
  藏起来了(SL-382,`web/output/index.html` 的 `data-gb="wave-seg-sensitivity"` 挂 `hidden`,页面级冒烟钉着它
  不产生布局盒),MIN SEG 滑杆发 `setSegmentation` 时带的是回声缓存里的原值。唯一能在编辑器开着时改掉它的是
  **宿主重灌工程 state** —— 而那一幕里特征本身也换了,包络列有同一个缓存问题,不是本 PR 引入的。
  灵敏度滑杆重新上屏时,须同批让瓦片随灵敏度失效(`setSegmentation` 回执后对各轨 `waveSource.invalidate`)。
- **单次调用的代价**:上限处(该轨一小时的已覆盖素材)要逐 hop 算一次 ℓ 并跑两趟单调栈,全在消息线程、持
  `lifecycleMutex_`。实测(本机 Release,35.9 万 hop、5 次取最快,测时机器上还有别的构建在跑):**单次 27.9 ms**;
  按 hop 数线性外推,4 分钟的一轨约 2 ms。最坏的组合是「1 小时工程 × 15 轨 × 缩到全览」:视口块与概览块各一轮
  ≈ 30 次调用,谷点这一半合计约 0.8 s 消息线程时间(分散在 30 次回调里,每次约 28 ms)。同一次调用里包络那一段
  本来就逐 hop 做三次页索引查找(那一段本 PR 没有单独计时,不在这里给数)。**可选缓解(本 PR 未做)**:列宽远大于捕获半径时
  (例如 512 列的全曲概览块 —— 吸附从来不读它)直接不算谷点。

## 兼容性影响

- 工程 / state / 自动化 / IPC:零变化(谷点不落盘)。
- 桥面:字段与类型不变;此前恒空的数组现在有值。web 侧读法本来就按「秒、升序」写,零改动。
- 用户可感知:拖段边界会吸附到能量谷(按住 Alt 不吸);极远缩放(该轨一屏里已采集的素材超过 1 小时)时不吸附。

## 审批

- 用户 2026-09-29 裁定 J145(masterPlan `plan/adjudications.md`),原话见文首。
- 「depth 口径不照搬 `detectValleys`」一节是实现选择,已在 PR 描述里向统筹报告。
