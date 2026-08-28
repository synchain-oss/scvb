// SPDX-License-Identifier: GPL-3.0-or-later
// test_state_features_roundtrip —— T21 特征持久化:FeaturesCodec + gzip + SidecarStore。
// 覆盖:embedded 逐字节往返(vadPresent 存/省)、sidecar 引用往返、>8MB 自动转 sidecar 且回读、
// 删除 sidecar 后特征缺失、双开同 GUID copy-on-write、owner.lock 判活与 [SL-233] 10s 续租、8MB/6MB 回滞、
// gzip 5min/15轨 ≤2.25MB、sha256 已知向量、压缩炸弹防护、不可信字节校验。
// 纯 C++17,无 JUCE(ADR-011)。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "state/FeaturesCodec.h"
#include "state/SidecarStore.h"

using scvb::state::ChannelFeatures;
using scvb::state::FeatDecode;
using scvb::state::FeaturesData;
using scvb::state::HopRange;
using scvb::state::OwnerLock;
using scvb::state::ProcessIdentity;
using scvb::state::SidecarRef;
using scvb::state::SidecarStore;

namespace
{

const std::string kGuid = "11111111-2222-4333-8444-555555555555"; // 36 字符 dashed UUID

// 唯一临时目录,析构删除。
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        path = std::filesystem::temp_directory_path() /
               ("scvb-t21-" + std::to_string(scvb::state::epochMsNow() % 1000000) + "-" + std::to_string(seq++));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    static int seq;
};
int TempDir::seq = 0;

// 确定性小夹具:2 通道(ch1=5 hop 两段 coverage,ch2=2 hop 一段 coverage)。
FeaturesData makeSmallFixture(bool vadPresent = true)
{
    FeaturesData d;
    d.sampleRate = 48000;
    d.hopMs = 10;
    d.vadPresent = vadPresent;

    ChannelFeatures c1;
    c1.channelId = 1;
    c1.coverage.push_back(HopRange{0, 3});
    c1.coverage.push_back(HopRange{10, 12});
    c1.kwDbq = {-12000, -4000, -3500, -5000, -6000};
    c1.peakDbq = {-12000, -3000, -2500, -4000, -5000};
    if (vadPresent)
        c1.vadPosterior = {0, 200, 180, 220, 240};
    d.channels.push_back(std::move(c1));

    ChannelFeatures c2;
    c2.channelId = 2;
    c2.coverage.push_back(HopRange{5, 7});
    c2.kwDbq = {-8000, -7500};
    c2.peakDbq = {-7000, -6500};
    if (vadPresent)
        c2.vadPosterior = {10, 20};
    d.channels.push_back(std::move(c2));
    return d;
}

// 满配夹具:15 轨 × 30000 hop(5 分钟 @10ms)。确定性 AR(1) 包络 + 短语门控(仿 S4)。
FeaturesData makeFullFixture()
{
    FeaturesData d;
    d.sampleRate = 48000;
    d.hopMs = 10;
    d.vadPresent = true;

    std::mt19937 rng(0xC0FFEEu);
    constexpr std::uint32_t kHops = 30000;
    for (std::uint8_t ch = 1; ch <= 15; ++ch)
    {
        ChannelFeatures c;
        c.channelId = ch;
        c.coverage.push_back(HopRange{0, kHops});
        c.kwDbq.reserve(kHops);
        c.peakDbq.reserve(kHops);
        c.vadPosterior.reserve(kHops);

        std::int16_t kw = -6000;
        for (std::uint32_t i = 0; i < kHops; ++i)
        {
            const std::uint32_t phrase = (i / 200u) % 5u;
            if (phrase >= 3u) // 40% 静音
            {
                c.kwDbq.push_back(-12000);
                c.peakDbq.push_back(-12000);
                c.vadPosterior.push_back(0);
                kw = -6000;
            }
            else
            {
                const std::int32_t next =
                    std::clamp<std::int32_t>(kw + static_cast<std::int32_t>(rng() % 241u) - 120, -9000, -2000);
                kw = static_cast<std::int16_t>(next);
                c.kwDbq.push_back(kw);
                c.peakDbq.push_back(static_cast<std::int16_t>(
                    std::clamp<std::int32_t>(kw + 500 + static_cast<std::int32_t>(rng() % 800u), -9000, -500)));
                c.vadPosterior.push_back(static_cast<std::uint8_t>(rng() % 256u));
            }
        }
        d.channels.push_back(std::move(c));
    }
    return d;
}

