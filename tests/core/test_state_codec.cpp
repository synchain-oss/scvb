// SPDX-License-Identifier: GPL-3.0-or-later
// test_state_codec —— T19 state 层:TLV 容器 + CRVS 曲线节 + 迁移框架 + golden。
// 覆盖:save→load→save 逐字节一致、未知 fourcc 回写、abi 拒载/迁移、不可信字节校验、
// CRVS segments(origin/locked)/excluded_ranges/versionMeta/pan_curve 往返、abi1.bin golden 兼容。
// 纯 C++17,无 JUCE(ADR-011)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "state/StateCodec.h"
#include "state/StateMigration.h"

using scvb::state::Chunk;
using scvb::state::CrvsData;
using scvb::state::DecodeStatus;
using scvb::state::ExcludedRange;
using scvb::state::Segment;
using scvb::state::SegmentOrigin;
using scvb::state::StateChunks;
using scvb::state::StateLoadResult;
using scvb::state::StateLoadStatus;

namespace
{

constexpr std::uint32_t fourcc(char a, char b, char c, char d)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a))) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

std::vector<std::uint8_t> opaque(std::string_view s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// ---- golden 夹具(确定性;golden 读写共用同一构造,保证格式锁一致)----
CrvsData makeGoldenCrvs()
{
    CrvsData d;
    d.versions[0].meta.name = "V1";
    auto& t0 = d.versions[0].tracks[0];
    t0.segments.push_back(Segment{0, 48000, 0.0f, -3.0f, scvb::state::makeSegmentFlags(SegmentOrigin::Auto, false)});
    t0.segments.push_back(
        Segment{48000, 96000, 12.5f, 1.5f, scvb::state::makeSegmentFlags(SegmentOrigin::UserEdited, true)});
    t0.excludedRanges.push_back(ExcludedRange{12000, 24000});
    d.versions[0].panCurve.push_back(
        scvb::PanCurvePoint{0.0f, 0.0f, scvb::PanCurveShape::bell, 1.5f, scvb::PanCurveSide::out});

    d.versions[1].meta.name = "V2";
    d.versions[1].meta.copiedFrom = 1;
    d.versions[1].meta.copiedAtMs = 1234567890123;
    // track 5:无 segments(count=0)但有两段 excluded_ranges(count=N)
    d.versions[1].tracks[5].excludedRanges.push_back(ExcludedRange{0, 48000});
    d.versions[1].tracks[5].excludedRanges.push_back(ExcludedRange{96000, 120000});
    return d;
}

StateChunks makeGoldenChunks()
{
    StateChunks c;
    c.abi = scvb::state::kCurrentAbi;
    c.flags = 0;
    std::vector<std::uint8_t> crvs;
    (void)scvb::state::encodeCrvs(makeGoldenCrvs(), crvs);
    c.chunks.push_back(Chunk{scvb::state::kFourccPrms, opaque("PARAMS")});
    c.chunks.push_back(Chunk{scvb::state::kFourccCfgs, opaque("CONFIG")});
    c.chunks.push_back(Chunk{scvb::state::kFourccCrvs, std::move(crvs)});
    c.chunks.push_back(Chunk{scvb::state::kFourccFeat, opaque("FEATURES")});
    return c;
}

std::string goldenPath()
{
    return std::string(SCVB_TEST_GOLDEN_DIR) + "/state/abi1.bin";
}

// ---- 最小合法 CRVS(2 版本 × 15 轨全空、无 pan 曲线);供不可信字节校验夹具改字段用 ----
struct W
{
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v)
    {
        b.push_back(static_cast<std::uint8_t>(v & 0xffu));
        b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
    void i32(std::int32_t v)
    {
        std::uint32_t u = 0;
        std::memcpy(&u, &v, 4u);
        u32(u);
    }
    void i64(std::int64_t v)
    {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, 8u);
        for (int i = 0; i < 8; ++i)
            b.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xffu));
    }
    void raw(const void* p, std::size_t n)
    {
        const auto* x = static_cast<const std::uint8_t*>(p);
        b.insert(b.end(), x, x + n);
    }
};

} // namespace

// ============================================================================
// 容器:save→load→save 逐字节一致 + 未知 fourcc 回写
// ============================================================================

