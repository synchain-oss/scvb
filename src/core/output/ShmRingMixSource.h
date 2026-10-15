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
//   I3 尾段可读:上一代已确认写过、且新一代不可能覆盖到的区间,换代后继续可读(上一代窗)。[A-16] 上一代尾段里由读方
//      私有副本覆盖的那一段不再查环(见下「读方预拷贝」)。
//   I4 同步宿主不变:读写在同一块里一起跳变(先 Input 后 Output 的宿主)时,与旧实现逐块同读
//      (A-3 同步族差分:新读方成功集合 ⊇ 旧读方)。
//   I5 不越界:不改布局 / abi / 写方行为;下面的状态全是定长成员,只由音频线程访问,
//      read() 不分配、不加锁。[A-16] 私有副本的存储在第一次 bind()(非实时线程)一次分配,read() 只用。
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
//      (先 Input 后 Output)看不到这个状态。[A-16] 写方循环回绕恰好撞上这个窗口时,上一代私有副本当场闩死(新一代
//      第一次被看到的写头还是上一代的、在读方之上),这一圈退回环路径(残余 RP7)。
//   R2 读方自己跳变的那一块(同步规则 / 推迟锚定)默认「宿主给读写两方的跳变目标是同一个位置」;
//      目标不一致(起播时间戳怪癖)时靠「写头相对读方的位置不超过实测提前量上界」挡:读方目标
//      早于写方目标的偏差小于提前量锯齿宽度时挡不住。
//   R3 读方跳变之前写方已经换了 ≥2 代(例:Input 侧起播怪癖连续好几块报错位置)时,推迟到写头
//      确认是新一代的之后再按同一判据锚定;写方最后一次跳变恰好就在读方眼前时仍可能锚早。
//   R4 写方单独换代、读方没跳时,读方不知道写方新一代从哪一帧写起,只能锚在论证得了的写头上(不错音、不报警):
//      · 写方领先时从停调 / 无 region 恢复(H4):句首最多丢一个写方突发块(设计稿 §8 第 4 条);
//      · [A-13] 宿主播放中原地重算预取轨(从 Output 游标重新渲染,LS-16)、位置连续的换代:新一代第一次被看到的头
//        正好比上一代最后观测到的头多一个写方块,按 R5 的「正好多一步」锚在这个头上,[上一代最后观测到的头, 这个头)
//        那一块不读 —— 丢一个写方块(A-13 之前要等写头再动一次、锚在那时的头上,丢两块)。上一代里量到过更大的步长
//        (写方节拍不稳)时,多一块就不算「正好多一步」,仍丢两块。
//   R5 「远超一步」只是暂定(firstObservation):换代后第一次看到的写头比上一代最后观测到的头多走了
//      超过上一代观测到的最大步长,多半是新一代的头,先按它锚;但最大步长是观测值、不是上界,R1 那段
//      窗口里它也可能是上一代在读方最后一次观测之后又多写了一大笔的旧头。所以这样下的锚要等写头
//      第一次动时复核(往回走、或一步超过 spanBound 就撤锚重来,与锚在未确认写头上同一套);写方单独
//      往前跳、跳距加第一笔不超过 spanBound 时复核分不出来,[旧头, 新一代起点) 可能被当成本代数据。
//      只在 R1 窗口里出现。
//      [A-13] 「正好多一步」(多走的正好等于上一代最大步长)也按暂定先锚(上面 R4 第二条靠它)。这一档同时是 R1 窗口里
//      最常见的形状(上一代恰好又写了一步、随后换代、新一代还没发布),上面那道复核挡不住「新一代往前跳了不到
//      spanBound」(test_mix_source [a13] 实测:只改这一档时错 512–4096 帧)。所以这一档在写头第一次动时锚点再抬到
//      不低于「此时的写头 − 上一代量到过的最小步长」:新一代到这时至少发布了一笔,一笔按不短于最小步长估,新一代起点
//      就在它之下;那个头真是新一代的头时锚点不动。剩下的:新一代到写头第一次动时总共写了不到一个最小步长、又恰好往前跳,
//      [旧头, 新一代起点)(不到一个最小步长)会被当成本代数据。同样只在 R1 窗口里出现。
//   R6 读方自己的跳变只看 podEpoch_(规则 1),而 OutputProcessor 在跨零点的块与 bypass 路径上不更新它
//      (它的口径归打印器 / playhead,本卡不动):这两处读方那一次跳变读环看不到。同步宿主上由锁步规则接住
//      (SL-523 跨零点用例);写方领先时退到保守规则,那一圈 / 那一次定位多丢至多一个写方块。
//
// ## [A-6] 持续读不到 → 失准(E1b;设计依据 A 线设计稿 §3.4,判据 = A-4 LS-14 / N2b)
//
// 旧口径(P1-7 起逐字保留,见 countFailure):本代 primed 之前的失败一律不计 —— 空环 / 起播瞬间 / 宿主先渲染
// Output / 换代交接中,都是「尚未上线」。缺陷 H3 就藏在这里:读方从一代开始就一直领先写头(宿主先算总线、
// 人声轨没送进装 Output 的总线、Aux 位置没做延迟补偿),这一代永远 primed 不了,于是一直无声、一次都不报。
// A-6 在 primed 之前加一条「持续读不到」:同一段连续读失败(读方时间线连续,中间没有一块读成功)
//   (a) 读方在锚点之上、写头还没写到块尾(unprimedFail 那一形):持续超过宽限 G 之后,写方在推进的块每块计一笔缺口;
//   (b) 读方在锚点之下、上一代窗也接不住(stuckBelowAnchor 那一形,H1 的安全网):持续超过「提前量上界 + G」之后同样计。
//   G = max(200 ms 读方时间线, 2·实测提前量上界 + 本块长),按样本计 —— 离线导出下同一条规则(墙钟不进来)。
//   写头停着(写方没在推进)的块照旧不计(P1-7 / 挂起归 suspended);一段失败里写方停了又动,动的那一块才计。
// 这段连续失败在这些时候重新起算:读成功(任一窗)、换代、重绑、读方自己跳变、读方漏读了块(本轨被移出注入集
// 一段时间又回来)。段头 channels 与快照不一致的块(几何改写、[M] 还没换绑,≤ 一拍)单独计 layoutMismatch:
// 它既不把这段失败续长(不让几何改写的短暂丢失凑满宽限),也不让它重新起算(不让几何改写掩盖真的读不到)。
// primed 之后的失败仍走旧口径(写方在推进 / 套圈即计,停着只记 stallFail),A-6 不动。
//
// ## [A-16] 读方预拷贝(私有副本;设计依据 A-16 设计稿 §2/§3,判据 = test_mix_source [a16] / A-3 oracle I1' / LS-4b)
//
// 圈长落在 k·环长前后一个提前量一带时(③' (R−A, R]、物理段 (R, R+A)、② 余量 [0, 896),以及突发之后的第 0 圈),
// 写方回绕之后读方还要播的上一圈尾段在环路径上读不出来:要么已被新一圈换掉,要么别名判据 / 在途保护不放行 ——
// A-15 证明这两样都放宽不得(读方逐块输入相同、环里不同的四对世界 X1 / X2 / X3 / B256)。所以改成「不再需要从环里
// 读这些帧」:
//   · 拷:本代写头已确认、当前窗已锚定(锚点此后只降不升)时,每块把 [max(锚点, 写头 − P), 写头) 里还没拷的部分
//     (每块至多 C 帧)从环拷进私有副本 pcCur_。拷完在规则 7 那一次「acquire 栅栏 + epoch / 写头」上复核:没换代、
//     写头没往回走、拷贝期间写方前进不足半环才提交,否则这一截作废。上沿是写头(先写数据后发布写头,写头之下都已
//     完整可见);下沿是写头 − P,不是「写头 + 在途保护 − 环长」—— 本代在途写都在写头之上,踩的是 [写头 − 环长, …)
//     的槽,P ≤ 环长/4 时与拷贝区间不相交,不需要知道在途有多少、有几笔(DEP-2)。拷完复核的三条只在拷贝期间写方
//     并发推进时才起作用,单线程用例里碰不到:目前只有论证,没有确定性的删除式见证(要的话得在本类里放测试钩子)。
//   · 交接:换代且当前窗降为上一代窗的同一刻、同一判据(正好隔一代、读方这一块没跳没漏读……),pcCur_ 降为上一代
//     副本 pcPrev_。
//   · 交:每块先做闩死判定 —— 新一代已发布的最高写头 + λ > 本块起点(λ = min(P, 实测提前量上界 + 本块长);没量到过
//     提前量也算)⇒ pcPrev_ 作废,本代不再打开。没作废时本块整个在新一代已发布的跨度之上(所以一定不与当前窗相交),
//     落在 pcPrev_ 里的那一段直接从副本交出,不查环、不复核;其余部分照旧走上一代窗,判据一字不改。
//   · 作废:读方跳了 / 漏读了 / 静止重读 ⇒ pcPrev_ 作废(读方离开了那一遍);漏读 / 静止重读 / 同代写头回退 ⇒
//     pcCur_ 清空(写方可能同代原地重写过);换代但不降级 ⇒ 两个都清;写头归 0 / 重绑 ⇒ clearWindows 两个都清;
//     段头 channels 与快照不一致的块既不拷也不交。「漏读 ⇒ 清 pcCur_」那一条没有单独的删除式:构造不出「读方漏读期间
//     写方同代原地重写、之后又被交出」的现实形状(按 A-14 先例写明);它与静止重读那一条同一个理由。
// 私有副本交出的每一帧满足 I1,另满足:(a) 是读方这次观测时写方为该位置最后一次已发布的写入;(b) 读方自做副本的那一遍
// 以来没离开过那一遍;(c) 宿主已作废、正在重算的旧版本按闩死判定尽力不交(LS-16 原地重算:新一代第一次被看到的头就在
// 本块之上 ⇒ 当场作废)。证明不用在途保护、别名判据、新一代跨度下界,也不依赖 λ 准不准(λ 只影响用不用副本)。
// 私有段只替换来源 ⇒ 成功集合 ⊇ 关预拷贝的读方;同步宿主上新一代一换代就闩死、私有段恒空 ⇒ I4 不变。
// 容量 P = min(构造参数(默认 2^17 帧), 环长/4):生产环 2^19 时 2^17 帧(48 kHz 约 2.7 s)。存储在第一次 bind()
// (非实时线程、发布绑定之前)一次分配 2 × P × 2 个 float(约 2 MiB / 每条已绑定的轨),此后不再写这个指针;
// 分配失败或构造参数 0 ⇒ 不拷不交,行为与 A-16 之前逐位相同(测试与紧急回退都用它)。
// 依赖:
//   DEP-1 新一代的数据不先于这次 epoch+1 被读方看见。x86 TSO 天然成立;arm64 靠 B 线 M04 在 AudioRing::bumpEpoch 之后
//         加的 release fence(与本类规则 7 的 acquire 栅栏配对)。A 线这一份 AudioRing.cpp 没有那道 fence ⇒ arm64 包
//         只从含 M04 的树(B 线 feature/macos-au)出。缺它时回绕前最后一次拷贝可能把新一圈的数据当上一圈拷下,
//         epoch 复核看不出来(受害帧的槽正是新一圈第一笔写的槽)。
//   DEP-2 本代在途写总量 ≤ 环长/4(生产环约 2.7 s),一次拷贝期间写方前进不足半环。不依赖「同一时刻只有一笔在途」。
//   DEP-3 同一代里位置连续、写头单调,唯一例外是停走带静止重写(写方协议,A-5 前提)。
// 预拷贝之后的残余(带内带外同一口径):
//   RP1 上一代最后一次被观测之后写方又写的那一截(稳态 ≤ 一个写方块;宿主一口气渲染 k 块时 ≤ k 块)来历证明不了,不读。
//   RP2 回绕之前突发还没拷完(每块至多 C 帧):突发紧贴回绕时,没拷到的那一截照旧走环路径。
//   RP3 P 不够:提前量 + 本块长 > P 时尾段前面那一截照旧走环路径(生产环、48 kHz 下提前量超过约 2.7 s 才会出现)。
//   RP4 往前定位约 k·环长:新一代发布在读方之上 ⇒ 闩死,别名照旧读不到(与 A-16 之前相同)。
//   RP5 私有段之外仍走环路径的上一代帧,照旧依赖「在途写不超过在途保护」(A-15 指出的既有残余,本卡不放大)。
//   RP6 宿主从读方游标之前很远处重算:新一代追到读方 λ 之内以前,副本交的是重算前的版本((a) 仍成立,(c) 尽力而为)。
//   RP7 回绕恰好撞上 R1:闩死,这一圈退回环路径(只在读写真正并发的宿主上)。
//   RP8 停走带静止重写有时看不出来:重写的块长与上一块相同 ⇒ 写头不动;一次拷贝之内退回又回到原处 ⇒ 两次装载都看不到
//       回退。已拷下的那一截于是可能不是该位置最后一次写入,(a) 在这一形上不成立(A-3 oracle 的代号只分代,也看不出)。
//       读方自己静止重读时副本清空,所以只在写方静止重写、读方那几块却没在静止重读,而且写方停的位置离回绕点不到一个
//       提前量时,才可能交出重写之前的版本。没有确定性用例(与拷完复核同理,要测试钩子)。
// 既有口径(A-16 不修):读方自己回绕进下一圈之后,环路径的上一代窗不撤(读方跳变不关它);写方领先、又在新一圈停调时,
// 读方走到写头之上可能把上一圈的音频交出来(A-3 oracle 明文接受这一类;loop-writer-stall 族量过:提前量 4096、2 s 圈、
// 写方在新一圈停 1 s 时开 / 关预拷贝都交出 43776 帧上一圈的音频)。私有路径用「读方跳变即作废」,不扩进这一口径。

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
    // [A-16] 私有副本容量上限的产品默认值(帧,2 的幂;实际容量还要不超过环长/4,见头注)。
    static constexpr int64_t kPreCopyFrames = int64_t{1} << 17;

    ShmRingMixSource() noexcept : ShmRingMixSource(kPreCopyFrames) {}
    // preCopyFrames:私有副本容量上限(帧,向下取到 2 的幂);0 = 关预拷贝(行为与 A-16 之前逐位相同:测试 / 紧急回退)。
    explicit ShmRingMixSource(int64_t preCopyFrames) noexcept : pcFrames_(floorPow2(preCopyFrames)) {}

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
    // handoverLoss:换代交接期读不出的块(当前窗还没能锚定、上一代窗别名判据不过、拷贝后复核不过等)。
    // [A-6] 起不再含「段头 channels 与快照不一致」那一类(见 layoutMismatchCount)。
    u32 handoverLossCount() const noexcept { return handoverLossCount_.load(std::memory_order_relaxed); }

    // [A-6] 持续读不到(E1b,见头注)的宽限下限:读方时间线 200 ms(按快照采样率折成样本)。
    static constexpr int64_t kUnreadGraceMs = 200;
    // [A-6] 段头 channels 与快照不一致、整块不读的块(几何改写之后、[M] 换绑之前,最多一个 [M] 周期约 40 ms)。
    // A-5 时记在 handoverLoss 里;它不是交接丢失,也不进「持续读不到」的计时,单独计。
    u32 layoutMismatchCount() const noexcept { return layoutMismatchCount_.load(std::memory_order_relaxed); }
    // [A-6] 因「持续读不到」计进 gapCount 的块(gapCount 的子集;诊断 / 测试用)。
    u32 persistentFailCount() const noexcept { return persistentFailCount_.load(std::memory_order_relaxed); }
    // [A-6] read() 被调用的块数(成功与失败都算;未绑定 / 参数非法的空调用不算)。OutputSession 拿它分辨
    // 「在注入集里、却一次都没被读」(面板关了这一轨 / Output 没被调用)与「读了、读不到」。
    u32 readAttemptCount() const noexcept { return readAttemptCount_.load(std::memory_order_relaxed); }
    // [A-6] 自最近一笔缺口(gapCount 的任何一次 +1)以来,是否读成功过一块(任一窗)。OutputSession 的失准恢复要求
    // 它为真(正向在场)。读方在计缺口的同一处清掉、读成功时置上 —— 精确到块,不受 [M] 拍粒度影响:缺口之后同一拍里
    // 紧接着读到的那一块也算(普通的瞬时失准撤下时机与修前相同)。
    bool readOkSinceLastGap() const noexcept { return readOkSinceGap_.load(std::memory_order_relaxed); }

    // [A-16] 预拷贝的进程内计数(只计数,不报警)。[A] 写(relaxed),任意线程读。
    // privateRead / privateFrame:从私有副本交出过帧的块数 / 帧数(成功的块才算)。
    u32 privateReadCount() const noexcept { return privateReadCount_.load(std::memory_order_relaxed); }
    u64 privateFrameCount() const noexcept { return privateFrameCount_.load(std::memory_order_relaxed); }
    // preCopyFrame:提交进私有副本的帧数;preCopyDiscard:拷完复核不过、作废的那几截。
    u64 preCopyFrameCount() const noexcept { return preCopyFrameCount_.load(std::memory_order_relaxed); }
    u32 preCopyDiscardCount() const noexcept { return preCopyDiscardCount_.load(std::memory_order_relaxed); }
    // privateLatchOff:上一代副本被闩死判定作废的次数(新一代发布到读方附近)。
    u32 privateLatchOffCount() const noexcept { return privateLatchOffCount_.load(std::memory_order_relaxed); }

