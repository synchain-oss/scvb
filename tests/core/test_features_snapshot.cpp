// SPDX-License-Identifier: GPL-3.0-or-later
// [SL-226] FrameStore ↔ FEAT 的搬运层往返单测(FeaturesSnapshot)。
//
// 这一层此前**整个不存在**:FeaturesCodec 与 FrameStore 各自都有单测且都是对的,但没有任何
// 代码把两头接起来 —— 于是采集的特征从不落盘,重开工程泳道全空(用户 v5.6 实测 SL-226)。
// 本文件钉住搬运层本身;整条「采集→存→载→出图」链路在 host harness 里另有端到端用例。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "analysis/FrameStore.h"
#include "state/FeaturesCodec.h"
#include "state/FeaturesSnapshot.h"

using scvb::analysis::FrameStore;
using scvb::analysis::HopRange;

namespace
{
// 往 ch 的 [begin,end) 写一段可辨认的特征(值随 hop 变化,便于逐 hop 对拍)。
void fill(FrameStore& fs, std::uint32_t ch, std::uint64_t begin, std::uint64_t end)
{
    auto& c = fs.channel(ch);
    c.setReadOnly(false); // 采集 ON 才写得进(write() 的第一道门)
    for (std::uint64_t h = begin; h < end; ++h)
    {
        // kw/peak 取互不相同且非地板的值;vadP 单独填(write 不管 vad)。
        const float kw = 0.001f + static_cast<float>(h % 97) * 0.0011f;
        const float pk = 0.01f + static_cast<float>(h % 89) * 0.0021f;
        c.write(h, kw, pk);
        c.setVadP(h, static_cast<std::uint8_t>(h % 256));
    }
}

// 逐 hop 对拍两个 FrameStore 在 ranges 上的量化值与覆盖。
void expectSame(const FrameStore& a, const FrameStore& b, std::uint32_t ch)
{
    const auto& ca = a.channel(ch);
    const auto& cb = b.channel(ch);
    REQUIRE(ca.coverage().ranges().size() == cb.coverage().ranges().size());
    for (std::size_t i = 0; i < ca.coverage().ranges().size(); ++i)
    {
        REQUIRE(ca.coverage().ranges()[i] == cb.coverage().ranges()[i]);
    }
    for (const auto& r : ca.coverage().ranges())
    {
        for (std::uint64_t h = r.begin; h < r.end; ++h)
        {
            INFO("ch=" << ch << " hop=" << h);
            REQUIRE(ca.kwDbq(h) == cb.kwDbq(h));
            REQUIRE(ca.peakDbq(h) == cb.peakDbq(h));
            REQUIRE(ca.vadP(h) == cb.vadP(h));
            REQUIRE(cb.hasHop(h));
        }
    }
}
} // namespace

TEST_CASE("[SL-226] FrameStore → FEAT → FrameStore 逐 hop 逐字往返", "[state][features][SL226]")
{
    FrameStore src;
    fill(src, 1, 0, 500);
    fill(src, 3, 12000, 12345); // 跨页(4096 hop/页)且不从 0 起
    fill(src, 15, 4090, 4110); // 恰好跨页边界

    const auto data = scvb::state::snapshotFeatures(src, 48000u, 10u);
    REQUIRE(data.sampleRate == 48000u);
    REQUIRE(data.hopMs == 10u);
    REQUIRE(data.channels.size() == 3); // 只导出采过的三轨,没采的 12 轨不占体积

    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());

    const auto decoded = scvb::state::decodeFeatures(gz.data(), gz.size());
    REQUIRE(decoded.ok);
    REQUIRE(decoded.embedded);

    FrameStore dst;
    scvb::state::restoreFeatures(decoded.features, dst);

    expectSame(src, dst, 1);
    expectSame(src, dst, 3);
    expectSame(src, dst, 15);

    // 没采过的轨回灌后仍是空的(不能凭空长出覆盖)。
    REQUIRE(dst.channel(2).coverage().empty());
    REQUIRE(dst.channel(7).coverage().empty());
}

TEST_CASE("[SL-226] 多段不连续覆盖:空洞不得被填平", "[state][features][SL226]")
{
    FrameStore src;
    fill(src, 2, 100, 200);
    fill(src, 2, 500, 600); // 与上一段之间留 300 hop 空洞
    fill(src, 2, 5000, 5050);
    REQUIRE(src.channel(2).coverage().ranges().size() == 3);

    const auto data = scvb::state::snapshotFeatures(src, 44100u, 10u);
    const auto gz = scvb::state::encodeFeatures(data);
    const auto decoded = scvb::state::decodeFeatures(gz.data(), gz.size());
    REQUIRE(decoded.ok);

    FrameStore dst;
    scvb::state::restoreFeatures(decoded.features, dst);

    expectSame(src, dst, 2);
    // 空洞仍是空洞 —— 这正是 waveformOf 判「这一列有没有数据」的依据,填平了会画出假波形。
    REQUIRE(dst.channel(2).coverage().ranges().size() == 3);
    REQUIRE_FALSE(dst.channel(2).hasHop(300));
    REQUIRE_FALSE(dst.channel(2).hasHop(4999));
    REQUIRE(dst.channel(2).hasHop(199));
    REQUIRE_FALSE(dst.channel(2).hasHop(200));
}

