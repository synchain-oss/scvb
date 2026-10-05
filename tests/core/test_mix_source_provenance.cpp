// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source_provenance —— 读环 oracle 模糊测试 + 旧读方差分基线(A 线 A-3)。
//
// 问题:读方(ShmRingMixSource::read)交出来的每一帧,内容必须来自时间线上**被请求的那个位置**
// (设计稿 §3.3 不变式 I1;属于哪一代都可以)。读环的错法有两类:读出错的东西(安全,I1)、
// 该读到的没读到(可用性)。本文件用一个**逐帧溯源 tag** 的 oracle 把两类都量出来。
//
// ## 组成
//
// 1. 溯源 tag:写方写进环里的不是音频,而是每个 float 自己的出处 ——「时间线位置 / 声道 /
//    布局(mono|stereo)/ 写方实例号」编码成一个正规 float(编码见 encodeTag)。读方把它当
//    样本原样拷出来,oracle 逐帧解码,和「这一块请求的位置」逐字段比。
// 2. 写方模型(WriterModel):逐条照 Input 的写环协议生成「微操作」序列,每一步都标了
//    源码行号(以 21d4520 为准),供评审逐条核对:
//      · InputProcessor.cpp:264-274  跳变判定:t0 != expectedNext_ 且(在播 或 t0 != lastT0_)⇒ bumpEpoch
//                                    (startRun 只管特征段,不进本模型)
//      · InputProcessor.cpp:285-286  lastT0_ = t0; expectedNext_ = t0 + nRender(nRender = numIn,
//                                    OutputStage.h:39-50)
//      · InputProcessor.cpp:291-298  按段(SL-523,段长 = preparedMaxBlock_)逐段 AudioRing::write
//      · InputProcessor.cpp:226-253  负 t0 分支:跨零点那一段走 writeTailFromZero,其后的正段只补写环
//      · InputProcessor.cpp:141-162  writeTailFromZero:0 != expectedNext_ 才换代(:152-157),写 [0, 段尾)
//      · InputProcessor.cpp:99-100   每次 prepareToPlay 把两个哨兵复位成 lowest()(下一块必换代)
//      · AudioRing.cpp:33-50         AudioRing::write:先写数据、后 release 发布写头(write_head = 段尾)
//      · AudioRing.cpp:52-58         bumpEpoch = epoch.fetch_add(1, release)
//      · InputSession.cpp:428-437    新建段:几何写定 → write_head 归零 → epoch+1(:469 发布写方快照)
//      · InputSession.cpp:460-467    attach 到存活旧段且几何不符:同上一条路;几何相符则一个字节都不写
//      · InputSession.cpp:507-521    rebuildAudioGeometry(布局变了才走,:59-65):几何 → w=0 → epoch+1
//                                    → 重发布写方绑定快照
//    微操作是「数据写一段 / 发布写头 / 换代 / 几何改写 / 写头归零」这一粒度,所以调度器能让
//    读方落在写方一块的**中间**(换代了还没写、写了还没发布、写了一半)—— 多线程宿主上这些
//    状态都真实存在。
// 3. 读方:被测读方 = 现行 scvb::output::ShmRingMixSource(A-5 会改它,所有调用只经
//    readUnderTest 一处);LegacyReader = 现行 bind()/read() 的测试内原样副本(来源见类头注),
//    给 A-5 之后做差分用。读方侧的 Output 行为照 OutputProcessor.cpp:820-831(自身时间线代号
//    podEpoch)与 :870-875(负 t0 整段直通、不读环)建模;几何快照刷新照 OutputSession.cpp:190-205
//    (按值比对,变了才重绑)。
// 4. 调度族(见下方各 family 函数):同步;写方领先的循环;回跳与前跳;写方领先时从停调恢复;
//    Δ 内两次定位(含第一次往前跳将近一个环距);接近一个环距的单次跳变;读写目标不一致;
//    Output 起播垃圾 t0;几何重写(读方快照按 25 Hz 刷新);bump-before-write 交错(随机前缀与
//    「恰好停在换代之后」两种切法)。
//
// ## 判据
//
//   · 安全(I1):读方返回 true 的每一帧,tag 的(时间线位置, 声道, 布局)必须等于(请求位置,
//     声道, 读方快照布局)。写方实例号记在 tag 里、出错时打印,但**不参与比较** —— I1 说
//     「属于哪一代都可以」:新 Input 实例接手前,旧实例在同一位置写下的数据仍是这条通道在该
//     位置的数据。布局参与比较:按旧快照的声道步长解新布局的数据是真撕裂(SL-486 那类噪音)。
//   · 可用性:oracle 判「环里整块都是当前写方实例在本走带时刻(或之后)写下的新鲜数据」却读失败
//     的帧记为丢失;按读方自身跳变切圈,每圈丢失 ≤ 1024 帧(≈ 1 个分析帧,设计稿 §1.4 第 4 条 LS-4)。
//     上一圈留下、位置也对的旧数据不算「可读」:读方读它不算错,不读它也不算丢。
//   · 差分:同步调度下,被测读方的成功集合 ⊇ LegacyReader(现在两者是同一份逻辑,恒成立)。
//
// 现行代码在 H4(写方领先时从停调恢复)、读写目标不一致、H1 可用性等族上**预期失败**:
// 这些用例打 [!shouldfail],另配同名「- precondition」普通用例,REQUIRE 场景确实跑到
// (回绕次数、读次数、oracle 确实记录到可读数据 / 危险状态)。修复卡(A-5)去掉标记后转绿。
// 设计稿 LS-13 列为「可能出现 WRONG」的几族(Δ 内两次定位、接近一个环距的跳变)在现行代码上
// 实测安全的,写成普通用例(A-5 改读方时必须保持);只有构造出真实危险状态并实测出错帧的才打
// [!shouldfail]。同一族里只有部分格出错时按格拆开(RunCfg::knownBad):[!shouldfail] 只收实测
// 出错的格,其余格进普通用例 —— 否则族里现在安全的格以后退化了,会被同一条 shouldfail 吞掉。
//
// 全部调度用固定种子,结果确定;不起线程(x86 上的双线程压力格测不出 ARM 内存序问题,
// 本卡不做,见 PR 说明)。运行期文案一律 ASCII(本机 CP936 上中文字面量会触发 C4819)。
//
// 手工看全部族的数字:`scvb_tests "[provenance-report]" -s`(隐藏用例,ctest 不跑)。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ipc/AudioRing.h"
#include "output/ShmRingMixSource.h"
#include "support/presence_meter.h"

namespace
{

namespace presence = scvb::testsupport::presence;

static_assert(presence::kRingR == static_cast<std::int64_t>(scvb::kDefaultRingFrames),
              "presence meter R must be the production ring length");

// 与 test_mix_source.cpp 的 RingFixture 同形,只是默认帧数取生产环长。
struct RingFixture
{
    scvb::AudioRingHeader header{};
    std::vector<float> data;

    explicit RingFixture(scvb::u32 frames = scvb::kDefaultRingFrames, scvb::u32 channels = 2)
    {
        header.magic.store(scvb::kScvbMagic, std::memory_order_release);
        header.abi.store(scvb::kScvbAbi, std::memory_order_release);
        header.sample_rate = 48000;
        header.ring_frames = frames;
        header.channels = channels;
        header.write_head_samples.store(0, std::memory_order_release);
        header.epoch.store(0, std::memory_order_release);
        data.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
    }
};

} // namespace

TEST_CASE("PROVENANCE scaffold: a covered read at the production ring length returns the tone written for that "
          "timeline position",
          "[mix][provenance]")
{
    RingFixture f;
    const std::int64_t mask = presence::kRingR - 1;
    // 已经绕过环 3 圈的位置:环槽 = t & mask。
    const std::int64_t t0 = 3 * presence::kRingR + 5 * static_cast<std::int64_t>(presence::kFrame);

    std::vector<float> mono(static_cast<std::size_t>(presence::kFrame), 0.0f);
    presence::addLane(0, t0, mono.data(), presence::kFrame);
    for (int i = 0; i < presence::kFrame; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>((t0 + i) & mask);
        f.data[slot * 2] = mono[static_cast<std::size_t>(i)];
        f.data[slot * 2 + 1] = mono[static_cast<std::size_t>(i)];
    }
    f.header.write_head_samples.store(static_cast<scvb::u64>(t0 + presence::kFrame), std::memory_order_release);

    scvb::output::ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    REQUIRE(src.ringFrames() == scvb::kDefaultRingFrames);

    std::vector<float> interleaved(static_cast<std::size_t>(presence::kFrame) * 2, 0.0f);
    REQUIRE(src.read(t0, interleaved.data(), presence::kFrame));

    std::vector<float> left(static_cast<std::size_t>(presence::kFrame));
    std::vector<float> right(static_cast<std::size_t>(presence::kFrame));
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }
    const presence::CombReport rep =
        presence::analyze(presence::Span{left.data(), right.data(), t0, presence::kFrame}, {0});
    REQUIRE(rep.lanes.size() == 1u);
    REQUIRE(rep.frameStarts.size() == 1u);
    CHECK(rep.lanes[0].count(presence::Verdict::Present) == 1);
    CHECK(rep.lanes[0].frames[0].rho >= 0.999);
}

namespace
{
namespace prov
{

using scvb::u32;
using scvb::u64;

constexpr u32 kRingFrames = scvb::kDefaultRingFrames; // 2^19 = 生产环长(SegmentLayout.h)
constexpr int64_t kRing = static_cast<int64_t>(kRingFrames);
constexpr int64_t kSampleRate = 48000;
constexpr int64_t kNever = std::numeric_limits<int64_t>::lowest();
// 每圈允许的丢失:1 个 1024 样本分析帧(设计稿 §1.4 第 4 条,LS-4「换圈处丢失 ≤ 1 个分析帧」)。
constexpr int64_t kLapLossAllowance = 1024;

// ===========================================================================
// §1 溯源 tag
// ===========================================================================
// 一个 float 编码一份出处。code(29 bit)= pos(24 bit)| ch << 24 | stereo << 25 | writer << 26;
// 位型 = (64 + code >> 23) << 23 | (code & 0x7FFFFF):指数域落在 [64,127],恒为正规正数 ——
// 不是 0、不是非正规数、不是 NaN/Inf,所以读方按 float 赋值拷贝时逐位不变。0.0f(指数域 0)
// 解不出来,当作「从未写过」。时间线位置必须 < 2^24(写方模型生成操作时 REQUIRE)。
constexpr int64_t kTagPosLimit = int64_t{1} << 24;

struct Tag
{
    int64_t pos = -1;
    u32 ch = 0;
    u32 layout = 0; // 写它时的声道数(1|2)
    u32 writer = 0; // 写方实例号(0..7)