private:
    static constexpr int64_t floorPow2(int64_t v) noexcept
    {
        int64_t p = 1;
        while (v > 0 && p <= v / 2)
        {
            p *= 2;
        }
        return v > 0 ? p : 0;
    }

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
        int64_t privLo = 0; // [A-16] 由上一代私有副本交出的那一段(privLen > 0 时有效;不查环、不复核)
        int64_t privLen = 0;
    };

    // [A-16] 读方私有副本的一个窗:连续位置 [lo, hi)(hi − lo ≤ 容量 P),帧 x 存在 buf[(x & (P−1)) × 声道数]。
    // 窗里的帧都是同一代、同一绑定下连续拷的;代号与来历记在窗上,不逐帧记。
    struct PrivWin
    {
        float* buf = nullptr;
        bool on = false;
        int64_t lo = 0;
        int64_t hi = 0;
    };

    // [A-16] 本块要追加进 pcCur_ 的 [a, b)(拷完复核过才提交)。
    struct PreCopyPlan
    {
        bool on = false;
        bool restart = false; // pcCur_ 从 a 重新起(否则接在 pcCur_.hi 之后)
        int64_t a = 0;
        int64_t b = 0;
    };

    // 上一代窗别名判的参照(certify 与拷贝后复核共用一份,见 aliasRef)。
    struct AliasRef
    {
        bool r1Trust = false; // 写头没确认、还停在上一代最后观测到的头上:新一代一段都没发布,不判(R1)
        bool bLowOk = false; // 新一代起点下界界定得了(写头已确认,或本代首次观测以来读方没漏读)
        int64_t bLow = 0; // 新一代可能写过的跨度下界
    };

    void onRebind() noexcept;
    void clearWindows() noexcept;
    void newGeneration(u64 e, int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void sameGeneration(int64_t w, bool flux, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void firstObservation(int64_t w, bool jumped, bool skipped, int64_t prevN, int64_t ring) noexcept;
    void confirmHead(int64_t w, int64_t prevN, int64_t ring) noexcept;
    void tryAnchor(int64_t t0, int64_t w, int n, int64_t slack, int64_t ring) noexcept;
    Cover certify(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t prevN, int64_t ring) const noexcept;
    AliasRef aliasRef(int64_t w, int64_t prevN, int64_t ring) const noexcept;
    bool prevAliasFree(int64_t a0, int64_t a1, int64_t bLow, int64_t wN, int64_t ring) const noexcept;
    void countFailure(int64_t t0, int64_t t1, int64_t w, int64_t guard, int64_t ring, u32 sampleRate) noexcept;
    // 计一笔缺口(失准计数)。所有计缺口的地方都走这里:顺带清掉「自最近一笔缺口以来读成功过」。
    void countGap() noexcept;
    // [A-6] (a) 类持续读不到的宽限(样本):max(kUnreadGraceMs 读方时间线, 2·实测提前量上界 + 本块长)。
    int64_t unreadGrace(int64_t n, u32 sampleRate) const noexcept;
    int64_t leadHiRef() const noexcept; // 实测提前量上界(没量到过 → -1)
    // 新一代在被首次观测前可能已写的跨度上界(prevN = 上一次 read() 的块长)。
    int64_t spanBound(int64_t prevN, int64_t ring) const noexcept;
    int64_t inflightGuard() const noexcept { return stepMaxPrev_ > stepMaxCur_ ? stepMaxPrev_ : stepMaxCur_; }
    // [A-16] 本块的预拷贝:定 [a, b) 并从环拷进 pcCur_ 的缓冲(还不提交)。
    PreCopyPlan planPreCopy(const AudioRingBinding& b, int64_t w, bool flux, int n) noexcept;
    // [A-16] 拷完复核:commit = 拷贝期间没换代、写头没往回走、写方前进不足半环。
    void commitPreCopy(const PreCopyPlan& plan, bool commit) noexcept;

    std::atomic<const AudioRingBinding*> binding_{nullptr}; // [M] 写 / [A] 读
    std::vector<std::unique_ptr<AudioRingBinding>> owned_; // 旧绑定保活(进程寿命;T16 已改回收,SL-445)

    // [A-16] 私有副本的容量上限与存储。pcFrames_ 构造时定;pcStore_ 由 [M] 在第一次 bind() 里、发布绑定之前分配一次
    // (2 × pcFrames_ × 2 个 float),此后不再写 —— 音频线程只在 acquire 到绑定之后读这个指针,写入先于那次 release 发布。
    const int64_t pcFrames_;
    std::unique_ptr<float[]> pcStore_;
    bool pcAllocTried_ = false; // [M] 独占:只试分配一次(失败就一直关着,不在之后的 bind 里改指针)

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
    // [A-13] ……而且正好多「上一代最大步长」那么多(不是远超):写头第一次动时锚点再按上一代最小步长抬一次(R5)
    bool headAtStep_ = false;
    bool headZero_ = false; // 本代写头为 0(几何改写 / 新段:还一帧没写)
    bool headMonotonic_ = true; // 本代写头没被观测到回退过(停走带静止重写换短块会让写头回退,见 sameGeneration)
    bool firstSeen_ = false; // 本代已有过一次写头归属明确(e1==e1b)的观测
    int64_t lastW_ = 0; // 本代上一次明确观测到的写头
    bool curAnchored_ = false;
    int64_t curVf_ = 0;
    int64_t curHi_ = 0; // 本代观测到的最大写头(降级成上一代窗时的 hi)
    bool anchorOnUnconfirmed_ = false; // 当前锚点是在写头尚未确认时下的(确认时要核一致)
    bool anchorAtStepHead_ = false; // [A-13] ……而且是保守规则对着「正好多一步」的暂定头下的(写头首动时只抬这种锚)
    bool jumpedInGen_ = false; // 读方在本代首次被观测之时或之后自己跳过
    bool jumpedAtGenStart_ = false; // ……而且就是在首次观测的那一块跳的(同步规则),否则是推迟锚定
    bool syncUsed_ = false; // 同步规则 / 推迟锚定本代已用过(每代一次)
    u64 genBacklog_ = 0; // 首次观测本代时距上一次观测积压的代数(≥2 = 中间有没见过的代)
    bool contiguousInGen_ = true; // 本代首次观测以来读方没有漏读过块
    int64_t framesSinceGen_ = 0; // 本代首次观测以来读方读过的帧数
    LeadStat leadCur_{};
    int64_t stepMaxCur_ = 0; // 本代相邻两次观测之间写头的最大前进量
    int64_t stepMinCur_ = 0; // [A-13] ……最小前进量(只算动了的;与 stepMaxCur_ 同时有值)

    // 上一代窗(I3)。
    bool prevOn_ = false;
    int64_t prevLo_ = 0;
    int64_t prevHi_ = 0;
    bool prevBLowKnown_ = false; // 新一代起点下界已知(写头已确认);未知时不做别名判(R1)
    int64_t prevBLow_ = 0;
    int64_t refHi_ = 0; // 上一代最后观测到的写头(规则 3:新写头低于它 ⇒ 必是新一代写的)
    bool refHiValid_ = false;

    // [A-16] 读方私有副本(头注「读方预拷贝」)。pcCap_ = 本绑定上的容量 P(min(pcFrames_, 环长/4);0 = 关),重绑时重算。
    int64_t pcCap_ = 0;
    PrivWin pcCur_; // 本代(gen_)从当前窗拷下的帧:只备着,不直接交出
    PrivWin pcPrev_; // 上一代(gen_ − 1):降级那一刻由 pcCur_ 交接而来,只服务上一代尾段

    // 跨代保留的测量(调度关系是宿主的性质,不随换代 / 重绑重来)。
    LeadStat leadPrev_{};
    int64_t stepMaxPrev_ = 0;
    int64_t stepMinPrev_ = 0; // [A-13] 与 stepMaxPrev_ 同一代量到的最小步长

    // 旧实现的失准 / 饿读判别(P1-7),语义逐字保留。
    // 本代是否已在当前窗里成功读到过数据。未 primed 的失败 = 写方还没追到本位置(刚 attach 的空环 /
    // 起播瞬间 / 宿主先渲染 Output 再渲染 Input / 换代交接中),是「尚未上线」而非「失准」,不得计数 ——
    // 否则所有注入轨会在同一块同时 +1(T37 三轮 A 族「五轨几乎同时报失准」)。
    // [A-6] 「尚未上线」有期限:同一段失败持续超过宽限仍读不到(写方在推进),就是「持续读不到」(H3),照计失准。
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

    // [A-6] primed 之前的一段连续读失败(见头注「持续读不到」)。unreadFrom_ = 这段失败的起点(读方时间线);
    // 段头 channels 不一致的块把它往后挪一块,所以「t1 − unreadFrom_」只量真正读不到的那几块。
    bool unreadRun_ = false;
    int64_t unreadFrom_ = 0;

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
    std::atomic<u32> layoutMismatchCount_{0};
    std::atomic<u32> persistentFailCount_{0};
    std::atomic<u32> readAttemptCount_{0};
    std::atomic<bool> readOkSinceGap_{false}; // [A] 写(relaxed),[M] 读;见 readOkSinceLastGap()
    static_assert(decltype(readOkSinceGap_)::is_always_lock_free, "readOkSinceGap_ must be lock-free (audio thread)");
    std::atomic<u32> privateReadCount_{0};
    std::atomic<u64> privateFrameCount_{0};
    std::atomic<u64> preCopyFrameCount_{0};
    std::atomic<u32> preCopyDiscardCount_{0};
    std::atomic<u32> privateLatchOffCount_{0};
    static_assert(decltype(privateFrameCount_)::is_always_lock_free, "u64 counters must be lock-free (audio thread)");
};

} // namespace scvb::output
