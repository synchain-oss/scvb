> 本文件是 masterPlan/constitution 的仓内只读副本，改动须走修宪流程（sha256 同步由 scripts/check-constitution-sync.ps1 断言）。
# SCVB IPC 契约 v0(共享内存)——P1 宪法

状态:**v1.4**(2026-08-11,J57/J59 修宪,正文已就地改写;历史见修订节;v0+统稿修订,修订见文末;裁决依据 plan/adjudications.md)。01-architecture.md 负责细化(错误处理/生命周期时序图),04 负责特征面语义;段名/布局字段/对齐规则不得改,如需改在「对宪法的异议」提出。

## 0. 总则
- Windows 命名共享内存(CreateFileMapping,`Local\` 命名空间,同会话可见);macOS 后续用 POSIX shm 同布局
- 所有跨进程字段 `std::atomic`,static_assert lock-free + 布局(offsetof);缓存行 64B 对齐;ABI 版本字段先行
- 心跳:各写入方每 ~250ms 更新自己的 heartbeat(steady clock ms);读取方 >2000ms 视为陈旧(实例已死/卸载),按未连接处理。无法枚举/删除残留段 → 固定段名 + 覆盖式重新初始化(带 generation 计数)

## 1. Registry 段:`Local\SynchainSCVB.v1.registry`(4 KB)

```c
struct RegistryHeader {            // cacheline 0
  u32 magic;                       // 'SCVB'
  u32 abi;                         // =1
  atomic<u32> generation;          // 每次重新初始化 +1
  u32 _pad;
};
struct InputSlot {                 // ×15(v1.4/J59),每 slot 一条 cacheline
  atomic<u32> state;               // 0=空闲 1=已声明 2=活跃
  u32 pid;
  u32 sample_rate;
  u32 max_block;
  atomic<u64> heartbeat_ms;
  atomic<u64> capture_write_pos;   // 特征段已写到的时间线位置(样本)
  atomic<u32> flags;               // bit0: capturing(J48:一律 fetch_or/fetch_and;bit1: muted 确认)
};
struct OutputSlot {                // ×1
  atomic<u32> state; u32 pid;
  atomic<u64> heartbeat_ms;
  atomic<u32> connected_mask;      // Output 视角:哪些 channel 数据健康(供 Input 状态灯)
  atomic<u32> config_seq;          // Output 配置版本号,Input UI 变化检测
};
```
- Input 实例 claim slot[channel_id-1]:CAS state 0→1;若已被占且心跳新鲜 → UI 报"channel 冲突";陈旧 → 覆盖
- 第二个 Output 实例:OutputSlot 已活跃 → 本实例进只读观察模式 + UI 警告(ADR-002)

## 2. 音频环段(每 channel 一个):`Local\SynchainSCVB.v1.audio.ch{N}`(N=1..15,v1.4)

```c
struct AudioRingHeader {
  u32 magic, abi;
  u32 sample_rate;
  u32 ring_frames;                 // 固定 2^k,时长目标 ≈8 秒 @48k(stereo 时尺寸按双通道预算)
  u32 channels;                    // v1.4/J57:1=mono 2=stereo(prepareToPlay 写定,运行期不变)
  atomic<u64> write_head_samples;  // 时间线绝对样本位置:下一帧将写到的 timeline pos
  atomic<u64> epoch;               // 时间线跳变(定位/循环回跳)时 +1,读方据此丢弃跨代数据
};
float32 ring[ring_frames*channels]; // v1.4/J57:channels=1|2,stereo 为 interleaved LR(ADR-003 v2.0),index = timeline_pos & (ring_frames-1)
```
- 写(Input 音频线程):按块以 playhead `timeInSamples` 为地址写入;transport 非线性跳变 → epoch+1 后继续
- 读(Output 音频线程):按自身块的 [t0,t1) 读取;区间未被覆盖(write_head 落后或 epoch 不符)→ 该轨该块静音 + 失准计数(UI 警告)
- 单写单读 SPSC;时间线寻址天然容忍预测性引擎的提前写(ADR-002/D5)

## 3. 特征段(每 channel 一个):`Local\SynchainSCVB.v1.feat.ch{N}`

```c
struct FeatHeader {
  u32 magic, abi;
  u32 hop_ms;                      // =10
  u32 capacity_hops;               // 环容量(≈20 分钟)
  atomic<u64> base_hop;            // 环起始对应的时间线 hop 序号
  atomic<u64> write_hop;           // 已写到的 hop 序号(时间线寻址)
};
struct FeatFrame { f32 kw_ms; f32 peak; };   // K-weighted mean-square + peak
FeatFrame ring[capacity_hops];
```
- 仅采集开关 ON 且播放时写;Output 在"分析"时按选区拉取快照并入 state(持久化归 Output,段只是运输)
- VAD 后验由 Output 离线从 kw_ms 计算(v1 能量域,ADR-008),特征段不存后验

## 4. 控制面:`Local\SynchainSCVB.v1.ctrl`(16 KB)

- Output → Input 广播区:当前 channels[15] 配置快照(v1.4)(label/priority/lead/pair/auto 开关)+ `config_seq`;Input UI 轮询(25Hz Timer)显示,写操作通过命令环发回
- Input → Output 命令环(SPSC,每 slot 一条):`{seq, channel, op, value}`,op ∈ {set_priority, set_label, …};Output 消息线程消费并落 state(唯一真源,ADR-004)
- 连接状态灯:Input 读 OutputSlot.heartbeat + connected_mask;Output 读各 InputSlot.heartbeat

## 5. 冻结字段与演进
- 段名前缀 `SynchainSCVB.v1.` 与各 header 前两字段(magic/abi)永久冻结
- 布局改动 → abi+1 且段名 v2,新旧不互认(避免半兼容惨案)
- 采样率不一致(理论不应发生,同一会话同一引擎):Input slot 写入自己的 sample_rate,Output 发现不一致 → 该轨禁用 + UI 错误

---

# v1 修订(2026-08-10,编号对应 plan/adjudications.md)

- **[J10]** 心跳阈值拆双:**2000ms=UI 显示陈旧;slot 接管/覆盖需 ≥5000ms 且 pid 存活探测失败**(双条件,保护 SPSC 前提)。
- **[J09]** ctrl 段广播区(§4)增补 **Output 全局信息小节**:采集开关状态、Output 采样率、失准计数(gapCount/overlapCount/epoch 摘要)等机器可读字段——供 Input 状态灯与自动化验证读取。属细化授权:不改既有字段、不改段名。
- **[J12 联动]** Input 未连接直通档(ADR-002 v1 修订)所需的「健康 Output 判定」= OutputSlot 心跳新鲜(2000ms 口径)且 connected_mask 含本 channel。
- **[J11]** abi+1 增补清单:AudioRingHeader.epoch_base_samples(消除换代后 validFrom 的 1 块保守损失)。

## v1.1 补充(2026-08-10,R1 补裁)

- **[J40]** abi/版本不匹配的用户可见行为:拒连 + UI 横幅(显示两端版本与升级指引),绝不崩溃、绝不半兼容。
- **[J32]** ctrl 段细化授权扩展:InputSlot 或 ctrl 广播可增 **muted 确认位**(Input 已完成静音切换的回执),供 Output 延迟注入判定;属 J09 同类细化,不改既有字段段名。
- **[J36]** ctrl 命令环 op 全集由 01 文档枚举并与 05 §1.4 双向核对,禁止两端各自发明。
- **[J35]** abi+1 增补清单追加:FeatHeader.run_id、FeatFrame.band_mid。

## v1.2 补充(2026-08-10,收口后裁决)

- **[J46]** ctrl 命令环记录的 `value` 字段定型为 **u64**;`fp_report` op 载荷打包 `tile_idx(高16位) | fingerprint 截断(低48位)`,单条记录原子;「跨轨上游延迟汇总」op 不进 v1,入 abi+1 增补清单。

## v1.5 修订(2026-08-11,J66 分组架构)

- **[J66]** 全部段名加组号:`Local\SynchainSCVB.v1.g{G}.registry|audio.ch{N}|feat.ch{N}|ctrl`,G=1..8。每组一套完整独立的 registry(InputSlot×15 + OutputSlot×1)/音频环/特征环/控制面;跨组互不可见、互不影响。Input/Output 按各自 state 的 `group_id`(1..8,默认 1)映射到对应组;改组=释放旧组 slot→按新组重走 claim 生命周期。ADR-002「第二个 Output 只读观察」改为**同组内**语义;不同组的 Output 各自独立为主实例。同一人声轨只能属一组(物理:首个 Input 截断音频)。§0 心跳/接管、§5 abi 规则对每组独立适用。

## v1.4 修订(2026-08-11,J57/J59 用户变更)

- **[J59]** InputSlot 数 10→**15**(registry 段布局相应扩容;段名 v1 前缀不变——rc 前布局可改,发布后才受"布局改动 abi+1"约束);音频/特征段名 `ch{N}` N=1..15;pair 上限 7。
- **[J57]** AudioRingHeader 增 `u32 channels`(1|2);stereo 时 ring 为 **interleaved LR**(帧数容量减半,时长目标 ≈8s 不变→ring_frames 尺寸按 stereo 预算);FeatFrame 的 kw_ms/peak 为 BS.1770 多通道求和口径。Input 在 prepareToPlay 依轨道布局写 channels,运行期不变;Output 读侧按 channels 解码。
- 采样率/布局不符处理沿用 §5;01/04 细化捕获与读取伪代码。

## v1.3 补充(2026-08-11,R2 裁决)

- **[J48]** §1 InputSlot.`flags` 声明修正为 **`atomic<u32>`**(尺寸/偏移不变,非布局改动;§0 总则本要求如此)。bit0(capturing,[A] 写)与 bit1(muted,[M] 写)一律 **fetch_or/fetch_and**,禁止 load-modify-store——否则 [A] 的写回可覆盖 [M] 刚清的 muted 位,重新打开 J32 双路叠加窗口。L1 契约测试加双线程混写 flags 用例。
