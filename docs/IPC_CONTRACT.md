# IPC_CONTRACT —— SCVB 共享内存契约(冻结契约)

> 状态: 冻结
> 最后更新: 2026-08-25(J81 修宪转正:ctrl 广播区正式布局 + viz 段;内容依据 `docs/constitution/ipc-contract-v0.md` v1.6)
> 真源: 本文件(由 `docs/constitution/ipc-contract-v0.md` 蒸馏转正)

> ⛔ **本文件是冻结契约。** 修改前必读 `CONTRIBUTING.md` §8 与 `CLAUDE.md` §7。未经批准的改动 PR 会被直接关闭。

仲裁规则:`docs/constitution/` 只读副本是**修订源**(改动须走修宪流程);本文件是**实现/审查基准**(06 §3.4 review bot 比对对象)。两者分歧时,以已冻结实现代码与 `tests/golden/` 快照为准。

本文件是 IPC 冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。内容蒸馏自 `docs/constitution/ipc-contract-v0.md`(v1.6,含 J66 分组架构、J81 ctrl 广播区布局与 viz 段);段名/布局字段/对齐规则不得改。段名前缀 `SynchainSCVB.v1.` 与各 header 前两字段(magic/abi)永久冻结;布局改动必须 `abi+1` 且段名升 v2,新旧不互认。

## 0. 总则

