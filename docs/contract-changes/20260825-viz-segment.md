# 契约变更说明 —— 20260825-viz-segment

> **状态:待用户批准。** masterPlan 宪法 `constitution/ipc-contract-v0.md` 的 **ipc v1.6 修宪由统筹执行**;
> 本文件是 scvb 仓侧的**变更真源**。合并 + 批准后,再由后续统一把本节转正进 `docs/IPC_CONTRACT.md` 本体
> (本 PR **不改** `docs/IPC_CONTRACT.md` 本体)。
>
> 卡:**T44**(`plan/07-execution-plan.md` §T44)/ 裁决:**J75**(`plan/adjudications.md`)/
> 规格真源:`plan/05-ui-spec.md` 文末「J75 增补规格」节 C。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **新增一个段**,既有四类段(registry/audio/feat/ctrl)一字未动
- [ ] docs/STATE_SCHEMA.md(state schema)
- [x] tests/golden/(golden 快照)—— `tests/golden/ipc-layout.txt` 追加 viz 段的段名/结构体/偏移/常量行

## 变更内容

新增第五类共享内存段 **`Local\SynchainSCVB.v1.g{G}.viz`**(G = 1..8,每组一份,跨组互不可见),
为 **SCVB Monitor**(T45,只读监视插件)提供跨进程只读数据面。

- **唯一写方** = Output 的**消息线程 [M]**,4 Hz 低频发布。
- **唯一读方** = Monitor 的消息线程,**只读 attach**(`FILE_MAP_READ`),不 claim、无 ctrl 写权、一个字节都不写。
- **`processBlock`(音频线程 [A])对本段零写入** —— 见下「零写入的落实方式」。

### 段名

