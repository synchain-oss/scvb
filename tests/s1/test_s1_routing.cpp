// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_s1_tests —— S1 路由 spike 离线验证(10-validation §1.1.3 能离线判定的部分)。
//   用 s1_core 直测:环往返按位相等(mono/stereo interleaved,捕获 L/R 互换/单路复制)、
//   epoch 换代、套圈弃块、负 playhead 不写、Registry 双阈值接管、OutputStage/BusXfade
//   稳态、15 轨对位 null、双组隔离。DAW 内八格矩阵(验收 1-5/7/8)在周日上机。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AudioRing.h"
#include "BusXfade.h"
#include "OutputStage.h"
#include "Registry.h"
#include "SegmentLayout.h"

namespace
{
using namespace scvb;

u32 floatBits(float x)
{
    u32 u = 0;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

// 生成 click 素材(镜像 scvb_nulltest --gen-click 的相位口径):mono 轨 t 脉冲在 (f-t)%period==0;
// stereo 轨 L 在偶数偏移、R 在奇数偏移(互换/复制即可见)。
std::vector<float> makeClickTrack(int trackIndex, bool stereo, int frames, int period)
{
    const int ch = stereo ? 2 : 1;
    std::vector<float> v(static_cast<std::size_t>(frames) * ch, 0.0f);
    if (stereo)
    {
        for (int f = 2 * trackIndex; f < frames; f += period)
        {
            v[static_cast<std::size_t>(f) * 2 + 0] = 1.0f;
        }
        for (int f = 2 * trackIndex + 1; f < frames; f += period)
        {
            v[static_cast<std::size_t>(f) * 2 + 1] = 1.0f;
        }
    }
    else
    {
        for (int f = trackIndex; f < frames; f += period)
        {
            v[static_cast<std::size_t>(f)] = 1.0f;
        }
    }
    return v;
}

// 用独立组号隔离测试(与可能运行的 DAW 实例不串扰)。
constexpr u32 kTestGroupRing = 8;
constexpr u32 kTestGroupRegistry = 6;
constexpr u32 kTestGroupFull = 7;
constexpr u32 kTestGroupA = 1;
constexpr u32 kTestGroupB = 2;
} // namespace

TEST_CASE("AudioRing mono 往返按位相等", "[s1][ring]")
{
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 13;
    AudioRing writer(kTestGroupRing, 1);
    REQUIRE(writer.createForInput(sr, ringFrames, 1) == InitResult::kOk);
    AudioRing reader(kTestGroupRing, 1);
    REQUIRE(reader.openForOutput() == InitResult::kOk);
    REQUIRE(reader.channels() == 1);
    REQUIRE(reader.ringFrames() == ringFrames);

    const int block = 256;
    const int total = 2048;
    std::vector<float> src(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i)
    {
        src[static_cast<std::size_t>(i)] = (i % 7 == 0) ? 0.5f : -0.25f;
    }
    for (int t0 = 0; t0 < total; t0 += block)
    {
        writer.writeBlock(t0, src.data() + t0, block);
    }
    std::vector<float> dst(static_cast<std::size_t>(total));
    for (int t0 = 0; t0 < total; t0 += block)
    {
        REQUIRE(reader.readBlock(t0, block, dst.data() + t0) == AudioRing::ReadStatus::kOk);
    }
    for (int i = 0; i < total; ++i)
    {
        REQUIRE(floatBits(dst[static_cast<std::size_t>(i)]) == floatBits(src[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("AudioRing stereo interleaved LR 往返按位相等(L/R 互换可判)", "[s1][ring][j57]")
{
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 13;
    AudioRing writer(kTestGroupRing, 2);
    REQUIRE(writer.createForInput(sr, ringFrames, 2) == InitResult::kOk);
    AudioRing reader(kTestGroupRing, 2);
    REQUIRE(reader.openForOutput() == InitResult::kOk);
    REQUIRE(reader.channels() == 2);

    const int total = 2048;
    std::vector<float> src(static_cast<std::size_t>(total) * 2);
    for (int i = 0; i < total; ++i)
    {
        src[static_cast<std::size_t>(i) * 2] = (i % 2 == 0) ? 1.0f : -1.0f; // L:偶数=1,奇数=-1
        src[static_cast<std::size_t>(i) * 2 + 1] = (i % 2 == 0) ? -1.0f : 1.0f; // R:与 L 相反(互换立即可见)
    }
    const int block = 128;
    for (int t0 = 0; t0 < total; t0 += block)
    {
        writer.writeBlock(t0, src.data() + static_cast<std::size_t>(t0) * 2, block);
    }
    std::vector<float> dst(static_cast<std::size_t>(total) * 2);
    for (int t0 = 0; t0 < total; t0 += block)
    {
        REQUIRE(reader.readBlock(t0, block, dst.data() + static_cast<std::size_t>(t0) * 2) ==
                AudioRing::ReadStatus::kOk);
    }
    for (int i = 0; i < total; ++i)
    {
        REQUIRE(floatBits(dst[static_cast<std::size_t>(i) * 2]) == floatBits(src[static_cast<std::size_t>(i) * 2]));
        REQUIRE(floatBits(dst[static_cast<std::size_t>(i) * 2 + 1]) ==
                floatBits(src[static_cast<std::size_t>(i) * 2 + 1]));
    }
}

TEST_CASE("AudioRing epoch 换代 → 读侧 kGap", "[s1][ring]")
{
    const u32 sr = 48000;
    AudioRing writer(kTestGroupRing, 3);
    REQUIRE(writer.createForInput(sr, 1u << 12, 1) == InitResult::kOk);
    AudioRing reader(kTestGroupRing, 3);
    REQUIRE(reader.openForOutput() == InitResult::kOk);

    std::vector<float> block(256, 1.0f);
    writer.writeBlock(0, block.data(), 256);
    writer.bumpEpoch(); // 定位跳变
    std::vector<float> dst(256);
    // write_head 仍是 256,读 [256,512) 未覆盖 → kGap。
    REQUIRE(reader.readBlock(256, 256, dst.data()) == AudioRing::ReadStatus::kGap);
}

TEST_CASE("AudioRing 套圈(过旧数据)→ 弃块 kGap", "[s1][ring][g5]")
{
    const u32 sr = 48000;
    const u32 small = 1u << 6; // 64 帧,刻意缩小逼套圈
    AudioRing writer(kTestGroupRing, 4);
    REQUIRE(writer.createForInput(sr, small, 1) == InitResult::kOk);
    AudioRing reader(kTestGroupRing, 4);
    REQUIRE(reader.openForOutput() == InitResult::kOk);

    std::vector<float> b0(64, 1.0f);
    std::vector<float> b1(64, 2.0f);
    writer.writeBlock(0, b0.data(), 64);
    writer.writeBlock(128, b1.data(), 64); // 写方超前 128 > ring 64,已套圈覆盖 t0=0 位置
    std::vector<float> dst(64);
    REQUIRE(reader.readBlock(0, 64, dst.data()) == AudioRing::ReadStatus::kGap);
}

TEST_CASE("AudioRing 负 playhead 不写环", "[s1][ring][g6]")
{
    const u32 sr = 48000;
    AudioRing writer(kTestGroupRing, 5);
    REQUIRE(writer.createForInput(sr, 1u << 12, 1) == InitResult::kOk);
    std::vector<float> block(256, 1.0f);
    const u64 before = writer.header()->write_head_samples.load();
    writer.writeBlock(-10, block.data(), 256); // 负 t0 → 不写
    REQUIRE(writer.header()->write_head_samples.load() == before);
}

TEST_CASE("Registry 双阈值接管(J10)", "[s1][registry][j10]")
{
    Registry reg(kTestGroupRegistry);
    REQUIRE(reg.open() == Registry::ClaimResult::kClaimed);
    const u32 myPid = static_cast<u32>(::GetCurrentProcessId());
    const u64 now = steadyNowMs();

    REQUIRE(reg.claimInput(1, myPid, 48000, 512, now) == Registry::ClaimResult::kClaimed);
    // 心跳新鲜 + pid 存活 → 冲突
    REQUIRE(reg.claimInput(1, myPid + 1, 48000, 512, now) == Registry::ClaimResult::kConflict);

    // 陈旧 + pid 死 → 接管
    InputSlot* s = reg.inputSlot(1);
    s->heartbeat_ms.store(now - 6000, std::memory_order_relaxed);
    s->pid = 0; // 探活失败(0 = 无进程)
    REQUIRE(reg.claimInput(1, myPid + 2, 48000, 512, now) == Registry::ClaimResult::kClaimed);
    REQUIRE(reg.inputSlot(1)->pid == myPid + 2);
    reg.releaseInput(1, myPid + 2);

    // 陈旧但 pid 存活 → 冲突(不接管,保 SPSC 单写前提)
    REQUIRE(reg.claimInput(2, myPid, 48000, 512, now) == Registry::ClaimResult::kClaimed);
    reg.inputSlot(2)->heartbeat_ms.store(now - 6000, std::memory_order_relaxed);
    REQUIRE(reg.claimInput(2, myPid + 1, 48000, 512, now) == Registry::ClaimResult::kConflict);
    reg.releaseInput(2, myPid);
}

TEST_CASE("RampSwitcher 稳态静音/直通", "[s1][stage]")
{
    RampSwitcher rs;
    rs.prepare(48000.0);
    float one = 1.0f;
    float* ch1[1] = {&one};
    rs.render(ch1, 1, 1); // 默认直通 → no-op
    REQUIRE(one == 1.0f);

    rs.setTargetPassthrough(false); // 切静音
    std::vector<float> buf(10000, 1.0f);
    float* p[1] = {buf.data()};
    rs.render(p, 1, 10000); // 一次超 ramp 时长的块 → ramp 完成 + 余下清零
    REQUIRE(buf[9999] == 0.0f);

    std::vector<float> buf2(64, 1.0f);
    float* p2[1] = {buf2.data()};
    rs.render(p2, 1, 64); // 稳态静音
    REQUIRE(buf2[0] == 0.0f);
    REQUIRE(!rs.isPassthrough());
}

TEST_CASE("BusXfade 稳态激活 = 完全替换", "[s1][busxfade]")
{
    BusXfade bf;
    bf.prepare(48000.0, 1024);
    std::vector<float> mixL(1024, 0.5f);
    std::vector<float> mixR(1024, -0.5f);
    std::vector<float> bufL(1024, 0.0f);
    std::vector<float> bufR(1024, 0.0f);
    float* b[2] = {bufL.data(), bufR.data()};
    for (int i = 0; i < 10; ++i) // 超过 120ms 交叉期
    {
        bf.render(b, mixL.data(), mixR.data(), 1024);
    }
    REQUIRE(bf.isActive());
    std::fill(bufL.begin(), bufL.end(), 999.0f);
    std::fill(bufR.begin(), bufR.end(), -999.0f);
    bf.render(b, mixL.data(), mixR.data(), 1024);
    REQUIRE(floatBits(bufL[0]) == floatBits(0.5f));
    REQUIRE(floatBits(bufR[0]) == floatBits(-0.5f));
}

TEST_CASE("15 轨对位 null(13 mono + 2 stereo)逐样本按位相等", "[s1][null][j57][j59]")
{
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 13;
    const int block = 128;
    const int total = 4096;
    const int period = 4800; // 100ms click 周期

    std::vector<std::vector<float>> tracks(kMaxChannels);
    std::vector<int> chCount(kMaxChannels);
    for (u32 ch = 0; ch < kMaxChannels; ++ch)
    {
        const bool stereo = (ch >= 13); // V14/V15 stereo
        chCount[ch] = stereo ? 2 : 1;
        tracks[ch] = makeClickTrack(static_cast<int>(ch), stereo, total, period);
    }

    // 写侧(模拟 15 个 Input 实例)
    std::vector<std::unique_ptr<AudioRing>> writers;
    for (u32 ch = 0; ch < kMaxChannels; ++ch)
    {
        auto ring = std::make_unique<AudioRing>(kTestGroupFull, ch + 1);
        REQUIRE(ring->createForInput(sr, ringFrames, static_cast<u32>(chCount[ch])) == InitResult::kOk);
        writers.push_back(std::move(ring));
    }
    // 读侧(模拟 Output)
    std::vector<std::unique_ptr<AudioRing>> readers;
    for (u32 ch = 0; ch < kMaxChannels; ++ch)
    {
        auto ring = std::make_unique<AudioRing>(kTestGroupFull, ch + 1);
        REQUIRE(ring->openForOutput() == InitResult::kOk);
        readers.push_back(std::move(ring));
    }

    // 参考求和(与 Output 同序:channel 顺序;mono → L/R,stereo → L→L/R→R)
    std::vector<float> refL(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> refR(static_cast<std::size_t>(total), 0.0f);
    for (u32 ch = 0; ch < kMaxChannels; ++ch)
    {
        const float* t = tracks[ch].data();
        if (chCount[ch] == 1)
        {
            for (int i = 0; i < total; ++i)
            {
                refL[static_cast<std::size_t>(i)] += t[i];
                refR[static_cast<std::size_t>(i)] += t[i];
            }
        }
        else
        {
            for (int i = 0; i < total; ++i)
            {
                refL[static_cast<std::size_t>(i)] += t[static_cast<std::size_t>(i) * 2];
                refR[static_cast<std::size_t>(i)] += t[static_cast<std::size_t>(i) * 2 + 1];
            }
        }
    }

    // 模拟逐块写读 + 求和
    std::vector<float> accumL(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> accumR(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> trackBuf(static_cast<std::size_t>(block) * 2);
    u32 gapTotal = 0;
    for (int t0 = 0; t0 < total; t0 += block)
    {
        for (u32 ch = 0; ch < kMaxChannels; ++ch)
        {
            writers[ch]->writeBlock(t0, tracks[ch].data() + static_cast<std::size_t>(t0) * chCount[ch], block);
        }
        for (u32 ch = 0; ch < kMaxChannels; ++ch)
        {
            const auto st = readers[ch]->readBlock(t0, block, trackBuf.data());
            REQUIRE(st == AudioRing::ReadStatus::kOk); // 连续播放不得有缺口
            (void)st;
            if (chCount[ch] == 1)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float v = trackBuf[static_cast<std::size_t>(i)];
                    accumL[static_cast<std::size_t>(t0 + i)] += v;
                    accumR[static_cast<std::size_t>(t0 + i)] += v;
                }
            }
            else
            {
                for (int i = 0; i < block; ++i)
                {
                    accumL[static_cast<std::size_t>(t0 + i)] += trackBuf[static_cast<std::size_t>(i) * 2];
                    accumR[static_cast<std::size_t>(t0 + i)] += trackBuf[static_cast<std::size_t>(i) * 2 + 1];
                }
            }
        }
    }
    REQUIRE(gapTotal == 0);
    for (int i = 0; i < total; ++i)
    {
        REQUIRE(floatBits(accumL[static_cast<std::size_t>(i)]) == floatBits(refL[static_cast<std::size_t>(i)]));
        REQUIRE(floatBits(accumR[static_cast<std::size_t>(i)]) == floatBits(refR[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("双组隔离(同号 channel 零 CAS 冲突、环互不串扰)", "[s1][j66][group]")
{
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 12;

    // g1 与 g2 各自 claim channel 1(同号,不同组 → 互不冲突)
    Registry r1(kTestGroupA);
    Registry r2(kTestGroupB);
    REQUIRE(r1.open() == Registry::ClaimResult::kClaimed);
    REQUIRE(r2.open() == Registry::ClaimResult::kClaimed);
    const u32 pid = static_cast<u32>(::GetCurrentProcessId());
    const u64 now = steadyNowMs();
    REQUIRE(r1.claimInput(1, pid, sr, 512, now) == Registry::ClaimResult::kClaimed);
    REQUIRE(r2.claimInput(1, pid, sr, 512, now) == Registry::ClaimResult::kClaimed);

    AudioRing wa(kTestGroupA, 1);
    AudioRing wb(kTestGroupB, 1);
    REQUIRE(wa.createForInput(sr, ringFrames, 1) == InitResult::kOk);
    REQUIRE(wb.createForInput(sr, ringFrames, 1) == InitResult::kOk);

    std::vector<float> da(512, 1.0f);
    std::vector<float> db(512, -1.0f);
    wa.writeBlock(0, da.data(), 512);
    wb.writeBlock(0, db.data(), 512);

    AudioRing ra(kTestGroupA, 1);
    AudioRing rb(kTestGroupB, 1);
    REQUIRE(ra.openForOutput() == InitResult::kOk);
    REQUIRE(rb.openForOutput() == InitResult::kOk);
    std::vector<float> outa(512);
    std::vector<float> outb(512);
    REQUIRE(ra.readBlock(0, 512, outa.data()) == AudioRing::ReadStatus::kOk);
    REQUIRE(rb.readBlock(0, 512, outb.data()) == AudioRing::ReadStatus::kOk);
    for (int i = 0; i < 512; ++i)
    {
        REQUIRE(floatBits(outa[static_cast<std::size_t>(i)]) == floatBits(1.0f));
        REQUIRE(floatBits(outb[static_cast<std::size_t>(i)]) == floatBits(-1.0f));
    }
}