// 超阈值夹具:单通道 2.25M hop 的满幅随机 int16(不可压缩,gzip 后 ≈ raw > 8MB)。
FeaturesData makeHugeIncompressibleFixture()
{
    FeaturesData d;
    d.sampleRate = 48000;
    d.hopMs = 10;
    d.vadPresent = false; // 省内存:不编 vad(4 B/hop)

    ChannelFeatures c;
    c.channelId = 1;
    constexpr std::uint64_t kHops = 2250000; // kw 4.5MB + peak 4.5MB = 9MB raw
    c.coverage.push_back(HopRange{0, kHops});
    c.kwDbq.resize(static_cast<std::size_t>(kHops));
    c.peakDbq.resize(static_cast<std::size_t>(kHops));

    std::mt19937 rng(0xDEADBEEFu);
    for (std::uint64_t i = 0; i < kHops; ++i)
    {
        c.kwDbq[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(static_cast<std::uint16_t>(rng() & 0xFFFFu));
        c.peakDbq[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(static_cast<std::uint16_t>(rng() & 0xFFFFu));
    }
    d.channels.push_back(std::move(c));
    return d;
}

ProcessIdentity makeIdentity(std::uint64_t pid, const std::string& host)
{
    ProcessIdentity id;
    id.pid = pid;
    id.processStartEpochMs = 1000000u + pid;
    id.hostName = host;
    return id;
}

// 完整加载(复刻 Output 侧 setState 语义):解码 FEAT payload;embedded=0 时读 sidecar + sha256 校验。
struct LoadOutcome
{
    bool ok = false;
    bool featuresMissing = false;
    FeaturesData features;
};

LoadOutcome loadFeatures(const std::vector<std::uint8_t>& payload, SidecarStore& store)
{
    LoadOutcome out;
    const FeatDecode dec = scvb::state::decodeFeatures(payload.data(), payload.size());
    if (!dec.ok)
        return out;
    out.ok = true;
    if (dec.embedded)
    {
        out.features = dec.features;
        return out;
    }

    std::vector<std::uint8_t> gz;
    if (!store.read(dec.ref.sessionGuid, gz))
    {
        out.featuresMissing = true; // sidecar 缺失 → 特征清空 + 横幅(曲线/配置不受影响)
        return out;
    }
    if (scvb::state::sha256(gz.data(), gz.size()) != dec.ref.sha256)
    {
        out.featuresMissing = true; // sha256 不符 → 按缺失处理(04 §5.3)
        return out;
    }
    const FeatDecode side = scvb::state::decodeFeatures(gz.data(), gz.size());
    if (!side.ok || !side.embedded)
    {
        out.featuresMissing = true;
        return out;
    }
    out.features = side.features;
    return out;
}

} // namespace

// ============================================================================
// FeaturesCodec:roundtrip 逐字节一致
// ============================================================================

TEST_CASE("FEAT-ROUNDTRIP-1 embedded 逐字节往返(vadPresent=true)", "[state][features]")
{
    const FeaturesData data = makeSmallFixture(true);
    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());

    const FeatDecode dec = scvb::state::decodeFeatures(gz.data(), gz.size());
    REQUIRE(dec.ok);
    REQUIRE(dec.embedded);
    REQUIRE(dec.features.vadPresent);
    REQUIRE(dec.features.channels.size() == 2u);

    REQUIRE(dec.features.channels[0].channelId == 1);
    REQUIRE(dec.features.channels[0].coverage == data.channels[0].coverage);
    REQUIRE(dec.features.channels[0].kwDbq == data.channels[0].kwDbq);
    REQUIRE(dec.features.channels[0].peakDbq == data.channels[0].peakDbq);
    REQUIRE(dec.features.channels[0].vadPosterior == data.channels[0].vadPosterior);
    REQUIRE(dec.features.channels[1].coverage == data.channels[1].coverage);

    const auto gz2 = scvb::state::encodeFeatures(dec.features);
    REQUIRE(gz2 == gz); // 逐字节一致
}

TEST_CASE("FEAT-ROUNDTRIP-2 vadPosterior 省略(vadPresent=false)[J06]", "[state][features]")
{
    const FeaturesData data = makeSmallFixture(false);
    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());

    const FeatDecode dec = scvb::state::decodeFeatures(gz.data(), gz.size());
    REQUIRE(dec.ok);
    REQUIRE_FALSE(dec.features.vadPresent);
    REQUIRE(dec.features.channels[0].vadPosterior.empty()); // 省略 → 空,消费方按需重算
    REQUIRE(dec.features.channels[0].kwDbq == data.channels[0].kwDbq);

    const auto gz2 = scvb::state::encodeFeatures(dec.features);
    REQUIRE(gz2 == gz);
}

