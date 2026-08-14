// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:满配 state 夹具构造(确定性)。

#include "StateFixture.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace scvb::s4
{
namespace
{

std::int16_t dbToI16(float db)
{
    // 0.01 dB 量化,截到 int16 表示域(-327.68..+327.67 dB,实际人声范围远小于此)。
    const float scaled = std::round(db * 100.0f);
    const float clamped = std::max(-32768.0f, std::min(32767.0f, scaled));
    return static_cast<std::int16_t>(clamped);
}

ChannelFeatures makeFeatures(std::uint8_t channelId, std::uint64_t totalHops, std::uint32_t seed)
{
    std::mt19937 gen(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::normal_distribution<float> gauss(0.0f, 1.0f);

    ChannelFeatures f;
    f.channelId = channelId;
    f.coverage.push_back(Range{0u, totalHops});
    f.kwDbq.resize(totalHops);
    f.peakDbq.resize(totalHops);
    f.vadPosterior.resize(totalHops);

    // 短语门控:慢 AR(1) 起伏 + 硬阈值切出「有声/无声」段落;包络 AR(1) 叠加小幅 hop 抖动,
    // 使 int16 dB 序列具备真实人声能量包络的相邻相关性(gzip 据此获得 2–3× 压缩)。
    float gate = 0.5f;
    float env = 0.3f;
    const float gateAlpha = 0.995f;
    const float envAlpha = 0.92f;

    for (std::uint64_t i = 0; i < totalHops; ++i)
    {
        gate = gateAlpha * gate + (1.0f - gateAlpha) * unit(gen);
        env = envAlpha * env + (1.0f - envAlpha) * unit(gen);

        const bool active = gate > 0.55f;
        const float kwDb =
            active ? (-55.0f + 40.0f * env + 0.15f * gauss(gen)) : (-90.0f + 15.0f * env + 0.10f * gauss(gen));
        const float peakDb = kwDb + (2.0f + 2.0f * env) + 0.15f * gauss(gen);

        f.kwDbq[i] = dbToI16(kwDb);
        f.peakDbq[i] = dbToI16(peakDb);
        f.vadPosterior[i] =
            active ? static_cast<std::uint8_t>(std::min(255u, static_cast<std::uint32_t>(100.0f + 155.0f * env))) : 0u;
    }
    return f;
}

VersionCurves makeVersion(const std::string& name, std::size_t trackCount, std::uint64_t totalSamples,
                          std::uint32_t seed)
{
    std::mt19937 gen(seed);

    VersionCurves v;
    v.name = name;

    // 满配分段:每轨按 ~5s 一段覆盖整曲;pan/vol 取固定阶梯保证确定性。
    const std::uint64_t segCount = std::max<std::uint64_t>(1u, totalSamples / (5u * 48000u));
    const float panLadder[5] = {-80.0f, -40.0f, 0.0f, 40.0f, 80.0f};
    const float volLadder[5] = {-6.0f, -3.0f, 0.0f, 3.0f, 6.0f};

    for (std::size_t t = 0; t < trackCount; ++t)
    {
        TrackCurves tc;
        for (std::uint64_t i = 0; i < segCount; ++i)
        {
            CurveSegment s;
            s.t0 = static_cast<std::int64_t>((i * totalSamples) / segCount);
            s.t1 = static_cast<std::int64_t>(((i + 1) * totalSamples) / segCount);
            s.pan = panLadder[(i + t) % 5];
            s.volDb = volLadder[(i + t * 2u) % 5];
            s.flags = (i % 7u == 0u) ? 1u : 0u; // 偶发 user_edited(origin bit0),不锁
            tc.segments.push_back(s);
        }
        // 每轨 2 条 excluded_ranges(用户删段防复活记录,J34;per-version per-track;sample 单位)。
        for (std::uint32_t r = 0; r < 2; ++r)
        {
            const std::uint64_t a = (totalSamples / 5u) * (r + 1u);
            const std::uint64_t b = a + (totalSamples / 40u);
            tc.excludedRanges.push_back(SampleRange{static_cast<std::int64_t>(a), static_cast<std::int64_t>(b)});
        }
        v.tracks.push_back(std::move(tc));
    }

    // pan_curve:8 点(EQ 式,J07)。
    for (int i = 0; i < 8; ++i)
    {
        PanCurvePoint p;
        p.angle = -100.0f + static_cast<float>(i) * (200.0f / 7.0f);
        p.gainDb = static_cast<float>(std::sin(i) * 3.0);
        p.shape = static_cast<std::uint32_t>(i % 3);
        p.q = 1.0f + 0.25f * static_cast<float>(i % 3);
        p.side = static_cast<std::uint32_t>(i % 3);
        v.panCurve.points.push_back(p);
    }
    (void)gen;
    return v;
}

} // namespace

FullState buildFullState(const FixtureOptions& opts)
{
    FullState s;
    s.sampleRate = opts.sampleRate;
    s.hopMs = kDefaultHopMs;

    const std::uint64_t hopsPerSec = 1000u / s.hopMs; // hop=10ms -> 100 hop/s(与采样率无关)
    const std::uint64_t totalHops = static_cast<std::uint64_t>(opts.seconds) * hopsPerSec;
    const std::uint64_t totalSamples = static_cast<std::uint64_t>(opts.seconds) * opts.sampleRate;

    // channels[15]:13 mono + 2 stereo(J57/J59 口径,同 scvb_bench)。
    for (std::size_t t = 0; t < opts.tracks; ++t)
    {
        ChannelConfig c;
        c.enabled = true;
        c.label = "T" + std::to_string(t + 1);
        c.sourceChannels = (t + 2u >= opts.tracks) ? 2u : 1u;
        c.participateInAutoPan = (c.sourceChannels == 1u); // J60:mono true / stereo false
        c.priority = static_cast<std::uint32_t>((t * 7u) % 11u);
        c.pairId = ((t % 2u) == 0u && t + 1u < opts.tracks) ? static_cast<std::uint32_t>(t / 2u + 1u) : 0u;
        s.channels.push_back(std::move(c));
    }

    // 2 版曲线(J59 4→2)。
    s.versions.push_back(makeVersion("V1", opts.tracks, totalSamples, opts.seed + 1u));
    s.versions.push_back(makeVersion("V2", opts.tracks, totalSamples, opts.seed + 2u));

    // 特征:每轨一条,覆盖整曲 0..totalHops。
    for (std::size_t t = 0; t < opts.tracks; ++t)
        s.features.push_back(makeFeatures(static_cast<std::uint8_t>(t + 1u), totalHops,
                                          static_cast<std::uint32_t>(opts.seed + 100u + t)));

    // PRMS:123 参数(pan/vol/width/freeze 默认值 + 全局三件)。
    s.params.fill(0.0f);
    s.params[0] = 100.0f; // width
    s.params[1] = 0.0f; // ms_balance
    s.params[2] = 0.0f; // lead_select
    for (std::size_t i = 3; i < kParamCount; ++i)
    {
        const std::size_t k = (i - 3) % 4;
        if (k == 2) // width
            s.params[i] = 100.0f;
        else if (k == 0) // pan
            s.params[i] = 0.0f;
        else if (k == 1) // vol
            s.params[i] = 0.0f;
        else // freeze
            s.params[i] = 0.0f;
    }

    return s;
}

} // namespace scvb::s4
