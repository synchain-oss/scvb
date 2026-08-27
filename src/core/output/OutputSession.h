// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputSession —— Output 实例的 IPC 会话(claim/接管/心跳/attach 环/per-channel 判定/改组,01 §4.2)。
// JUCE-free,可离线单测(用 SegmentBackendInProcess 模拟多实例;跨进程真实行为归 T07b)。
//
// 状态机:O0/O1 未映射/abi 不符(总线直通)、O2 ACTIVE(claim 本组 OutputSlot,读本组环混音)、
// O3 OBSERVER(同组 OutputSlot 已被占用 → 总线直通,25Hz 重试 claim/接管)。改组(J66)=
// 释放旧组 OutputSlot → Unmap 旧组段 → 新组 claim。
//
// per-channel 子状态(§4.2,[M] 25Hz 评估):CH_OFFLINE / CH_SR_MISMATCH / CH_SUSPENDED /
// CH_MISALIGNED / CH_ONLINE。CH_ONLINE 才置 connected_mask 位;注入经 [J32] 延迟 ——
// 置位后等该轨 muted 确认位或延迟 ≥200ms(先到者)才置 injectMask 位([A] 只读 injectMask 混音)。
//
// 所有段操作只发生在持 lifecycleMutex 的消息线程([M] 或宿主生命周期回调);音频线程只经
// mixSource()/injectMask() 拿裸指针做原子读写。

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "analysis/FeatureFingerprint.h"
#include "analysis/FrameStore.h"
#include "ipc/CtrlPlane.h"
#include "ipc/FeatRing.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Registry.h"
#include "output/ShmRingMixSource.h"

namespace scvb::output
{

// [J66] group_id 默认 1(UI 显示 A)。
inline constexpr u32 kOutputDefaultGroup = 1;
// [J32] 新 channel 上线的注入延迟(置 connected_mask 位后 ≥200ms 再注入,或等 muted 确认位)。
inline constexpr u64 kInjectDelayMs = 200;
// [01 §4.2] 近 1s 无失准 → 重新在线(mask 位恢复)。
inline constexpr u64 kMisalignRecoverMs = 1000;
// [01 §4.2] write_head 停滞判定(CH_SUSPENDED 软启发式)。
inline constexpr u64 kSuspendStallMs = 500;

// 心跳年龄「无数据」哨兵(契约 §2.3 heartbeatAgeMs:slotState=0 / 从未心跳时发此值)。
inline constexpr u32 kHeartbeatAgeUnknown = 0xFFFFFFFFu;

// per-channel 连接实况(契约 §2.3 scvb.conn 的数据面;[M] 只读本组 registry 的 InputSlot)。
struct ChannelConnInfo
{
    u32 slotState = 0; // 逐字照 ipc §1:0=空闲 1=已声明 2=活跃
    u32 heartbeatAgeMs = kHeartbeatAgeUnknown; // 哨兵 = 无数据
    bool capturing = false; // InputSlot.flags bit0(kFlagCapturing)
    bool srMismatch = false; // 该轨 Input 采样率 ≠ 本 Output 采样率(ipc §5,该轨禁用)
    // CH_SUSPENDED:写方停在那儿(宿主在无信号段挂起 Input / 用户 bypass / 轨未激活)。
    // 与 misalign **是两件事**,必须分开呈现 —— 见 misalignCountRecent 的头注。
    bool suspended = false;
};

// 心跳年龄换算(纯函数,可离线断言):空闲槽 / 从未心跳 → 哨兵;时钟倒退 → 0;
// 溢出钳到「哨兵-1」,免得真实的超长年龄被误读成「无数据」。
inline u32 heartbeatAgeMsOf(u32 slotState, u64 heartbeatMs, u64 nowMs) noexcept
{
    if (slotState == kSlotFree || heartbeatMs == 0)
    {
        return kHeartbeatAgeUnknown;
    }
    if (nowMs <= heartbeatMs)
    {
        return 0;
    }
    const u64 age = nowMs - heartbeatMs;
    return age >= static_cast<u64>(kHeartbeatAgeUnknown) ? (kHeartbeatAgeUnknown - 1u) : static_cast<u32>(age);
}

// Output 实例的 claim 态(01 §4.2)。
enum class OutputClaimState
{
    kActive, // O2:占据 OutputSlot,读环混音
    kObserver, // O3:同组 OutputSlot 已被占用(只读观察,总线直通)
    kAbiMismatch, // O1:registry/ctrl abi 不符(拒连,J40)
    kUnavailable // O0:段未打开/映射失败
};

class OutputSession
{
public:
    OutputSession(ISegmentBackend& backend, u32 pid);
    ~OutputSession();