TEST_CASE("FEAT-ROUNDTRIP-3 sidecar 引用往返", "[state][features]")
{
    SidecarRef ref;
    ref.sessionGuid = kGuid;
    for (std::size_t i = 0; i < ref.sha256.size(); ++i)
        ref.sha256[i] = static_cast<std::uint8_t>(i);
    ref.sidecarBytes = 12345678u;

    const auto payload = scvb::state::encodeReference(ref, 44100, 10, 15);
    REQUIRE_FALSE(payload.empty());

    const FeatDecode dec = scvb::state::decodeFeatures(payload.data(), payload.size());
    REQUIRE(dec.ok);
    REQUIRE_FALSE(dec.embedded);
    REQUIRE(dec.ref.sessionGuid == kGuid);
    REQUIRE(dec.ref.sha256 == ref.sha256);
    REQUIRE(dec.ref.sidecarBytes == 12345678u);
}

// ============================================================================
// 压缩率与压缩炸弹
// ============================================================================

TEST_CASE("FEAT-COMPRESS-1 5min/15轨 gzip ≤2.25MB", "[state][features]")
{
    const FeaturesData data = makeFullFixture();
    const auto gz = scvb::state::encodeFeatures(data);
    REQUIRE_FALSE(gz.empty());
    // 2.25MB = 2,359,296 字节([J59] 线性外推;S4 实测 1,225,082 B,zlib 格式)。
    REQUIRE(gz.size() <= 2359296u);
}

TEST_CASE("FEAT-GZIP-1 压缩炸弹防护", "[state][features]")
{
    std::vector<std::uint8_t> data(1024, 0xABu); // 高度可压缩
    const auto gz = scvb::state::gzipCompress(data.data(), data.size());
    REQUIRE_FALSE(gz.empty());

    std::vector<std::uint8_t> out;
    REQUIRE_FALSE(scvb::state::gzipDecompress(gz.data(), gz.size(), out, 16)); // 输出上限 16B < 1024B
}

TEST_CASE("FEAT-GZIP-2 64KB 整数倍往返(解压边界)", "[state][features]")
{
    // 输出恰为 64KB / 128KB / 64KB+1:证伪 gzipDecompress 曾以 avail_in==0 判截断、
    // 在输出为 64KB 整数倍时把合法流误判为损坏(Claude 审查 #1)。
    for (const std::size_t n : {64u * 1024u, 128u * 1024u, 64u * 1024u + 1u})
    {
        std::vector<std::uint8_t> data(n);
        std::mt19937 rng(static_cast<std::uint32_t>(n));
        for (auto& b : data)
            b = static_cast<std::uint8_t>(rng() % 32u); // 5-bit 值,可压缩且确定性
        const auto gz = scvb::state::gzipCompress(data.data(), data.size());
        REQUIRE_FALSE(gz.empty());
        std::vector<std::uint8_t> out;
        REQUIRE(scvb::state::gzipDecompress(gz.data(), gz.size(), out, scvb::state::kMaxDecompressedFeatBytes));
        REQUIRE(out == data);
    }
}

// ============================================================================
// 不可信字节校验
// ============================================================================

TEST_CASE("FEAT-VALIDATE-1 截断/坏输入 → 解码失败", "[state][features]")
{
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    REQUIRE_FALSE(gz.empty());

    REQUIRE_FALSE(scvb::state::decodeFeatures(gz.data(), gz.size() - 1).ok); // 截断 gzip
    REQUIRE_FALSE(scvb::state::decodeFeatures(nullptr, 0).ok); // 空输入
}

TEST_CASE("FEAT-VALIDATE-2 codecVer 高于当前 → 按空处理", "[state][features]")
{
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    std::vector<std::uint8_t> raw;
    REQUIRE(scvb::state::gzipDecompress(gz.data(), gz.size(), raw, scvb::state::kMaxDecompressedFeatBytes));
    REQUIRE(raw.size() >= 6u);
    raw[4] = 0x02; // codecVer 1 → 2(节布局:u32 tag 0..3,u16 codecVer 4..5)
    raw[5] = 0x00;
    const auto gz2 = scvb::state::gzipCompress(raw.data(), raw.size());

    const FeatDecode dec = scvb::state::decodeFeatures(gz2.data(), gz2.size());
    REQUIRE_FALSE(dec.ok); // 不解码,防止接线层把「空特征」当作可回写结果而抹掉高版本数据
    REQUIRE(dec.codecVerNewer); // 接线层据此原样保留原始 chunk + 升级横幅(对齐 T19 三态)
}

TEST_CASE("FEAT-VALIDATE-3 vad 长度与 hopCount 不符 → 编码失败", "[state][features]")
{
    FeaturesData data = makeSmallFixture(true);
    data.channels[0].vadPosterior.pop_back(); // 破坏一致性
    REQUIRE(scvb::state::encodeFeatures(data).empty());
}

