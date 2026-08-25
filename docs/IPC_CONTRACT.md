# IPC_CONTRACT —— SCVB 共享内存契约(冻结契约)

> 状态: 冻结
> 最后更新: 2026-08-24(转正回填;内容依据 `docs/constitution/ipc-contract-v0.md` v1.5)
> 真源: 本文件(由 `docs/constitution/ipc-contract-v0.md` 蒸馏转正)

> ⛔ **本文件是冻结契约。** 修改前必读 `CONTRIBUTING.md` §8 与 `CLAUDE.md` §7。未经批准的改动 PR 会被直接关闭。

仲裁规则:`docs/constitution/` 只读副本是**修订源**(改动须走修宪流程);本文件是**实现/审查基准**(06 §3.4 review bot 比对对象)。两者分歧时,以已冻结实现代码与 `tests/golden/` 快照为准。

本文件是 IPC 冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。内容蒸馏自 `docs/constitution/ipc-contract-v0.md`(v1.5,含 J66 分组架构);段名/布局字段/对齐规则不得改。段名前缀 `SynchainSCVB.v1.` 与各 header 前两字段(magic/abi)永久冻结;布局改动必须 `abi+1` 且段名升 v2,新旧不互认。

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
- **Output 全局信息小节(J09,细化授权;不改既有字段、不改段名)**:采集开关状态、Output 采样率、失准计数(gapCount/overlapCount/epoch 摘要)等机器可读字段——供 Input 状态灯与自动化验证读取

```c
struct OutputGlobalInfo {
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
struct CtrlRecord {                // [J46]:value 定型 u64;四字段 atomic 化(尺寸/偏移不变)
  atomic<u32> seq;                 // 0 = 记录在写(生产者先写 0 标记),非 0 = 已提交
  atomic<u32> channel;
  atomic<u32> op;                  // 底层存储 CtrlOp 的 u32 值
  atomic<u64> value;               // kFpReport 载荷 = (u64(tile_idx) << 48) | (hash & 0x0000FFFFFFFFFFFF)
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
