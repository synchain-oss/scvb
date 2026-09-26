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
#include <limits>
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

std::string goldenPath(const char* name)
{
    return std::string(SCVB_TEST_GOLDEN_DIR) + "/state/" + name;
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

TEST_CASE("STATE-ABI-1 abi>当前 拒载 + preservedOriginal 原样保留", "[state][abi]")
{
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), enc));
    enc[4] = static_cast<std::uint8_t>(scvb::state::kCurrentAbi + 1); // abi 在 offset 4(当前 4 → 5)
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

    // 与 abi=1(经 no-op migrate_1_to_2)解码结果逐 chunk 一致(迁移链不丢字段)。
    StateChunks baseline;
    enc[4] = 1;
    REQUIRE(scvb::state::loadState(enc.data(), enc.size(), baseline).status == StateLoadStatus::Migrated);
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

TEST_CASE("STATE-GOLDEN StateAbiCompat:abi1/abi2/abi3/abi4 迁移 + abi5.bin 格式锁", "[state][golden]")
{
    // abi1.bin:历史 abi=1,经 no-op migrate_1_to_2 迁移后字段语义正确(CRVS 不丢字段)。
    {
        std::ifstream in(goldenPath("abi1.bin"), std::ios::binary);
        REQUIRE(in.good());
        std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(fileBytes.empty());

        StateChunks chunks;
        StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
        REQUIRE(res.status == StateLoadStatus::Migrated); // abi=1 → 4(三级 no-op 链)
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
    }

    // abi2.bin:[SL-279] abi 升到 3 之后,它从「格式锁」变成**第二条迁移基线**。
    // 旧金样保留不动(与 abi1.bin 同待遇)—— 迁移用例要跑在**真的旧文件**上,
    // 而不是拿当前 codec 现造一个「假装是旧版」的字节串。
    {
        std::ifstream in(goldenPath("abi2.bin"), std::ios::binary);
        REQUIRE(in.good());
        std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(fileBytes.empty());

        StateChunks chunks;
        StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
        REQUIRE(res.status == StateLoadStatus::Migrated); // abi=2 → 4
        REQUIRE(chunks.abi == scvb::state::kCurrentAbi);
    }

    // abi3.bin:[SL-411] abi 升到 4 之后,它同样从「格式锁」降为**第三条迁移基线**
    // (接的是上一版 abi2.bin 的位置),abi1/abi2/abi3 三份并存,一份都不许动。
    {
        std::ifstream in(goldenPath("abi3.bin"), std::ios::binary);
        REQUIRE(in.good());
        std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(fileBytes.empty());

        StateChunks chunks;
        StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
        REQUIRE(res.status == StateLoadStatus::Migrated); // abi=3 → 5
        REQUIRE(chunks.abi == scvb::state::kCurrentAbi);
    }

    // abi4.bin:[SL-416] abi 升到 5 之后,它从「格式锁」降为**第四条迁移基线**(同 abi3 的待遇):
    // 迁移用例要跑在**真的旧文件**上,而不是拿当前 codec 现造一个「假装是旧版」的字节串。
    {
        std::ifstream in(goldenPath("abi4.bin"), std::ios::binary);
        REQUIRE(in.good());
        std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(fileBytes.empty());

        StateChunks chunks;
        StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
        REQUIRE(res.status == StateLoadStatus::Migrated); // abi=4 → 5
        REQUIRE(chunks.abi == scvb::state::kCurrentAbi);
    }

    // abi5.bin:当前 abi=5 格式锁。
    // ⚠ 它锁的是**容器头**(magic / abi / flags / chunkCount / TLV 框),夹具的 CFGS 载荷是
    // `opaque("CONFIG")` 字面量、根本不是 `encodeOutputState` 的产物 —— 所以 abi5.bin 与
    // abi4.bin 只差 abi 那一个字节(offset 4)是**必然**而非巧合,[SL-416] 那 24 个字节它一次都没见过。
    // CFGS 载荷的 wire 布局由 `test_output_session.cpp` 的长度断言(78u / 24u+5u+52u)与
    // [SL-416] 那几格(往返 / abi4 旧档六默认不计回落 / 越界回落计数 / 半截拒载)承担 ——
    // 改 vad/ramp 的编码顺序**不会**让本格红,别来金样这里找原因。
    {
        std::ifstream in(goldenPath("abi5.bin"), std::ios::binary);
        REQUIRE(in.good());
        std::vector<std::uint8_t> fileBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(fileBytes.empty());

        StateChunks chunks;
        StateLoadResult res = scvb::state::loadState(fileBytes.data(), fileBytes.size(), chunks);
        REQUIRE(res.status == StateLoadStatus::Ok);
        REQUIRE(chunks.abi == scvb::state::kCurrentAbi);

        // 格式锁:当前 codec 重编码同一夹具必须与提交的 abi5.bin 逐字节一致(改 wire 格式即红)。
        std::vector<std::uint8_t> reencoded;
        REQUIRE(scvb::state::encodeContainer(makeGoldenChunks(), reencoded));
        REQUIRE(reencoded == fileBytes);
    }
}