TEST_CASE("FEAT-VALIDATE-4 sessionGuid 路径穿越 → 拒解", "[state][features]")
{
    SidecarRef ref;
    ref.sessionGuid = kGuid;
    ref.sidecarBytes = 42u;
    const auto payload = scvb::state::encodeReference(ref, 48000, 10, 1);
    REQUIRE_FALSE(payload.empty());

    std::vector<std::uint8_t> raw;
    REQUIRE(scvb::state::gzipDecompress(payload.data(), payload.size(), raw, scvb::state::kMaxDecompressedFeatBytes));
    REQUIRE(raw.size() >= 17u + 36u);

    std::string evil;
    for (int i = 0; i < 12; ++i)
        evil += "../"; // 36 字符路径穿越串(12×"../")
    REQUIRE(evil.size() == 36u);
    std::copy(evil.begin(), evil.end(), raw.begin() + 17); // guid 位于节偏移 17(4+2+2+4+4+1)

    const auto gz2 = scvb::state::gzipCompress(raw.data(), raw.size());
    REQUIRE_FALSE(scvb::state::decodeFeatures(gz2.data(), gz2.size()).ok); // 非法 guid → 拒解
}

TEST_CASE("FEAT-VALIDATE-5 channelId/hopMs/sampleRate 越界 → 拒解", "[state][features]")
{
    const auto base = scvb::state::encodeFeatures(makeSmallFixture());
    REQUIRE_FALSE(base.empty());
    std::vector<std::uint8_t> raw;
    REQUIRE(scvb::state::gzipDecompress(base.data(), base.size(), raw, scvb::state::kMaxDecompressedFeatBytes));

    // channelId 越界:embedded 节 offset 17 是首 channel 的 channelId(1 → 200)。
    {
        auto r = raw;
        r[17] = 200;
        const auto gz = scvb::state::gzipCompress(r.data(), r.size());
        REQUIRE_FALSE(scvb::state::decodeFeatures(gz.data(), gz.size()).ok);
    }
    // hopMs=0:offset 12..15(消费方按 sampleRate*hopMs 换算会除零)。
    {
        auto r = raw;
        r[12] = r[13] = r[14] = r[15] = 0;
        const auto gz = scvb::state::gzipCompress(r.data(), r.size());
        REQUIRE_FALSE(scvb::state::decodeFeatures(gz.data(), gz.size()).ok);
    }
    // sampleRate=0:offset 8..11。
    {
        auto r = raw;
        r[8] = r[9] = r[10] = r[11] = 0;
        const auto gz = scvb::state::gzipCompress(r.data(), r.size());
        REQUIRE_FALSE(scvb::state::decodeFeatures(gz.data(), gz.size()).ok);
    }
}

// ============================================================================
// sha256
// ============================================================================

TEST_CASE("FEAT-SHA256-1 已知向量", "[state][features]")
{
    const char* msg = "abc";
    const auto d = scvb::state::sha256(msg, 3);
    static constexpr std::array<std::uint8_t, 32> kExpect = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    REQUIRE(d == kExpect);
}

// ============================================================================
// SidecarStore:8MB 阈值 / 缺失 / copy-on-write / owner.lock / 回滞
// ============================================================================

TEST_CASE("FEAT-SIDECAR-1 >8MB 自动转 sidecar 且回读成功", "[state][features][sidecar]")
{
    const FeaturesData huge = makeHugeIncompressibleFixture();
    const auto gz = scvb::state::encodeFeatures(huge);
    REQUIRE_FALSE(gz.empty());
    REQUIRE(gz.size() > scvb::state::kSidecarThresholdBytes); // 前提:确实 >8MB
    REQUIRE(SidecarStore::shouldUseSidecar(gz.size(), false)); // 自动转 sidecar

    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto digest = scvb::state::sha256(gz.data(), gz.size());
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 1, scvb::state::kFeatCodecVer, makeIdentity(1111, "A")));

    SidecarRef ref;
    ref.sessionGuid = kGuid;
    ref.sha256 = digest;
    ref.sidecarBytes = gz.size();
    const auto payload = scvb::state::encodeReference(ref, 48000, 10, 1);
    REQUIRE_FALSE(payload.empty());

    const LoadOutcome out = loadFeatures(payload, store);
    REQUIRE(out.ok);
    REQUIRE_FALSE(out.featuresMissing);
    REQUIRE(out.features.channels.size() == 1u);

    const auto gz2 = scvb::state::encodeFeatures(out.features);
    REQUIRE(gz2 == gz); // 回读后重编码逐字节一致
}

TEST_CASE("FEAT-SIDECAR-2 删除 sidecar 后特征缺失(曲线/配置不受影响)", "[state][features][sidecar]")
{
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto digest = scvb::state::sha256(gz.data(), gz.size());
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, makeIdentity(1111, "A")));

    SidecarRef ref;
    ref.sessionGuid = kGuid;
    ref.sha256 = digest;
    ref.sidecarBytes = gz.size();
    const auto payload = scvb::state::encodeReference(ref, 48000, 10, 2);

    store.remove(kGuid); // 删除 sidecar 目录

    const LoadOutcome out = loadFeatures(payload, store);
    REQUIRE(out.ok); // 加载不失败
    REQUIRE(out.featuresMissing); // 特征缺失标记(→ UI 横幅「采集数据缺失/过期,请重新采集」)
    REQUIRE(out.features.channels.empty()); // 特征清空
    // 曲线/配置在本层之外(StateChunks 的 CRVS/CFGS),FeaturesCodec/SidecarStore 不触碰它们。
}