    bool operator==(const Tag& o) const noexcept
    {
        return pos == o.pos && ch == o.ch && layout == o.layout && writer == o.writer;
    }
};

float encodeTag(const Tag& t) noexcept
{
    const u32 code = (static_cast<u32>(t.pos) & 0xFFFFFFu) | ((t.ch & 1u) << 24) | ((t.layout == 2u ? 1u : 0u) << 25) |
                     ((t.writer & 7u) << 26);
    const u32 bits = ((64u + (code >> 23)) << 23) | (code & 0x7FFFFFu);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

bool decodeTag(float f, Tag& t) noexcept
{
    u32 bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    const u32 e = (bits >> 23) & 0xFFu;
    if ((bits >> 31) != 0u || e < 64u || e > 127u)
    {
        return false;
    }
    const u32 code = ((e - 64u) << 23) | (bits & 0x7FFFFFu);
    t.pos = static_cast<int64_t>(code & 0xFFFFFFu);
    t.ch = (code >> 24) & 1u;
    t.layout = ((code >> 25) & 1u) != 0u ? 2u : 1u;
    t.writer = (code >> 26) & 7u;
    return true;
}

// 安全判据(I1)的唯一比较点(删除式落点:把它改成恒 true,所有安全用例都应失去牙齿)。
// 比位置、声道、布局;写方实例号不比(见文件头「判据」)。
bool frameMatches(float got, const Tag& want) noexcept
{
    Tag t;
    return decodeTag(got, t) && t.pos == want.pos && t.ch == want.ch && t.layout == want.layout;
}

std::string describe(float f)
{
    Tag t;
    std::ostringstream os;
    if (!decodeTag(f, t))
    {
        os << "<never-written>";
    }
    else
    {
        os << "pos=" << t.pos << " ch=" << t.ch << " layout=" << t.layout << " writer=" << t.writer;
    }
    return os.str();
}

// ===========================================================================
// §2 环(heap,非共享内存)+ oracle 影子
// ===========================================================================
struct Ring
{
    scvb::AudioRingHeader header{};
    // 段恒按 stereo 容量创建(ring_frames × 2 float;SegmentLayout.h:104-109 / Registry.h 几何纪律),
    // mono 只用前半。
    std::vector<float> data;
    // oracle 影子(不进被测路径):每个 float 最近一次**已发布**的写入(写数据之后,同一次
    // AudioRing::write 的写头也已发布)发生时,写方所在的走带计数。写了还没发布的数据读方按协议
    // 看不见,不该算「可读」;所以影子在发布那一步才更新,数据本身在写那一步就落进环里。
    std::vector<int64_t> writtenAt;

    Ring()
    {
        data.assign(static_cast<std::size_t>(kRingFrames) * 2u, 0.0f);
        writtenAt.assign(static_cast<std::size_t>(kRingFrames) * 2u, kNever);
        header.magic.store(scvb::kScvbMagic, std::memory_order_release);
        header.abi.store(scvb::kScvbAbi, std::memory_order_release);
        header.sample_rate = 0;
        header.ring_frames = kRingFrames;
        header.channels = 0;
        header.write_head_samples.store(0, std::memory_order_release);
        header.epoch.store(0, std::memory_order_release);
    }
};

// ===========================================================================
// §3 写方模型
// ===========================================================================
enum class OpKind
{
    kBump, // AudioRing::bumpEpoch(AudioRing.cpp:52-58)/ 几何改写里的 epoch.fetch_add(InputSession.cpp:435/466/520)
    kWrite, // AudioRing::write 的写数据部分(AudioRing.cpp:39-48),可再切成若干段
    kPublish, // AudioRing::write 的发布写头(AudioRing.cpp:49,release)
    kGeometry, // 段头几何 plain u32 改写(InputSession.cpp:431-433 / 462-464 / 517-518)
    kHeadZero, // write_head_samples.store(0)(InputSession.cpp:434 / 465 / 519)
    kAttach, // 新 Input 实例接手本通道(oracle 记账用,不动环)
};

struct WOp
{
    OpKind kind = OpKind::kBump;
    int64_t pos = 0; // kWrite:首帧时间线位置;kPublish:发布值
    int n = 0; // kWrite:帧数
    u32 channels = 0; // kWrite:写方绑定快照的声道数;kGeometry:新声道数
    u32 writer = 0; // kWrite / kAttach:写方实例号
    int64_t counter = 0; // kWrite:首帧所在的走带计数(oracle 影子用)
};

class WriterModel
{
public:
    WriterModel(Ring& ring, int preparedMaxBlock, int chunk) noexcept
        : ring_(ring), seg_(preparedMaxBlock), chunk_(chunk)
    {
    }

    // 新建段:InputSession::createSegments → initHeader 的 initData(InputSession.cpp:428-437):
    // 几何写定 → write_head 归零 → epoch+1;随后 audioRing_.bind(:469)发布写方绑定快照。
    void createFresh(u32 channels, std::deque<WOp>& q)
    {
        pushGeometryRewrite(channels, q);
        snap_ = channels;
        resetSentinels();
    }

    // 新 Input 实例 attach 到 Output 常驻持有的存活旧段(InputSession.cpp:446-469):段头几何与本次
    // 请求按值比对(:460),不一致才走「几何 → w=0 → epoch+1」;一致则一个字节都不写。:469 重发布
    // 写方绑定快照。新实例的 InputProcessor 哨兵是 lowest()(InputProcessor.h:250-251)。
    void attachExisting(u32 channels, std::deque<WOp>& q)
    {
        ++instance_;
        WOp a;
        a.kind = OpKind::kAttach;
        a.writer = instance_;
        q.push_back(a);
        if (headerChannels_ != channels)
        {
            pushGeometryRewrite(channels, q);
        }
        snap_ = channels;
        resetSentinels();
    }

    // 同一实例重新 prepareToPlay(InputProcessor.cpp:53-103):InputSession::prepare 仅当布局变了
    // 才 rebuildAudioGeometry(InputSession.cpp:59-65 → :516-521:几何 → w=0 → epoch+1 → 重发布快照);
    // 两个哨兵无条件复位(InputProcessor.cpp:99-100)。
    void reprepare(u32 channels, std::deque<WOp>& q)
    {
        if (channels != snap_)
        {
            pushGeometryRewrite(channels, q);
        }
        snap_ = channels;
        resetSentinels();
    }

    // InputProcessor::processBlock 的写环部分(InputProcessor.cpp:164-298),haveT0 恒真。
    // counter = 本块首样本的走带计数(只给 oracle 影子用)。
    void processBlock(int64_t t0, bool playing, int numIn, int64_t counter, std::deque<WOp>& q)
    {
        if (numIn <= 0 || t0 + numIn > kTagPosLimit)
        {
            FAIL("writer model: block out of the tag encoding range");
        }
        if (t0 < 0)
        {
            // :226-253 负 t0 分支:按段走;跨零点那一段(段首 <= 0 < 段尾)经 writeTailFromZero,
            // 其后的正段只补写环、不推 expectedNext_。
            for (int off = 0; off < numIn; off += seg_)
            {
                const int m = std::min(seg_, numIn - off);
                const int64_t ts = t0 + off;
                if (ts <= 0 && ts + m > 0)
                {
                    writeTailFromZero(ts, m, counter + off, q);
                }
                else if (ts > 0)
                {
                    audioRingWrite(ts, m, counter + off, q);
                }
            }
            return;
        }
        // :264-274 epoch 跳变检测(停走带静止重写不算跳变)。
        if (t0 != expectedNext_)
        {
            if (playing || t0 != lastT0_)
            {
                pushBump(q);
            }
        }
        // :285-286(nRender = planBlock().renderSamples = numIn,OutputStage.h:39-50)
        lastT0_ = t0;
        expectedNext_ = t0 + numIn;
        // :291-298 按段写整块。
        for (int off = 0; off < numIn; off += seg_)
        {
            const int m = std::min(seg_, numIn - off);
            audioRingWrite(t0 + off, m, counter + off, q);
        }
    }

    // 执行一个微操作(环上的效果)。
    void apply(const WOp& o)
    {
        scvb::AudioRingHeader& h = ring_.header;
        switch (o.kind)
        {
        case OpKind::kBump:
            h.epoch.fetch_add(1, std::memory_order_release);
            ++bumps_;
            genOpen_ = true;
            break;
        case OpKind::kWrite: {
            // 逐帧循环用裸指针:Debug 构建里 vector::operator[] 带越界检查,这里是全文件最热的几处之一。
            float* data = ring_.data.data();
            const u32 mask = kRingFrames - 1;
            for (int i = 0; i < o.n; ++i)
            {
                const int64_t p = o.pos + i;
                const std::size_t slot = static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(p)) & mask);
                for (u32 c = 0; c < o.channels; ++c)
                {
                    data[slot * o.channels + c] = encodeTag(Tag{p, c, o.channels, o.writer});
                }
            }
            unpublished_.push_back(o);
            if (genOpen_)
            {
                genStart_ = o.pos;
                genOpen_ = false;
            }
            break;
        }
        case OpKind::kPublish:
            h.write_head_samples.store(static_cast<u64>(o.pos), std::memory_order_release);
            for (const WOp& wr : unpublished_)
            {
                int64_t* writtenAt = ring_.writtenAt.data();
                const u32 mask = kRingFrames - 1;
                for (int i = 0; i < wr.n; ++i)
                {
                    const std::size_t slot =
                        static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(wr.pos + i)) & mask);
                    for (u32 c = 0; c < wr.channels; ++c)
                    {
                        writtenAt[slot * wr.channels + c] = wr.counter + i;
                    }
                }
            }
            unpublished_.clear();
            break;
        case OpKind::kGeometry:
            h.sample_rate = static_cast<u32>(kSampleRate);
            h.ring_frames = kRingFrames;
            h.channels = o.channels;
            break;
        case OpKind::kHeadZero:
            h.write_head_samples.store(0, std::memory_order_release);
            break;
        case OpKind::kAttach:
            appliedInstance_ = o.writer;
            break;
        }
    }

    u32 appliedInstance() const noexcept { return appliedInstance_; }
    int64_t bumps() const noexcept { return bumps_; }
    // 最近一次换代之后第一笔写的位置(还没写过 = genOpen)。oracle 用它认 H4 型危险状态。
    bool genOpen() const noexcept { return genOpen_; }
    int64_t genStart() const noexcept { return genStart_; }

private:
    void resetSentinels() noexcept
    {
        lastT0_ = kNever;
        expectedNext_ = kNever;
    }

    void pushBump(std::deque<WOp>& q)
    {
        WOp b;
        b.kind = OpKind::kBump;
        q.push_back(b);
    }

    void pushGeometryRewrite(u32 channels, std::deque<WOp>& q)
    {
        WOp g;
        g.kind = OpKind::kGeometry;
        g.channels = channels;
        q.push_back(g);
        WOp z;
        z.kind = OpKind::kHeadZero;
        q.push_back(z);
        pushBump(q);
        headerChannels_ = channels;
    }

    // InputProcessor.cpp:141-162。
    void writeTailFromZero(int64_t t0, int n, int64_t counter, std::deque<WOp>& q)
    {
        const int skip = static_cast<int>(-t0);
        if (skip >= n)
        {
            return;
        }
        const int tail = n - skip;
        if (0 != expectedNext_)
        {
            pushBump(q); // :152-157(startRun(0) 只管特征段)
        }
        lastT0_ = 0;
        expectedNext_ = tail;
        audioRingWrite(0, tail, counter + skip, q);
    }

    // AudioRing::write(AudioRing.cpp:33-50):先写数据,后发布写头 = t0 + n。
    void audioRingWrite(int64_t t0, int n, int64_t counter, std::deque<WOp>& q)
    {
        const int step = chunk_ > 0 ? chunk_ : n;
        for (int off = 0; off < n; off += step)
        {
            WOp w;
            w.kind = OpKind::kWrite;
            w.pos = t0 + off;
            w.n = std::min(step, n - off);
            w.channels = snap_;
            w.writer = instance_;
            w.counter = counter + off;
            q.push_back(w);
        }
        WOp p;
        p.kind = OpKind::kPublish;
        p.pos = t0 + n;
        q.push_back(p);
    }

    Ring& ring_;
    int seg_ = 1024; // Input 的 preparedMaxBlock_(SL-523 分段写的段长上限)
    int chunk_ = 0; // >0:每次 AudioRing::write 的写数据再切成 chunk_ 帧一个微操作
    u32 snap_ = 0; // 写方绑定快照的声道数(AudioRing::bind,InputSession.cpp:469 / :521)
    u32 headerChannels_ = 0; // 段头 channels(生成操作时的视角)
    u32 instance_ = 0; // 当前写方实例号(生成视角)
    u32 appliedInstance_ = 0; // 已执行到的写方实例号(oracle 视角)
    int64_t lastT0_ = kNever; // InputProcessor.h:250
    int64_t expectedNext_ = kNever; // InputProcessor.h:251
    int64_t bumps_ = 0;
    bool genOpen_ = false;
    int64_t genStart_ = kNever;
    std::vector<WOp> unpublished_; // 已写数据、写头还没发布的写操作(oracle 影子用)
};

// ===========================================================================
// §4 LegacyReader —— 现行读方的测试内原样副本
// ===========================================================================
// 来源:取自本卡基点 21d4520(.cpp 最后一次改动在 c4c623d,.h 最后一次改动在 82d25c0):
//   · bind()  = src/core/output/ShmRingMixSource.cpp:7-30
//   · read()  = src/core/output/ShmRingMixSource.cpp:75-167
//   · 成员与初值 = src/core/output/ShmRingMixSource.h:59-85
// 只改了与逻辑无关的三处:类名、read() 不再是 override、gapCount()/stallFailCount() 就地内联。
// 另抄了两个只读 getter:acquire()(ShmRingMixSource.h:44)与 channels()(ShmRingMixSource.cpp:45-49),
// 让旧读方一侧的几何刷新与记账都用它**自己**的快照,不借被测读方的(A-5 改被测读方的 bind /
// channels() 语义时,旧读方这条下界不跟着变)。
// A-5 会重写 ShmRingMixSource::read;本副本留作「旧读方」,差分判据拿它当下界。
// 现在两者逐块等价,由「legacy pin」用例钉住(A-5 改读方时删掉那一条,保留 ⊇ 差分)。
namespace legacy
{
using scvb::AudioRingBinding;
using scvb::AudioRingGeometry;
using scvb::AudioRingHeader;
using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::u32;
using scvb::u64;

class LegacyReader
{
public:
    void bind(AudioRingHeader* header, float* data) noexcept
    {
        if (header == nullptr || data == nullptr)
        {
            binding_.store(nullptr, std::memory_order_release);
            return;
        }

        // 构造不可变绑定:magic/abi 校验 + 几何快照(bind 时读一次,此后只读快照,绝不回读段头几何)。
        auto b = std::make_unique<AudioRingBinding>();
        b->header = header;
        b->data = data;
        const bool magicOk = header->magic.load(std::memory_order_acquire) == kScvbMagic;
        const bool abiOk = header->abi.load(std::memory_order_acquire) == kScvbAbi;
        const u32 sr = header->sample_rate;
        const u32 frames = header->ring_frames;
        const u32 channels = header->channels;
        const bool framesPow2 = frames != 0 && (frames & (frames - 1)) == 0;
        b->geo = AudioRingGeometry{sr, frames, channels};
        b->bound = magicOk && abiOk && framesPow2 && (channels == 1 || channels == 2);

        binding_.store(b.get(), std::memory_order_release);
        owned_.push_back(std::move(b));
    }