TEST_CASE("STATE-CRVS-4c 段 pan/volDb 非有限或越界 → 整份拒载;边界值照常解", "[state][crvs][nan][sl483]")
{
    // [SL-483] 段的两个 float 与 pan_curve 同属不可信字节,且一路原样进实时混音。
    // 构造法同 4b:encode 不设防(它写的是内存里的值),于是字节流里躺着的就是坏值。
    // 每个输入只打中**一个**判据原子,删掉任一原子恰有一行 CHECK 转红:
    //   NaN 专打 isfinite(值域比较对 NaN 恒假,放过它);±inf 同时被值域挡住,不单独计格;
    //   四个有限越界值各打一条值域比较。用 CHECK 不用 REQUIRE:一行红了其余行照样要跑。
    const auto decodesWith = [](float pan, float volDb) {
        CrvsData d;
        d.versions[1].tracks[4].segments.push_back(
            Segment{0, 48000, pan, volDb, scvb::state::makeSegmentFlags(SegmentOrigin::Auto, false)});
        std::vector<std::uint8_t> enc;
        REQUIRE(scvb::state::encodeCrvs(d, enc));
        CrvsData out;
        return scvb::state::decodeCrvs(enc.data(), enc.size(), out);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK_FALSE(decodesWith(nan, 0.0f)); // isfinite(pan)
    CHECK_FALSE(decodesWith(0.0f, nan)); // isfinite(volDb)
    CHECK_FALSE(decodesWith(-100.5f, 0.0f)); // pan < -100
    CHECK_FALSE(decodesWith(100.5f, 0.0f)); // pan > 100
    CHECK_FALSE(decodesWith(0.0f, -24.5f)); // volDb < -24
    CHECK_FALSE(decodesWith(0.0f, 12.5f)); // volDb > 12
    CHECK_FALSE(decodesWith(inf, 0.0f));
    CHECK_FALSE(decodesWith(0.0f, -inf));

    // 反向:闭区间两端是合法值(产品侧 jlimit 夹出来的正是端点),不得被拒。
    CHECK(decodesWith(-100.0f, -24.0f));
    CHECK(decodesWith(100.0f, 12.0f));
}

TEST_CASE("STATE-CRVS-4b pan_curve 的非有限 float 拒载(接线格)", "[state][crvs][nan]")
{
    // [SL-442 第2轮] 自本卡起 pan_curve 进实时音频链 —— 解码路的三个 float 是不可信字节,
    // 一个 NaN 就是灌进宿主母线的 NaN。这一格测的是**解码里那句校验被调用了**,
    // 不是校验函数本身(那在 SL442-NAN-*):摘掉 decodeCrvs 里那一句,只有这一格红。
    //
    // 构造:先用合法点编码(encode 不设防,它写的是内存里的值),再把内存里的那个点改成
    // 非有限值重新编码 —— 于是字节流里躺着一个 NaN,正是损坏文件/别的写入方会产出的形状。
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()})
    {
        SECTION("angle 非有限")
        {
            CrvsData d;
            d.versions[0].panCurve.push_back(
                scvb::PanCurvePoint{bad, 6.0f, scvb::PanCurveShape::bell, 2.0f, scvb::PanCurveSide::out});
            std::vector<std::uint8_t> enc;
            REQUIRE(scvb::state::encodeCrvs(d, enc));
            CrvsData out;
            REQUIRE_FALSE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
        }
        SECTION("gain_db 非有限")
        {
            CrvsData d;
            d.versions[0].panCurve.push_back(
                scvb::PanCurvePoint{-50.0f, bad, scvb::PanCurveShape::bell, 2.0f, scvb::PanCurveSide::out});
            std::vector<std::uint8_t> enc;
            REQUIRE(scvb::state::encodeCrvs(d, enc));
            CrvsData out;
            REQUIRE_FALSE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
        }
        SECTION("q 非有限")
        {
            CrvsData d;
            d.versions[0].panCurve.push_back(
                scvb::PanCurvePoint{-50.0f, 6.0f, scvb::PanCurveShape::bell, bad, scvb::PanCurveSide::out});
            std::vector<std::uint8_t> enc;
            REQUIRE(scvb::state::encodeCrvs(d, enc));
            CrvsData out;
            REQUIRE_FALSE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
        }
    }

    // 对照:合法点照常往返(守卫没有把正常工程也拒了)—— 少了这条,
    // 一个「decodeCrvs 恒回 false」的实现也能让上面全绿。
    CrvsData ok;
    ok.versions[0].panCurve.push_back(
        scvb::PanCurvePoint{-50.0f, 6.0f, scvb::PanCurveShape::bell, 2.0f, scvb::PanCurveSide::out});
    std::vector<std::uint8_t> enc;
    REQUIRE(scvb::state::encodeCrvs(ok, enc));
    CrvsData out;
    REQUIRE(scvb::state::decodeCrvs(enc.data(), enc.size(), out));
    REQUIRE(out.versions[0].panCurve.size() == 1u);
}
