// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ShmRingMixSource —— IMixSource 的共享内存实现(01 §5.2 读环语义)。
// bind() 一次性几何快照(sample_rate / ring_frames / channels,Registry.h 几何纪律);
// read() 寻址只用快照,绝不按段头几何寻址(宿主编排 mono⇄stereo 重建环时的撕裂防护)。
// [A-5] read() 每块回读的段头几何字段只有 channels,而且**只比对、不寻址**:与快照不一致 = 写方已按
// 新布局重写这条环、[M] 还没来得及换绑,整块不读(见 read() 里的注释)。
// [SL-486] 上面那句「一次性」是**对音频线程**说的:段头几何被 Input 侧原地改写后,[M] 线程会
// 拿同一个 header/data 再 bind 一次(OutputSession::refreshAudioGeometry),发布一份新的不可变
// 快照 —— 音频线程该读的仍然只有快照,只是快照本身会在 [M] 侧换新。这是安全的:旧绑定由
// owned_ 保活到进程结束,read() 靠绑定**指针变化**自行重置代际状态,不存在半新半旧的中间态。
//
// 跨线程发布协议(T16 DspArbiter / T23 AudioRing 同款 Snapshot 模式):
//   绑定是「不可变快照 + std::atomic<const AudioRingBinding*>」发布 —— 消息线程 bind()/unbind()
//   构造完整绑定后 release-store;音频线程每块 acquire-load 一次、整块复用同一份(无撕裂、无
//   bound==true 且 header==nullptr 的中间态)。旧绑定由本类 owned_ 保活(进程寿命);绑定内裸指针
//   指向的段由调用方(OutputSession)经 SegmentHandle 宽限期保活,音频线程块内使用、绝不跨块持有。
//
// ## [A-5] 读方换代处理(E1;设计依据 A 线设计稿 §3.3,判据 = A-3 读环 oracle + A-4 调度场景)
//
// 读环只靠两个原子量认数据:epoch(写方时间线跳变 / 几何改写时 +1)与 write_head(下一帧将写到
// 的时间线位置)。写方协议(InputProcessor / AudioRing / InputSession):换代 = epoch+1 → 写数据 →
// 发布写头;同一代内位置连续、写头单调(唯一例外:停走带静止重写,同一个 t0 反复写,块变短时写头
// 回退);几何改写 = 写 channels → 写头归 0 → epoch+1。
// 读方不知道写方本代**真正从哪开始写**(b_new),这是全部难点所在。
//
// 不变式:
//   I1 安全:返回 true 的每一帧,环里该槽的内容都是写方为时间线位置 t0+i 写下的数据(哪一代都行)。
//   I2 锚点:当前窗的起点 vf 只在「能论证 vf ≥ b_new」时才设(规则见 read())。
//   I3 尾段可读:上一代已确认写过、且新一代不可能覆盖到的区间,换代后继续可读(上一代窗)。
//   I4 同步宿主不变:读写在同一块里一起跳变(先 Input 后 Output 的宿主)时,与旧实现逐块同读
//      (A-3 同步族差分:新读方成功集合 ⊇ 旧读方)。
//   I5 不越界:不改布局 / abi / 写方行为;下面的状态全是定长成员,只由音频线程访问,
//      read() 不分配、不加锁。
//
// 两个窗:
//   当前窗 cur  = [vf, 写头) —— 本代从 vf 起连续写过(vf ≥ b_new);
//   上一代窗 prev = [lo, hi) —— 上一代最后一次被观测到的写头 hi 之下、它自己还没套圈覆盖的部分;
//                新一代可能写到的跨度 [bLow, 写头 + 在途) 若与其中某帧同槽、却不是同一个位置,
//                那一帧就不能读(别名判据,见 prevAliasFree)。
// 一块里的每一帧要么在 cur 里、要么在 prev 里且无别名,整块才读。
//
// 已知的残余风险(读方手里只有 epoch 与写头两个数,下面几种情形在信息上就分不出来,如实写明):
//   R1 换代之后、写方第一段数据发布之前(epoch 已 +1、写头仍是上一代的值),新一代正写在哪里读方
//      无从得知;这段时间里 prev 窗照读(设计稿规则 2 允许),别名判按「眼前这个写头就是新一代的」
//      暂定(它其实是上一代的旧头时只会更保守),在途的那第一段落点仍不明。若写方这第一段恰好写到
//      读方要读的同一批环槽(概率约 (块长+段长)/环长),拷出的可能是新一代的数据。只出现在 Input
//      与 Output 真正并发处理的宿主上,窗口为写方一次 processBlock 的时长;图依赖成立的宿主
//      (先 Input 后 Output)看不到这个状态。
//   R2 读方自己跳变的那一块(同步规则 / 推迟锚定)默认「宿主给读写两方的跳变目标是同一个位置」;
//      目标不一致(起播时间戳怪癖)时靠「写头相对读方的位置不超过实测提前量上界」挡:读方目标
//      早于写方目标的偏差小于提前量锯齿宽度时挡不住。
//   R3 读方跳变之前写方已经换了 ≥2 代(例:Input 侧起播怪癖连续好几块报错位置)时,推迟到写头
//      确认是新一代的之后再按同一判据锚定;写方最后一次跳变恰好就在读方眼前时仍可能锚早。
//   R4 写方领先时从停调 / 无 region 恢复(H4):读方不知道恢复点,只能锚在确认过的写头上,
//      句首最多丢一个写方突发块(计入 handoverLossCount,不报警;设计稿 §8 第 4 条)。
//   R5 「远超一步」只是暂定(firstObservation):换代后第一次看到的写头比上一代最后观测到的头多走了
//      超过上一代观测到的最大步长,多半是新一代的头,先按它锚;但最大步长是观测值、不是上界,R1 那段
//      窗口里它也可能是上一代在读方最后一次观测之后又多写了一大笔的旧头。所以这样下的锚要等写头
//      第一次动时复核(往回走、或一步超过 spanBound 就撤锚重来,与锚在未确认写头上同一套);写方单独
//      往前跳、跳距加第一笔不超过 spanBound 时复核分不出来,[旧头, 新一代起点) 可能被当成本代数据。
//      只在 R1 窗口里出现。
//   R6 读方自己的跳变只看 podEpoch_(规则 1),而 OutputProcessor 在跨零点的块与 bypass 路径上不更新它
//      (它的口径归打印器 / playhead,本卡不动):这两处读方那一次跳变读环看不到。同步宿主上由锁步规则接住
//      (SL-523 跨零点用例);写方领先时退到保守规则,那一圈 / 那一次定位多丢至多一个写方块。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "output/IMixSource.h"
#include "ipc/AudioRing.h"
#include "ipc/SegmentLayout.h"