    bool read(int64_t t0, float* dst, int n) noexcept
    {
        // 硬约束:本方法 acquire 的绑定裸指针只在「本块」内有效 —— 底层共享内存段由 OutputSession 的
        // SegmentHandle 500ms 宽限期(kReleaseGraceMs > 单块 wall-clock)保活,不得跨块持有裸指针(S1 验收)。
        const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
        if (b == nullptr || !b->bound || dst == nullptr || n <= 0 || t0 < 0)
        {
            return false;
        }

        // 绑定指针变化(重绑/换代)→ 重置代际状态(等效旧 resetTimeline,但只发生在音频线程)。
        if (b != lastBinding_)
        {
            lastBinding_ = b;
            lastEpoch_ = 0;
            validFrom_ = 0;
            primed_ = false;
            sawFail_ = false;
            lastFailWriteHead_ = 0;
        }

        const u64 e1 = b->header->epoch.load(std::memory_order_acquire);
        const u64 w = b->header->write_head_samples.load(std::memory_order_acquire);
        if (e1 != lastEpoch_)
        {
            // epoch 跳变:本代有效数据从此刻起算(§5.2)。
            lastEpoch_ = e1;
            validFrom_ = t0;
            primed_ = false; // 新一代要重新等写方追上,追赶期同样不算失准
            sawFail_ = false;
            lastFailWriteHead_ = 0;
        }

        // covered 判定(§5.2):区间在有效代内、写头覆盖整块、且未超环距(过旧数据已被覆盖)。
        const u64 t0u = static_cast<u64>(t0);
        const bool covered = (t0 >= validFrom_) && (w >= t0u + static_cast<u64>(n)) && (w - t0u <= b->geo.ringFrames);
        if (!covered)
        {
            // 写方套圈:要读的数据已被覆盖 —— 这是真失准,无条件计数。
            const bool lapped = (w > t0u) && ((w - t0u) > b->geo.ringFrames);
            // 写头停滞 = 写方根本没在写(宿主在静音段挂起 Input / bypass / 轨未激活)。
            // 这归 OutputSession 的 CH_SUSPENDED 判定管(靠 write_head 冻结 500ms 认定),
            // 不是读方跟丢,不得计失准 —— 否则挂起后的 500ms 判定窗里必然误报一串(P1-7)。
            // **本轮第一次失败不表态**:只有一个写头采样,判不出写方是在推进还是冻着。
            // 先记下来,等下一块再比 —— 老写法在这里乐观地算「在推进」,于是宿主挂起 Input 的
            // 第一块照样记了一个缺口,失准照样亮起(P1-7 的残余)。真失准(套圈)由 lapped
            // 独立判定,不受这一块延迟影响。
            const bool producerAdvancing = sawFail_ && (w != lastFailWriteHead_);
            lastFailWriteHead_ = w;
            sawFail_ = true;
            // 本代还没读到过任何数据 → 写方尚未追到本位置(空环 / 起播瞬间 / 宿主先渲染 Output),
            // 是「尚未上线」,静音直通但**不计失准**;primed 之后再断才是真缺口(ADR-002)。
            if (primed_ && (lapped || producerAdvancing))
            {
                gapCount_.fetch_add(1, std::memory_order_relaxed); // 缺口→该轨该块静音+失准计数
            }
            else if (primed_)
            {
                // 写头停滞造成的失败读:不计失准,但如实留痕 —— CH_SUSPENDED 靠它区分
                // 「写方停了」与「走带停了」(后者读得到数据,一次都不会走到这儿)。
                stallFailCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }

        // 读样本(重叠语义:同一时间线位置后写覆盖先写 → 按地址读到的即「时间线正确者」)。
        // [J57] 按环头 channels(1|2)解码:stereo = interleaved LR(契约 v1.4)。
        const u32 mask = b->geo.ringFrames - 1;
        const u32 nch = b->geo.channels;
        for (int i = 0; i < n; ++i)
        {
            const u32 frame = static_cast<u32>(static_cast<u64>(t0 + i)) & mask;
            for (u32 c = 0; c < nch; ++c)
            {
                dst[static_cast<std::size_t>(i) * nch + c] = b->data[static_cast<std::size_t>(frame) * nch + c];
            }
        }

        // 读中换代或写方套圈(R1):整块弃用(防 render-ahead 超环距的静默撕裂读)。
        const u64 e2 = b->header->epoch.load(std::memory_order_acquire);
        const u64 w2 = b->header->write_head_samples.load(std::memory_order_acquire);
        if (e2 != e1 || w2 > t0u + b->geo.ringFrames)
        {
            if (primed_)
            {
                gapCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        primed_ = true; // 本代已确有可读数据,此后 covered 失败即真缺口
        sawFail_ = false; // 成功一块即清停滞账:下一次失败重新从「写头是否在动」问起
        return true;
    }

    u32 gapCount() const noexcept { return gapCount_.load(std::memory_order_relaxed); }
    u32 stallFailCount() const noexcept { return stallFailCount_.load(std::memory_order_relaxed); }
    const AudioRingBinding* acquire() const noexcept { return binding_.load(std::memory_order_acquire); }
    u32 channels() const noexcept
    {
        const AudioRingBinding* b = binding_.load(std::memory_order_acquire);
        return b != nullptr ? b->geo.channels : 0;
    }

private:
    std::atomic<const AudioRingBinding*> binding_{nullptr}; // [M] 写 / [A] 读
    std::vector<std::unique_ptr<AudioRingBinding>> owned_; // 旧绑定保活(进程寿命;T16 已改回收,SL-445)

    // 音频线程独占(仅 read() 访问;换代/重绑由 lastBinding_ 指针变化检测,不回读成员)。
    const AudioRingBinding* lastBinding_ = nullptr; // 上次块所用绑定(变指针 → 重置代际状态)
    u64 lastEpoch_ = 0;
    int64_t validFrom_ = 0; // 本代有效数据起点(epoch 跳变后 = 当前块起点,§5.2)
    bool primed_ = false;
    u64 lastFailWriteHead_ = 0;
    bool sawFail_ = false;

    std::atomic<u32> gapCount_{0};
    std::atomic<u32> stallFailCount_{0};
};
} // namespace legacy

using legacy::LegacyReader;

// 被测读方的唯一调用点。readerEpoch = Output 自身时间线代号(OutputProcessor.cpp:820-831 的 podEpoch_),
// 现行接口用不到;A-5 给 read() 加这个参数时只改这一行。
bool readUnderTest(scvb::output::ShmRingMixSource& src, int64_t t0, float* dst, int n, u64 readerEpoch) noexcept
{
    static_cast<void>(readerEpoch);
    return src.read(t0, dst, n);
}

// ===========================================================================
// §5 走带脚本(每条 lane 一份):走带计数 c → 时间线位置
// ===========================================================================
struct Seg
{
    int64_t c = 0; // 本段起点的走带计数
    int64_t pos = 0; // 本段起点的时间线位置
    bool playing = true; // false = 停走带:位置不随计数前进(宿主每块给同一个 t0)
};

struct Script
{
    std::vector<Seg> segs; // 按 c 升序,segs[0].c == 0
    std::vector<std::pair<int64_t, int64_t>> gaps; // [c0, c1):本 lane 不被宿主调用(无 region / 停调)

    Script& seg(int64_t c, int64_t pos, bool playing = true)
    {
        segs.push_back(Seg{c, pos, playing});
        return *this;
    }
    Script& gap(int64_t c0, int64_t c1)
    {
        gaps.emplace_back(c0, c1);
        return *this;
    }
    // 从走带计数 c0 起在 [b, e) 内循环(首圈从 startPos 起),一直排到走带计数 cEnd。
    Script& loop(int64_t c0, int64_t startPos, int64_t b, int64_t e, int64_t cEnd)
    {
        int64_t c = c0;
        int64_t p = startPos;
        while (c < cEnd)
        {
            seg(c, p);
            c += e - p;
            p = b;
        }
        return *this;
    }

    const Seg& at(int64_t c) const
    {
        const auto it = std::upper_bound(segs.begin(), segs.end(), c, [](int64_t v, const Seg& s) { return v < s.c; });
        return *(it - 1);
    }
    int64_t posAt(int64_t c, bool& playing) const
    {
        const Seg& s = at(c);
        playing = s.playing;
        return s.playing ? s.pos + (c - s.c) : s.pos;
    }
    int64_t nextEdge(int64_t c) const
    {
        int64_t e = std::numeric_limits<int64_t>::max();
        const auto it = std::upper_bound(segs.begin(), segs.end(), c, [](int64_t v, const Seg& s) { return v < s.c; });
        if (it != segs.end())
        {
            e = it->c;
        }
        for (const auto& g : gaps)
        {
            if (g.first > c)
            {
                e = std::min(e, g.first);
            }
            if (g.second > c)
            {
                e = std::min(e, g.second);
            }
        }
        return e;
    }
    bool gapAt(int64_t c, int64_t& end) const
    {
        for (const auto& g : gaps)
        {
            if (c >= g.first && c < g.second)
            {
                end = g.second;
                return true;
            }
        }
        return false;
    }
    bool wellFormed() const
    {
        if (segs.empty() || segs.front().c != 0)
        {
            return false;
        }
        for (std::size_t i = 1; i < segs.size(); ++i)
        {
            if (segs[i].c <= segs[i - 1].c)
            {
                return false;
            }
        }
        return true;
    }
};

// ===========================================================================
// §6 调度器
// ===========================================================================
enum class EvKind
{
    kReprepare, // 同一 Input 实例重新 prepareToPlay(可能改布局)
    kAttach, // 新 Input 实例接手本通道(布局可能不同)
};

struct WriterEvent
{
    int64_t counter = 0; // 写方走带计数到这里时(块边界)发生
    EvKind kind = EvKind::kReprepare;
    u32 channels = 2;
};

// 读方落在写方一块中间的方式(仅对「够到 need 的那一块」生效,其余写方块整块原子)。
enum class Micro
{
    kOff, // 写方块整块原子(先 Input 后 Output 的宿主)
    kRandomPrefix, // 随机前缀:换代了还没写 / 写了一半 / 写了还没发布,哪种都可能
    kCutAfterBump, // 这一块里有换代就恰好停在换代之后(InputProcessor.cpp:270 已执行、:296 还没执行);
                   // 没有换代的块仍按随机前缀
};

struct RunCfg
{
    std::string name;
    Script writer;
    Script reader;
    int writerBlock = 1024;
    int writerMaxBlock = 1024; // Input 的 preparedMaxBlock_
    int readerBlock = 256;
    // 每个 Output 块之前,写方至少处理到走带计数 cR + lead + 本块长(设计稿 §3.1 的预取条件
    // c_i >= C_O + L_i + b_out)。lead = 0 且两份脚本相同 = 同步宿主(先 Input 后 Output,同块跳变)。
    int64_t lead = 0;
    int64_t leadJitter = 0; // 每块在 [0, leadJitter] 里随机加量
    // micro:每个 Output 块之前,写方「够到 need 的那一块」只执行一个前缀(其余留到读完再做)——
    // 读方会落在写方一块的中间:换代了还没写、写了一半、写了还没发布。多线程宿主上 Input 与
    // Output 并发跑时(人声轨没有送进装 Output 的总线,图上没有依赖)这些状态都真实存在。
    Micro micro = Micro::kOff;
    int chunk = 0; // micro 下每次 AudioRing::write 的数据部分切成 chunk 帧一个微操作
    u32 channels = 2;
    std::vector<WriterEvent> events; // 按 counter 升序
    // 读方 [M] 线程几何快照刷新(OutputSession::refreshAudioGeometry,25 Hz):周期(样本)。
    // 0 = 每个 Output 块之前都核一次(等效「刷新总抢在读之前」)。
    int64_t refreshPeriod = 0;
    int64_t readerEnd = 0; // 读方走带计数上界
    u32 seed = 1;
    // 按构造,现行实现在这一格上预期失败。族里只有部分格会红时,[!shouldfail] 用例只收这些格,
    // 其余格进普通用例 —— 否则族里现在安全的格以后退化了,会被同一条 shouldfail 吞掉。
    bool knownBad = false;
};

struct LapStat
{
    int64_t startPos = 0;
    int64_t availFrames = 0;
    int64_t lostFrames = 0;
};

struct ReaderStat
{
    int64_t reads = 0; // read() 调用数(t0 >= 0 的 Output 块)
    int64_t framesRead = 0; // 这些块请求的帧数
    int64_t ok = 0; // 返回 true 的块
    int64_t wrongReads = 0; // 返回 true 但至少一帧 tag 不符
    int64_t wrongFrames = 0; // 返回 true 的块里 tag 不符的帧数(I1 违例)
    int64_t availFrames = 0; // oracle 判整块可读(本走带时刻写下的新鲜数据)的帧数
    int64_t lostFrames = 0; // 其中读方返回 false 的帧数
    std::string firstWrong;
};

struct RunResult
{
    std::string name;
    bool knownBad = false; // 抄自 RunCfg::knownBad
    ReaderStat cur; // 被测读方 = 现行 scvb::output::ShmRingMixSource
    ReaderStat legacy; // LegacyReader
    std::vector<LapStat> laps; // 按读方自身跳变(podEpoch)切圈,统计被测读方
    int64_t readerJumps = 0; // podEpoch 的跳变次数(不含起播那一次)
    int64_t writerBumps = 0;
    // H4 型危险状态:读时写方本代第一笔写在 t0 之后,而写头已覆盖本块 —— 读方若在这里认本块,
    // [t0, 本代起点) 就是没写过的旧内容。
    int64_t hazardReads = 0;
    // 换代了还没写(bump-before-write):读时 epoch 已 +1 而本代一帧都没写。
    int64_t bumpBeforeWriteReads = 0;
    // 写方已经跳了、读方还没跳:读时写方的换代次数多于读方上一次自身跳变时看到的。
    int64_t leadObservedReads = 0;
    int64_t staleSnapshotReads = 0; // 读方几何快照与段头 channels 不一致时的读次数
    // 读时本块至少有一个环槽里放的是**另一圈**的同槽数据(tag 位置 = 请求位置 ± k·R,布局相符):
    // 读方若认这块,就是把 ≈ 10.9 s 前 / 后的音频当成本位置。现行实现靠 `w - t0 <= R` 挡它。
    int64_t lapAliasReads = 0;
    int64_t partialBlockReads = 0; // micro:读时写方有一块只做了一半
    int64_t legacyOnlyOk = 0; // 差分:旧读方成功、被测读方失败的块数(⊇ 判据要求 0)
    int64_t curOnlyOk = 0;
    int64_t pinMismatches = 0; // 两读方返回值 / 输出 / 计数任一不同的块数
};

// oracle 可用性:读方(以它的绑定快照布局 nch)按地址能读到的每个 float,都是写方在本走带时刻
// (或之后)为这个位置写下的、当前写方实例的数据。
bool oracleAvailable(const Ring& r, int64_t cR, int64_t t0, int n, u32 nch, u32 writer)
{
    if (nch != 1u && nch != 2u)
    {
        return false;
    }
    const float* data = r.data.data();
    const int64_t* writtenAt = r.writtenAt.data();
    const u32 mask = kRingFrames - 1;
    for (int i = 0; i < n; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(t0 + i)) & mask);
        for (u32 c = 0; c < nch; ++c)
        {
            const std::size_t idx = slot * nch + c;
            if (writtenAt[idx] < cR + i)
            {
                return false;
            }
            Tag t;
            if (!decodeTag(data[idx], t) || t.pos != t0 + i || t.ch != c || t.layout != nch || t.writer != writer)
            {
                return false;
            }
        }
    }
    return true;
}

// oracle 危险状态:本块里有没有环槽放着「同一环槽、另一圈」的数据(只看声道 0)。
bool blockHasLapAlias(const Ring& r, int64_t t0, int n, u32 nch)
{
    if (nch != 1u && nch != 2u)
    {
        return false;
    }
    const float* data = r.data.data();
    const u32 mask = kRingFrames - 1;
    for (int i = 0; i < n; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>(static_cast<u32>(static_cast<u64>(t0 + i)) & mask);
        Tag t;
        if (decodeTag(data[slot * nch], t) && t.layout == nch && t.pos != t0 + i && ((t.pos - (t0 + i)) % kRing) == 0)
        {
            return true;
        }
    }
    return false;
}

int countWrongFrames(const float* dst, int64_t t0, int n, u32 nch, u32 writer, std::string* first)
{
    int wrong = 0;
    for (int i = 0; i < n; ++i)
    {
        bool bad = false;
        for (u32 c = 0; c < nch; ++c)
        {
            const float got = dst[static_cast<std::size_t>(i) * nch + c];
            const Tag want{t0 + i, c, nch, writer};
            if (!frameMatches(got, want))
            {
                bad = true;
                if (first != nullptr && first->empty())
                {
                    std::ostringstream os;
                    os << "t0=" << t0 << " frame=" << i << " ch=" << c << " want{pos=" << want.pos
                       << " layout=" << want.layout << " writer=" << want.writer << "} got{" << describe(got) << "}";
                    *first = os.str();
                }
            }
        }
        if (bad)
        {
            ++wrong;
        }
    }
    return wrong;
}

// badKnown >= 0:调用方已经算好本块错帧数(两个读方交出的样本逐位相同),不再逐帧解码。
// 例外:本块有错帧而 firstWrong 还空着时照样逐帧解码一次,好把首个错帧记下来(每次运行最多一次),
// 免得旧读方的 firstWrong 因为一直走复用这条路而空着。
void account(ReaderStat& s, LapStat* lap, bool ok, bool avail, const float* dst, int64_t t0, int n, u32 nch, u32 writer,
             int badKnown = -1)
{
    ++s.reads;
    s.framesRead += n;
    if (avail)
    {
        s.availFrames += n;
        if (lap != nullptr)
        {
            lap->availFrames += n;
        }
        if (!ok)
        {
            s.lostFrames += n;
            if (lap != nullptr)
            {
                lap->lostFrames += n;
            }
        }
    }
    if (ok)
    {
        ++s.ok;
        const bool needFirst = s.firstWrong.empty();
        const int bad = (badKnown == 0 || (badKnown > 0 && !needFirst))
                            ? badKnown
                            : countWrongFrames(dst, t0, n, nch, writer, needFirst ? &s.firstWrong : nullptr);
        if (bad > 0)
        {
            ++s.wrongReads;
            s.wrongFrames += bad;
        }
    }
}

// 读方 [M] 线程的几何刷新(OutputSession.cpp:190-205):快照与段头不一致才重绑。两个读方各按自己的
// 快照判定、各自重绑。
template<typename Reader>
void refreshGeometry(Reader& reader)
{
    const scvb::AudioRingBinding* b = reader.acquire();
    if (b == nullptr || !b->bound || b->header == nullptr || b->data == nullptr)
    {
        return;
    }
    if (b->header->sample_rate == b->geo.sampleRate && b->header->channels == b->geo.channels &&
        b->header->ring_frames == b->geo.ringFrames)
    {
        return;
    }
    reader.bind(b->header, b->data);
}

Ring& sharedRing()
{
    static Ring r;
    return r;
}

void resetRing(Ring& r)
{
    std::fill(r.data.begin(), r.data.end(), 0.0f);
    std::fill(r.writtenAt.begin(), r.writtenAt.end(), kNever);
    r.header.sample_rate = 0;
    r.header.ring_frames = kRingFrames;
    r.header.channels = 0;
    r.header.write_head_samples.store(0, std::memory_order_release);
    r.header.epoch.store(0, std::memory_order_release);
}

RunResult runOne(const RunCfg& cfg)
{
    REQUIRE(cfg.writer.wellFormed());
    REQUIRE(cfg.reader.wellFormed());
    REQUIRE(cfg.readerBlock > 0);
    REQUIRE(cfg.writerBlock > 0);
    REQUIRE(cfg.writerMaxBlock > 0);

    RunResult res;
    res.name = cfg.name;
    res.knownBad = cfg.knownBad;
    Ring& ring = sharedRing();
    resetRing(ring);
    WriterModel w(ring, cfg.writerMaxBlock, cfg.chunk);
    std::deque<WOp> q;
    auto applyAll = [&] {
        while (!q.empty())
        {
            w.apply(q.front());
            q.pop_front();
        }
    };
    w.createFresh(cfg.channels, q);
    applyAll();

    // Output 侧 attach(OutputSession::attachAudioRings → ShmRingMixSource::bind)。
    scvb::output::ShmRingMixSource cur;
    LegacyReader leg;
    cur.bind(&ring.header, ring.data.data());
    leg.bind(&ring.header, ring.data.data());
    REQUIRE(cur.bound());

    std::mt19937 rng(cfg.seed);
    const std::size_t bufLen = static_cast<std::size_t>(cfg.readerBlock) * 2u;
    std::vector<float> dstCur(bufLen);
    std::vector<float> dstLeg(bufLen);

    int64_t cW = 0;
    int64_t cR = 0;
    std::size_t evIdx = 0;
    int64_t lastT0Out = kNever; // OutputProcessor.h:1087
    int64_t expectedNextOut = kNever; // OutputProcessor.h:1088
    u64 podEpoch = 0; // OutputProcessor.h:1089
    int64_t bumpsAtPodJump = 0;
    int64_t nextRefresh = cfg.refreshPeriod > 0 ? static_cast<int64_t>(rng() % static_cast<u32>(cfg.refreshPeriod)) : 0;

    auto genWriterBlock = [&] {
        while (evIdx < cfg.events.size() && cfg.events[evIdx].counter <= cW)
        {
            const WriterEvent& ev = cfg.events[evIdx];
            if (ev.kind == EvKind::kAttach)
            {
                w.attachExisting(ev.channels, q);
            }
            else
            {
                w.reprepare(ev.channels, q);
            }
            ++evIdx;
        }
        int64_t gapEnd = 0;
        if (cfg.writer.gapAt(cW, gapEnd))
        {
            cW = gapEnd;
            return;
        }
        int64_t edge = cfg.writer.nextEdge(cW);
        if (evIdx < cfg.events.size())
        {
            edge = std::min(edge, cfg.events[evIdx].counter);
        }
        const int len = static_cast<int>(std::min<int64_t>(cfg.writerBlock, edge - cW));
        bool playing = true;
        const int64_t t0 = cfg.writer.posAt(cW, playing);
        w.processBlock(t0, playing, len, cW, q);
        cW += len;
    };

    // 读方块也在写方的事件点与停调边界处切开:同步宿主给所有插件同一套块边界,重新 prepare /
    // 停调都发生在块边界上。对领先族这只是让读方块偶尔短一点,不改变任何语义。
    auto readerEdge = [&](int64_t c) {
        int64_t e = std::min(cfg.reader.nextEdge(c), cfg.readerEnd);
        for (const auto& g : cfg.writer.gaps)
        {
            if (g.first > c)
            {
                e = std::min(e, g.first);
            }
            if (g.second > c)
            {
                e = std::min(e, g.second);
            }
        }
        for (const auto& ev : cfg.events)
        {
            if (ev.counter > c)
            {
                e = std::min(e, ev.counter);
                break;
            }
        }
        return e;
    };
    // [M] 线程的几何刷新与 Input 的 prepareToPlay 都在消息线程上,彼此不会交错:写方几何改写的
    // 微操作还没做完时不刷新(音频线程上的读方照样可以落在它们中间)。
    auto geometryPending = [&] {
        return std::any_of(q.begin(), q.end(),
                           [](const WOp& o) { return o.kind == OpKind::kGeometry || o.kind == OpKind::kHeadZero; });
    };

    while (cR < cfg.readerEnd)
    {
        applyAll(); // 上一轮只做了一半的写方块,读方这块读完后写方接着做完
        int64_t gapEnd = 0;
        if (cfg.reader.gapAt(cR, gapEnd))
        {
            cR = gapEnd;
            continue;
        }
        const int64_t edge = readerEdge(cR);
        const int len = static_cast<int>(std::min<int64_t>(cfg.readerBlock, edge - cR));
        const int64_t jitter =
            cfg.leadJitter > 0 ? static_cast<int64_t>(rng() % static_cast<u32>(cfg.leadJitter + 1)) : 0;
        const int64_t need = cR + cfg.lead + jitter + len;
        bool partial = false;
        while (cW < need)
        {
            genWriterBlock();
            if (cfg.micro != Micro::kOff && cW >= need && !q.empty())
            {
                std::size_t k = static_cast<std::size_t>(rng() % static_cast<u32>(q.size() + 1));
                if (cfg.micro == Micro::kCutAfterBump)
                {
                    const auto bump =
                        std::find_if(q.begin(), q.end(), [](const WOp& o) { return o.kind == OpKind::kBump; });
                    if (bump != q.end())
                    {
                        k = static_cast<std::size_t>(bump - q.begin()) + 1u;
                    }
                }
                for (std::size_t j = 0; j < k; ++j)
                {
                    w.apply(q.front());
                    q.pop_front();
                }
                partial = !q.empty();
                break;
            }
            applyAll();
        }

        if (!geometryPending())
        {
            if (cfg.refreshPeriod == 0)
            {
                refreshGeometry(cur);
                refreshGeometry(leg);
            }
            else if (cR >= nextRefresh)
            {
                refreshGeometry(cur);
                refreshGeometry(leg);
                while (nextRefresh <= cR)
                {
                    nextRefresh += cfg.refreshPeriod;
                }
            }
        }

        bool playing = true;
        const int64_t t0 = cfg.reader.posAt(cR, playing);
        if (t0 >= 0)
        {
            // OutputProcessor.cpp:820-831:自身时间线代号(停走带静止 t0 不变不算跳变)。
            if (t0 != expectedNextOut && (playing || t0 != lastT0Out))
            {
                ++podEpoch;
                res.laps.push_back(LapStat{t0, 0, 0});
                bumpsAtPodJump = w.bumps();
            }
            lastT0Out = t0;
            expectedNextOut = t0 + len;
            if (w.bumps() > bumpsAtPodJump)
            {
                ++res.leadObservedReads;
            }

            const u32 nch = cur.channels();
            const u32 nchL = leg.channels();
            const u32 inst = w.appliedInstance();
            const bool avail = oracleAvailable(ring, cR, t0, len, nch, inst);
            const bool availL = nchL == nch ? avail : oracleAvailable(ring, cR, t0, len, nchL, inst);
            const u64 wNow = ring.header.write_head_samples.load(std::memory_order_acquire);
            if (!w.genOpen() && w.genStart() > t0 && wNow >= static_cast<u64>(t0 + len))
            {
                ++res.hazardReads;
            }
            if (w.genOpen())
            {
                ++res.bumpBeforeWriteReads;
            }
            if (nch != ring.header.channels)
            {
                ++res.staleSnapshotReads;
            }
            // 整块「可读」意味着每个环槽都是本位置的数据,不可能有别圈的同槽数据,不必再扫。
            if (!avail && blockHasLapAlias(ring, t0, len, nch))
            {
                ++res.lapAliasReads;
            }
            if (partial)
            {
                ++res.partialBlockReads;
            }

            std::memset(dstCur.data(), 0, dstCur.size() * sizeof(float));
            std::memset(dstLeg.data(), 0, dstLeg.size() * sizeof(float));
            const u32 gapC0 = cur.gapCount();
            const u32 stallC0 = cur.stallFailCount();
            const u32 gapL0 = leg.gapCount();
            const u32 stallL0 = leg.stallFailCount();
            const bool okC = readUnderTest(cur, t0, dstCur.data(), len, podEpoch);
            const bool okL = leg.read(t0, dstLeg.data(), len);

            LapStat* lap = res.laps.empty() ? nullptr : &res.laps.back();
            const int64_t wrongBefore = res.cur.wrongFrames;
            account(res.cur, lap, okC, avail, dstCur.data(), t0, len, nch, inst);
            const int badC = static_cast<int>(res.cur.wrongFrames - wrongBefore);
            const bool sameBytes = nchL == nch && std::memcmp(dstCur.data(), dstLeg.data(),
                                                              static_cast<std::size_t>(len) * nch * sizeof(float)) == 0;
            account(res.legacy, nullptr, okL, availL, dstLeg.data(), t0, len, nchL, inst,
                    (okL && okC && sameBytes) ? badC : -1);
            if (okL && !okC)
            {
                ++res.legacyOnlyOk;
            }
            if (okC && !okL)
            {
                ++res.curOnlyOk;
            }
            const bool sameCounts = (cur.gapCount() - gapC0) == (leg.gapCount() - gapL0) &&
                                    (cur.stallFailCount() - stallC0) == (leg.stallFailCount() - stallL0);
            if (okC != okL || !sameCounts || !sameBytes)
            {
                ++res.pinMismatches;
            }
        }
        cR += len;
    }
    applyAll();
    res.readerJumps = podEpoch > 0 ? static_cast<int64_t>(podEpoch) - 1 : 0;
    res.writerBumps = w.bumps();
    return res;
}

// ===========================================================================
// §7 汇总与打印
// ===========================================================================
std::string pct(int64_t num, int64_t den)
{
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << (den > 0 ? 100.0 * static_cast<double>(num) / static_cast<double>(den) : 100.0) << "%";
    return os.str();
}

// 最差一圈:可读帧里读到的比例最低的那一圈。
const LapStat* worstLap(const RunResult& r)
{
    const LapStat* worst = nullptr;
    double worstRate = 2.0;
    for (const auto& lap : r.laps)
    {
        if (lap.availFrames <= 0)
        {
            continue;
        }
        const double rate = 1.0 - static_cast<double>(lap.lostFrames) / static_cast<double>(lap.availFrames);
        if (rate < worstRate)
        {
            worstRate = rate;
            worst = &lap;
        }
    }
    return worst;
}

int64_t maxLapLoss(const RunResult& r)
{
    int64_t m = 0;
    for (const auto& lap : r.laps)
    {
        m = std::max(m, lap.lostFrames);
    }
    return m;
}

std::string summary(const RunResult& r)
{
    std::ostringstream os;
    const LapStat* wl = worstLap(r);
    os << r.name << ": reads=" << r.cur.reads << " ok=" << r.cur.ok << " wrongFrames=" << r.cur.wrongFrames
       << " wrongReads=" << r.cur.wrongReads << " availFrames=" << r.cur.availFrames
       << " lostFrames=" << r.cur.lostFrames
       << " availability=" << pct(r.cur.availFrames - r.cur.lostFrames, r.cur.availFrames) << " laps=" << r.laps.size()
       << " maxLapLoss=" << maxLapLoss(r)
       << " worstLap=" << (wl != nullptr ? pct(wl->availFrames - wl->lostFrames, wl->availFrames) : std::string("n/a"))
       << " readerJumps=" << r.readerJumps << " writerBumps=" << r.writerBumps
       << " leadObservedReads=" << r.leadObservedReads << " hazardReads=" << r.hazardReads
       << " bumpBeforeWriteReads=" << r.bumpBeforeWriteReads << " staleSnapshotReads=" << r.staleSnapshotReads
       << " lapAliasReads=" << r.lapAliasReads << " partialBlockReads=" << r.partialBlockReads
       << " legacyOnlyOk=" << r.legacyOnlyOk << " pinMismatches=" << r.pinMismatches;
    if (!r.cur.firstWrong.empty())
    {
        os << " firstWrong{" << r.cur.firstWrong << "}";
    }
    return os.str();
}

struct FamilyTotals
{
    int64_t reads = 0;
    int64_t wrongFrames = 0;
    int64_t availFrames = 0;
    int64_t lostFrames = 0;
    int64_t legacyWrongFrames = 0;
    int64_t legacyOnlyOk = 0;
    int64_t pinMismatches = 0;
};

FamilyTotals totals(const std::vector<RunResult>& rs)
{
    FamilyTotals t;
    for (const auto& r : rs)
    {
        t.reads += r.cur.reads;
        t.wrongFrames += r.cur.wrongFrames;
        t.availFrames += r.cur.availFrames;
        t.lostFrames += r.cur.lostFrames;
        t.legacyWrongFrames += r.legacy.wrongFrames;
        t.legacyOnlyOk += r.legacyOnlyOk;
        t.pinMismatches += r.pinMismatches;
    }
    return t;
}

// ===========================================================================
// §8 调度族
// ===========================================================================
constexpr int64_t kSec = kSampleRate;
constexpr int64_t kLeads[] = {1024, 4096, 48000};

// --- 同步族:同一份脚本、lead = 0、先 Input 后 Output、整块原子(图依赖成立的宿主)----------------
// 随机事件:回跳 / 前跳 / 循环 / 停走带 / 写方停调(无 region)/ 负位置(pre-roll 过零)/
// 同实例重新 prepare(换或不换布局)/ 新实例 attach(换或不换布局)。读方几何快照每块前刷新。
RunCfg makeSyncCfg(u32 seed)
{
    std::mt19937 rng(seed * 7919u + 17u);
    auto uni = [&rng](int64_t lo, int64_t hi) {
        return lo + static_cast<int64_t>(rng() % static_cast<u32>(hi - lo + 1));
    };
    static const int kBlocks[] = {64, 128, 256, 480, 512, 1024, 4096};
    RunCfg cfg;
    const int b = kBlocks[rng() % 7u];
    cfg.writerBlock = b;
    cfg.readerBlock = b;
    cfg.writerMaxBlock = (rng() % 3u == 0u) ? std::max(32, b / 3) : b; // SL-523:宿主块长超过 prepare 预算
    cfg.channels = (rng() % 2u == 0u) ? 2u : 1u;
    cfg.lead = 0;
    cfg.seed = seed;

    static const int64_t kStarts[] = {0, -383, -179, 12345, 200000};
    int64_t pos = kStarts[rng() % 5u];
    if (rng() % 4u == 0u)
    {
        pos = -static_cast<int64_t>(b / 2); // 块中间过零
    }
    Script s;
    s.seg(0, pos);
    int64_t c = uni(4 * b, 40000);
    u32 ch = cfg.channels;
    const int kEvents = 9;
    for (int k = 0; k < kEvents; ++k)
    {
        bool playing = true;
        const int64_t here = s.posAt(c, playing);
        const u32 kind = rng() % 9u;
        if (kind == 0u)
        {
            s.seg(c, uni(0, 3 * kRing)); // 定位(前跳或回跳)
            c += uni(4 * b, 60000);
        }
        else if (kind == 1u)
        {
            const int64_t bLoop = uni(0, 2 * kRing);
            const int64_t len = uni(4 * b, 200000);
            const int64_t laps = uni(2, 3);
            s.loop(c, bLoop, bLoop, bLoop + len, c + laps * len);
            c += laps * len;
            s.seg(c, bLoop + len); // 出循环,接着往后放
            c += uni(4 * b, 20000);
        }
        else if (kind == 2u)
        {
            s.seg(c, std::max<int64_t>(here, 0), false); // 停走带
            c += uni(b, 20000);
            s.seg(c, std::max<int64_t>(here, 0)); // 原地起播
            c += uni(4 * b, 20000);
        }
        else if (kind == 3u)
        {
            const int64_t g = uni(b, 50000);
            s.gap(c, c + g); // 写方停调(无 region);同步宿主下读方照常跑
            c += g + uni(4 * b, 20000);
        }
        else if (kind == 4u)
        {
            s.seg(c, -uni(1, 5000)); // 定位到负位置:pre-roll 后过零
            c += uni(5000 + 4 * b, 30000);
        }
        else if (kind == 5u || kind == 6u)
        {
            ch = (kind == 5u) ? (ch == 1u ? 2u : 1u) : ch; // 5 = 换布局(几何重写),6 = 只重新 prepare
            cfg.events.push_back(WriterEvent{c, EvKind::kReprepare, ch});
            c += uni(4 * b, 40000);
        }
        else
        {
            if (kind == 8u)
            {
                ch = (ch == 1u ? 2u : 1u);
            }
            cfg.events.push_back(WriterEvent{c, EvKind::kAttach, ch});
            c += uni(4 * b, 40000);
        }
    }
    // 写方停调的 gap 只给写方;同步宿主下读方照常被调用(读不到 = 写头停滞)。
    cfg.reader.segs = s.segs;
    cfg.writer = s;
    cfg.readerEnd = c;
    std::ostringstream os;
    os << "sync seed=" << seed << " block=" << b << " seg=" << cfg.writerMaxBlock << " ch0=" << cfg.channels;
    cfg.name = os.str();
    return cfg;
}

std::vector<RunResult> runSyncFamily()
{
    std::vector<RunResult> out;
    for (u32 seed = 1; seed <= 16; ++seed)
    {
        out.push_back(runOne(makeSyncCfg(seed)));
    }
    return out;
}

// --- 写方领先的循环(H1 / LS-4)-------------------------------------------------------------------
// 两条 lane 同一份走带脚本(同一首歌),写方在走带计数上领先 Δ:写方先到循环终点、先回绕换代,
// 读方 Δ 之后才回绕。被领先轨的「整圈无声」就出在这里。
std::vector<RunResult> runLeadLoopFamily()
{
    std::vector<RunResult> out;
    static const int64_t kLoopLens[] = {2 * kSec, 8 * kSec, 12 * kSec};
    for (const int64_t lead : kLeads)
    {
        for (const int64_t len : kLoopLens)
        {
            RunCfg cfg;
            const int64_t b = 3 * kSec;
            cfg.writer.loop(0, b, b, b + len, 4 * len + lead + 4096);
            cfg.reader = cfg.writer;
            cfg.lead = lead;
            cfg.readerEnd = 3 * len;
            std::ostringstream os;
            os << "lead-loop lead=" << lead << " loop=" << len;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- 写方领先时的定位(LS-6 回跳 / LS-7 前跳)--------------------------------------------------
// 同一份脚本,走带计数 cs 处定位到 T(cs) ± d。写方先到 cs、先换代并往前写;读方还要再放 Δ 的
// 旧位置才轮到它自己跳。定位后默认再放 d + Δ + 60000(回跳的格要放回原位置之后);tail >= 0 时
// 只放 tail(只关心定位前后那一段的格用它省时间)。
RunCfg makeSeekCfg(int64_t lead, int64_t d, bool back, const char* tag, int64_t tail = -1)
{
    RunCfg cfg;
    const int64_t p0 = 1000000;
    const int64_t cs = 100000;
    const int64_t here = p0 + cs;
    const int64_t target = back ? here - d : here + d;
    cfg.writer.seg(0, p0).seg(cs, target);
    cfg.reader = cfg.writer;
    cfg.lead = lead;
    cfg.readerEnd = cs + (tail >= 0 ? tail : d + lead + 60000);
    std::ostringstream os;
    os << tag << " lead=" << lead << " d=" << (back ? "-" : "+") << d;
    cfg.name = os.str();
    return cfg;
}

std::vector<RunResult> runLeadSeekFamily(bool back)
{
    std::vector<RunResult> out;
    for (const int64_t lead : kLeads)
    {
        // 起播位置 1000000、走带计数 100000 处定位:回跳 80000 落在放过的区间里,回跳 400000 落在
        // 从没放过的位置;前跳同样落在没放过的位置。
        // 回跳超过 Δ/2 时,定位目标落在读方锚点(读方在旧位置上看到换代时的 t0)之前 —— 现行实现
        // 读不到,直到读方重新走过锚点(LS-6),这几格 knownBad;回跳 Δ/2 落在锚点之后,不丢。
        const int64_t ds[] = {lead / 2, 2 * lead + 3000, 80000, 400000};
        for (const int64_t d : ds)
        {
            RunCfg cfg = makeSeekCfg(lead, d, back, back ? "lead-seek-back" : "lead-seek-fwd");
            cfg.knownBad = back && d > lead / 2;
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- 写方领先时从停调恢复(H4 / LS-12 领先)--------------------------------------------------------
// 写方在走带计数 [g0, g1) 内不被调用(无 region / 宿主挂起 Input),读方照常走;写方恢复时换代,
// 读方此时还落后 Δ。
std::vector<RunResult> runLeadResumeFamily()
{
    std::vector<RunResult> out;
    static const int64_t kResumeLeads[] = {1024, 8192, 48000};
    static const int64_t kGaps[] = {1 * kSec, 6 * kSec};
    for (const int64_t lead : kResumeLeads)
    {
        for (const int64_t g : kGaps)
        {
            RunCfg cfg;
            const int64_t g0 = 200000;
            cfg.writer.seg(0, 200000).gap(g0, g0 + g);
            cfg.reader.seg(0, 200000);
            cfg.lead = lead;
            cfg.readerEnd = g0 + g + 3 * lead + 20000;
            std::ostringstream os;
            os << "lead-resume lead=" << lead << " gap=" << g;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- Δ 内两次定位(LS-13)-------------------------------------------------------------------------
// 写方在读方还没赶到第一次定位之前就又定位了一次:读方观测到的是两次叠加的换代。
std::vector<RunResult> runTwoSeekFamily()
{
    std::vector<RunResult> out;
    static const int64_t kTwoLeads[] = {4096, 48000};
    static const int64_t kD[] = {-100000, -30000, 30000, 100000};
    for (const int64_t lead : kTwoLeads)
    {
        for (const int64_t d1 : kD)
        {
            for (const int64_t d2 : kD)
            {
                RunCfg cfg;
                const int64_t p0 = 1000000;
                const int64_t c1 = 100000;
                const int64_t c2 = c1 + lead / 2;
                const int64_t s1 = p0 + c1 + d1;
                const int64_t s2 = s1 + (c2 - c1) + d2;
                cfg.writer.seg(0, p0).seg(c1, s1).seg(c2, s2);
                cfg.reader = cfg.writer;
                cfg.lead = lead;
                cfg.readerEnd = c2 + 3 * lead + 150000;
                std::ostringstream os;
                os << "two-seeks lead=" << lead << " d1=" << d1 << " d2=" << d2;
                cfg.name = os.str();
                out.push_back(runOne(cfg));
            }
        }
    }
    return out;
}

// --- Δ 内两次定位,第一次往前跳将近一个环距(LS-13「回跳 ≈ R−Δ」的组合形态)-------------------------
// 走带计数 c1 处(写方位置 X)往前定位到 S1 = X + R − Δ + m:写方随后写的环槽恰好是读方前方 m 帧
// 处的环槽,里面换成了「下一圈」的数据(位置 + R)。写方 Δ/2 之后又定位回来(S2,撤销式 X + Δ/2
// 或往回 X − Δ/4),写头回到读方前方不足一个环距的地方。读方在旧位置上看到第二次换代、锚在自己的
// t0,`w − t0 <= R` 这道套圈判据此时已经拦不住:读方前方那 m 帧的环槽里是下一圈的数据。
std::vector<RunResult> runTwoSeekAcrossRingFamily()
{
    std::vector<RunResult> out;
    static const int64_t kAcrossLeads[] = {4096, 48000};
    for (const int64_t lead : kAcrossLeads)
    {
        for (const int64_t m : {lead / 8, lead / 4})
        {
            for (const bool undo : {true, false})
            {
                RunCfg cfg;
                const int64_t p0 = 1000000;
                const int64_t c1 = 100000;
                const int64_t h = lead / 2;
                const int64_t c2 = c1 + h;
                const int64_t x = p0 + c1;
                const int64_t s1 = x + kRing - lead + m;
                const int64_t s2 = undo ? x + h : x - lead / 4;
                cfg.writer.seg(0, p0).seg(c1, s1).seg(c2, s2);
                cfg.reader = cfg.writer;
                cfg.lead = lead;
                cfg.readerEnd = c2 + 3 * lead + 60000;
                std::ostringstream os;
                os << "two-seeks-across-ring lead=" << lead << " m=" << m
                   << (undo ? " back-to=X+lead/2" : " back-to=X-lead/4");
                cfg.name = os.str();
                out.push_back(runOne(cfg));
            }
        }
    }
    return out;
}

// --- 读写目标不一致(LS-8 / LS-13)------------------------------------------------------------------
// 同一次定位(从 1100000 回跳到 400000,那里从没放过),Output 拿到的目标比 Input 的偏 ε(宿主
// 起播时间戳怪癖、各插件取到的位置不一致)。分两支:
//   · 同块跳变(lead = 0):读方在自己跳变的那一块观测到换代,锚在 S+ε;
//   · 写方领先(lead > 0):读方在还没跳的旧位置上先观测到换代,锚在旧位置 —— 现行实现里
//     validFrom_ 唯一真正挡下错读的地方就在这一支(锚点 > S+ε,于是 [S+ε, S) 一直读不出来)。
std::vector<RunResult> runMismatchFamily(bool leading)
{
    std::vector<RunResult> out;
    static const int64_t kSyncLead[] = {0};
    static const int64_t kLeadingLeads[] = {1024, 4096};
    static const int64_t kEps[] = {-4096, -1868, -179, 179, 1868};
    const int64_t* leads = leading ? kLeadingLeads : kSyncLead;
    const std::size_t nLeads = leading ? 2u : 1u;
    for (std::size_t li = 0; li < nLeads; ++li)
    {
        const int64_t lead = leads[li];
        for (const int64_t eps : kEps)
        {
            RunCfg cfg;
            const int64_t p0 = 1000000;
            const int64_t cs = 100000;
            const int64_t s = 400000;
            cfg.writer.seg(0, p0).seg(cs, s);
            cfg.reader.seg(0, p0).seg(cs, s + eps);
            cfg.lead = lead;
            cfg.readerEnd = cs + 100000;
            std::ostringstream os;
            os << "mismatch lead=" << lead << " eps=" << eps;
            // 同块跳变、Output 目标在写方目标之前(ε < 0):[S+ε, S) 从没写过,而读方锚在 S+ε、写头已
            // 覆盖 ⇒ 现行实现交出没写过的数据。ε > 0 时读方要的位置写方都写过(或还没覆盖到就读不到)。
            cfg.knownBad = !leading && eps < 0;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- Output 起播第一块拿到离谱的大 t0(H1' / LS-8)------------------------------------------------
std::vector<RunResult> runGarbageStartFamily()
{
    std::vector<RunResult> out;
    static const int64_t kGarbage[] = {540544, 3 * 540544};
    for (const int64_t garbage : kGarbage)
    {
        for (const int k : {1, 2})
        {
            RunCfg cfg;
            const int64_t p0 = 100000;
            cfg.writer.seg(0, p0);
            cfg.reader.seg(0, garbage).seg(static_cast<int64_t>(k) * cfg.readerBlock, p0 + k * cfg.readerBlock);
            cfg.lead = 0;
            cfg.readerEnd = 4 * kSec;
            std::ostringstream os;
            os << "garbage-start t0=" << garbage << " blocks=" << k;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- 几何重写,读方快照按 25 Hz 刷新(SL-486 的读侧窗口)------------------------------------------
std::vector<RunResult> runGeometryFamily()
{
    std::vector<RunResult> out;
    static const int64_t kGeoLeads[] = {0, 4096};
    for (const int64_t lead : kGeoLeads)
    {
        for (u32 seed = 1; seed <= 4; ++seed)
        {
            RunCfg cfg;
            cfg.writer.seg(0, 100000);
            cfg.reader = cfg.writer;
            cfg.lead = lead;
            cfg.channels = 1;
            cfg.refreshPeriod = kSampleRate / 25; // [M] 25 Hz
            cfg.seed = seed;
            u32 ch = 1;
            for (int64_t c = 50000; c < 600000; c += 80000 + 7919 * static_cast<int64_t>(seed))
            {
                ch = ch == 1u ? 2u : 1u;
                cfg.events.push_back(WriterEvent{c, (c / 80000) % 3 == 0 ? EvKind::kAttach : EvKind::kReprepare, ch});
            }
            cfg.readerEnd = 640000;
            std::ostringstream os;
            os << "geometry-25hz lead=" << lead << " seed=" << seed;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// --- 接近一个环距的跳变(LS-13「回跳 ≈ R−Δ」及其前跳镜像)----------------------------------------
std::vector<RunResult> runNearRingFamily()
{
    std::vector<RunResult> out;
    for (const int64_t lead : kLeads)
    {
        // 危险状态只出现在定位前后各约 Δ 的那一段,定位后放 3Δ + 60000 就够。
        const int64_t tail = 3 * lead + 60000;
        out.push_back(runOne(makeSeekCfg(lead, kRing - lead, true, "near-ring-back", tail)));
        out.push_back(runOne(makeSeekCfg(lead, kRing - lead / 2, true, "near-ring-back", tail)));
        out.push_back(runOne(makeSeekCfg(lead, kRing - 3 * lead, false, "near-ring-fwd", tail)));
        out.push_back(runOne(makeSeekCfg(lead, kRing - 2 * lead - 1000, false, "near-ring-fwd", tail)));
        out.push_back(runOne(makeSeekCfg(lead, kRing - lead, false, "near-ring-fwd", tail)));
    }
    return out;
}

// --- bump-before-write 交错:读方落在写方一块的中间 ---------------------------------------------
// 两支:循环(新一代写的是上一圈写过的同一批位置)与回跳到从没放过的位置(旧写头仍覆盖它)。
// 每个 lead 跑 3 个随机前缀种子 + 1 个「恰好停在换代之后」(Micro::kCutAfterBump):后者让读方
// 确定性地看到「epoch 已 +1、写头还是上一代的值」(InputProcessor.cpp:270 与 :296 之间)。
std::vector<RunResult> runInterleaveFamily(bool seekBack)
{
    std::vector<RunResult> out;
    static const int64_t kIlLeads[] = {0, 1024, 4096};
    for (const int64_t lead : kIlLeads)
    {
        for (u32 seed = 1; seed <= 4; ++seed)
        {
            RunCfg cfg;
            if (seekBack)
            {
                // 回跳到从没放过、且离旧写头不足一个环距的位置(旧写头仍「覆盖」它)
                cfg.writer.seg(0, 1000000).seg(100000, 900000);
                cfg.readerEnd = 250000;
            }
            else
            {
                cfg.writer.loop(0, 3 * kSec, 3 * kSec, 5 * kSec, 9 * kSec);
                cfg.readerEnd = 6 * kSec;
            }
            cfg.reader = cfg.writer;
            cfg.lead = lead;
            cfg.leadJitter = lead / 4;
            cfg.micro = seed <= 3 ? Micro::kRandomPrefix : Micro::kCutAfterBump;
            // 同块回跳(Δ = 0)且读时写方恰好换代了还没写:读方锚在自己的 t0,上一代写头仍覆盖这一块。
            cfg.knownBad = seekBack && lead == 0 && cfg.micro == Micro::kCutAfterBump;
            cfg.chunk = 256;
            cfg.seed = seed;
            std::ostringstream os;
            os << (seekBack ? "interleave-seek-back" : "interleave-loop") << " lead=" << lead
               << (seed <= 3 ? " random-prefix seed=" : " cut-after-bump seed=") << seed;
            cfg.name = os.str();
            out.push_back(runOne(cfg));
        }
    }
    return out;
}

// 每族只跑一次,同族的几个用例共用结果。
const std::vector<RunResult>& syncFamily()
{
    static const std::vector<RunResult> r = runSyncFamily();
    return r;
}
const std::vector<RunResult>& leadLoopFamily()
{
    static const std::vector<RunResult> r = runLeadLoopFamily();
    return r;
}
const std::vector<RunResult>& leadSeekBackFamily()
{
    static const std::vector<RunResult> r = runLeadSeekFamily(true);
    return r;
}
const std::vector<RunResult>& leadSeekFwdFamily()
{
    static const std::vector<RunResult> r = runLeadSeekFamily(false);
    return r;
}
const std::vector<RunResult>& leadResumeFamily()
{
    static const std::vector<RunResult> r = runLeadResumeFamily();
    return r;
}
const std::vector<RunResult>& twoSeekFamily()
{
    static const std::vector<RunResult> r = runTwoSeekFamily();
    return r;
}
const std::vector<RunResult>& twoSeekAcrossRingFamily()
{
    static const std::vector<RunResult> r = runTwoSeekAcrossRingFamily();
    return r;
}
const std::vector<RunResult>& mismatchSyncFamily()
{
    static const std::vector<RunResult> r = runMismatchFamily(false);
    return r;
}
const std::vector<RunResult>& mismatchLeadFamily()
{
    static const std::vector<RunResult> r = runMismatchFamily(true);
    return r;
}
const std::vector<RunResult>& nearRingFamily()
{
    static const std::vector<RunResult> r = runNearRingFamily();
    return r;
}
const std::vector<RunResult>& garbageStartFamily()
{
    static const std::vector<RunResult> r = runGarbageStartFamily();
    return r;
}
const std::vector<RunResult>& geometryFamily()
{
    static const std::vector<RunResult> r = runGeometryFamily();
    return r;
}
const std::vector<RunResult>& interleaveLoopFamily()
{
    static const std::vector<RunResult> r = runInterleaveFamily(false);
    return r;
}
const std::vector<RunResult>& interleaveSeekFamily()
{
    static const std::vector<RunResult> r = runInterleaveFamily(true);
    return r;
}

struct NamedFamily
{
    const char* name;
    const std::vector<RunResult>& (*get)();
};

const NamedFamily kAllFamilies[] = {
    {"sync", &syncFamily},
    {"lead-loop", &leadLoopFamily},
    {"lead-seek-back", &leadSeekBackFamily},
    {"lead-seek-fwd", &leadSeekFwdFamily},
    {"lead-resume", &leadResumeFamily},
    {"two-seeks", &twoSeekFamily},
    {"two-seeks-across-ring", &twoSeekAcrossRingFamily},
    {"mismatch-sync", &mismatchSyncFamily},
    {"mismatch-lead", &mismatchLeadFamily},
    {"near-ring", &nearRingFamily},
    {"garbage-start", &garbageStartFamily},
    {"geometry-25hz", &geometryFamily},
    {"interleave-loop", &interleaveLoopFamily},
    {"interleave-seek", &interleaveSeekFamily},
};

} // namespace prov
} // namespace

using namespace prov;

// ===========================================================================
// 报告(隐藏):打印全部族的数字,供 PR 引用。ctest 不跑。
// ===========================================================================
TEST_CASE("PROVENANCE report: per-family numbers for every schedule", "[.][provenance-report]")
{
    for (const NamedFamily& f : kAllFamilies)
    {
        const std::vector<RunResult>& fam = f.get();
        const FamilyTotals t = totals(fam);
        std::ostringstream os;
        os << "== family " << f.name << ": runs=" << fam.size() << " reads=" << t.reads
           << " wrongFrames=" << t.wrongFrames << " availability=" << pct(t.availFrames - t.lostFrames, t.availFrames)
           << " (lost " << t.lostFrames << " of " << t.availFrames << ") legacyWrongFrames=" << t.legacyWrongFrames
           << " legacyOnlyOk=" << t.legacyOnlyOk << " pinMismatches=" << t.pinMismatches << "\n";
        for (const auto& r : fam)
        {
            os << "   " << summary(r) << "\n";
        }
        WARN(os.str());
    }
    SUCCEED();
}

// ===========================================================================
// 仪器自测:tag 编码、写方模型、oracle 判据本身(尺子先要准)
// ===========================================================================
TEST_CASE("PROVENANCE oracle: the tag codec round-trips every field and a never-written slot decodes as invalid",
          "[mix][provenance]")
{
    static const int64_t kPos[] = {0, 1, 1023, kRing - 1, kRing, 3 * kRing + 7, kTagPosLimit - 1};
    int roundTrips = 0;
    bool allOk = true;
    bool allNormal = true;
    for (const int64_t p : kPos)
    {
        for (u32 ch = 0; ch < 2u; ++ch)
        {
            for (u32 layout = 1; layout <= 2u; ++layout)
            {
                for (u32 wr = 0; wr < 8u; ++wr)
                {
                    const Tag want{p, ch, layout, wr};
                    const float f = encodeTag(want);
                    Tag got;
                    allOk = allOk && decodeTag(f, got) && got == want && frameMatches(f, want);
                    allNormal = allNormal && std::fpclassify(f) == FP_NORMAL && f > 0.0f;
                    ++roundTrips;
                }
            }
        }
    }
    CHECK(roundTrips == 7 * 2 * 2 * 8);
    CHECK(allOk);
    CHECK(allNormal); // 正规正数:按 float 赋值拷贝逐位不变

    Tag t;
    CHECK_FALSE(decodeTag(0.0f, t)); // 环初值 = 从未写过
    CHECK_FALSE(decodeTag(-encodeTag(Tag{5, 0, 2, 0}), t));

    // I1 比较的三个字段各改一处,都要判不等(不能只看 pos);写方实例号不同仍判相等(I1 不分代)。
    const Tag base{4242, 1, 2, 3};
    const float f = encodeTag(base);
    CHECK(frameMatches(f, base));
    CHECK_FALSE(frameMatches(f, Tag{4242 + kRing, 1, 2, 3})); // 同一环槽的下一圈
    CHECK_FALSE(frameMatches(f, Tag{4243, 1, 2, 3}));
    CHECK_FALSE(frameMatches(f, Tag{4242, 0, 2, 3}));
    CHECK_FALSE(frameMatches(f, Tag{4242, 1, 1, 3}));
    CHECK(frameMatches(f, Tag{4242, 1, 2, 4}));
    CHECK_FALSE(frameMatches(0.0f, base));
}

TEST_CASE("PROVENANCE writer model: its ops leave the same ring bytes as the real AudioRing::write and bumpEpoch",
          "[mix][provenance]")
{
    // 同一串调用:A 环按写方模型的微操作执行;B 环把同一串微操作翻成真实的 scvb::AudioRing 调用
    // (一段 kWrite… + kPublish ⇒ AudioRing::write,kBump ⇒ AudioRing::bumpEpoch;几何改写两步照
    // InputSession.cpp:517-521 直写段头后重绑)。两环逐字节相同 = 模型的寻址、声道交错、写头发布值、
    // 换代都与真实写侧一致。chunk = 100 再跑一遍:写数据切成多段后结果不变。
    // 注:B 环在 kGeometry 那一步就重绑,真实代码是在 epoch+1 之后才重发布写方快照(InputSession.cpp:521)。
    // 两步之间没有写操作,字节结果相同 —— 所以这条用例**不**验证「几何 → w=0 → epoch+1」三步的顺序;
    // 顺序由下一条用例的 kinds() 逐项钉住。
    for (const int chunk : {0, 100})
    {
        INFO("chunk=" << chunk);
        Ring& a = sharedRing();
        resetRing(a);
        auto twin = std::make_unique<Ring>();
        Ring& b = *twin;
        scvb::AudioRing realRing;

        WriterModel wm(a, 300, chunk);
        std::deque<WOp> q;
        std::vector<float> pending;
        int64_t pendingStart = kNever;
        u32 pendingCh = 0;
        int ops = 0;
        auto drain = [&] {
            while (!q.empty())
            {
                const WOp o = q.front();
                q.pop_front();
                wm.apply(o);
                ++ops;
                switch (o.kind)
                {
                case OpKind::kBump:
                    scvb::AudioRing::bumpEpoch(realRing.acquire());
                    break;
                case OpKind::kWrite:
                    if (pendingStart == kNever)
                    {
                        pendingStart = o.pos;
                        pendingCh = o.channels;
                        pending.clear();
                    }
                    for (int i = 0; i < o.n; ++i)
                    {
                        for (u32 c = 0; c < o.channels; ++c)
                        {
                            pending.push_back(encodeTag(Tag{o.pos + i, c, o.channels, o.writer}));
                        }
                    }
                    break;
                case OpKind::kPublish: {
                    const int n = static_cast<int>(pending.size() / pendingCh);
                    CHECK(o.pos == pendingStart + n);
                    scvb::AudioRing::write(realRing.acquire(), pendingStart, pending.data(), n);
                    pendingStart = kNever;
                    break;
                }
                case OpKind::kGeometry:
                    b.header.sample_rate = static_cast<u32>(kSampleRate);
                    b.header.ring_frames = kRingFrames;
                    b.header.channels = o.channels;
                    realRing.bind(&b.header, b.data.data());
                    break;
                case OpKind::kHeadZero:
                    b.header.write_head_samples.store(0, std::memory_order_release);
                    break;
                case OpKind::kAttach:
                    break;
                }
            }
        };
        auto same = [&] {
            return a.data == b.data && a.header.channels == b.header.channels &&
                   a.header.write_head_samples.load() == b.header.write_head_samples.load() &&
                   a.header.epoch.load() == b.header.epoch.load();
        };

        wm.createFresh(2, q);
        drain();
        REQUIRE(realRing.bound());
        CHECK(same());

        struct Blk
        {
            int64_t t0;
            bool playing;
            int n;
        };
        // 正常推进、跳变、停走带重写、负 t0 过零、超过 preparedMaxBlock 的分段块、跨环尾(槽位回绕)。
        static const Blk kBlocks[] = {{1000, true, 256},  {1256, true, 700},        {9000, true, 256},
                                      {9256, false, 256}, {9256, false, 256},       {-150, true, 400},
                                      {250, true, 1024},  {kRing - 100, true, 300}, {kRing + 200, true, 256}};
        for (const Blk& blk : kBlocks)
        {
            wm.processBlock(blk.t0, blk.playing, blk.n, 0, q);
            drain();
            CHECK(same());
        }
        // 同实例换成 mono(rebuildAudioGeometry),再接着写;再换一个 stereo 的新实例 attach。
        wm.reprepare(1, q);
        drain();
        wm.processBlock(kRing + 456, true, 512, 0, q);
        drain();
        CHECK(same());
        wm.attachExisting(2, q);
        drain();
        wm.processBlock(kRing + 968, true, 512, 0, q);
        drain();
        CHECK(same());
        CHECK(ops > 30);
    }
}

TEST_CASE("PROVENANCE writer model: jump detection and segment writes follow InputProcessor", "[mix][provenance]")
{
    Ring& r = sharedRing();
    resetRing(r);
    std::deque<WOp> q;
    auto kinds = [&q] {
        std::vector<OpKind> k;
        for (const WOp& o : q)
        {
            k.push_back(o.kind);
        }
        return k;
    };
    auto epoch = [&r] { return r.header.epoch.load(); };
    auto head = [&r] { return r.header.write_head_samples.load(); };

    WriterModel wm(r, 1024, 0);
    auto drainWith = [&q](WriterModel& m) {
        while (!q.empty())
        {
            m.apply(q.front());
            q.pop_front();
        }
    };

    // InputSession.cpp:428-437:新段 = 几何 → w=0 → epoch+1。
    wm.createFresh(2, q);
    CHECK(kinds() == std::vector<OpKind>{OpKind::kGeometry, OpKind::kHeadZero, OpKind::kBump});
    drainWith(wm);
    CHECK(epoch() == 1u);
    CHECK(head() == 0u);
    CHECK(r.header.channels == 2u);

    struct Step
    {
        int64_t t0;
        bool playing;
        int n;
        u64 epoch;
        u64 head;
        const char* what;
    };
    static const Step kSteps[] = {
        {1000, true, 256, 2, 1256, "first block after prepare: sentinel lowest() => bump (InputProcessor.cpp:99-100)"},
        {1256, true, 256, 2, 1512, "continuous block: no bump"},
        {5000, true, 256, 3, 5256, "jump while playing: bump"},
        {5256, false, 256, 3, 5512, "stop exactly at expectedNext: no bump"},
        {5256, false, 256, 3, 5512, "stopped, same t0 rewritten: no bump (InputProcessor.cpp:268)"},
        {7000, false, 256, 4, 7256, "stopped, t0 moved (locate while stopped): bump"},
        {-100, true, 256, 5, 156, "zero crossing: writeTailFromZero bumps and writes [0,156)"},
        {156, true, 256, 5, 412, "continue from the zero-crossing tail: no bump"},
        {-300, true, 256, 5, 412, "block entirely before zero: no write, no bump"},
        {-44, true, 256, 6, 212, "zero crossing again (expectedNext != 0): bump"},
    };
    for (const Step& s : kSteps)
    {
        INFO(s.what);
        wm.processBlock(s.t0, s.playing, s.n, 0, q);
        drainWith(wm);
        CHECK(epoch() == s.epoch);
        CHECK(head() == s.head);
    }

    // SL-523 分段:块长 256 > preparedMaxBlock 100 ⇒ 三段,每段各发布一次写头(InputProcessor.cpp:292-298)。
    WriterModel segw(r, 100, 0);
    segw.createFresh(2, q);
    drainWith(segw);
    segw.processBlock(1000, true, 256, 0, q);
    CHECK(kinds() == std::vector<OpKind>{OpKind::kBump, OpKind::kWrite, OpKind::kPublish, OpKind::kWrite,
                                         OpKind::kPublish, OpKind::kWrite, OpKind::kPublish});
    CHECK((q.size() == 7u && q[2].pos == 1100 && q[4].pos == 1200 && q[6].pos == 1256));
    drainWith(segw);
    // 负 t0 的分段块(InputProcessor.cpp:226-253):[-150,-50) 不写;[-50,50) 过零 ⇒ 换代 + 写 [0,50),
    // expectedNext_ = 50;其后两段只补写环。下一块 t0 = 250 != 50 ⇒ 再换代(:228-233 注释的行为)。
    const u64 e0 = epoch();
    segw.processBlock(-150, true, 400, 0, q);
    drainWith(segw);
    CHECK(epoch() == e0 + 1u);
    CHECK(head() == 250u);
    segw.processBlock(250, true, 256, 0, q);
    drainWith(segw);
    CHECK(epoch() == e0 + 2u);
    CHECK(head() == 506u);

    // 同实例重新 prepare、布局不变:不动环,只复位哨兵 ⇒ 下一块(位置连续)也换代。
    segw.reprepare(2, q);
    CHECK(q.empty());
    segw.processBlock(506, true, 256, 0, q);
    drainWith(segw);
    CHECK(epoch() == e0 + 3u);
    // 布局变了(rebuildAudioGeometry,InputSession.cpp:517-520):几何 → w=0 → epoch+1,顺序固定。
    segw.reprepare(1, q);
    CHECK(kinds() == std::vector<OpKind>{OpKind::kGeometry, OpKind::kHeadZero, OpKind::kBump});
    drainWith(segw);
    CHECK(r.header.channels == 1u);
    CHECK(head() == 0u);
    CHECK(epoch() == e0 + 4u);
    segw.processBlock(762, true, 256, 0, q);
    drainWith(segw);
    CHECK(epoch() == e0 + 5u); // 哨兵复位 ⇒ 位置连续也换代
    CHECK(head() == 1018u);
    // 新实例 attach,段头几何与请求相符:一个字节都不写(InputSession.cpp:460 的按值比对)。
    segw.attachExisting(1, q);
    CHECK(kinds() == std::vector<OpKind>{OpKind::kAttach});
    drainWith(segw);
    CHECK(segw.appliedInstance() == 1u);
    // 新实例 attach,几何不符:走同一条「几何 → w=0 → epoch+1」(InputSession.cpp:460-467)。
    segw.attachExisting(2, q);
    CHECK(kinds() == std::vector<OpKind>{OpKind::kAttach, OpKind::kGeometry, OpKind::kHeadZero, OpKind::kBump});
    drainWith(segw);
    CHECK(segw.appliedInstance() == 2u);
    CHECK(r.header.channels == 2u);
}

TEST_CASE("PROVENANCE oracle: the checks flag a planted stale frame, a wrong layout and a stale write",
          "[mix][provenance]")
{
    Ring& r = sharedRing();
    resetRing(r);
    WriterModel wm(r, 1024, 0);
    std::deque<WOp> q;
    wm.createFresh(2, q);
    wm.processBlock(5000, true, 256, 777, q);
    while (!q.empty())
    {
        wm.apply(q.front());
        q.pop_front();
    }
    scvb::output::ShmRingMixSource src;
    src.bind(&r.header, r.data.data());
    REQUIRE(src.bound());
    std::vector<float> dst(512, 0.0f);

    REQUIRE(readUnderTest(src, 5000, dst.data(), 256, 1));
    std::string first;
    CHECK(countWrongFrames(dst.data(), 5000, 256, 2, 0, &first) == 0);
    CHECK(first.empty());
    CHECK(oracleAvailable(r, 777, 5000, 256, 2, 0));
    CHECK_FALSE(oracleAvailable(r, 778, 5000, 256, 2, 0)); // 要更晚的走带时刻写下的数据:不算新鲜
    CHECK_FALSE(oracleAvailable(r, 777, 5000, 256, 2, 1)); // 写方实例不符
    CHECK_FALSE(oracleAvailable(r, 777, 5000, 256, 1, 0)); // 读方布局不符
    CHECK_FALSE(oracleAvailable(r, 777, 4999, 256, 2, 0)); // 整块错位 1 帧

    // 读方按 mono 解 stereo 数据(几何快照过期):每一帧都错。
    CHECK(countWrongFrames(dst.data(), 5000, 256, 1, 0, nullptr) == 256);

    // 写了还没发布:数据已在环里,但读方按协议看不见 ⇒ 不算可读;发布之后才算。
    wm.processBlock(5256, true, 256, 1033, q);
    REQUIRE(q.size() == 2u); // 位置连续、不换代:写数据 + 发布写头
    REQUIRE(q.front().kind == OpKind::kWrite);
    wm.apply(q.front());
    q.pop_front();
    CHECK_FALSE(oracleAvailable(r, 1033, 5256, 256, 2, 0));
    wm.apply(q.front());
    q.pop_front();
    CHECK(oracleAvailable(r, 1033, 5256, 256, 2, 0));

    // 在环里种一帧「下一圈」的旧数据:读方照读不误(它分不出来),oracle 必须正好抓到这一帧。
    const std::size_t slot = static_cast<std::size_t>((5000 + 10) & (kRing - 1));
    r.data[slot * 2] = encodeTag(Tag{5010 + kRing, 0, 2, 0});
    REQUIRE(readUnderTest(src, 5000, dst.data(), 256, 1));
    first.clear();
    CHECK(countWrongFrames(dst.data(), 5000, 256, 2, 0, &first) == 1);
    CHECK(first.find("frame=10 ") != std::string::npos);
    CHECK_FALSE(oracleAvailable(r, 777, 5000, 256, 2, 0));
}

// ===========================================================================
// 调度族判据
// ===========================================================================
// 约定:
//   · 「no wrong frames」= 安全(I1)。现行实现安全的族里两个读方都断言(旧读方副本的注入在这里被抓)。
//   · 「every lap loses at most 1024 readable frames」= 可用性(按读方自身跳变切圈)。
//   · [!shouldfail] = 现行实现预期失败;配同名「- precondition」普通用例 REQUIRE 场景确实跑到。
//     修复卡去掉标记后,这条转成普通用例必须绿。
//   · 族里只有部分格会红时按格拆开(RunCfg::knownBad):shouldfail 只收预期红的格,其余格进普通用例。
namespace
{
std::vector<RunResult> cells(const std::vector<RunResult>& fam, bool knownBad)
{
    std::vector<RunResult> out;
    for (const auto& r : fam)
    {
        if (r.knownBad == knownBad)
        {
            out.push_back(r);
        }
    }
    return out;
}

void checkNoWrongFrames(const std::vector<RunResult>& fam, bool alsoLegacy)
{
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.cur.wrongFrames == 0);
        if (alsoLegacy)
        {
            CHECK(r.legacy.wrongFrames == 0);
        }
    }
}

void checkLapLoss(const std::vector<RunResult>& fam)
{
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(maxLapLoss(r) <= kLapLossAllowance);
    }
}
} // namespace

TEST_CASE("PROVENANCE sync family: both readers are I1-safe and lose nothing, and the new reader's successes "
          "include the legacy reader's",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = syncFamily();
    REQUIRE(fam.size() == 16u);
    int64_t reads = 0;
    int64_t availFrames = 0;
    int64_t jumps = 0;
    int64_t bumps = 0;
    int64_t staleSnapshot = 0;
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.cur.wrongFrames == 0);
        CHECK(r.legacy.wrongFrames == 0);
        CHECK(r.cur.lostFrames == 0);
        CHECK(r.legacy.lostFrames == 0);
        CHECK(r.legacyOnlyOk == 0); // 差分:被测读方的成功集合 ⊇ 旧读方(I4)
        reads += r.cur.reads;
        availFrames += r.cur.availFrames;
        jumps += r.readerJumps;
        bumps += r.writerBumps;
        staleSnapshot += r.staleSnapshotReads;
    }
    // 前提:族确实跑到了 —— 读了、跳了、换代了、oracle 记到了可读数据;同步族里几何快照每块前都刷新。
    INFO("reads=" << reads << " availFrames=" << availFrames << " jumps=" << jumps << " bumps=" << bumps);
    CHECK(reads > 20000);
    CHECK(availFrames > 5000000);
    CHECK(jumps > 16 * 4);
    CHECK(bumps > jumps);
    CHECK(staleSnapshot == 0);
}

TEST_CASE("PROVENANCE legacy pin: LegacyReader is read-for-read identical to the current ShmRingMixSource on every "
          "family (A-5 deletes this pin)",
          "[mix][provenance]")
{
    // 钉住「副本 == 现行实现」:返回值、交出的样本(逐位)、gap / stall 计数增量,每一块都相同。
    // A-5 改写 ShmRingMixSource::read 时删掉这一条;差分判据(⊇)留在同步族里。
    int64_t reads = 0;
    for (const NamedFamily& f : kAllFamilies)
    {
        for (const auto& r : f.get())
        {
            INFO(f.name << ": " << summary(r));
            CHECK(r.pinMismatches == 0);
            // 逐块相同,则错帧数与首个错帧也相同。旧读方一侧多半走「复用被测读方错帧数」那条路
            // (account 的 badKnown),这两条钉住复用不丢账、也不丢首个错帧。
            CHECK(r.legacy.wrongFrames == r.cur.wrongFrames);
            CHECK(r.legacy.firstWrong == r.cur.firstWrong);
            reads += r.cur.reads;
        }
    }
    CHECK(reads > 50000);
}

// --- H1:写方领先的循环 -----------------------------------------------------------------------------
TEST_CASE("PROVENANCE writer-leading loops (H1): no wrong frames", "[mix][provenance]")
{
    checkNoWrongFrames(leadLoopFamily(), true);
}

TEST_CASE("PROVENANCE writer-leading loops (H1): every lap loses at most 1024 readable frames",
          "[mix][provenance][!shouldfail]")
{
    checkLapLoss(leadLoopFamily());
}

TEST_CASE("PROVENANCE writer-leading loops (H1): every lap loses at most 1024 readable frames - precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = leadLoopFamily();
    REQUIRE(fam.size() == 9u);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.laps.size() == 3u); // 三圈,读方自己回绕了两次
        REQUIRE(r.readerJumps == 2);
        REQUIRE(r.writerBumps >= 4); // 新段 + 起播 + 写方的两次回绕
        REQUIRE(r.leadObservedReads > 0); // 读方确实先看到了写方的回绕
        for (const LapStat& lap : r.laps)
        {
            REQUIRE(lap.availFrames > 0); // oracle 每一圈都记到了可读数据
        }
        REQUIRE(r.cur.availFrames == r.cur.framesRead); // 每一块请求的数据都在环里且新鲜
    }
}

// --- LS-6 / LS-7:写方领先时的定位 ------------------------------------------------------------------
TEST_CASE("PROVENANCE writer-leading back seeks (LS-6): no wrong frames", "[mix][provenance]")
{
    checkNoWrongFrames(leadSeekBackFamily(), true);
}

// 回跳超过 Δ/2:目标落在读方锚点之前,现行实现要等读方重新走过锚点才读得到。
TEST_CASE("PROVENANCE writer-leading back seeks (LS-6) past the reader's anchor: every lap loses at most 1024 "
          "readable frames",
          "[mix][provenance][!shouldfail]")
{
    checkLapLoss(cells(leadSeekBackFamily(), true));
}

TEST_CASE("PROVENANCE writer-leading back seeks (LS-6) past the reader's anchor: every lap loses at most 1024 "
          "readable frames - precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = leadSeekBackFamily();
    REQUIRE(fam.size() == 12u);
    REQUIRE(cells(fam, true).size() == 9u);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.readerJumps == 1);
        REQUIRE(r.laps.size() == 2u);
        REQUIRE(r.leadObservedReads > 0);
        REQUIRE(r.laps.back().availFrames > 0);
    }
}

// 回跳 Δ/2:目标仍在读方锚点之后,现行实现不丢(A-5 必须保持)。
TEST_CASE("PROVENANCE writer-leading back seeks (LS-6) within half the lead: every lap loses at most 1024 readable "
          "frames",
          "[mix][provenance]")
{
    const std::vector<RunResult> safe = cells(leadSeekBackFamily(), false);
    REQUIRE(safe.size() == 3u);
    checkLapLoss(safe);
}

TEST_CASE("PROVENANCE writer-leading forward seeks (LS-7): no wrong frames and every lap loses at most 1024 "
          "readable frames",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = leadSeekFwdFamily();
    REQUIRE(fam.size() == 12u);
    checkNoWrongFrames(fam, true);
    checkLapLoss(fam);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.readerJumps == 1);
        CHECK(r.leadObservedReads > 0);
        CHECK(r.cur.availFrames > 0);
    }
}

// --- H4:写方领先时从停调恢复 -----------------------------------------------------------------------
TEST_CASE("PROVENANCE writer-leading resume from a stall (H4): no wrong frames", "[mix][provenance][!shouldfail]")
{
    checkNoWrongFrames(leadResumeFamily(), false);
}

TEST_CASE("PROVENANCE writer-leading resume from a stall (H4): no wrong frames - precondition", "[mix][provenance]")
{
    const std::vector<RunResult>& fam = leadResumeFamily();
    REQUIRE(fam.size() == 6u);
    int64_t failedReads = 0;
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.writerBumps >= 3); // 新段 + 起播 + 恢复
        REQUIRE(r.leadObservedReads > 0); // 读方在写方恢复之后、自己到达恢复点之前读过
        REQUIRE(r.hazardReads > 0); // oracle 记到了 H4 危险状态:写头已覆盖、本代起点却在 t0 之后
        REQUIRE(r.cur.availFrames > 0);
        failedReads += r.cur.reads - r.cur.ok;
    }
    // 停调比提前量长的那几格里,读方确实追上了冻住的写头、读不到(写头停滞)。间隙 == 提前量时
    // 读方恰好没追上,一次都不失败 —— 所以按族统计,不逐格要求。
    REQUIRE(failedReads > 0);
}

// --- Δ 内两次定位 ----------------------------------------------------------------------------------
// 两次定位都落在环距之内:写方写的每个环槽都是「本位置」的数据,现行实现在这里是安全的(实测)。
// 设计稿 LS-13 把它列为「可能出现 WRONG」,这里钉成普通用例,A-5 改读方时必须保持。
TEST_CASE("PROVENANCE two seeks within the lead: no wrong frames", "[mix][provenance]")
{
    const std::vector<RunResult>& fam = twoSeekFamily();
    REQUIRE(fam.size() == 32u);
    checkNoWrongFrames(fam, true);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.readerJumps == 2);
        CHECK(r.writerBumps >= 4);
        CHECK(r.leadObservedReads > 0);
    }
}

// 第一次往前跳将近一个环距、Δ 内又跳回来:读方前方的环槽里是下一圈的数据,写头却又回到了一个环距
// 之内 —— 现行实现的套圈判据(`w - t0 <= R`)拦不住,读方把下一圈的数据当成本位置交出去。
TEST_CASE("PROVENANCE two seeks within the lead, the first one nearly a ring forward (LS-13): no wrong frames",
          "[mix][provenance][!shouldfail]")
{
    checkNoWrongFrames(twoSeekAcrossRingFamily(), false);
}

TEST_CASE("PROVENANCE two seeks within the lead, the first one nearly a ring forward (LS-13): no wrong frames - "
          "precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = twoSeekAcrossRingFamily();
    REQUIRE(fam.size() == 8u);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.readerJumps == 2);
        REQUIRE(r.writerBumps >= 4);
        REQUIRE(r.leadObservedReads > 0);
        REQUIRE(r.lapAliasReads > 0); // oracle 记到了:读方要读的环槽里确实放着下一圈的数据
    }
}

// --- 读写目标不一致 --------------------------------------------------------------------------------
TEST_CASE("PROVENANCE reader/writer target mismatch in the same block, Output target before the writer's: no wrong "
          "frames",
          "[mix][provenance][!shouldfail]")
{
    checkNoWrongFrames(cells(mismatchSyncFamily(), true), false);
}

TEST_CASE("PROVENANCE reader/writer target mismatch in the same block, Output target before the writer's: no wrong "
          "frames - precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = mismatchSyncFamily();
    REQUIRE(fam.size() == 5u);
    const std::vector<RunResult> bad = cells(fam, true);
    REQUIRE(bad.size() == 3u);
    for (const auto& r : bad)
    {
        INFO(summary(r));
        REQUIRE(r.readerJumps == 1);
        REQUIRE(r.writerBumps >= 3);
        REQUIRE(r.hazardReads > 0); // 读方目标落在写方本代起点之前、写头却已覆盖的状态确实出现过
    }
}

// ε > 0:读方要的位置写方都写过,或者还没覆盖到就读不到 —— 现行实现安全(A-5 必须保持)。
TEST_CASE("PROVENANCE reader/writer target mismatch in the same block, Output target after the writer's: no wrong "
          "frames",
          "[mix][provenance]")
{
    const std::vector<RunResult> safe = cells(mismatchSyncFamily(), false);
    REQUIRE(safe.size() == 2u);
    checkNoWrongFrames(safe, true);
    for (const auto& r : safe)
    {
        INFO(summary(r));
        CHECK(r.readerJumps == 1);
        CHECK(r.cur.ok > 0);
    }
}

TEST_CASE("PROVENANCE reader/writer target mismatch behind a leading writer: no wrong frames", "[mix][provenance]")
{
    // 现行实现里 validFrom_ 唯一真正挡下错读的一支(见 runMismatchFamily 头注)。
    const std::vector<RunResult>& fam = mismatchLeadFamily();
    REQUIRE(fam.size() == 10u);
    checkNoWrongFrames(fam, true);
    int64_t hazard = 0;
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.leadObservedReads > 0);
        hazard += r.hazardReads;
    }
    CHECK(hazard > 0);
}