`SynchainSCVB.v1.g{G}.viz`(逻辑名,不带 OS 前缀;Win32 全名加 `Local\`)。
`SegmentKind` 新增 `kViz`;mac 长度复核:最长 `/SynchainSCVB.v1.g8.viz` = 23 字符 ≤ 31(PSHMNAMLEN),余量充足。

### 段布局(总 **64 KB**,全部区块 64 字节对齐)

| 偏移 | 大小 | 区块 | 说明 |
|---|---|---|---|
| 0 | 64 | `VizHeader` | magic/abi 先行 + generation + 几何字段 |
| 64 | 128 | `VizFrame` | seqlock 帧头:序号 + playhead + 循环区 + 窗口 + 掩码 |
| 192 | 64 | `VizTrackColors` | 15 个轨色索引(u32) |
| 256 | 1920 | `VizCoverage` | 分段 activity 位图,15 轨 × 1024 位 |
| 2176 | 30720 | `VizLanes` | 降采样 pan 车道,15 轨 × 1024 × int16 |
| 32896 | 128 | `VizTrackState` | 每轨**当前值**:panNow / volDb / widthPct(定点 int16) |
| 33024 | 512 | `VizTrackLabels` | 每轨 32 字节 UTF-8 轨名 |
| 33536 | 32000 | (预留) | 尾部留白,后续增补不改既有偏移 |

**`VizHeader`(64 B,cacheline 0)**

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | `atomic<u32>` | `magic` | `kScvbMagic`;初始化完成标志,release-store 后行 |
| 4 | `atomic<u32>` | `abi` | `kScvbAbi`(= 1,挂总 abi,见下) |
| 8 | `atomic<u32>` | `generation` | 覆盖式重初始化 +1 |
| 12 | `u32` | `column_count` | = 1024(几何,magic 发布前写定) |
| 16 | `u32` | `track_count` | = 15 |
| 20 | `u32` | `pan_scale` | = 100 |
| 24 | `u32[10]` | `_reserved` | 填满 cacheline 0 |

**`VizFrame`(128 B;seqlock 临界区起点)**

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | `atomic<u32>` | `seq` | 偶 = 稳定,奇 = 写入中 |
| 4 | `atomic<u32>` | `playhead_flags` | bit0 playing / bit1 looping / bit2 loopValid |
| 8 | `atomic<u64>` | `publish_ms` | 发布时刻(`steadyNowMs`;读侧据此判写方停摆) |
| 16 | `atomic<u64>` | `window_start_samples` | 降采样窗口起点(v1 恒 0 = 工程起点) |
| 24 | `atomic<u64>` | `window_span_samples` | 窗口跨度(0 = 未 prepare / 无有效窗口) |
| 32 | `atomic<i64>` | `playhead_samples` | −1 = 无时间线 |
| 40 | `atomic<i64>` | `loop_start_samples` | −1 = 无效 |
| 48 | `atomic<i64>` | `loop_end_samples` | −1 = 无效 |
| 56 | `atomic<u32>` | `sample_rate` | 0 = 未 prepare |
| 60 | `atomic<u32>` | `version_active` | 1 \| 2(车道取自哪个版本) |
| 64 | `atomic<u32>` | `playhead_epoch` | 时间线跳变代数 |
| 68 | `atomic<u32>` | `track_online_mask` | bit{N−1} = 该轨启用 |
| 72 | `atomic<u32>` | `track_covered_mask` | bit{N−1} = 该轨有分段 |
| 76 | `atomic<u32>` | `track_stereo_mask` | bit{N−1} = 该轨立体声源 |
| 80 | `atomic<u32>` | `lane_revision` | 车道/位图内容版本;只在重算车道时 +1 |
| 84 | `atomic<u32>` | `track_lead_mask` | bit{N−1} = 该轨 `lead_lock`(分布图柱顶绿帽,同 Tab1 规格) |
| 88 | `u32[10]` | `_reserved` | |

**`VizTrackColors`(64 B)**:`atomic<u32> index[15]` + 4 B pad。值 = 调色板槽位(1..15;0 = 未指定)。
v1 恒等于轨号 —— web 侧 `--track-color-1..15`「顺序即轨号」(T43)。字段先落段,将来若引入 native 侧
重映射,读写两侧无需改布局。

**`VizCoverage`(1920 B)**:`atomic<u32> bits[15][32]`。每轨 1024 位,**LSB 优先**(列 i → `bits[i/32]` 的 `bit(i%32)`;
web 侧 `word = i>>>5, bit = i&31` 同源)。位序写反的失败形态很阴——**图照画**,只是断线位置整体错开 32 的倍数,
肉眼几乎看不出来,故 L1 用例按 bit 0/31/32 与整字 `0xFFFFFFFF` 逐个钉死。

**`VizTrackState`(128 B)**:`atomic<int16_t> panNow[16] / volDb[16] / widthPct[16]` + 16 保留(索引 15 空置,
只为让每个数组按 32 字节对齐)。定点标度同 pan(×100):panNow ∈ [−100,100]、volDb ∈ [−24,12] dB、
widthPct ∈ [0,100]。哨兵 `−32768` = 无数据。

⚠ **口径**:`panNow` 是**播放头精确时刻**的曲线求值,**不是** pan 车道在播放头所在列的采样——
后者是列中心点采样(列宽 = span/1024)。分布图要的是「此刻」,轨迹图的线才走车道。

**`VizTrackLabels`(512 B)**:`atomic<u32> utf8[15][8]`,即每轨 **32 字节 UTF-8**,NUL 补齐。
超长按 **UTF-8 字符边界**截断到 ≤31 字节——绝不切出半个多字节序列(半个汉字到了 web 侧就是一个替换字符)。

**`VizLanes`(30720 B)**:`atomic<int16_t> pan[15][1024]`。定点 = `round(clamp(pan, −100, +100) × 100)`,
即 ±10000;哨兵 **`−32768`(`kVizPanNone`)** = 该轨整条无数据。

所有跨进程字段一律 `std::atomic`(ipc §0),含 15×1024 的车道元素 —— relaxed 存取在 x86 上即普通 `mov`,
零额外开销,同时消除 seqlock 载荷的形式化数据竞争。`is_always_lock_free` + `sizeof` + `offsetof`
静态断言见 `src/core/ipc/VizPlane.h` 文末;`tests/golden/ipc-layout.txt` 逐行冻结。

### 降采样口径(冻结语义,读写两侧必须一致)

- 窗口 = `[window_start, window_start + window_span)`,均分 **1024** 列;第 i 列覆盖
  `[start + i·span/N, start + (i+1)·span/N)`。
- **车道值** `pan[t][i]` = 该轨曲线在**列中心时刻**的求值(**点采样**,不是列内均值),
  数据源 = 引擎同一个 `CurveEvaluator` —— 与打印同源(05 J75 节 A「与引擎打印 30 条同源」)。
- **覆盖位图** `coverage[t][i]` = 该列时间区间与该轨任一 CRVS 分段**有交集**即置 1(保守口径:
  短于一列的分段不会消失)。
- **断线渲染以位图为准**:曲线求值本身会填补空隙(`CurveEvaluator` 的 hold/外推语义),
  只有分段表才是覆盖真源。整轨无分段 → 车道全哨兵 + 位图全 0。
- **窗口跨度** = `max(最大分段末端, playhead+1, 60 s)` 向上取整到 **30 s** 边界,上限 24 h。
  量化是为了让跨度在播放推进中保持稳定,车道无须逐帧重算。CRVS 里 `t1 = 1<<40` 的「无末端」哨兵
  (`setTrackManual` 的「真末端由宿主时间线提供」)只以其 `t0` 参与跨度计算,在位图上一路覆盖到窗口末端。

### 发布节拍

- 帧头标量(playhead / 循环区 / 掩码 / 时刻)与 **`VizTrackState` 每轨当前值**:**250 ms(4 Hz)**,
  与既有 4 Hz 心跳闸门同款。当前值不受车道分频影响——它们是「此刻」。
- 车道 + 位图 + 轨色(15×1024 次曲线求值):**按需重算** —— CRVS 修订变化 / 活动版本切换 /
  窗口跨度变化 / **轨名**变化(`metaRevision`,FNV-1a 64 位)四者之一触发,外加 **30 s 兜底**。
  稳态下 4 Hz 只写帧头 + 每轨当前值(共 256 字节)。轨名随车道块一起落段。

  兜底是给 `metaRevision` 哈希碰撞留的后路,不是常态开销——一次重算是 15 360 次曲线求值,
  早期设成 1 s 等于把它变成每秒都做。**width 不进 `metaRevision`**:它走帧头段、每帧都刷,
  与车道块无关;掺进去会让「width 被自动化」变成每秒 15 360 次求值。

### 一致性机制

整帧 **seqlock**:写方 `seq` 先 +1 变奇 → 写载荷 → 再 +1 变偶;读方读前后两次 `seq`,
奇数或不相等即重试(上限 8 次),超限返回 false 由调用方沿用上帧。**读侧全程无锁、无写**。
协议与进程内的 `scvb::engine::PlayheadShot` 同款,区别只在跨进程 + 载荷 atomic 化。

### 零写入的落实方式(不只是注释)

1. `VizPlane::open()` 记下调用线程为 owner([M]);`publish()` 若发现自己跑在别的线程上,
   **一个字节都不写**,只累加 `foreignThreadWrites()`。误从 `processBlock` 接线时计数会非零。
2. 只读 attach(`attachReadOnly()`)状态下 `publish()` 是彻底 no-op —— Monitor 侧的零写入在类型外再兜一层。
3. `attachReadOnly()` 走 `openExistingReadOnly` + `checkHeaderReadOnly`(只读触碰 + 单次 acquire 读),
   **绝不 memset、绝不覆盖式重初始化** —— 覆盖分支仅限创建者角色。
4. 断言:`tests/core/test_viz_plane.cpp` 的「非 owner 线程 publish 零写入」「只读 attach 方 publish 不写任何字节」两例。

### 映射生命周期:viz **不用** `SegmentHandle` 的租约 + 宽限期

Registry/CtrlPlane/AudioRing 用 `SegmentHandle`(引用计数租约 + 500 ms 宽限期)的**唯一理由**是
「音频线程可能仍持有裸指针」。viz 段没有任何音频线程访问,全部存取都在同一条消息线程上,
那套机制在这里不但没用,还有两个实害:

1. **读方 release 后仍把段吊住** —— 写方进程都退出了,段却因为读方的映射还在而不消失,
   读方永远等不到「空态」(Monitor 会一直显示一份僵尸数据);
2. **宽限期未满就析构** → `SegmentHandle` 退回「进程退出统一回收」= 每次换组泄漏一份映射。

故 `VizPlane` 直接持有 `SegmentView`,`release()` 立即 `unmap`。这条由
`tests/ipc/test_ipc_viz.cpp` 的 **VIZ-3**(写方退出 → 读方空态 → 再上线重连)钉死 ——
本条正是先写出 VIZ-3 才发现的,不是纸面推演。

## abi 策略:**挂总 abi**,本次**不 +1**

- `VizHeader` 的 `abi` 恒 = `kScvbAbi`(= 1),与既有各段同构,直接复用 `initHeader` /
  `checkHeaderReadOnly` 的既有机制,J40 的「拒连 + 横幅、绝不半兼容」行为免费得到。
- **本次新增段不触发 abi+1**:「加一个新段」不改任何既有段的一个字节,新旧进程对 registry/audio/feat/ctrl
  的互认完全不变。降级路径优雅 —— 旧版 Output 不建 viz 段 → Monitor `attachReadOnly()` 得 `kFailed`
  → 空态(而 abi 不符则得 `kAbiMismatch` → 拒连横幅,两者可区分)。
- **不设独立 `viz_abi`**:独立版本号会造出「registry 认、viz 不认」的半兼容态,正是 J40 要禁的。
- **viz 段自身**将来的布局改动与其余各段同规(ipc §5):**abi+1 且段名 v2**。
- 额外一道几何自检:段内 `column_count`/`track_count`/`pan_scale` 与读方编译期常量不一致时,
  `attachReadOnly()` 返回 `kAbiMismatch`(同 abi 下的几何漂移理论不应发生,按拒连处理,不半兼容)。

## 读写方约定

**唯一写方 = 本组 claim 到 OutputSlot 的那一个 Output**(`OutputClaimState::kActive`)。
同组第二个 Output 是 `kObserver`([J66]「同组内只读观察」),它**既不建段也不发布** ——
否则两个写方会同时推同一个 seqlock,读方拿到的帧可以是两次发布的拼接(`seq` 是偶数、内容却撕裂),
而且**看起来完全正常**。claim 态在接管/让位/改组时会翻转,故这条判定由 Output 的 [M] **每拍**重做:
成为 `kActive` 即建段,失去 `kActive` 立刻释放。

| | Output([M],**仅 kActive**) | Output([M],kObserver) | Monitor([M]) | 任何音频线程 [A] |
|---|---|---|---|---|
| 建段 | ✅ `open()`(create-or-open,owner) | ❌ 绝不创建 | ❌ 绝不创建 | ❌ |
| 写 | ✅ 仅 `publish()`,仅 owner 线程 | ❌ 不发布 | ❌ no-op | ❌ 零写入 |
| 读 | (自读不需要) | — | ✅ `read()` seqlock 一致性读 | ❌ |
| 生命周期 | 按 claim 态每拍裁决建/释放 | 失去 kActive 即释放 | attach 失败即空态,由 [M] 周期重试 | — |

> owner 线程在**首次 `publish()`** 时绑定,不是 `open()` 时 —— JUCE/VST3 不保证 `prepareToPlay`
> 与 `timerCallback` 同线程,绑错会让此后每次发布都被护栏静默挡掉、段永远全零。

Monitor 掉线、崩溃、或从不存在,对 Output 侧**零影响**(无握手、无引用计数、无等待)。
反之 Output 不在线时 Monitor `attachReadOnly()` 得 `kFailed`,显示空态,不崩溃、不重试风暴。

## 兼容性影响

- **既有工程**:零影响。viz 段不参与 state/CRVS/参数,不进任何持久化。
- **既有 DAW 自动化**:零影响。**参数面 123 一个不动**(J75:Monitor 0 自动化参数)。
- **新旧版本互通**:registry/audio/feat/ctrl 四段的 abi 与布局逐字节不变,新旧 Input/Output 互认不变。
  旧 Output + 新 Monitor → Monitor 空态;新 Output + 无 Monitor → 多建一个 64 KB 段,无其他影响。
- **内存**:每组 +64 KB(仅 Output 在线时创建)。

## 变更清单(本 PR 触碰的冻结面)

- `tests/golden/ipc-layout.txt`:追加 56 行(viz 常量 / 段名 g1+g8 / 5 个结构体的 size+align+field offset / 5 个区块偏移)
- `src/core/ipc/SegmentLayout.h`:`SegmentKind` 增 `kViz` + 段名分支(既有枚举值顺序不变,既有段名一字未动)

## 审批

- [ ] 用户批准(挂 `status/frozen-contract` 标签)
- [ ] 合并后:统筹执行 masterPlan `constitution/ipc-contract-v0.md` **v1.5 → v1.6** 修宪,
      并把本节转正进仓 `docs/IPC_CONTRACT.md` §5(新增 §6「viz 段」)
