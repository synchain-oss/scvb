# 长链端到端覆盖矩阵(SL-231)

> 状态:活文档。**新增或改动任何跨进程/跨线程长链时,连同本表一起改。**
> 立卡背景:04 §4.5 的指纹链在 v5.6 之前**零端到端护栏**,于是 J87「布防替用户开的采集态泄漏进工程」
> 那个洞一路睡到用户手里才被发现(#146 定谳)。本表是为了让「这条链有没有人从头走到尾」这个问题
> 有个能查的地方,而不是每次都重新 grep 一遍 8000 行测试。

## 0. 本表的口径

**什么算「e2e 用例」**:一条用例同时满足三条才记进「e2e」列 ——

1. **真链路,不 mock**:两端是真 processor / 真共享内存段 / 真消息循环,不是 in-process 模拟,也不是
   手搓输入结构体喂给中间那一层;
2. **判据落在链末端的可观测量**上(段表 / 参数面 / 覆盖率 / 段字节 / 宿主回调 / 只读方读回的帧),
   **不看被测物自己的内部计数器**;
3. 有**反向验证** —— 把链中段掐断(或换成一个已知会坏的配置)时,同一条断言必须变红。
   第 3 条不是形式要求:`tests/core/test_viz_plane.cpp:456-460` 记着教训 —— 第一版反向验证
   没加抖动,于是它测不出自己要防的东西,**一条测不出自己要防的东西的断言等于没有**。

只满足「每一跳各有单测」不算 e2e。四族历史 bug(T37)全部是**跳与跳之间从未接线**,而每一跳的
单测当时都是绿的。

**引用纪律**:本表引生产代码一律用**符号名**(`ScvbOutputAudioProcessor::publishVizFrame()`、
`isKnownCtrlOp`),引用例一律用**全名**,都不写行号。理由是本表自己踩过 —— 初版按行号引,
本 PR 合并一次 `feature/v1` 就烂了五处;而本表的全部价值是「不用每次重 grep 8000 行」,
一张行号已烂的表比没有表更费事。`HOST P0-1` 这类前缀在本仓有 7 条同名开头的用例,
只有全名能消歧。

**三个 harness 各自的能耐**(决定一条链只能在哪里测):

| harness | 目标 | 能做什么 | 够不着什么 |
|---|---|---|---|
| `tests/host/`(`scvb_host_tests`) | 免 DAW 宿主 harness | 单进程托管**真 Output + 真 Input**,经**真 Win32 段**通信,手推 `processBlock` + 真跑 25Hz 消息循环 | **桥面 handler**(不编 `*PluginEntry.cpp`,见 `tests/CMakeLists.txt:239-242`);跨进程 |
| `tests/ipc/`(`scvb_ipc_tests`) | IPC 契约 | **真起子进程 peer**(`tests/tools/scvb_ipc_peer.cpp` 13 个角色),真强杀,跨进程一致性/生命周期 | 真 processor —— peer 手搓输入结构体 |
| `tests/core/test_monitor_harness.cpp`(`scvb_monitor_tests`) | Monitor 全链 | **真 Monitor processor** + 跨进程 viz peer | 真 Output(写方是 peer) |
| `web-preview/tests/*.mjs` | 页面冒烟 | 真页面 + mock 桥 | 真段、真 native |

三个 harness 都跑**真实命名共享内存段**,段名只按**组号**区分、不带 run-id,所以**两个人(或两个
agent)同时跑就会互相假红**,而且假红点常常与本次改动毫不相干(实测:一次没取锁的 `gates` 把
`VIZ-2`「viz 段不存在时只读方拿到 kFailed」打红,而那次改动根本没碰 viz —— 并发实例留下的活段让
「段不存在」这个前提不成立)。

因此**跑之前必须取一把机器级互斥锁**(`gates.ps1` 的任何档位都算,gate 6 = ctest 会把三个套件
一起跑掉)。锁的**位置是每台开发机自己的约定、不入库**;协议形式是「`mkdir` 型目录锁 + 锁目录内
`owner.txt`(持有者 + ISO 时间戳 + PID)」:

- `mkdir` 成功后**立刻**写 `owner.txt` —— mkdir 型锁天然是空目录,「空 = 孤儿」这条启发式必然
  误判**正在持锁**的人;
- 判孤儿的唯一合法依据是 `owner.txt` 的时间戳**超过 30 分钟**;
- 释放前**核对 `owner.txt` 是自己**再 `rmdir`,绝不无条件删。

---

## 1. 覆盖矩阵

图例:**✅ e2e** = 有满足 §0 三条的用例;**🟡 单测** = 只有这一跳的单测/假段/手搓输入;
**❌ 零** = 没有任何用例走过这一跳。

### 链 A —— 采集记账链

`Input[A] 音频 → FeatRing → (feat 段) → Output[M] pull → ChannelFrames → FrameStore/CoverageMap → 桥面`

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| A1 | Input[A] → FeatRing 写(run 协议、`base_hop` 先于 `write_hop`) | ✅ | `IPC-14 §J33 交错注入`;并发安全 `test_feat_ring_concurrency.cpp:39` |
| A2 | feat 段跨进程 | ✅ | `IPC-14 §跨进程特征环 writer→reader coverage 一致`(真 peer) |
| A3 | Output[M] `pullIncremental` → `ChannelFrames` | ✅ | `IPC-14 §w 单调性防御` / `§受限追赶分拍追平` |
| A4 | → `FrameStore` / `CoverageMap` | ✅ | `IPC-14 §coverage 无洞`;负向 `§停顿中切 run 旧积压跳过` |
| A5 | 布防门控两维(时间 × 轨掩码) | ✅ | `HOST N1`×2、`HOST J87②`(波形逐列对拍)、`HOST J87②b`、`HOST J87 tracksMask 只点保留位` |
| A6 | → 桥面可观测量(`coverageOf`/`waveformOf`) | ✅ | `HOST P0-4`、`HOST SL-206`×3 |
| **整链** | **A1→A6** | **✅ e2e** | **`HOST L-6c:开采集 → 播放 → coverage 真实落账`**(真 Rig、真段、判据 = `coverageOf`;反向 = `clearCoverage` 后归零) |

### 链 B —— ctrl 命令环

`Input[M] enqueue → (ctrl 段 SPSC) → Output[M] dequeue → 落 state → 广播区 → Input 读回`

op 全集(`docs/IPC_CONTRACT.md:149`,v1 冻结)= `{kSetPriority, kFpReport}`;`kNone` 是哨兵,
`isKnownCtrlOp`(`src/core/ipc/CtrlPlane.h`)只认这三个。**两个真 op 各有一条 e2e**,
不存在「某个 op 从没端到端跑过」的缺口。

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| B1 | 生产方 `enqueue`(含非活跃实例拒写) | ✅ | `T30 conflict 实例不得写 ctrl 命令环`(反向:第二生产者);`IPC-13`(跨进程满环) |
| B2 | ctrl 环语义(满环丢最旧 / seq 由 write_pos 派生 / 多 ch 独立) | ✅ | `IPC-13`;`test_ipc_lifecycle.cpp:765`(双线程 10 万条无撕裂)、`:1017`、`:987` |
| B3 | Output dequeue → 落 state | ✅ | `HOST L-4b` |
| B4 | Output → ctrl 广播区(seqlock) | ✅ | `HOST L-4a`;布局护栏 `T37-C 广播区落在预算内` |
| B5 | Input 读广播 → 桥面快照 | ✅ | `HOST L-4a`、`HOST I3`(孤儿 Input 的 `participate` 判据) |
| **op `kSetPriority`** | **B1→B5 闭环** | **✅ e2e** | **`HOST L-4b:Input 远程改优先级 → 落到 Output 的 state 并广播回来`** |
| **op `kFpReport`** | 见链 D | **✅ e2e** | **`HOST SL-225`**(见下) |

### 链 C —— viz 发布链

`Output 装配 → VizPublisher 降采样/分频 → (viz 段 seqlock) → Monitor read → 桥面 → 页面`

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| C1a | 真 Output 装配 `VizPublishInput` —— **已断言的字段**:轨名 `toStdString`、`leadMask`、`stereoMask`、`enabledMask`(落段为 `onlineMask`)、`widthPct`(取**活动版本**的参数句柄)、`versionActive`、`playheadSnapshot`、车道(经 `activeCurves`) | **✅ e2e(本卡新增)** | **`HOST SL-231:真 Output 的配置与曲线经 viz 段发布,只读方逐字段读回`** —— 本卡之前是 **❌ 零**,详见 §2 |
| C1b | 装配里**未被直接断言**的部分:`metaRevision`(轨名 FNV,只驱动车道重算、不落段;间接体现为「改了轨名之后车道与 label 确实刷新了」)、`coveredMask`/`trackColor`/`volDb`/`panNow` | 🟡 间接 | `metaRevision` 的分频/重算语义由 `test_viz_plane.cpp:386` 覆盖(假段);其余字段由 `VIZ-1`/`VIZ-4`/`MON-CHAIN` 在 **peer 写方**一侧覆盖,**不经过真 Output 的装配** |
| C2 | `VizPublisher` 降采样 / 车道重算 / 30Hz 分频闸 | 🟡 单测 | `test_viz_plane.cpp:318/386/493/520`(真 `VizPublisher`,**假段**;含最完整的反向验证范式) |
| C3 | viz 段跨进程 + 生命周期 | ✅ | `VIZ-1`/`VIZ-3`/`VIZ-4`(真 peer);反向 `VIZ-2`(段不存在拿空态、绝不建段) |
| C4 | Monitor attach + read + 陈旧判定 | ✅ | `MON-CHAIN`×3(真 Monitor processor + 跨进程 peer;含「段还在但写方停摆 → 不假装在线」反向) |
| C5 | Monitor 桥面 → 页面 | 🟡 mock | `web-preview/tests/smoke-monitor-page.mjs`(真页面 + **mock 桥**,不经真段) |

**C1a 此前为什么是零**:C2/C3/C4 的写方**全是手搓 `VizPublishInput` 的 peer 或裸 `VizPublisher`**
(`scvb_ipc_peer.cpp` 的 `viz-writer`/`viz-publisher` 两个角色)。真 Output 的
`ScvbOutputAudioProcessor::publishVizFrame()` 那段装配没有任何用例走过 ——
装配错一位(`leadMask` 取错字段、`label` 差一个下标、`widthPct` 的**版本下标错一**)段里就是错值,
Monitor 画的就是错图,而 C2/C3/C4 照样全绿。这正是 T37「数据面从未接线」那一族的形状。

⚠ **本行只认「真的被断言到」的字段**(C1a/C1b 因此分开列)。列了字段却不断言,会让这张表本身
失去可信度 —— 后来人查到 ✅ 就不会再补,而那一格其实是空的。这条纪律由 PR #155 复审【重要】②
立下:初版把 `stereoMask`/`enabledMask`/`widthPct`/`metaRevision` 全列进 C1,用例却一条都没断言;
`versionActive` 那条更甚 —— 两侧结构体默认值都是 1、Rig 又恒在版本 1,**把装配里那一行整行删掉
断言照样绿**。现已补齐断言,并在用例里先 `setVersionActive(2)` 让它真的有牙齿。

**仍留的缺口(不在本卡范围)**:C5 只有 mock 桥冒烟;「真段 → 真页面」全链需要页面侧能连真 native,
属 web 冒烟体系的能力边界,记账在此。

### 链 D —— 指纹链(04 §4.5)

`Input[A] kw → tile FNV → Input[M] drain/pack → (ctrl kFpReport) → Output drainCtrl → FingerprintWatch → 基线重算 + 滞回 + 门槛 → 上游改动 ⚠`

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| D1 | tile 指纹量化 + FNV 累加 | 🟡 单测 | `test_feature_fingerprint.cpp:84/101` |
| D2 | Input[M] `drainFpReports` + `packFpReport` | ✅ | `IPC-13b`(打包往返 + u64 宽度);`test_ipc_lifecycle.cpp:384` |
| D3 | ctrl 环传输 | ✅ | 同 B2 |
| D4 | Output → `FingerprintWatch` | ✅ | 见整链 |
| D5 | 基线重算 / 滞回 / 10% 门槛 / 自愈 / 无基线不计分母 | 🟡 单测(**很厚**) | `test_feature_fingerprint.cpp` 共 17 条,含 6 条反向 |
| D6 | → `captureStale`(桥面 ⚠) | ✅ | 见整链 |
| **整链** | **D1→D6** | **✅ e2e** | **`HOST SL-225:常规采集(未布防)下上游改动仍须翻出 stale`** + 布防/撤防三个 SECTION + **`HOST SL-225:布防替用户开的采集不得被存进工程`**(指纹链 × state 往返链交叉,#146 定谳落点)。含一条负向断言并紧跟一条正向防空转 |

### 链 E —— 自动化打印链

`曲线真身/CRVS → activeCurves → 权威仲裁 → AutomationPrinter 求值 → setValueNotifyingHost → APVTS → 宿主 gesture`

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| E1 | CRVS → `activeCurves` 曲线对象 | ✅ | `HOST SL-202`×3(曲线指针非空 + `panAt()` 采样) |
| E2 | 权威仲裁的 gesture **决策** | 🟡 单测 | `test_authority.cpp:105-230`(对 `AuthorityMode` 返回的 `beginGesture`/`endAllGestures` 标志位,纯结构体) |
| E3 | 打印器逐车道求值 → `setValueNotifyingHost` | ✅ | `HOST P0-1:分析产物经打印器写进宿主参数(自动化非零写入)`(本文件有 7 条以 `HOST P0-1` 开头的用例,故引全名) |
| E4 | → APVTS 参数值 | ✅ | `HOST P1-4`、`HOST J85`×2、`HOST SL-187`(自写不得被记成 hostEcho) |
| E5 | 打印器 → 宿主 gesture 事件(begin/end 成对) | 🟡 单测 | `test_printer.cpp` PRINTER-GUARD 系列:真 JUCE 参数 + `CountingListener::gestureBegins`,但驱动的是**独立 fixture**(`printer.setMode()` + `printer.tick()` 直驱) |
| E5e | **真 processor 的权威仲裁 → 打印器 → 宿主 gesture** | **✅ e2e(本卡新增)** | **`HOST SL-231:打印器的 gesture 真的到达宿主且 begin/end 成对`** —— 本卡之前是 **❌ 零**,详见 §2 |
| E6 | 参数 → DSP(反向:host 参数是权威时) | ✅ | `HOST P1-D`×3(判据 = 总线电平比);`HOST SL-189`(四种配置对拍) |

**E5e 此前为什么是零(以及 E5 并不是零)**:先把话说准 —— 「打印器开不开 gesture」**有单测**,
`test_printer.cpp` 的 PRINTER-GUARD 系列早就挂了真 JUCE 参数监听器
(`CountingListener::gestureBegins`,`tests/core/test_printer.cpp:214` 起)。缺的不是这一跳本身,
而是**把它和上游仲裁串起来的那一段**:那些用例直接 `printer.setMode(...)` + `printer.tick()`
驱动一个独立的 `AutomationPrinter` fixture,从不经过 `ScvbOutputAudioProcessor` 自己的档位仲裁
(`outputEnabled_ && playing && inRange` → Print/Armed/Follow,见 `ScvbOutputAudioProcessor::timerCallback` 里 `printer_.setMode(mode)` 之前那段)、
车道绑定(`bindVersion`/`setCurves`)与 25Hz 真驱动。

于是这一族会整族漏过去:**仲裁算错档 → 打印器根本没进 Print → 宿主自动化车道整条是空的**,
而 E2(决策标志位)、E3/E4(参数值)、E5(fixture 直驱)三处的单测**全部照样绿**。
漏了会怎样:裸 `setValueNotifyingHost` 在 Cubase 这类宿主看来是一次没有起止的孤立写入,
要么被记成孤立自动化点,要么在 Read 档下当场把值顶回去(那样写入根本不生效)——
参数值断言全绿,用户那边自动化车道却是空的。这条真机现象记在
`docs/contract-changes/20260826-j85-freeze-param-plane.md`。

### 链 F —— state 往返链

`getStateInformation → chunk 编码 → 容器 TLV(abi/未知 chunk 保留)→ setStateInformation → 各面恢复`

| # | 跳 | 覆盖 | 用例 |
|---|---|---|---|
| F1 | 各 chunk 编解码 | 🟡 单测 | `test_state_codec.cpp`、`test_state_features_roundtrip.cpp`、`test_input_state.cpp` |
| F2 | 容器 TLV / abi / 未知 chunk 原样回写 / 早退点顺序 | ✅ | `HOST UICF`;`HOST SL-226` 高 codecVer 两条(payload 逐字节) |
| F3 | 缺 chunk / 坏 chunk 不得当成「清空」 | ✅ | `HOST SL-217`×4(缺 CRVS / 解码失败 / 正常对照 / gesture 中途取 state) |
| F4a | 段表面恢复 | ✅ | `HOST SL-202:state 往返必须保住段表` |
| F4b | 特征面恢复(FEAT/sidecar) | ✅ | `HOST SL-226`×7(波形瓦片逐列对拍 + 3 条反向) |
| F4c | GUID / UI 档位 / 布防运行时态 | ✅ | `HOST SL-215`、`HOST SL-208`、`HOST J87 工程恢复复位布防运行时态`(十个字段先弄脏再回读) |
| **整链** | **F1→F4** | **✅ e2e** | 上述多条;最完整的一条 = **`HOST SL-226:采集特征随工程往返,重开后泳道波形仍在`**(真 Rig 析构重建 + 瓦片逐列对拍 + 三条反向) |

---

## 2. 本卡补的两条护栏

| 链 | 用例 | 真链路 | 末端判据 | 反向验证 |
|---|---|---|---|---|
| **C1a** | `HOST SL-231:真 Output 的配置与曲线经 viz 段发布,只读方逐字段读回` | 真 `Rig`(Output+Input)+ 真 Win32 viz 段;写方是**真 Output 的 `publishVizFrame()`**,不是 peer | 只读方 `attachReadOnly()` + `read()` 读回的帧:`sampleRate`、`versionActive`(用例先 `setVersionActive(2)` —— 否则两侧结构体默认值都是 1、这条恒绿)、`label[ch]` 逐字节(UTF-8)、`leadMask` 对位、`widthPct` 断到**具体数值**(先把版本 2 的 width 设 42,默认是 100)、`stereoMask`(单向蕴含,理由见反向③)、`onlineMask`、车道非全哨兵、`playheadSamples` 随走带推进、`later->seq > frame->seq`(发布器在持续发帧) | ① **对照轨**(除关掉 `enabled` 外一字未改)必须是空 label + `leadMask`/`stereoMask`/`onlineMask` 三位全清 + 车道全哨兵 —— 挡住「装配把同一份值填满所有轨」;② **邻组** `attachReadOnly()` 必须失败 —— 挡住「publisher 发错组、断言靠残段恰好成立」;③ **实跑注入过两种断链**:`leadMask` 装配失效 → 红;`rawTrkW[0][ch]`(**版本下标错一**)→ `widthPct` 红(`10000 == 4200`)。`stereoMask` 用单向蕴含而非等式:`source_channels` 是最终一致回填的(实测单独跑本用例停在 1、跟 `[analyze]` 一起跑是 2),硬写等式会把护栏变成时序炸弹;错位方向由对照轨那一位兜住 |
| **E5e** | `HOST SL-231:打印器的 gesture 真的到达宿主且 begin/end 成对` | 真 `MonoMultiRig` + 真采集 + 真分析 + 真仲裁 + 真打印;宿主替身 = 挂在**真 processor** 上的 `juce::AudioProcessorListener` | 监听器收到的 `audioProcessorParameterChangeGestureBegin/End`:`begins > 0`、同一参数不重复 begin、无孤儿 end;关输出后 `open` 集合清空、`ends == begins` 且不再新开 | **关掉输出(Follow 档)时 `begins == 0`**。⚠ 这一段能成立有个前提:`outputEnabled_` 的**成员初始化值就是 `true`**(`ScvbOutputAudioProcessor` 里 `bool outputEnabled_ = true;`),分析一出段表、走带一进区间打印器当场就进 Print —— 所以用例必须**先显式关输出再挂监听器**,否则 gesture 早开着(begin 幂等),「重新 begin」的计数恒为 0,反向段会假绿 |

两条都刻意**不看被测物的内部计数器**(`VizPublisher::publishCount()` / `AutomationPrinter::numGesturesOpen()`)——
那两个变绿只说明代码跑到了,不说明数据真的出现在链末端。

---

## 3. 盘点中发现、**本卡未处理**的三处(留给后来人)

1. **`C5`:Monitor 桥面 → 页面只有 mock 桥冒烟**。`smoke-monitor-page.mjs` 跑的是真页面 + mock 桥,
   真段驱动真页面这一跳没有任何护栏。属 web 冒烟体系的能力边界,不是一条用例能补上的。
2. **`tests/support/peer_spawn.h:6-10` 的待办未清**:`tests/ipc/test_ipc_contract.cpp:60-274` 至今
   保留一份 200 余行的功能等价副本(当初为避开 IPC-16 修复那一路的合并冲突),而 IPC-16 早已合入。
   合并时要给 `peer_spawn.h` 补一个只有本地副本才有的 `csvU64Hex()`。
3. **`tests/ipc/` 的防串扰靠人工静态分配 channel/group 号**(ch1..ch15、g1/g2/g3 已排满),
   只在 `IPC-12b`/`IPC-17` 两处调了 `resetRegistry()`。新增跨进程用例若复用已占号段,
   复现的会是 `test_ipc_contract.cpp:485-489` 注释里那类「陈旧 geometry → 哈希偶发漂移」的 flake,
   而不是可读的失败。扩这批用例前建议先补一个「按用例名派生段后缀」的段名工具。

**顺带澄清一条不必做的事**:`scvb_host_tests` / `scvb_ipc_tests` / `scvb_monitor_tests` 在
`tests/CMakeLists.txt` 里既无 `RUN_SERIAL` 也无 `RESOURCE_LOCK`,乍看是缺口 —— 但
`gates.ps1` 调 `ctest` 时不带 `-j`,**同一次 ctest 内本来就是串行**,加 `RESOURCE_LOCK` 今天不解决
任何问题。真正会撞的是**两个 agent 各跑一次 ctest**,那是进程外的事,只能靠
`.ipc-test-lock` 文件锁挡,CMake 层管不着。