TEST_CASE("FEAT-SIDECAR-3 双开同 GUID → copy-on-write", "[state][features][sidecar]")
{
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    TempDir tmp;
    SidecarStore store(tmp.path);
    const ProcessIdentity idA = makeIdentity(1111, "A");
    const ProcessIdentity idB = makeIdentity(2222, "B");

    // 实例 A 写 sidecar(owner.lock pid=A,活)。
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));

    // 实例 B 打开同 GUID:owner.lock 活且 pid≠B → copy-on-write。
    std::string newGuid;
    REQUIRE(store.copyOnWriteIfNeeded(kGuid, idB, scvb::state::epochMsNow(), newGuid));
    REQUIRE(newGuid != kGuid);
    REQUIRE(newGuid.size() == 36u);

    // 新目录复制了 features.bin.gz(逐字节一致)。
    std::vector<std::uint8_t> copied;
    REQUIRE(store.read(newGuid, copied));
    REQUIRE(copied == gz);

    // 新目录 owner.lock 归属 B。
    OwnerLock lock;
    REQUIRE(store.readOwnerLock(newGuid, lock));
    REQUIRE(lock.pid == idB.pid);

    // 锁为己有(A)→ 不再 CoW。
    std::string unused;
    REQUIRE_FALSE(store.copyOnWriteIfNeeded(kGuid, idA, scvb::state::epochMsNow(), unused));
}

TEST_CASE("FEAT-SIDECAR-4 owner.lock 判活与过期", "[state][features][sidecar]")
{
    TempDir tmp;
    SidecarStore store(tmp.path);
    const std::int64_t now = 1000000000000;

    OwnerLock alive;
    alive.pid = 42;
    alive.heartbeatEpochMs = static_cast<std::uint64_t>(now); // 刚刚
    REQUIRE(scvb::state::isOwnerLockAlive(alive, now));

    OwnerLock expired;
    expired.pid = 42;
    expired.heartbeatEpochMs = static_cast<std::uint64_t>(now - scvb::state::kOwnerLockAliveHeartbeatMs); // 恰 30s
    REQUIRE_FALSE(scvb::state::isOwnerLockAlive(expired, now));

    OwnerLock noPid;
    noPid.heartbeatEpochMs = static_cast<std::uint64_t>(now);
    REQUIRE_FALSE(scvb::state::isOwnerLockAlive(noPid, now)); // pid=0 → 无持有者

    // 过期锁不触发 CoW(即使 pid≠self)。
    REQUIRE(store.writeOwnerLock(kGuid, expired));
    std::string unused;
    REQUIRE_FALSE(store.copyOnWriteIfNeeded(kGuid, makeIdentity(999, "C"), now, unused));
}

TEST_CASE("FEAT-SIDECAR-5 回滞 8MB/6MB", "[state][features][sidecar]")
{
    const std::uint64_t kB8 = scvb::state::kSidecarThresholdBytes;
    const std::uint64_t kB6 = scvb::state::kReembedThresholdBytes;

    // 未走 sidecar:>8MB 才转。
    REQUIRE_FALSE(SidecarStore::shouldUseSidecar(kB8, false)); // ==8MB 仍内嵌
    REQUIRE(SidecarStore::shouldUseSidecar(kB8 + 1, false)); // >8MB 转

    // 已走 sidecar:<6MB 收回,>=6MB 保持(回滞)。
    REQUIRE_FALSE(SidecarStore::shouldUseSidecar(kB6 - 1, true)); // <6MB 收回内嵌
    REQUIRE(SidecarStore::shouldUseSidecar(kB6, true)); // ==6MB 保持
    REQUIRE(SidecarStore::shouldUseSidecar(kB8 - 1, true)); // 6..8MB 保持(不再横跳)
}