namespace scvb::output
{

class ShmRingMixSource final : public IMixSource
{
public:
    ShmRingMixSource() = default;

    // 绑定段头与环数据;一次性快照几何。channels ∉ {1,2} 或 ring_frames 非 2^k 或
    // magic/abi 不符 → 发布 bound==false 的绑定(或 nullptr),read() 恒 false 且不计数。
    void bind(AudioRingHeader* header, float* data) noexcept;

    // 解绑(释放/改组路径,防悬垂):release-store nullptr,此后 read()/bound() 均空操作。
    void unbind() noexcept;

    // 音频线程:每 block acquire-load 一次不可变绑定快照,整 block 复用。
    const AudioRingBinding* acquire() const noexcept { return binding_.load(std::memory_order_acquire); }

    // IMixSource
    bool bound() const noexcept override;
    u32 channels() const noexcept override;
    // 见 stallFailCount_ 头注。[M] 读,[A] 写(relaxed)。
    u32 stallFailCount() const noexcept { return stallFailCount_.load(std::memory_order_relaxed); }
    u32 sampleRate() const noexcept override;
    u32 ringFrames() const noexcept override;
    using IMixSource::read; // 三 / 四参数便捷版,见 IMixSource
    bool read(int64_t t0, float* dst, int n, u64 readerEpoch, int64_t hostBlockEnd) noexcept override;
    u32 gapCount() const noexcept override { return gapCount_.load(std::memory_order_relaxed); }
    u64 writeHead() const noexcept override;
    u64 epoch() const noexcept override;