    OutputSession(const OutputSession&) = delete;
    OutputSession& operator=(const OutputSession&) = delete;

    // prepare(消息线程/生命周期回调,持 lifecycleMutex):映射 registry + ctrl、claim OutputSlot、
    // attach 活跃 channel 的 audio 环。返回 claim 态。
    OutputClaimState prepare(u32 sampleRate, u32 maxBlock, u64 nowMs);

    // [M] 4Hz 心跳(kActive 才写)。
    void heartbeat(u64 nowMs);

    // [M] 25Hz 主循环:attach 缺失环 → per-channel 判定 → connected_mask/injectMask → 停摆看门狗
    // → ctrl 全局小节刷新(250ms)→ 命令环消费 → 延迟释放回收;O3 时重试 claim。
    void tick(u64 nowMs);

    // [M] 延迟释放回收(registry + ctrl + audio 段句柄)。
    void reap(u64 nowMs);
    std::size_t pendingReleaseCount() const { return registry_.pendingReleaseCount() + pendingSegments_.size(); }

    // 改组(J66):释放旧组 OutputSlot → Unmap 旧组段 → 换新组 → 重走 claim。返回新组 claim 态。
    OutputClaimState changeGroup(u32 newGroup, u32 sampleRate, u32 maxBlock, u64 nowMs);

    // 释放(析构/releaseResources,消息线程):释放 slot + Unmap 段。
    void release(u64 nowMs);

    // 音频线程访问([A])。channel ∈ [1,15];非法返回未绑定源。
    ShmRingMixSource& mixSource(u32 channel);
    const ShmRingMixSource& mixSource(u32 channel) const;
    u32 injectMask() const noexcept { return injectMask_.load(std::memory_order_acquire); }