TEST_CASE("[SL-226] 回灌绕开 readOnly/gate —— 否则加载时被静默吃掉", "[state][features][SL226]")
{
    FrameStore src;
    fill(src, 1, 0, 100);
    const auto data = scvb::state::snapshotFeatures(src, 48000u, 10u);

    // 加载工程时的真实处境:采集 OFF(readOnly=true)、布防门还停在一个窄区间。
    // 走 write() 的话这两道门会把回灌全部吃掉 —— 那正是「接了持久化但波形还是空」的样子。
    FrameStore dst;
    dst.channel(1).setReadOnly(true);
    dst.channel(1).setGate(HopRange{9000, 9001});

    scvb::state::restoreFeatures(data, dst);

    REQUIRE(dst.channel(1).coverage().ranges().size() == 1);
    REQUIRE(dst.channel(1).hasHop(0));
    REQUIRE(dst.channel(1).hasHop(99));
    expectSame(src, dst, 1);
}

TEST_CASE("[SL-226] 回灌先清场:上一个工程的覆盖不得残留成幽灵波形", "[state][features][SL226]")
{
    FrameStore dst;
    fill(dst, 5, 0, 300); // 上一个工程采过 ch5
    REQUIRE_FALSE(dst.channel(5).coverage().empty());

    // 新工程只采过 ch1,压根没有 ch5。
    FrameStore src;
    fill(src, 1, 0, 50);
    const auto data = scvb::state::snapshotFeatures(src, 48000u, 10u);

    scvb::state::restoreFeatures(data, dst);

    REQUIRE(dst.channel(1).hasHop(10));
    REQUIRE(dst.channel(5).coverage().empty()); // ch5 被清干净,不留别的工程的波形
}

TEST_CASE("[SL-226] 反向:空 FrameStore 快照不产出 channel(不写空 FEAT)", "[state][features][SL226]")
{
    FrameStore empty;
    const auto data = scvb::state::snapshotFeatures(empty, 48000u, 10u);
    REQUIRE(data.channels.empty());
}

TEST_CASE("[SL-226] 反向:越界 channelId 静默跳过,不回落到 ch1", "[state][features][SL226]")
{
    scvb::state::FeaturesData data;
    data.sampleRate = 48000u;
    data.hopMs = 10u;

    scvb::state::ChannelFeatures bad;
    bad.channelId = 99; // 不可信字节:越界
    bad.coverage.push_back(scvb::state::HopRange{0, 3});
    bad.kwDbq = {-100, -200, -300};
    bad.peakDbq = {-10, -20, -30};
    bad.vadPosterior = {1, 2, 3};
    data.channels.push_back(bad);

    FrameStore dst;
    scvb::state::restoreFeatures(data, dst);

    for (std::uint32_t ch = 1; ch <= dst.maxChannels(); ++ch)
    {
        INFO("ch=" << ch);
        REQUIRE(dst.channel(ch).coverage().empty()); // 一条都不该落地,尤其不该落到 ch1
    }
}

TEST_CASE("[SL-226] 反向:列数据短于覆盖声明时截断,不越界读", "[state][features][SL226]")
{
    scvb::state::FeaturesData data;
    data.sampleRate = 48000u;
    data.hopMs = 10u;

    scvb::state::ChannelFeatures cf;
    cf.channelId = 1;
    cf.coverage.push_back(scvb::state::HopRange{0, 3});
    cf.coverage.push_back(scvb::state::HopRange{10, 20}); // 声明 10 个 hop
    cf.kwDbq = {-100, -200, -300}; // 但只给了 3 个样本
    cf.peakDbq = {-10, -20, -30};
    cf.vadPosterior = {1, 2, 3};
    data.channels.push_back(cf);

    FrameStore dst;
    scvb::state::restoreFeatures(data, dst); // 不得越界读/崩溃

    REQUIRE(dst.channel(1).hasHop(0));
    REQUIRE(dst.channel(1).hasHop(2));
    REQUIRE_FALSE(dst.channel(1).hasHop(10)); // 第二段样本不够 → 整段不写
    REQUIRE(dst.channel(1).coverage().ranges().size() == 1);
}

TEST_CASE("[SL-226] 反向:[J06] vadPosterior 省略时按 0 回灌,kw/peak 不受影响", "[state][features][SL226]")
{
    FrameStore src;
    fill(src, 4, 0, 64);

    auto data = scvb::state::snapshotFeatures(src, 48000u, 10u);
    data.vadPresent = false; // [J06] 允许省略(消费方按需重算)
    for (auto& cf : data.channels)
    {
        cf.vadPosterior.clear();
    }

    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());
    const auto decoded = scvb::state::decodeFeatures(gz.data(), gz.size());
    REQUIRE(decoded.ok);

    FrameStore dst;
    scvb::state::restoreFeatures(decoded.features, dst);

    const auto& s = src.channel(4);
    const auto& d = dst.channel(4);
    for (std::uint64_t h = 0; h < 64; ++h)
    {
        INFO("hop=" << h);
        REQUIRE(d.hasHop(h));
        REQUIRE(d.kwDbq(h) == s.kwDbq(h)); // 响度/峰值照常往返
        REQUIRE(d.peakDbq(h) == s.peakDbq(h));
        REQUIRE(d.vadP(h) == 0); // vad 省略 → 回灌为 0,由消费方重算
    }
}