// --- 接近一个环距的跳变 ---------------------------------------------------------------------------
// 写方领先时单次定位 ≈ R−Δ(前跳那几格会把读方前方的环槽换成下一圈的数据):现行实现靠套圈判据
// `w - t0 <= R` 拦住,实测安全。钉成普通用例,A-5 改读方时必须保持。
TEST_CASE("PROVENANCE jumps of nearly one ring length (LS-13): no wrong frames", "[mix][provenance]")
{
    const std::vector<RunResult>& fam = nearRingFamily();
    REQUIRE(fam.size() == 15u);
    checkNoWrongFrames(fam, true);
    int64_t alias = 0;
    for (const auto& r : fam)
    {
        INFO(summary(r));
        CHECK(r.readerJumps == 1);
        CHECK(r.leadObservedReads > 0);
        alias += r.lapAliasReads;
    }
    CHECK(alias > 0); // 危险状态确实出现过(读方前方的环槽被换成了下一圈),只是被拦住了
}

// --- H1':Output 起播拿到离谱的大 t0 ----------------------------------------------------------------
TEST_CASE("PROVENANCE garbage start position on the Output (H1'): no wrong frames", "[mix][provenance]")
{
    checkNoWrongFrames(garbageStartFamily(), true);
}

TEST_CASE("PROVENANCE garbage start position on the Output (H1'): every lap loses at most 1024 readable frames",
          "[mix][provenance][!shouldfail]")
{
    checkLapLoss(garbageStartFamily());
}