    // 状态访问。
    // 走带是否在跑([M] 每拍由 OutputProcessor 从 playhead 快照喂进来)。
    // 只用于停流判定:走带停住时所有 Input 的写头本来就该冻着,那不是故障。
    void setTransportPlaying(bool on) noexcept { transportPlaying_ = on; }
    void setCaptureEnabled(bool on) noexcept { captureEnabled_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setOutputEnabled(bool on) noexcept { outputEnabled_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    bool captureEnabled() const noexcept { return captureEnabled_.load(std::memory_order_relaxed) != 0; }
    bool outputEnabled() const noexcept { return outputEnabled_.load(std::memory_order_relaxed) != 0; }
    void setGroupId(u32 g) noexcept { groupId_ = (g >= 1 && g <= kMaxGroups) ? g : kOutputDefaultGroup; }
    u32 groupId() const noexcept { return groupId_; }
    u32 pid() const noexcept { return pid_; }
    u32 sampleRate() const noexcept { return sampleRate_; }
    OutputClaimState state() const noexcept { return state_.load(std::memory_order_acquire); }

    // [M] 桥面 scvb.conn 数据面(契约 §2.3):直接读本组 registry 的 InputSlot 实况。
    // 与 evaluateChannels 的在线判定同源(同一 InputSlot 字段),但**不做**失准/停摆折算 ——
    // 那两项 UI 侧另有 misalignCount 与自己的口径,conn 只报「槽/心跳/采集/采样率」四件事。
    // channel ∈ [1,15];非法或 registry 未映射 → 全默认(slotState=0 + 哨兵年龄)。
    ChannelConnInfo channelConn(u32 channel, u64 nowMs) const;
    // 本组 RegistryHeader.generation(契约 §2.3 顶层 generation;覆盖式重初始化 +1)。
    u32 registryGeneration() const { return registry_.generation(); }

    // [M] 发布配置广播区(Output→Input 只读镜像,契约 §4.3 / ADR-004)。值变化才调用。
    void publishConfigBroadcast(const CtrlBroadcastSnapshot& s) { ctrl_.writeBroadcast(s); }

    // [M] 取走某轨经命令环收到的远程优先级(§3.4 remoteSetPriority)。返回 false = 该轨无待应用值;
    // 返回 true 后该值被清空(取走即消费,不重复应用)。命令在 tick() 的 consumeCommands 里入队。
    bool takeRemotePriority(u32 channel, u32& valueOut);

    // ---- 特征拉取(04 §3.3;采集覆盖的数据面)----------------------------------------
    // Input 每块把特征写进本组 feat 段,Output [M] 25Hz 增量拉进 FrameStore —— 这条读侧此前
    // 完全没接线(FeatPuller/FrameStore 只在 tests/ 里出现),于是 Output 永远拿不到覆盖数据,
    // 「已分析区域共 0 段」且分析按钮恒置灰(T37 三轮 A 族 L-6)。
    //
    // 特征权威存储(04 §3.1):按 channel 独立分页量化存储 + coverage 记账。桥面覆盖率读它。
    const analysis::FrameStore& frameStore() const noexcept { return frameStore_; }
    analysis::FrameStore& frameStore() noexcept { return frameStore_; }
    // 特征 hop 时长(ms):秒 ↔ hop 换算的唯一真源(桥面按它折算范围与覆盖率)。
    static constexpr u32 featHopMs() noexcept { return kFeatHopMs; }

    // ---- 上游改动的过期检测(04 §4.5 fingerprint watchdog)--------------------------
    // 该轨「上游音频与已采集特征不一致」= 桥面 §2.8 channels[].stale。只提示,不自动失效、
    // 不阻断任何操作。数据来自 Input [M] 经命令环上报的 kFpReport(consumeCommands 消费)。
    bool channelStale(u32 channel) const { return fpWatch_.stale(channel); }
    // 诊断/测试:该轨已比对 tile 数与已定谳失配 tile 数。
    u32 fpTilesChecked(u32 channel) const { return fpWatch_.tilesChecked(channel); }
    u32 fpTilesMismatched(u32 channel) const { return fpWatch_.tilesMismatched(channel); }
    // 重采集/清除覆盖后清账(打洞/整轨作废处调用):旧的失配定谳不该跨过一次重采集。
    void resetChannelStale(u32 channel) { fpWatch_.resetChannel(channel); }

    // [M] 布防的**时间维**(契约 §1 setCaptureEnabled:ON = 对 {enabled 轨} × {global.range} 布防)。
    // 落到 ChannelFrames::setGate —— 写入口按 hop ∈ gate 判,范围外静默丢弃、不记账。
    // 不设这道门的话特征按整条时间线累计,内嵌特征的 8MB 预算会先于功能问题暴露。
    // follow 档传全域即可(follow 的语义就是不限范围)。
    void setFeatureGate(analysis::HopRange gate) noexcept { featureGate_ = gate; }

    // [M] 布防的**轨维**([J87] 04 §4.2 ①「落在选区外或未勾选轨的 hop 一律丢弃不记账,
    // 时间维与轨维同为硬约束」)。0 = 不限轨(常态:轨维在 Input 侧按广播区 enabled 位布防);
    // 非 0 = 局部重采集布防期的选中轨掩码,只有这些轨会被拉取记账。
    // 未选中轨**不是**跳过不动:pullTick 会拿空 gate 把它们的读游标推到写头(排空但不写),
    // 否则撤防后那段积压会被补拉进来,把选区外的既有特征覆盖掉 —— 正是本卡要守住的性质。
    void setFeatureTrackMask(u32 mask) noexcept { featureTrackMask_ = mask & 0x7FFFu; }
    u32 featureTrackMask() const noexcept { return featureTrackMask_; }

    // [M] 立刻按**当前**门控补拉一次(不等下一拍 25Hz tick)。
    // 唯一用途:布防生效**之前**把积压清干净(见 armRecapture)。未选中轨在布防期走 drainOnly,
    // 游标直接跳到写头 —— 若布防那一刻读方本来就落后(离线快速渲染会),那段**布防前**就该
    // 记账的积压会被一起丢掉,而它属于「选区外既有特征」,不该少(PR #131 评审 pr-agent)。
    // 先按旧门控补拉、再换门控,这条缝就没了。
    void flushFeaturePull() { pullFeatures(); }

    // [M] 聚合用([J09] 全局小节 / 看门狗)。gapCount 是**进程寿命累计值**(ctrl 全局小节与诊断用)。
    u32 gapCount(u32 channel) const;

    // [M] 桥面 scvb.conn.misalignCount 的数据面:**本次失准发作**内的缺口数 —— 该轨连续
    // kMisalignRecoverMs 无新缺口(evaluateChannels 判定 !misaligned)即归零。累计值不适合直接上桥:
    // 它只增不减,起播抖一次就把「路由失准」横幅永久钉住,恢复健康也撤不下来(T37 三轮 A 族)。
    u32 misalignCountRecent(u32 channel) const;
    u64 writeHead(u32 channel) const;
    u64 epoch(u32 channel) const;

    // [A] 停摆看门狗存活计数([J52]):processBlock 首行 fetch_add。
    void bumpBlockCounter() noexcept { blockCounter_.fetch_add(1, std::memory_order_relaxed); }

    // 时间线健康前置(§4.2 [J51]):[M] 判定连续无时间线 ≥0.5s → 清空 mask(Inputs 走 J12 直通)。
    void forceClearMask() noexcept;
    void forceSetConnectedMaskBit(u32 channel);

private:
    OutputClaimState openAndClaim(u64 nowMs);
    void attachAudioRings();
    void attachFeatRings(); // 只读 attach 本组 feat 段并绑 FeatPuller(与 attachAudioRings 同构)
    void pullFeatures(); // [M] 25Hz 增量拉取 → frameStore_(只拉在线轨)
    void releaseSegments();
    void releaseHandle(SegmentHandle& handle);
    void releaseSlot();
    void resetChannelTracking() noexcept;
    void evaluateChannels(u64 nowMs);
    void refreshGlobalInfo(u64 nowMs);
    void consumeCommands(u64 nowMs);

    ISegmentBackend& backend_;
    u32 pid_;
    u32 groupId_ = kOutputDefaultGroup;
    u32 sampleRate_ = 0;
    u32 maxBlock_ = 0;
    // claim 态([M] 写 / [A] 经 state() acquire-load;processBlock 据此判 observer)。
    std::atomic<OutputClaimState> state_{OutputClaimState::kUnavailable};
    static_assert(decltype(state_)::is_always_lock_free, "OutputSession::state_ 必须 lock-free(§8 实时线程纪律)");

    Registry registry_;
    CtrlPlane ctrl_;
    std::array<ShmRingMixSource, kMaxChannels> sources_{}; // index = channel-1
    std::array<SegmentHandle, kMaxChannels> audioHandles_{};
    std::vector<SegmentHandle> pendingSegments_;

    // 特征读侧([M] 独占;音频线程不触):feat 段只读映射 + 增量拉取游标 + 权威存储。
    std::array<SegmentHandle, kMaxChannels> featHandles_{};
    std::array<bool, kMaxChannels> featBound_{};
    FeatPuller featPuller_;
    analysis::FrameStore frameStore_;
    // fingerprint watchdog 的比对账本([M] 独占;基线现算自 frameStore_,不占持久化字段)。
    analysis::FingerprintWatch fpWatch_{kMaxChannels};
    // 布防时间维(§1 setCaptureEnabled);默认全域 = 不门控。
    analysis::HopRange featureGate_{0, std::numeric_limits<u64>::max()};
    // 布防轨维([J87] 04 §4.2);默认 0 = 不限轨。
    u32 featureTrackMask_ = 0;

    // [A] 只读的注入掩码(bit{N-1} = channel N 可注入混音);[M] 25Hz 写。
    std::atomic<u32> injectMask_{0};
    // [A] fetch_add 的存活计数;[M] 读(停摆看门狗)。
    std::atomic<u64> blockCounter_{0};

    // 配置态([M] 写 / [A] 或全局小节读)。
    std::atomic<u32> captureEnabled_{0};
    std::atomic<u32> outputEnabled_{1};

    // [M] per-channel 状态。
    std::array<bool, kMaxChannels> onlinePrev_{};
    std::array<u64, kMaxChannels> onlineSinceMs_{};
    std::array<u64, kMaxChannels> misalignedSinceMs_{};
    std::array<u32, kMaxChannels> lastGapCount_{};
    // 本次失准发作的起算点:恢复健康(!misaligned)时对齐到当前累计值,于是
    // misalignCountRecent = gapCount - baseline 归零。
    std::array<u32, kMaxChannels> misalignBaseline_{};
    std::array<u64, kMaxChannels> lastWriteHead_{};
    std::array<u64, kMaxChannels> lastWriteHeadChangeMs_{};
    // 停流(CH_SUSPENDED)独立成一条上桥状态(ChannelConnInfo::suspended),**不并进失准计数**。
    // 写头冻结不再由 ShmRingMixSource 计缺口(那会让静音段的 500ms 判定窗每块都记一笔,
    // 就是 v5 实测 P1-7 的「偶发短暂失准」);但持续停流仍必须对用户可见 —— scvb.conn 里
    // 除 misalignCount 外没有第二个「这条轨没在出数据」的位。于是改成:短停(<500ms)零信号,
    // 一旦坐实挂起就记一笔并在挂起期间持续刷新发作时刻,警告不会因「不再有新缺口」自行到期。
    // 上一拍看到的「写头停滞造成的失败读」笔数。停流要记账,除写头冻结外还必须**确有失败读** ——
    // 否则走带一停(所有轨的写头都冻住,而 Output 照常读到已写区、一次都不失败)就会
    // 全 15 轨同时报停流。
    std::array<u32, kMaxChannels> lastStallFail_{};
    // 本段写头停滞期内是否出现过饿读(写头一推进即作废)。见 .cpp 里的跨拍留存说明。
    std::array<bool, kMaxChannels> starvingSeen_{};
    // 停流发作是否已开场。开场要「写头冻结 + 确有饿读」两个条件同时成立;**维持**只要写头
    // 还冻着即可 —— 一旦本轨因停流退出注入集,read() 就不再被调用,饿读自然停止增长,
    // 再要求它就等于让警告在故障持续期间自己到期(v4 实测 P0-2 的假恢复正是这个形状)。
    std::array<bool, kMaxChannels> stallEpisode_{};
    bool transportPlaying_ = false;

    u64 lastGlobalInfoMs_ = 0;

    // [M] 命令环派发结果:Input 远程改优先级的待应用值(§3.4)。桥层每拍 takeRemotePriority 取走
    // 并落 Output 的 runtime state —— 配置真源在 Output(ADR-004),session 只做投递,不自己存配置。
    std::array<u32, kMaxChannels> pendingPriority_{};
    std::array<bool, kMaxChannels> hasPendingPriority_{};
};

} // namespace scvb::output