TEST_CASE("FEAT-SIDECAR-6 PID 复用竞态与路径穿越防护", "[state][features][sidecar]")
{
    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());

    // ---- PID 复用:同 pid、不同启动时间 → 视为他人,触发 CoW(不误判为己有)。----
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, makeIdentity(1111, "A")));

    ProcessIdentity reused; // pid=1111 被复用,但启动时间不同
    reused.pid = 1111;
    reused.processStartEpochMs = 999999; // ≠ A(1001111)
    reused.hostName = "A-reused";
    std::string newGuid;
    REQUIRE(store.copyOnWriteIfNeeded(kGuid, reused, scvb::state::epochMsNow(), newGuid));
    REQUIRE(newGuid != kGuid);

    // ---- 路径穿越防护:非法 guid 不得越出 sessions 目录(读/写/删均拒绝)。----
    std::string evil;
    for (int i = 0; i < 12; ++i)
        evil += "../";
    std::vector<std::uint8_t> out;
    REQUIRE_FALSE(store.read(evil, out));
    REQUIRE_FALSE(store.write(evil, gz.data(), gz.size(), 1, scvb::state::kFeatCodecVer, makeIdentity(1, "x")));
    store.remove(evil); // no-op,不越界删
    REQUIRE(std::filesystem::exists(tmp.path));
}

// ---------------------------------------------------------------------------
// [SL-233] owner.lock 的 10s 周期刷新(STATE_SCHEMA §4.3)。
//
// 契约写了「每 10s 由消息线程刷新;判活 = pid 存在 ∧ 心跳 < 30s」,而实现里 writeOwnerLock
// 只有保存工程与 CoW 两个调用点 —— 于是「开着工程但不保存」的实例在上次保存 30s 后就被判死,
// 后开的第二个实例读到死锁,不 copy-on-write 而直接共享同一份 sidecar(正是 CoW 要防的那一幕)。
// 本用例把两个方向都钉住:同一个时刻 t+40s,续过租的判活、没续过的判死。
// ---------------------------------------------------------------------------
TEST_CASE("FEAT-SIDECAR-7 owner.lock 周期刷新维持租约", "[state][features][sidecar]")
{
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    const ProcessIdentity idA = makeIdentity(1111, "A");
    const ProcessIdentity idB = makeIdentity(2222, "B");

    // 刷新周期必须真的比判活窗口短,否则「每 10s 刷一次」根本救不了「30s 判死」。
    REQUIRE(scvb::state::kOwnerLockRefreshIntervalMs < scvb::state::kOwnerLockAliveHeartbeatMs);

    OwnerLock written;
    std::int64_t t0 = 0;

    // ---- ① 反向验证:摘掉刷新 → A 的锁在 40s 后被判死,B 直接共享同一份 sidecar(修复前的行为)。----
    {
        TempDir tmp;
        SidecarStore store(tmp.path);
        REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));
        REQUIRE(store.readOwnerLock(kGuid, written));
        t0 = static_cast<std::int64_t>(written.heartbeatEpochMs);

        const std::int64_t t40 = t0 + 40000;
        OwnerLock stale;
        REQUIRE(store.readOwnerLock(kGuid, stale));
        REQUIRE_FALSE(scvb::state::isOwnerLockAlive(stale, t40)); // 判死
        std::string unused;
        REQUIRE_FALSE(store.copyOnWriteIfNeeded(kGuid, idB, t40, unused)); // → 不 CoW,共享
    }

    // ---- ② 正向:A 每 10s 续租 → 同一个 t+40s,B 判活并走 CoW。----
    {
        TempDir tmp;
        SidecarStore store(tmp.path);
        REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));
        REQUIRE(store.readOwnerLock(kGuid, written));
        t0 = static_cast<std::int64_t>(written.heartbeatEpochMs);

        for (std::int64_t t = t0 + scvb::state::kOwnerLockRefreshIntervalMs; t <= t0 + 40000;
             t += scvb::state::kOwnerLockRefreshIntervalMs)
        {
            REQUIRE(store.refreshOwnerLock(kGuid, idA, t)); // 消息线程每 10s 一次
        }

        const std::int64_t t40 = t0 + 40000;
        OwnerLock fresh;
        REQUIRE(store.readOwnerLock(kGuid, fresh));
        REQUIRE(fresh.heartbeatEpochMs == static_cast<std::uint64_t>(t40)); // 心跳确实推进了
        REQUIRE(fresh.pid == idA.pid); // 只动时间戳,持有者原样
        REQUIRE(fresh.processStartEpochMs == idA.processStartEpochMs);
        REQUIRE_FALSE(fresh.heartbeatIso8601.empty()); // 契约里的可读字段同步刷新
        REQUIRE(scvb::state::isOwnerLockAlive(fresh, t40)); // 判活

        std::string newGuid;
        REQUIRE(store.copyOnWriteIfNeeded(kGuid, idB, t40, newGuid)); // → 走 CoW
        REQUIRE(newGuid != kGuid);

        // ---- ③ 停止刷新 >30s 后仍会判死(租约不是永久的:实例关掉就该让出去)。----
        const std::int64_t tDead = t40 + scvb::state::kOwnerLockAliveHeartbeatMs;
        OwnerLock afterStop;
        REQUIRE(store.readOwnerLock(kGuid, afterStop));
        REQUIRE_FALSE(scvb::state::isOwnerLockAlive(afterStop, tDead));
    }
}