- Windows 命名共享内存(CreateFileMapping,`Local\` 命名空间,同会话可见);macOS 后续用 POSIX shm 同布局
- 所有跨进程字段 `std::atomic`,static_assert lock-free + 布局(offsetof);缓存行 64B 对齐;ABI 版本字段先行
- 心跳双阈值(J10):各写入方每 ~250ms 更新自己的 heartbeat(steady clock ms);读取方 **>2000ms 视为陈旧**(UI 显示陈旧,按未连接处理);slot 接管/覆盖需 **≥5000ms 且 pid 存活探测失败**(双条件,保护 SPSC 前提)
- 无法枚举/删除残留段 → 固定段名 + 覆盖式重新初始化(带 generation 计数)

## 1. Registry 段:`Local\SynchainSCVB.v1.g{G}.registry`(4 KB;G=1..8,J66)

```c
// registry 段布局(4 KB;golden:InputSlot 起始 @64,OutputSlot 起始 @1024 = 64 + 15×64)
struct alignas(64) RegistryHeader {   // size 64(cacheline 0)
  atomic<u32> magic;                  // 0   = kScvbMagic('SCVB')
  atomic<u32> abi;                    // 4   = kScvbAbi(1)
  atomic<u32> generation;             // 8   覆盖式重初始化 +1
  u32 _pad;                           // 12
  u32 _reserved[12];                  // 16..64 填满 cacheline 0
};
struct alignas(64) InputSlot {        // size 64;×15(v1.4/J59),InputSlot0 @64
  atomic<u32> state;                  // 0   0=空闲 1=已声明 2=活跃
  u32 pid;                            // 4
  u32 sample_rate;                    // 8
  u32 max_block;                      // 12
  atomic<u64> heartbeat_ms;           // 16
  atomic<u64> capture_write_pos;      // 24  特征段已写到的时间线位置(样本)
  atomic<u32> flags;                  // 32  bit0 capturing / bit1 muted([J48],fetch_or/fetch_and)
  u32 _pad[7];                        // 36..64 填满 cacheline
};
struct alignas(64) OutputSlot {       // size 64;×1,OutputSlot0 @1024
  atomic<u32> state;                  // 0   0=空闲 1=已声明 2=活跃(主实例)
  u32 pid;                            // 4
  atomic<u64> heartbeat_ms;           // 8
  atomic<u32> connected_mask;         // 16  Output 视角:哪些 channel 数据健康(bit{N-1})
  atomic<u32> config_seq;             // 20  Output 配置版本号,Input UI 变化检测
  u32 _pad[10];                       // 24..64 填满 cacheline
};
```

- Input 实例 claim slot[channel_id-1]:CAS state 0→1;若已被占且心跳新鲜 → UI 报"channel 冲突";若心跳陈旧(≥5000ms 且 pid 存活探测失败,双阈值 J10)→ 覆盖
- 第二个 Output 实例:OutputSlot 已活跃 → 本实例进只读观察模式 + UI 警告(ADR-002;**同组内**语义,J66;不同组的 Output 各自独立为主实例)

## 2. 音频环段(每 channel 一个):`Local\SynchainSCVB.v1.g{G}.audio.ch{N}`(N=1..15,v1.4)

```c
struct AudioRingHeader {              // size 40 align 8
  atomic<u32> magic;                  // 0   = kScvbMagic
  atomic<u32> abi;                    // 4   = kScvbAbi
  u32 sample_rate;                    // 8
  u32 ring_frames;                    // 12  kDefaultRingFrames = 1<<19 = 524288 帧(≈10.9s @48k;mono/stereo 同帧数,字节数 ×channels)
  u32 channels;                       // 16  v1.4/J57:1=mono 2=stereo(prepareToPlay 写定,运行期不变)
  u32 _pad;                           // 20  对齐 8
  atomic<u64> write_head_samples;     // 24  时间线绝对样本位置:下一帧将写到的 timeline pos
  atomic<u64> epoch;                  // 32  时间线跳变(定位/循环回跳)时 +1,读方据此丢弃跨代数据
};
float32 ring[ring_frames*channels]; // v1.4/J57:channels=1|2,stereo 为 interleaved LR(ADR-003 v2.0),地址 = ((timeline_pos & (ring_frames-1)) * channels + c)
```

- 写(Input 音频线程):按块以 playhead `timeInSamples` 为地址写入;transport 非线性跳变 → epoch+1 后继续
- 读(Output 音频线程):按自身块的 [t0,t1) 读取;区间未被覆盖(write_head 落后或 epoch 不符)→ 该轨该块静音 + 失准计数(UI 警告)
- 单写单读 SPSC;时间线寻址天然容忍预测性引擎的提前写(ADR-002/D5)

## 3. 特征段(每 channel 一个):`Local\SynchainSCVB.v1.g{G}.feat.ch{N}`

```c
struct FeatHeader {                   // size 32 align 8
  atomic<u32> magic;                  // 0   = kScvbMagic
  atomic<u32> abi;                    // 4   = kScvbAbi
  u32 hop_ms;                         // 8   = kFeatHopMs(10)
  u32 capacity_hops;                  // 12  kFeatCapacityHops = 1<<17 = 131072(≈20 分钟)
  atomic<u64> base_hop;               // 16  环起始对应的时间线 hop 序号
  atomic<u64> write_hop;              // 24  已写到的 hop 序号(时间线寻址)
};
struct FeatFrame {                    // size 8 align 4
  f32 kw_ms;                          // 0   K-weighted mean-square
  f32 peak;                           // 4
};
FeatFrame ring[capacity_hops];
```

- 仅采集开关 ON 且播放时写;Output 在"分析"时按选区拉取快照并入 state(持久化归 Output,段只是运输)
- VAD 后验由 Output 离线从 kw_ms 计算(v1 能量域,ADR-008),特征段不存后验

## 4. 控制面:`Local\SynchainSCVB.v1.g{G}.ctrl`(16 KB;per-组各一份,J66)

- **ctrl 段布局落点(16 KB = kCtrlSegmentSize 16384)**:`CtrlHeader`@0(64B)→ 广播区@64(9344B)→ `OutputGlobalInfo`@9408(256B)→ 命令环 `CtrlRing`×15 @9664(每 channel 一条,各 448B = 64 头 + 16×24 记录);9664 + 15×448 = 16384 恰占满预算。

- **Output → Input 广播区**:当前 channels[15] 配置快照(v1.4)(label/priority/lead/pair;每轨 `auto_pan`/`auto_vol` 开关已删除,J65,由 freeze 自动化参数取代)+ `config_seq`;Input UI 轮询(25Hz Timer)显示,写操作通过命令环发回

- **广播区正式布局(v1.6/J81;落在 T06 冻结的预算内,不移动、不改写任何既有结构体)**

| 常量 | 值 | 变化 |
|---|---|---|
| `kCtrlBroadcastOffset` | 64 | 不变(T06 冻结) |
| `kCtrlBroadcastBytes` | 9344 | 不变(T06 冻结) |
| `sizeof(CtrlBroadcast)` | **2048** | 新增(落在 9344 预算内,余 7296 字节留给后续) |

```c
struct CtrlChannelConfig {         // size 32 align 4;每轨配置镜像
  u32 priority;                    // 0    0..10
  u32 flags;                       // 4    bit0 enabled / bit1 lead_lock / bit2 lead_vol_exempt / bit3 participate_in_auto_pan
  u32 pair_id;                     // 8    0=无配对,1..7
  u32 freeze;                      // 12   bit0=pan 冻结,bit1=vol 冻结(J65 同一参数两位)
  u32 source_channels;             // 16   1|2(Input 实测值,Output 回镜像给全组)
  u32 _reserved[3];                // 20..32
};
struct CtrlBroadcast {             // size 2048 align 64;广播区总布局
  atomic<u32> seq;                 // 0    seqlock:写前 +1(奇)→ 写载荷 → 写后 +1(偶)
  atomic<u32> config_seq;          // 4    广播区整体版本号;**从 1 起算**,0 保留给「本组无 Output 在广播」
  u32 lead_select;                 // 8    0=无,1..15
  u32 _pad;                        // 12
  u32 _reserved[12];               // 16..64   填满 cacheline 0(seq 与载荷分离)
  CtrlChannelConfig channels[15];  // 64..544
  char labels[15][100];            // 544..2044  UTF-8、NUL 结尾;≤24 字符最坏 4 字节/字符 + 余量
  char _tail[4];                   // 2044..2048 显式补齐到 64 的整数倍
};
```

  `_tail` 是**显式**补齐:`alignas(64)` 的结构体若不是 64 的整数倍,编译器会隐式补齐(MSVC /W4 C4324),而隐式补齐意味着布局由编译器决定——跨进程共享内存结构不接受这种不确定性。

  **并发口径**:写方 = 本组 `kActive` 的那一个 Output 的 **[M]**(只读观察实例**不写**,避免两个实例抢写让 Input 在两份配置之间抖动);读方 = 各 Input 的 **[M]**,跨进程 **seqlock**,撕裂即沿用上帧、**不自旋**。写侧奇数增量取 `relaxed` + 紧跟一道 `atomic_thread_fence(release)` —— release **store** 只挡「之前的写下沉」,挡不住「其后的载荷写上浮到奇数 seq 之前」,而那正是这里要防的方向(载荷 2KB、跨进程、读写双方分别编译,窗口比进程内的 `PlayheadShot` 宽两个量级)。

  **abi 不变**:布局只在 T06 留白的广播区内新增,既有结构体的偏移/尺寸/段名全部未动;`tests/golden/ipc-layout.txt` 既有行一行没改,只新增两个结构体块。旧 Output 不写广播区 → 新 Input 读到 `config_seq == 0` → 判定「本组没有 Output 在广播」→ 走默认值分支,与旧行为一致,不误把全零当实况;反向(新 Output + 旧 Input)旧 Input 不读该区,无影响。这正是 `config_seq` 从 1 起算的用途。
- **Output 全局信息小节(J09,细化授权;不改既有字段、不改段名)**:采集开关状态、Output 采样率、失准计数(gapCount/overlapCount/epoch 摘要)等机器可读字段——供 Input 状态灯与自动化验证读取

```c
struct OutputGlobalInfo {          // size 256 align 64
  atomic<u32> capture_enabled;     // 0   采集开关(Input「已布防」显示依据)
  atomic<u32> output_sample_rate;  // 4   Output 采样率
  atomic<u32> flags;               // 8   bit0: output_enabled,其余保留
  u32 _pad;                        // 12
  atomic<u32> gap_count[15];       // 16..76   失准(缺口)计数导出([J59] 15 轨)
  atomic<u32> overlap_count[15];   // 76..136  重叠计数导出
  atomic<u64> epoch_summary[15];   // 136..256 各轨当前 epoch
};
```

- **Input → Output 命令环**(SPSC,每 slot 一条):`{seq, channel, op, value}`,op 全集 = `{kSetPriority, kFpReport}`(v1 冻结,[J36] 以 01 §4.4-c 枚举为准,禁止两端各自发明;`set_label` 等字符串型 op 不在 v1,需 abi+1 增补变长区);Output 消息线程消费并落 state(唯一真源,ADR-004)。[J46] `value` 字段定型为 **u64**;`kFpReport` 载荷打包 `tile_idx(高16位) | fingerprint 截断(低48位)`,单条记录原子;「跨轨上游延迟汇总」op 不进 v1,入 abi+1 增补清单
- **连接状态灯**:Input 读 OutputSlot.heartbeat + connected_mask;Output 读各 InputSlot.heartbeat
- **健康 Output 判定(J12)**:Input 直通↔静音仲裁的「健康 Output」= OutputSlot 心跳新鲜(≤2000ms 口径)∧ `connected_mask` 含本 channel;检测不到健康 Output → Input 直通(80ms ramp + 5s 滞回,ADR-002 v1)

```c
struct CtrlRecord {                // size 24 align 8([J46]:value 定型 u64;四字段 atomic 化,尺寸/偏移不变)
  atomic<u32> seq;                 // 0    = 记录在写(生产者先写 0 标记),非 0 = 已提交
  atomic<u32> channel;             // 4    目标 channel(1..15)
  atomic<u32> op;                  // 8    底层存储 CtrlOp 的 u32 值
  atomic<u64> value;               // 16   kFpReport 载荷 = (u64(tile_idx) << 48) | (hash & 0x0000FFFFFFFFFFFF)
};

struct CtrlHeader {                // size 64 align 64(cacheline 0;§5「各 header 前两字段 magic/abi」)
  atomic<u32> magic;               // 0   = kScvbMagic
  atomic<u32> abi;                 // 4   = kScvbAbi
  atomic<u32> generation;          // 8   覆盖式重初始化 +1
  u32 _pad;                        // 12
  u32 _reserved[12];               // 16..64
};
struct CtrlRing {                  // size 448 align 64(头 64 + 16×24 记录)
  atomic<u32> write_pos;           // 0   生产者写位置(单调递增)
  atomic<u32> read_pos;            // 4   消费者读位置(单调递增)
  atomic<u32> overflow_count;      // 8   满环丢最旧计数
  u32 _pad;                        // 12
  u32 _reserved[12];               // 16..64  SPSC 索引独占 cacheline 0
  CtrlRecord records[16];          // 64..448  kCtrlRingCapacity = 16;满环 = write_pos - read_pos >= 16
};
```

## 5. 冻结字段与演进

- 段名前缀 `SynchainSCVB.v1.` 与各 header 前两字段(magic/abi)永久冻结;[J66] 组号 `g{G}`(G=1..8)为前缀的变量部分,每组一套完整独立的 registry(InputSlot×15 + OutputSlot×1)/音频环/特征环/控制面,跨组互不可见、互不影响;Input/Output 按各自 state 的 `group_id`(1..8,默认 1)映射到对应组
- 布局改动 → abi+1 且段名 v2,新旧不互认(避免半兼容惨案);abi/版本不匹配的用户可见行为:拒连 + UI 横幅(显示两端版本与升级指引),绝不崩溃、绝不半兼容(J40)
- 采样率不一致(理论不应发生,同一会话同一引擎):Input slot 写入自己的 sample_rate,Output 发现不一致 → 该轨禁用 + UI 错误
- 同一人声轨只能属一组(物理:首个 Input 截断音频);改组 = 释放旧组 slot→按新组重走 claim 生命周期(J66)

### 增补清单(abi+1 时一并落地,现不进 v1)

- AudioRingHeader.epoch_base_samples(J11,消除换代后 validFrom 的 1 块保守损失)
- FeatHeader.run_id、FeatFrame.band_mid(J35)
- 跨轨上游延迟汇总 op(J46)
- `InitResult::kGeometryMismatch`(J81/C11)—— 把「同 abi 下的几何漂移」从 `kAbiMismatch` 拆出单独成码。v1 **不做**:两者的用户可见处置完全相同(拒连 + 升级横幅,J40),区分只有诊断价值而无行动价值,却要在**全段共用**的 `InitResult` 上加值(五类段的所有调用点都得过一遍);理由详见 §6.5

---

> **本节由 masterPlan `constitution/ipc-contract-v0.md` v1.6 §6 逐字转正**(J81 修宪并批;变更文档 `docs/contract-changes/20260825-viz-segment.md`,PR #89 / T44 / 裁决 J75)。
> 结构体尺寸/对齐/字段偏移的**编译期真值**由 `tests/golden/ipc-layout.txt` 冻结,本节与其逐行对拍(`scripts/check-ipc-doc-parity.mjs`)。

## 6. viz 段:`Local\SynchainSCVB.v1.g{G}.viz`(64 KB;G=1..8,v1.6/J81)

第五类共享内存段,为 **SCVB Monitor**(ADR-001 v2.1 第三 VST3 主目标,只读监视插件)提供跨进程只读数据面。每组一份,跨组互不可见。`SegmentKind` **在枚举尾部追加** `kViz`(既有 `kRegistry`/`kAudio`/`kFeat`/`kCtrl` 四值的顺序与数值一字未动)。mac 长度复核:最长 `/SynchainSCVB.v1.g8.viz` = 23 字符 ≤ 31(PSHMNAMLEN)。

- **唯一写方** = 本组 claim 到 OutputSlot 的那一个 Output(`OutputClaimState::kActive`)的**消息线程 [M]**,**30 Hz** 发布(SL-192;原 4 Hz,变更文档 `docs/contract-changes/20260827-viz-30hz.md`)
- **唯一读方** = Monitor 的消息线程,**只读 attach**(`FILE_MAP_READ`),不 claim、无 ctrl 写权、一个字节都不写
- **`processBlock`(音频线程 [A])对本段零写入**

### 6.1 段布局(总 64 KB,全部区块 64 字节对齐)

| 偏移 | 大小 | 区块 | 说明 |
|---|---|---|---|
| 0 | 64 | `VizHeader` | magic/abi 先行 + generation + 几何字段 |
| 64 | 128 | `VizFrame` | seqlock 帧头:序号 + playhead + 循环区 + 窗口 + 四张掩码 + 车道版本号 |
| 192 | 64 | `VizTrackColors` | 15 个轨色索引(u32) |
| 256 | 1920 | `VizCoverage` | 分段 activity 位图,15 轨 × 1024 位 |
| 2176 | 30720 | `VizLanes` | 降采样 pan 车道,15 轨 × 1024 × int16 |
| 32896 | 128 | `VizTrackState` | 每轨当前值:panNow / volDb / widthPct(定点 int16) |
| 33024 | 512 | `VizTrackLabels` | 每轨 32 字节 UTF-8 轨名 |
| 33536 | 32000 | (预留) | 尾部留白,后续增补不改既有偏移 |

**常量**:`column_count = 1024`、`track_count = 15`、`pan_scale = 100`、`kVizPanNone = −32768`、段预算 65536。

```c
struct VizHeader {                 // size 64 align 64(cacheline 0)
  atomic<u32> magic;               // 0    kScvbMagic;初始化完成标志,release-store 后行
  atomic<u32> abi;                 // 4    kScvbAbi(=1,挂总 abi,见 §6.5)
  atomic<u32> generation;          // 8    覆盖式重初始化 +1
  u32 column_count;                // 12   = 1024(几何,magic 发布前写定)
  u32 track_count;                 // 16   = 15
  u32 pan_scale;                   // 20   = 100
  u32 _reserved[10];               // 24..64
};
struct VizFrame {                  // size 128 align 64(seqlock 临界区起点)
  atomic<u32> seq;                 // 0    偶=稳定,奇=写入中
  atomic<u32> playhead_flags;      // 4    bit0 playing / bit1 looping / bit2 loopValid
  atomic<u64> publish_ms;          // 8    发布时刻(steadyNowMs;读侧据此判写方停摆)
  atomic<u64> window_start_samples;// 16   降采样窗口起点(v1 恒 0 = 工程起点)
  atomic<u64> window_span_samples; // 24   窗口跨度(0 = 未 prepare / 无有效窗口)
  atomic<i64> playhead_samples;    // 32   −1 = 无时间线
  atomic<i64> loop_start_samples;  // 40   −1 = 无效
  atomic<i64> loop_end_samples;    // 48   −1 = 无效
  atomic<u32> sample_rate;         // 56   0 = 未 prepare
  atomic<u32> version_active;      // 60   1|2(车道取自哪个版本)
  atomic<u32> playhead_epoch;      // 64   时间线跳变代数
  atomic<u32> track_online_mask;   // 68   bit{N−1} = 该轨启用
  atomic<u32> track_covered_mask;  // 72   bit{N−1} = 该轨有分段
  atomic<u32> track_stereo_mask;   // 76   bit{N−1} = 该轨立体声源
  atomic<u32> lane_revision;       // 80   车道/位图内容版本;只在重算车道时 +1
  atomic<u32> track_lead_mask;     // 84   bit{N−1} = 该轨 lead_lock
  u32 _reserved[10];               // 88..128
};
struct VizTrackColors {            // size 64 align 64
  atomic<u32> index[15];           // 0    调色板槽位(1..15;0=未指定),v1 恒等于轨号
  u32 _pad;                        // 60
};
struct VizCoverage {               // size 1920 align 64
  atomic<u32> bits[15][32];        // 0    每轨 1024 位,LSB 优先
};
struct VizLanes {                  // size 30720 align 64
  atomic<int16_t> pan[15][1024];   // 0    定点 ±10000;哨兵 −32768 = 该轨整条无数据
};
struct VizTrackState {             // size 128 align 64
  atomic<int16_t> panNow[16];      // 0    播放头精确时刻的曲线求值(不是车道采样)
  atomic<int16_t> volDb[16];       // 32   −24..+12 dB(×100)
  atomic<int16_t> widthPct[16];    // 64   0..100 %(×100)
  int16_t _reserved[16];           // 96   索引 15 空置,只为让每个数组按 32 字节对齐
};
struct VizTrackLabels {            // size 512 align 64
  atomic<u32> utf8[15][8];         // 0    每轨 32 字节 UTF-8,NUL 补齐
  u32 _pad[8];                     // 480
};
```

- `VizTrackColors`:值 = 调色板槽位(1..15;0 = 未指定),v1 恒等于轨号(web 侧 `--track-color-1..15`「顺序即轨号」)
- `VizCoverage`:每轨 1024 位,**LSB 优先**(列 i → `bits[i/32]` 的 `bit(i%32)`;web 侧 `word = i>>>5, bit = i&31` 同源)
- `VizLanes`:定点 = `round(clamp(pan, −100, +100) × 100)`,即 ±10000;哨兵 `−32768`(`kVizPanNone`)= 该轨整条无数据
- `VizTrackState`:定点标度同 pan(×100);`panNow ∈ [−100,100]`、`volDb ∈ [−24,12]` dB、`widthPct ∈ [0,100]`;哨兵 `−32768` = 无数据。索引 15 空置,只为让每个数组按 32 字节对齐
- `VizTrackLabels`:每轨 **32 字节 UTF-8**,NUL 补齐;超长按 **UTF-8 字符边界**截断到 ≤31 字节,绝不切出半个多字节序列
- 所有跨进程字段一律 `std::atomic`(§0 总则),含 15×1024 的车道元素 —— relaxed 存取在 x86 上即普通 `mov`,零额外开销,同时消除 seqlock 载荷的形式化数据竞争

⚠ **`panNow` 口径**:是**播放头精确时刻**的曲线求值,**不是** pan 车道在播放头所在列的采样(后者是列中心点采样)。分布图要的是「此刻」,轨迹图的线才走车道。

### 6.2 降采样口径(冻结语义,读写两侧必须一致)

- 窗口 = `[window_start, window_start + window_span)`,均分 **1024** 列;第 i 列覆盖 `[start + i·span/N, start + (i+1)·span/N)`
- **车道值** `pan[t][i]` = 该轨曲线在**列中心时刻**的求值(**点采样**,不是列内均值),数据源 = 引擎同一个 `CurveEvaluator`(与打印 30 条同源)
- **覆盖位图** `coverage[t][i]` = 该列时间区间与该轨任一 CRVS 分段**有交集**即置 1(保守口径:短于一列的分段不会消失)
- **断线渲染以位图为准**:曲线求值本身会填补空隙(hold/外推语义),只有分段表才是覆盖真源。整轨无分段 → 车道全哨兵 + 位图全 0
- **窗口跨度** = `max(最大分段末端, playhead+1, 60 s)` 向上取整到 **30 s** 边界,上限 24 h。CRVS 里 `t1 = 1<<40` 的「无末端」哨兵只以其 `t0` 参与跨度计算,在位图上一路覆盖到窗口末端

### 6.3 发布节拍

- 帧头标量与 `VizTrackState` 每轨当前值:**30 Hz**(SL-192 升频;原 4 Hz)。实现口径:Output 的**独立 60 Hz 定时器** + 25 ms 闸门下限 ⇒ 每两拍发一帧 = 33.3 ms。闸门刻意**夹在一拍(16.7 ms)与两拍(33.3 ms)之间**并两头留余量 —— 写成 33 ms(= 目标周期本身)只剩 0.33 ms 余量,真机抖动一来就顺延到三拍、频率塌到 20 Hz
- 车道 + 位图 + 轨色(15×1024 次曲线求值):**按需重算** —— CRVS 修订变化 / 活动版本切换 / 窗口跨度变化 / **轨名**变化(`metaRevision`,FNV-1a 64 位)四者之一触发,外加 **30 s 兜底**。**升频不影响本条** —— 稳态下一帧只写帧头 + 每轨当前值(约 380 字节;实测单帧 p50 **1.6 µs**,30 Hz 即 0.005% 一颗核,证据面 = `tests/core/test_viz_publish_cost.cpp`)
- **width 不进 `metaRevision`**:它走帧头段、每帧都刷;掺进去会让「width 被自动化」变成每秒 15 360 次求值

### 6.4 一致性与生命周期

- 整帧 **seqlock**:写方 `seq` 先 +1 变奇 → 写载荷 → 再 +1 变偶;读方读前后两次 `seq`,奇数或不相等即重试(上限 8 次),超限返回 false 由调用方沿用上帧。**读侧全程无锁、无写**
- **owner 线程在首次 `publish()` 时绑定,不是 `open()` 时** —— JUCE/VST3 不保证 `prepareToPlay`(open 的触发点)与 `timerCallback`(publish 的触发点)在同一条线程上;绑错会让此后每次发布被护栏静默挡掉、段永远全零,比不设护栏更难查
- 只读 attach 状态下 `publish()` 是彻底 no-op;`attachReadOnly()` 走 `openExistingReadOnly` + `checkHeaderReadOnly`,**绝不 memset、绝不覆盖式重初始化**(覆盖分支仅限创建者角色)
- viz 段**不用** `SegmentHandle` 的租约 + 500ms 宽限期(那套机制的唯一理由是「音频线程可能仍持有裸指针」,viz 无任何 [A] 访问):否则①读方 release 后仍把段吊住,写方退出后读方永远等不到空态;②宽限期未满就析构会退回「进程退出统一回收」= 每次换组泄漏一份映射。故直接持有 `SegmentView`,`release()` 立即 `unmap`
- claim 态由 Output 的 [M] **每拍**重做:成为 `kActive` 即建段,失去 `kActive` 立刻释放;同组 `kObserver` 实例**既不建段也不发布**(两个写方同推一个 seqlock 会产出「`seq` 是偶数、内容却撕裂」且看起来完全正常的帧)
- Monitor 掉线、崩溃、或从不存在,对 Output 侧**零影响**(无握手、无引用计数、无等待);Output 不在线时 Monitor `attachReadOnly()` 得 `kFailed` → 空态,不崩溃、不重试风暴

### 6.5 abi 策略:挂总 abi,本次**不 +1**

- `VizHeader.abi` 恒 = `kScvbAbi`(= 1),与既有各段同构,复用 `initHeader` / `checkHeaderReadOnly`,J40 的「拒连 + 横幅、绝不半兼容」行为免费得到
- **新增段不触发 abi+1**:「加一个新段」不改任何既有段的一个字节,新旧进程对 registry/audio/feat/ctrl 的互认完全不变。降级路径优雅 —— 旧版 Output 不建 viz 段 → Monitor `attachReadOnly()` 得 `kFailed` → 空态;abi 不符则得 `kAbiMismatch` → 拒连横幅,两者可区分
- **不设独立 `viz_abi`**:独立版本号会造出「registry 认、viz 不认」的半兼容态,正是 J40 要禁的
- **viz 段自身**将来的布局改动与其余各段同规(§5):**abi+1 且段名 v2**
- 额外一道几何自检:段内 `column_count`/`track_count`/`pan_scale` 与读方编译期常量不一致时,`attachReadOnly()` 返回 `kAbiMismatch` —— **与 abi 不符刻意同码,不为几何漂移单设返回值**。三条理由:① 同 abi 下的几何漂移是**构建异常,理论不应发生**(它意味着两个二进制用同一个 abi 号编出了不同的编译期常量,属构建/分发出错,不是版本演进的正常态);② **用户可见处置完全相同** —— 两者都是「拒连 + 升级指引横幅」(J40),没有任何一条分支会因病因不同而走不同的恢复路径;③ **区分对用户无行动价值** —— 用户能做的只有换一个匹配的版本,多一个返回码不会让这件事更容易,却要在**全段共用**的 `InitResult` 上加值(五类段的所有调用点都得跟着过一遍)。若将来确有诊断需要,增设 `InitResult::kGeometryMismatch` 已记入 v1.6 修订节的 **abi+1 增补清单**