TEST_CASE("STATE-CONTAINER-1 save→load→save 逐字节一致", "[state][container]")
{
    StateChunks chunks = makeGoldenChunks();
    chunks.chunks.push_back(Chunk{fourcc('X', 'Y', 'Z', 'W'), opaque("future-data")});

    std::vector<std::uint8_t> bytes1;
    REQUIRE(scvb::state::encodeContainer(chunks, bytes1));

    StateChunks loaded;
    StateLoadResult res = scvb::state::loadState(bytes1.data(), bytes1.size(), loaded);
    REQUIRE(res.status == StateLoadStatus::Ok);

    std::vector<std::uint8_t> bytes2;
    REQUIRE(scvb::state::encodeContainer(loaded, bytes2));
    REQUIRE(bytes1 == bytes2);
}

TEST_CASE("STATE-CONTAINER-2 未知 fourcc 原样保留并回写(位置/载荷不变)", "[state][container]")
{
    StateChunks chunks = makeGoldenChunks();
    const std::uint32_t unknown = fourcc('Q', 'R', 'S', 'T');
    chunks.chunks.push_back(Chunk{unknown, opaque("keep-me")}); // 追加到末尾(原序第 5)

    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(chunks, enc));

    StateChunks loaded;
    REQUIRE(scvb::state::decodeContainer(enc.data(), enc.size(), loaded) == DecodeStatus::Ok);
    REQUIRE(loaded.chunks.size() == 5u);
    REQUIRE(loaded.chunks[4].fourcc == unknown);
    REQUIRE(loaded.chunks[4].payload == opaque("keep-me"));
    REQUIRE(loaded.find(unknown) == &loaded.chunks[4]);
}

TEST_CASE("STATE-CONTAINER-3 set 原位替换 / remove 删除", "[state][container]")
{
    StateChunks chunks = makeGoldenChunks();
    chunks.set(scvb::state::kFourccPrms, opaque("new-params"));
    REQUIRE(chunks.find(scvb::state::kFourccPrms)->payload == opaque("new-params"));
    REQUIRE(chunks.chunks.size() == 4u); // 原位替换,不新增

    chunks.remove(scvb::state::kFourccFeat);
    REQUIRE(chunks.find(scvb::state::kFourccFeat) == nullptr);
    REQUIRE(chunks.chunks.size() == 3u);
}

// ============================================================================
// 不可信字节:长度/范围字段先校验(CLAUDE.md §7.3)
// ============================================================================

TEST_CASE("STATE-VALIDATE-1 magic 不符 → Corrupt", "[state][validate]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), enc));
    enc[0] = 0x00; // 破坏 magic 首字节
    StateChunks out;
    REQUIRE(scvb::state::loadState(enc.data(), enc.size(), out).status == StateLoadStatus::Corrupt);
}

TEST_CASE("STATE-VALIDATE-2 头部截断 → Corrupt", "[state][validate]")
{
    const std::uint8_t head[8] = {0x53, 0x43, 0x56, 0x42, 1, 0, 0, 0}; // magic 'SCVB'(小端内存序)+ abi(不足 16 字节)
    StateChunks out;
    REQUIRE(scvb::state::loadState(head, sizeof(head), out).status == StateLoadStatus::Corrupt);
}

TEST_CASE("STATE-VALIDATE-3 chunk sizeBytes 越界 → Corrupt(不越读)", "[state][validate]")
{
    StateChunks chunks;
    chunks.chunks.push_back(Chunk{scvb::state::kFourccCrvs, opaque("ABCD")});
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(chunks, enc));
    // chunk 头在 offset 16;sizeBytes 在 offset 20..23。置为超大值制造越界。
    enc[20] = 0xFF;
    enc[21] = 0xFF;
    enc[22] = 0xFF;
    enc[23] = 0xFF;
    StateChunks out;
    REQUIRE(scvb::state::decodeContainer(enc.data(), enc.size(), out) == DecodeStatus::Corrupt);
}

TEST_CASE("STATE-VALIDATE-4 chunkCount 超上限 → Corrupt", "[state][validate]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), enc));
    enc[12] = 0xFF; // chunkCount 在 offset 12..15
    enc[13] = 0xFF;
    enc[14] = 0xFF;
    enc[15] = 0xFF;
    StateChunks out;
    REQUIRE(scvb::state::decodeContainer(enc.data(), enc.size(), out) == DecodeStatus::Corrupt);
}

// ============================================================================
// abi 判读与迁移
// ============================================================================

TEST_CASE("STATE-ABI-1 abi=2 拒载 + preservedOriginal 原样保留", "[state][abi]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), enc));
    enc[4] = 2; // abi 在 offset 4(原 1 → 2)
    StateChunks out;
    StateLoadResult res = scvb::state::loadState(enc.data(), enc.size(), out);
    REQUIRE(res.status == StateLoadStatus::RejectedNewer);
    REQUIRE(res.preservedOriginal == enc); // 原样回写,不毁高版本数据
}