// ---------------------------------------------------------------------------
// [SL-233] 续租的三道闸:非法 guid / 盘上无锁不新建 / 锁非己绝不覆盖。
// 第三道是要害 —— 覆盖他人的锁等于把别人正开着的 sidecar 抢过来,CoW 的判据就此消失。
// ---------------------------------------------------------------------------
TEST_CASE("FEAT-SIDECAR-8 owner.lock 刷新不越权", "[state][features][sidecar]")
{
    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    const ProcessIdentity idA = makeIdentity(1111, "A");
    const ProcessIdentity idB = makeIdentity(2222, "B");

    // ① 盘上没有 owner.lock → 不刷新、**也不新建**(没写过 sidecar 就没有租约可续)。
    REQUIRE_FALSE(store.refreshOwnerLock(kGuid, idA, scvb::state::epochMsNow()));
    REQUIRE_FALSE(
        std::filesystem::exists(store.sessionDir(kGuid) / std::string(scvb::state::kSidecarOwnerLockFilename)));

    // ② 锁归 A:B 刷不动,且盘上的心跳一个字节都没变。
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));
    OwnerLock before;
    REQUIRE(store.readOwnerLock(kGuid, before));
    const std::int64_t later = static_cast<std::int64_t>(before.heartbeatEpochMs) + 5000;
    REQUIRE_FALSE(store.refreshOwnerLock(kGuid, idB, later));
    OwnerLock after;
    REQUIRE(store.readOwnerLock(kGuid, after));
    REQUIRE(after.pid == before.pid);
    REQUIRE(after.heartbeatEpochMs == before.heartbeatEpochMs);
    REQUIRE(after.heartbeatIso8601 == before.heartbeatIso8601);

    // ③ 反向验证:同一时刻换成持有者 A 就刷得动(排除「这个用例恒 false」)。
    REQUIRE(store.refreshOwnerLock(kGuid, idA, later));
    OwnerLock mine;
    REQUIRE(store.readOwnerLock(kGuid, mine));
    REQUIRE(mine.heartbeatEpochMs == static_cast<std::uint64_t>(later));

    // ④ PID 复用:pid 相同、进程启动时间不同 → 不是己方,拒刷(与 CoW 同口径的双元组)。
    ProcessIdentity reused;
    reused.pid = idA.pid;
    reused.processStartEpochMs = idA.processStartEpochMs + 1;
    reused.hostName = "A-reused";
    REQUIRE_FALSE(store.refreshOwnerLock(kGuid, reused, later + 1000));

    // ⑤ pid=0(拿不到进程身份的退化值)不得宣示占有。
    ProcessIdentity unknown;
    unknown.pid = 0;
    unknown.processStartEpochMs = 0;
    REQUIRE_FALSE(store.refreshOwnerLock(kGuid, unknown, later + 1000));

    // ⑥ 路径穿越:非法 guid 一律拒绝(与 read/write/remove 同一道闸)。
    std::string evil;
    for (int i = 0; i < 12; ++i)
        evil += "../";
    REQUIRE_FALSE(store.refreshOwnerLock(evil, idA, later + 1000));
}