    // [A-5] 进程内计数(只计数,不报警 —— 告警归 A-6)。[A] 写(relaxed),任意线程读。
    // readOk:返回 true 的块。
    u32 readOkCount() const noexcept { return readOkCount_.load(std::memory_order_relaxed); }
    // unprimedFail:当前窗已锚定、块在锚点之上,但写头还没写到块尾,且写方在推进(设计稿 (a))。
    u32 unprimedFailCount() const noexcept { return unprimedFailCount_.load(std::memory_order_relaxed); }
    // stuckBelowAnchor:块起点在锚点之下(t0 < vf ≤ 写头)、上一代窗也接不住,且写方在推进(设计稿 (b))。
    u32 stuckBelowAnchorCount() const noexcept { return stuckBelowAnchorCount_.load(std::memory_order_relaxed); }
    // handoverLoss:换代交接期读不出的块(当前窗还没能锚定、上一代窗别名判据不过、
    // 段头 channels 与快照不一致等)。段头 channels 不一致那一类最多持续一个 [M] 换绑周期(约 40 ms),
    // A-6 拿它做告警时要与真正的交接丢失分开看。
    u32 handoverLossCount() const noexcept { return handoverLossCount_.load(std::memory_order_relaxed); }

private:
    // 一代里观测到的「写头相对读方」的提前量(只在整块落在当前窗里、成功读出的块上量)。
    struct LeadStat
    {
        int64_t hi = 0; // max(写头 − t0):提前量上界(含读方块长)
        int64_t lo = 0; // min(写头 − (t0+n)):≤ 0 = 写方与读方锁步(写头曾恰在读方块尾)
        int64_t samples = 0;
    };

    // 一块要读的区间被两个窗怎样认领(certify 的结果;复核时照这个重判)。
    struct Cover
    {
        bool ok = false;
        int64_t curLo = 0; // 落在当前窗里的那一段起点(curLen > 0 时有效)
        int64_t curLen = 0;
        int64_t prevA0 = 0, prevA1 = 0; // 由上一代窗认领的至多两段
        int64_t prevB0 = 0, prevB1 = 0;
    };