TEST_CASE("STATE-ABI-2 abi=0 走迁移不丢字段(Migrated)", "[state][abi]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), enc));
    enc[4] = 0; // abi 1 → 0(历史低版本)

    StateChunks migrated;
    StateLoadResult res = scvb::state::loadState(enc.data(), enc.size(), migrated);
    REQUIRE(res.status == StateLoadStatus::Migrated);
    REQUIRE(migrated.abi == scvb::state::kCurrentAbi);

    // 与 abi=1 解码结果逐 chunk 一致(迁移链不丢字段)。
    StateChunks baseline;
    enc[4] = 1;
    REQUIRE(scvb::state::loadState(enc.data(), enc.size(), baseline).status == StateLoadStatus::Ok);
    REQUIRE(migrated.chunks.size() == baseline.chunks.size());
    for (std::size_t i = 0; i < baseline.chunks.size(); ++i)
    {
        REQUIRE(migrated.chunks[i].fourcc == baseline.chunks[i].fourcc);
        REQUIRE(migrated.chunks[i].payload == baseline.chunks[i].payload);
    }
}

// ============================================================================
// CRVS 往返
// ============================================================================

TEST_CASE("STATE-CRVS-1 segments(origin/locked)逐字节往返", "[state][crvs]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeCrvs(makeGoldenCrvs(), enc));

    CrvsData out;
    REQUIRE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));

    const auto& s0 = out.versions[0].tracks[0].segments[0];
    REQUIRE(s0.t0 == 0);
    REQUIRE(s0.t1 == 48000);
    REQUIRE(s0.pan == 0.0f);
    REQUIRE(s0.volDb == -3.0f);
    REQUIRE(scvb::state::segmentOrigin(s0.flags) == SegmentOrigin::Auto);
    REQUIRE(scvb::state::segmentLocked(s0.flags) == false);

    const auto& s1 = out.versions[0].tracks[0].segments[1];
    REQUIRE(scvb::state::segmentOrigin(s1.flags) == SegmentOrigin::UserEdited);
    REQUIRE(scvb::state::segmentLocked(s1.flags) == true);

    std::vector<std::uint8_t> enc2;
    REQUIRE(scvb::state::encodeCrvs(out, enc2));
    REQUIRE(enc == enc2); // 逐字节一致
}

TEST_CASE("STATE-CRVS-2 excluded_ranges 往返(count=0 与 count=N)", "[state][crvs]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeCrvs(makeGoldenCrvs(), enc));
    CrvsData out;
    REQUIRE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));

    // count=N:version 1 track 5 两段删除记录(SEG-6 跨会话防复活载体)。
    REQUIRE(out.versions[1].tracks[5].excludedRanges.size() == 2u);
    REQUIRE(out.versions[1].tracks[5].excludedRanges[0].t0 == 0);
    REQUIRE(out.versions[1].tracks[5].excludedRanges[0].t1 == 48000);
    REQUIRE(out.versions[1].tracks[5].excludedRanges[1].t0 == 96000);
    REQUIRE(out.versions[1].tracks[5].excludedRanges[1].t1 == 120000);

    // count=0:其余轨(如 version 1 track 0)无删除记录。
    REQUIRE(out.versions[1].tracks[0].excludedRanges.empty());
    REQUIRE(out.versions[0].tracks[0].excludedRanges.size() == 1u);

    std::vector<std::uint8_t> enc2;
    REQUIRE(scvb::state::encodeCrvs(out, enc2));
    REQUIRE(enc == enc2);
}

TEST_CASE("STATE-CRVS-3 versionMeta(name UTF-8 + copiedFrom/copiedAtMs)往返", "[state][crvs]")
{
    CrvsData d;
    d.versions[0].meta.name = "一二三"; // 多字节 UTF-8
    d.versions[1].meta.copiedFrom = 1;
    d.versions[1].meta.copiedAtMs = 9876543210;
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeCrvs(d, enc));
    CrvsData out;
    REQUIRE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
    REQUIRE(out.versions[0].meta.name == "一二三");
    REQUIRE(out.versions[1].meta.copiedFrom == 1);
    REQUIRE(out.versions[1].meta.copiedAtMs == 9876543210);
}