// ---------------------------------------------------------------------------
// [SL-233] 加载期认领无主的 owner.lock(§4.3 CoW 的前置)。
//
// 「只有写过 sidecar 的实例才持锁」留下一条支路:打开一份**上次会话**存的工程、本会话还没
// 保存时,盘上躺的是上一会话留下的死锁,谁都不持有它 —— 续租在归属闸上被拒,后开的第二份
// 副本读到死锁,不 copy-on-write 而直接共享。认领只在「无人持有」时发生,活锁归他人一律不动。
// ---------------------------------------------------------------------------
TEST_CASE("FEAT-SIDECAR-9 加载期认领无主锁,但绝不抢活锁", "[state][features][sidecar]")
{
    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    const ProcessIdentity idA = makeIdentity(1111, "A"); // 上一会话
    const ProcessIdentity idB = makeIdentity(2222, "B"); // 本会话:只加载,没保存过
    const ProcessIdentity idC = makeIdentity(3333, "C"); // 第三方

    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));
    OwnerLock written;
    REQUIRE(store.readOwnerLock(kGuid, written));
    const std::int64_t t0 = static_cast<std::int64_t>(written.heartbeatEpochMs);

    // A 早已退出:t0+40s 时它的锁按契约判死。
    const std::int64_t tDead = t0 + 40000;
    OwnerLock stale;
    REQUIRE(store.readOwnerLock(kGuid, stale));
    REQUIRE_FALSE(scvb::state::isOwnerLockAlive(stale, tDead));

    // ---- ① 反向验证(修复前的行为):没人认领 → C 判死 → 不 CoW,直接共享。----
    std::string shared;
    REQUIRE_FALSE(store.copyOnWriteIfNeeded(kGuid, idC, tDead, shared));

    // ---- ② B 加载工程时认领这把死锁 → 归 B 且判活。----
    REQUIRE(store.claimOwnerLockIfUnheld(kGuid, idB, tDead));
    OwnerLock mine;
    REQUIRE(store.readOwnerLock(kGuid, mine));
    REQUIRE(mine.pid == idB.pid);
    REQUIRE(mine.processStartEpochMs == idB.processStartEpochMs);
    REQUIRE(mine.heartbeatEpochMs == static_cast<std::uint64_t>(tDead));
    REQUIRE(scvb::state::isOwnerLockAlive(mine, tDead));
    // 认领之后 refreshOwnerLock 的所有权前提才成立(此前 B 刷不动)。
    REQUIRE(store.refreshOwnerLock(kGuid, idB, tDead + 10000));

    // ---- ③ 正向:同一时刻 C 再来开同一份工程 → 判活 → 走 CoW。----
    std::string newGuid;
    REQUIRE(store.copyOnWriteIfNeeded(kGuid, idC, tDead + 10000, newGuid));
    REQUIRE(newGuid != kGuid);

    // ---- ④ 要害反向验证:B 的锁是活的,C **绝不**能把它认领走。----
    OwnerLock beforeSteal;
    REQUIRE(store.readOwnerLock(kGuid, beforeSteal));
    REQUIRE_FALSE(store.claimOwnerLockIfUnheld(kGuid, idC, tDead + 10000));
    OwnerLock afterSteal;
    REQUIRE(store.readOwnerLock(kGuid, afterSteal));
    REQUIRE(afterSteal.pid == beforeSteal.pid);
    REQUIRE(afterSteal.processStartEpochMs == beforeSteal.processStartEpochMs);
    REQUIRE(afterSteal.heartbeatEpochMs == beforeSteal.heartbeatEpochMs);

    // ---- ⑤ 已归自己 → 幂等返回 true,且不改心跳(续租是 refreshOwnerLock 的事)。----
    REQUIRE(store.claimOwnerLockIfUnheld(kGuid, idB, tDead + 20000));
    OwnerLock idempotent;
    REQUIRE(store.readOwnerLock(kGuid, idempotent));
    REQUIRE(idempotent.heartbeatEpochMs == beforeSteal.heartbeatEpochMs);

    // ---- ⑥ 三道守卫:pid=0 / 路径穿越 / 没有特征文件时不得凭空造出一个只有锁的目录。----
    ProcessIdentity unknown;
    REQUIRE_FALSE(store.claimOwnerLockIfUnheld(kGuid, unknown, tDead));
    std::string evil;
    for (int i = 0; i < 12; ++i)
        evil += "../";
    REQUIRE_FALSE(store.claimOwnerLockIfUnheld(evil, idB, tDead));

    const std::string kAbsent = "99999999-8888-4777-8666-555555555555";
    REQUIRE_FALSE(store.claimOwnerLockIfUnheld(kAbsent, idB, tDead));
    REQUIRE_FALSE(std::filesystem::exists(store.sessionDir(kAbsent)));
}

// ---------------------------------------------------------------------------
// [SL-233] owner.lock 的两个心跳字段必须出自同一个时间戳。
// heartbeatEpochMs 判活、heartbeatIso8601 可读(契约 §4.3 两者并列);分头取「现在」会在
// 注入时钟下写出一份自相矛盾的锁文件。
// ---------------------------------------------------------------------------
TEST_CASE("FEAT-SIDECAR-10 心跳的 epoch 与 ISO8601 同源", "[state][features][sidecar]")
{
    TempDir tmp;
    SidecarStore store(tmp.path);
    const auto gz = scvb::state::encodeFeatures(makeSmallFixture());
    const ProcessIdentity idA = makeIdentity(1111, "A");
    REQUIRE(store.write(kGuid, gz.data(), gz.size(), 2, scvb::state::kFeatCodecVer, idA));

    // 注入一个远离「现在」的时间戳(2001-09-09T01:46:40Z),两个字段必须都指向它。
    constexpr std::int64_t kFixedEpochMs = 1000000000000;
    REQUIRE(store.refreshOwnerLock(kGuid, idA, kFixedEpochMs));

    OwnerLock lock;
    REQUIRE(store.readOwnerLock(kGuid, lock));
    REQUIRE(lock.heartbeatEpochMs == static_cast<std::uint64_t>(kFixedEpochMs));
    REQUIRE(lock.heartbeatIso8601 == scvb::state::iso8601UtcFromEpochMs(kFixedEpochMs));
    // 反向验证:它确实不是「刷新那一刻的真实墙钟」(否则本条断言恒真,证明不了同源)。
    REQUIRE(lock.heartbeatIso8601 != scvb::state::iso8601UtcNow());
}