TEST_CASE("PROVENANCE garbage start position on the Output (H1'): every lap loses at most 1024 readable frames - "
          "precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = garbageStartFamily();
    REQUIRE(fam.size() == 4u);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.readerJumps == 1); // 垃圾值那几块之后读方跳回真位置
        REQUIRE(r.laps.size() == 2u);
        REQUIRE(r.laps.back().availFrames > 0);
    }
}

// --- 几何重写 + 读方快照 25 Hz 刷新 ----------------------------------------------------------------
TEST_CASE("PROVENANCE geometry rewrite seen through a stale reader snapshot: no wrong frames",
          "[mix][provenance][!shouldfail]")
{
    checkNoWrongFrames(geometryFamily(), false);
}

TEST_CASE("PROVENANCE geometry rewrite seen through a stale reader snapshot: no wrong frames - precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult>& fam = geometryFamily();
    REQUIRE(fam.size() == 8u);
    for (const auto& r : fam)
    {
        INFO(summary(r));
        REQUIRE(r.staleSnapshotReads > 0); // 读方确实带着过期的几何快照读过
        REQUIRE(r.writerBumps >= 6);
        REQUIRE(r.cur.availFrames > 0);
    }
}

// --- bump-before-write 交错 ------------------------------------------------------------------------
// 循环:新一代写的是上一圈写过的同一批位置,换代后旧写头覆盖的环槽里仍是本位置的数据 —— 安全。
TEST_CASE("PROVENANCE reads that land inside a writer block, loops: no wrong frames", "[mix][provenance]")
{
    const std::vector<RunResult>& fam = interleaveLoopFamily();
    REQUIRE(fam.size() == 12u);
    checkNoWrongFrames(fam, true);
    int64_t partial = 0;
    int cutAfterBump = 0;
    for (const auto& r : fam)
    {
        INFO(summary(r));
        partial += r.partialBlockReads;
        if (r.name.find(" cut-after-bump ") != std::string::npos)
        {
            ++cutAfterBump;
            CHECK(r.bumpBeforeWriteReads > 0); // 每一格「恰好停在换代之后」都确实落进了窗口
        }
    }
    INFO("partialBlockReads=" << partial);
    CHECK(partial > 1000);
    CHECK(cutAfterBump == 3);
}