    void onRebind() noexcept;
    void clearWindows() noexcept;
    void newGeneration(u64 e, int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void sameGeneration(int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void firstObservation(int64_t w, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void confirmHead(int64_t w, int64_t prevN, int64_t ring) noexcept;
    void tryAnchor(int64_t t0, int64_t w, int n, int64_t slack, int64_t ring) noexcept;
    Cover certify(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t prevN, int64_t ring) const noexcept;
    bool prevAliasFree(int64_t a0, int64_t a1, int64_t bLow, int64_t wN, int64_t ring) const noexcept;
    void countFailure(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t ring) noexcept;
    int64_t leadHiRef() const noexcept; // 实测提前量上界(没量到过 → -1)
    // 新一代在被首次观测前可能已写的跨度上界(prevN = 上一次 read() 的块长)。
    int64_t spanBound(int64_t prevN, int64_t ring) const noexcept;
    int64_t inflightGuard() const noexcept { return stepMaxPrev_ > stepMaxCur_ ? stepMaxPrev_ : stepMaxCur_; }

    std::atomic<const AudioRingBinding*> binding_{nullptr}; // [M] 写 / [A] 读
    std::vector<std::unique_ptr<AudioRingBinding>> owned_; // 旧绑定保活(进程寿命;T16 已改回收,SL-445)

    // ---- 以下全部音频线程独占(仅 read() 访问;重绑由 lastBinding_ 指针变化检测,不回读成员)----
    const AudioRingBinding* lastBinding_ = nullptr; // 上次块所用绑定(变指针 → 清窗、代际重来)

    // 读方自身时间线(跨重绑保留:它描述的是读方,不是环)。
    bool podSeen_ = false;
    u64 pod_ = 0; // 上一次 read() 的读方代号
    int64_t lastT0_ = 0; // 上一次 read() 的 t0(停走带静止重读)
    int64_t end_ = 0; // 上一次 read() 的 t0+n(时间线连续 = 这一块从这里接着读)
    int64_t lastN_ = 0;

    // 当前代。
    bool genSeen_ = false;
    u64 gen_ = 0;
    bool headConfirmed_ = false; // 本代写头已确认是本代写的(不是 bump 之后、写之前的旧头)
    bool headTentative_ = false; // 本代写头按「远超一步」暂定是本代的(没有证明,R5):可以先锚,写头第一次动时复核
    bool headZero_ = false; // 本代写头为 0(几何改写 / 新段:还一帧没写)
    bool headMonotonic_ = true; // 本代写头没被观测到回退过(停走带静止重写换短块会让写头回退,见 sameGeneration)
    bool firstSeen_ = false; // 本代已有过一次写头归属明确(e1==e1b)的观测
    int64_t lastW_ = 0; // 本代上一次明确观测到的写头
    bool curAnchored_ = false;
    int64_t curVf_ = 0;
    int64_t curHi_ = 0; // 本代观测到的最大写头(降级成上一代窗时的 hi)
    bool anchorOnUnconfirmed_ = false; // 当前锚点是在写头尚未确认时下的(确认时要核一致)
    bool jumpedInGen_ = false; // 读方在本代首次被观测之时或之后自己跳过
    bool jumpedAtGenStart_ = false; // ……而且就是在首次观测的那一块跳的(同步规则),否则是推迟锚定
    bool syncUsed_ = false; // 同步规则 / 推迟锚定本代已用过(每代一次)
    u64 genBacklog_ = 0; // 首次观测本代时距上一次观测积压的代数(≥2 = 中间有没见过的代)
    bool contiguousInGen_ = true; // 本代首次观测以来读方没有漏读过块
    int64_t framesSinceGen_ = 0; // 本代首次观测以来读方读过的帧数
    LeadStat leadCur_{};
    int64_t stepMaxCur_ = 0; // 本代相邻两次观测之间写头的最大前进量

    // 上一代窗(I3)。
    bool prevOn_ = false;
    int64_t prevLo_ = 0;
    int64_t prevHi_ = 0;
    bool prevBLowKnown_ = false; // 新一代起点下界已知(写头已确认);未知时不做别名判(R1)
    int64_t prevBLow_ = 0;
    int64_t refHi_ = 0; // 上一代最后观测到的写头(规则 3:新写头低于它 ⇒ 必是新一代写的)
    bool refHiValid_ = false;

    // 跨代保留的测量(调度关系是宿主的性质,不随换代 / 重绑重来)。
    LeadStat leadPrev_{};
    int64_t stepMaxPrev_ = 0;

    // 旧实现的失准 / 饿读判别(P1-7),语义逐字保留。
    // 本代是否已在当前窗里成功读到过数据。未 primed 的失败 = 写方还没追到本位置(刚 attach 的空环 /
    // 起播瞬间 / 宿主先渲染 Output 再渲染 Input / 换代交接中),是「尚未上线」而非「失准」,不得计数 ——
    // 否则所有注入轨会在同一块同时 +1(T37 三轮 A 族「五轨几乎同时报失准」)。
    bool primed_ = false;
    // covered 失败的**写头停滞**判别(P1-7)。covered 会因两个物理上相反的原因失败:
    //   ① 写头还没推到本块(w < t0+n)—— 写方压根没在写(宿主在静音段挂起了 Input 的
    //      processBlock、bypass、轨道未激活),这归 OutputSession 的 CH_SUSPENDED 管;
    //   ② 写方套圈(w-t0 > ringFrames)—— 数据已被覆盖,这才是真失准。
    // 老写法两者都 +1,于是「音频在但 −inf」的段落里,宿主一挂起 Input,失准就在
    // CH_SUSPENDED 的 500ms 判定期内被误报出来,500ms 后自愈(v5 实测 P1-7)。
    // 判别办法:连续失败期间写头一动不动 = ①,写头仍在推进 = 读方真的跟丢了。
    u64 lastFailWriteHead_ = 0;
    bool sawFail_ = false;

    std::atomic<u32> gapCount_{0};
    // 被「写头停滞」判据挡下来的失败读:不是失准,但也不是无事发生 —— 它精确表示
    // 「Output 要 t0 处的数据,而写方压根没推到那里」。OutputSession 拿它给 CH_SUSPENDED
    // 加第二个条件,以便把「Input 被 bypass / 宿主跳过该轨」与「走带停了,大家都没动」分开:
    // 后者读得到数据(t0 冻在已写区),一次失败都不会有。
    std::atomic<u32> stallFailCount_{0};
    std::atomic<u32> readOkCount_{0};
    std::atomic<u32> unprimedFailCount_{0};
    std::atomic<u32> stuckBelowAnchorCount_{0};
    std::atomic<u32> handoverLossCount_{0};
};

} // namespace scvb::output