TEST_CASE("STATE-CRVS-4 pan_curve points 往返", "[state][crvs]")
{
    CrvsData d;
    d.versions[0].panCurve.push_back(
        scvb::PanCurvePoint{-50.0f, 6.0f, scvb::PanCurveShape::shelf, 2.0f, scvb::PanCurveSide::left});
    d.versions[1].panCurve.push_back(
        scvb::PanCurvePoint{25.0f, -8.0f, scvb::PanCurveShape::cut, 0.75f, scvb::PanCurveSide::right});
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeCrvs(d, enc));
    CrvsData out;
    REQUIRE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
    REQUIRE(out.versions[0].panCurve.size() == 1u);
    REQUIRE(out.versions[0].panCurve[0].angle == -50.0f);
    REQUIRE(out.versions[0].panCurve[0].shape == scvb::PanCurveShape::shelf);
    REQUIRE(out.versions[0].panCurve[0].side == scvb::PanCurveSide::left);
    REQUIRE(out.versions[1].panCurve[0].shape == scvb::PanCurveShape::cut);
}

// ============================================================================
// CRVS 不可信字节校验
// ============================================================================

TEST_CASE("STATE-CRVS-VALIDATE-1 minorVersion 高于当前 → 拒解", "[state][crvs][validate]")
{
    W w;
    w.u16(2); // minor=2(布局未知,不可静默丢数据)
    w.u8(2);
    w.u8(15);
    CrvsData out;
    REQUIRE_FALSE(scvb::state::decodeCrvs(w.b.data(), w.b.size(), out));
}

TEST_CASE("STATE-CRVS-VALIDATE-2 版本/轨道数不符 → 拒解", "[state][crvs][validate]")
{
    W w;
    w.u16(1);
    w.u8(3); // versionCount=3(abi=1 只认 2)
    w.u8(15);
    CrvsData out;
    REQUIRE_FALSE(scvb::state::decodeCrvs(w.b.data(), w.b.size(), out));
}

TEST_CASE("STATE-CRVS-VALIDATE-3 segmentCount 超上限 → 拒解(不分配炸弹)", "[state][crvs][validate]")
{
    W w;
    w.u16(1);
    w.u8(2);
    w.u8(15);
    w.u16(2);
    w.raw("V1", 2);
    w.i32(0);
    w.i64(0);
    w.u32(0xFFFFFFFFu); // track 0 segmentCount 炸弹
    CrvsData out;
    REQUIRE_FALSE(scvb::state::decodeCrvs(w.b.data(), w.b.size(), out));
}

TEST_CASE("STATE-CRVS-VALIDATE-4 nameBytes 超上限 → 拒解", "[state][crvs][validate]")
{
    W w;
    w.u16(1);
    w.u8(2);
    w.u8(15);
    w.u16(0xFFFFu); // nameBytes = 65535 > 64
    CrvsData out;
    REQUIRE_FALSE(scvb::state::decodeCrvs(w.b.data(), w.b.size(), out));
}

// ============================================================================
// golden:abi1.bin 兼容 + 格式锁
// ============================================================================

TEST_CASE("STATE-GOLDEN StateAbiCompat:abi1.bin 可载且字段语义正确 + 格式锁", "[state][golden]")
{
    std::ifstream in(goldenPath(), std::ios::binary);
    REQUIRE(in.good());
    std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(fileBytes.empty());

    StateChunks chunks;
    StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
    REQUIRE(res.status == StateLoadStatus::Ok);
    REQUIRE(chunks.abi == scvb::state::kCurrentAbi);

    const Chunk* crvs = chunks.find(scvb::state::kFourccCrvs);
    REQUIRE(crvs != nullptr);
    CrvsData data;
    REQUIRE(scvb::state::decodeCrvs(crvs->payload.data(), crvs->payload.size(), data));
    REQUIRE(data.versions[0].meta.name == "V1");
    REQUIRE(data.versions[0].tracks[0].segments.size() == 2u);
    REQUIRE(data.versions[0].tracks[0].segments[0].t0 == 0);
    REQUIRE(scvb::state::segmentOrigin(data.versions[0].tracks[0].segments[1].flags) == SegmentOrigin::UserEdited);
    REQUIRE(scvb::state::segmentLocked(data.versions[0].tracks[0].segments[1].flags));
    REQUIRE(data.versions[1].tracks[5].excludedRanges.size() == 2u);

    // 格式锁:当前 codec 重编码同一夹具必须与提交的 golden 逐字节一致(改 wire 格式即红)。
    std::vector<std::uint8_t> reencoded;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), reencoded));
    REQUIRE(reencoded == fileBytes);
}