// 回跳到从没放过的位置:读方与写方同块跳变(lead = 0),读时写方恰好换代了还没写 —— 读方锚在自己的
// t0,上一代的写头仍「覆盖」这一块,读方把从没写过的环槽交出去。
TEST_CASE("PROVENANCE reads that land inside a writer block, same-block back seek to unplayed audio cut right after "
          "the bump: no wrong frames",
          "[mix][provenance][!shouldfail]")
{
    checkNoWrongFrames(cells(interleaveSeekFamily(), true), false);
}

TEST_CASE("PROVENANCE reads that land inside a writer block, same-block back seek to unplayed audio cut right after "
          "the bump: no wrong frames - precondition",
          "[mix][provenance]")
{
    const std::vector<RunResult> bad = cells(interleaveSeekFamily(), true);
    REQUIRE(bad.size() == 1u);
    INFO(summary(bad[0]));
    REQUIRE(bad[0].readerJumps == 1);
    REQUIRE(bad[0].partialBlockReads > 0);
    REQUIRE(bad[0].bumpBeforeWriteReads > 0); // 读方确实落在过「换代了还没写」的窗口里
}

// 同一族的其余格(随机前缀、写方领先):现行实现安全(A-5 必须保持)。
TEST_CASE("PROVENANCE reads that land inside a writer block, back seek to unplayed audio, other cuts and leads: no "
          "wrong frames",
          "[mix][provenance]")
{
    const std::vector<RunResult> safe = cells(interleaveSeekFamily(), false);
    REQUIRE(safe.size() == 11u);
    checkNoWrongFrames(safe, true);
    int64_t partial = 0;
    int cutAfterBump = 0;
    for (const auto& r : safe)
    {
        INFO(summary(r));
        CHECK(r.readerJumps == 1);
        partial += r.partialBlockReads;
        // 「恰好停在换代之后」的格逐格钉住窗口确实出现过(写方领先:读方在旧位置上看到,锚在旧位置,
        // 安全)。随机前缀格只在含换代的那几块上以约 1/(微操作数 + 1) 的概率落进窗口,不逐格要求。
        if (r.name.find(" cut-after-bump ") != std::string::npos)
        {
            ++cutAfterBump;
            CHECK(r.bumpBeforeWriteReads > 0);
        }
    }
    INFO("partialBlockReads=" << partial);
    CHECK(partial > 1000);
    CHECK(cutAfterBump == 2);
}
