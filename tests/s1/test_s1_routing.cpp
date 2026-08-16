// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_s1_tests —— S1 路由 spike 离线验证(10-validation §1.1.3 能离线判定的部分)。
//   用 s1_core 直测:环往返按位相等(mono/stereo interleaved,捕获 L/R 互换/单路复制)、
//   epoch 换代、套圈弃块、负 playhead 不写、Registry 双阈值接管、OutputStage/BusXfade
//   稳态、15 轨对位 null、双组隔离。DAW 内八格矩阵(验收 1-5/7/8)在周日上机。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // 禁用 windows.h 的 min/max 宏,避免与 std::min 冲突
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

TEST_CASE("Registry 同 pid 重认领(采样率切换)成功并刷新 slot", "[s1][registry][reclaim]")
{
    Registry reg(kTestGroupRegistry);
    REQUIRE(reg.open() == Registry::ClaimResult::kClaimed);
    const u32 pid = static_cast<u32>(::GetCurrentProcessId());
    const u64 now = steadyNowMs();

    REQUIRE(reg.claimInput(3, pid, 44100, 512, now) == Registry::ClaimResult::kClaimed);
    InputSlot* s = reg.inputSlot(3);
    REQUIRE(s != nullptr);
    REQUIRE(s->state.load(std::memory_order_acquire) == kSlotActive);
    REQUIRE(s->sample_rate == 44100);

    // 采样率切换后的重认领:slot 被【自己】占用 → 直接成功并刷新 sample_rate/max_block/心跳,
    // 不得 kConflict(否则 Input 停心跳停环 → Output mask 塌缩 → 总线静音,P0)。
    const u64 now2 = now + 100;
    REQUIRE(reg.claimInput(3, pid, 48000, 256, now2) == Registry::ClaimResult::kClaimed);
    REQUIRE(s->pid == pid);
    REQUIRE(s->sample_rate == 48000);
    REQUIRE(s->max_block == 256);
    REQUIRE(s->heartbeat_ms.load(std::memory_order_acquire) == now2);
    REQUIRE(s->state.load(std::memory_order_acquire) == kSlotActive);

    // 他人 pid 仍冲突(同 pid 分支不放宽接管语义)。
    REQUIRE(reg.claimInput(3, pid + 1, 48000, 256, now2) == Registry::ClaimResult::kConflict);
    reg.releaseInput(3, pid);
}

TEST_CASE("Registry claimOutput 同 pid 重认领不清 connected_mask", "[s1][registry][reclaim]")
{
    Registry reg(kTestGroupRegistry);
    REQUIRE(reg.open() == Registry::ClaimResult::kClaimed);
    const u32 pid = static_cast<u32>(::GetCurrentProcessId());
    const u64 now = steadyNowMs();

    REQUIRE(reg.claimOutput(pid, now) == Registry::ClaimResult::kClaimed);
    OutputSlot* out = reg.outputSlot();
    REQUIRE(out != nullptr);
    const u32 mask = 0x5a5a;
    out->connected_mask.store(mask, std::memory_order_relaxed);

    // 采样率切换重认领:同 pid → 成功,且 connected_mask 保持(清 mask 会打掉 Input 拓扑判定,P0)。
    REQUIRE(reg.claimOutput(pid, now + 100) == Registry::ClaimResult::kClaimed);
    REQUIRE(out->connected_mask.load(std::memory_order_relaxed) == mask);
    REQUIRE(out->state.load(std::memory_order_acquire) == kSlotActive);

    // 他人 pid 仍冲突。
    REQUIRE(reg.claimOutput(pid + 1, now + 200) == Registry::ClaimResult::kConflict);
    reg.releaseOutput(pid);
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

TEST_CASE("Output 几何读取门(magic 后行:读 magic 时几何字段必已就绪)", "[s1][ring][magic-last]")
{
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 12;
    const u32 channels = 2;

    AudioRing writer(kTestGroupRing, 1);
    REQUIRE(writer.createForInput(sr, ringFrames, channels) == InitResult::kOk);

    AudioRing reader(kTestGroupRing, 1);
    REQUIRE(reader.openForOutput() == InitResult::kOk);

    // magic 后行不变式:openForOutput 读到 magic==SCVB 时,几何字段必已由创建者写毕(review 1)。
    REQUIRE(reader.header()->sample_rate == sr);
    REQUIRE(reader.header()->ring_frames == ringFrames);
    REQUIRE(reader.header()->channels == channels);
    REQUIRE(reader.ringFrames() == ringFrames);
    REQUIRE(reader.channels() == channels);
}

TEST_CASE("oversized 块按 chunk 处理(固定缓冲,零堆分配)完整往返不截断", "[s1][ring][oversized]")
{
    // review 3 收口:processBlock 不再 resize/ensureCapacity(§8 红旗)。此处镜像 processor 的
    // chunk 循环——固定 maxBlock 尺寸的缓冲,把 oversized 块切成 ≤maxBlock 子块依次写/读,
    // 全程只用预分配缓冲,零堆分配;数据完整不截断。
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 14; // 16384 帧,容纳 oversized 块
    const int maxBlock = 512; // 典型 samplesPerBlock
    const int total = 8192; // 16× maxBlock,oversized

    AudioRing writer(kTestGroupRing, 2);
    REQUIRE(writer.createForInput(sr, ringFrames, 1) == InitResult::kOk);
    AudioRing reader(kTestGroupRing, 2);
    REQUIRE(reader.openForOutput() == InitResult::kOk);

    std::vector<float> src(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i)
    {
        src[static_cast<std::size_t>(i)] = (i % 3 == 0) ? 1.0f : -0.5f;
    }
    std::vector<float> dst(static_cast<std::size_t>(total));

    // 固定尺寸子块缓冲(对应 processor 的 capBuf_/trackBuf_,不 resize)。
    std::vector<float> capBuf(static_cast<std::size_t>(maxBlock), 0.0f);
    std::vector<float> trackBuf(static_cast<std::size_t>(maxBlock), 0.0f);

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min(maxBlock, total - start);
        for (int i = 0; i < n; ++i)
        {
            capBuf[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(start + i)];
        }
        writer.writeBlock(start, capBuf.data(), n);
    }
    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min(maxBlock, total - start);
        REQUIRE(reader.readBlock(start, n, trackBuf.data()) == AudioRing::ReadStatus::kOk);
        for (int i = 0; i < n; ++i)
        {
            dst[static_cast<std::size_t>(start + i)] = trackBuf[static_cast<std::size_t>(i)];
        }
    }
    for (int i = 0; i < total; ++i)
    {
        REQUIRE(floatBits(dst[static_cast<std::size_t>(i)]) == floatBits(src[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("AudioRing 重创建(声道数切换)往返正确且旧视图保活", "[s1][ring][v3]")
{
    // v3 崩溃修复回归:覆盖式重映射期间旧视图退休保活(prevView_),在途 writeBlock 不落悬空地址;
    // 几何字段刷新、新环写读往返正确。
    const u32 sr = 48000;
    const u32 ringFrames = 1u << 12;
    AudioRing ring(kTestGroupRing, 14);
    REQUIRE(ring.createForInput(sr, ringFrames, 2) == InitResult::kOk);
    REQUIRE(ring.isOpen());

    std::vector<float> stereo(64, 0.25f);
    ring.writeBlock(0, stereo.data(), 32); // 32 帧 stereo
    float* oldData = ring.data(); // 旧映射指针(重创建后仍须有效)

    // 同环重创建(模拟采样率/声道切换):stereo → mono,几何字段刷新。
    REQUIRE(ring.createForInput(96000, ringFrames, 1) == InitResult::kOk);
    REQUIRE(ring.isOpen());
    REQUIRE(ring.header()->sample_rate == 96000);
    REQUIRE(ring.header()->ring_frames == ringFrames);
    REQUIRE(ring.header()->channels == 1);
    REQUIRE(ring.channels() == 1);

    // 旧视图保活:解引用旧 data_ 不崩溃(读一次即止,退休视图不做内容断言)。
    const float probe = oldData[0];
    (void)probe;

    // 新环 mono 写读往返按位相等。
    std::vector<float> mono(32);
    for (int i = 0; i < 32; ++i)
    {
        mono[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 1.0f : -1.0f;
    }
    ring.writeBlock(0, mono.data(), 32);
    std::vector<float> dst(32);
    REQUIRE(ring.readBlock(0, 32, dst.data()) == AudioRing::ReadStatus::kOk);
    for (int i = 0; i < 32; ++i)
    {
        REQUIRE(floatBits(dst[static_cast<std::size_t>(i)]) == floatBits(mono[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("AudioRing createForInput 非法参数失败后 isOpen 为假且写安全", "[s1][ring][v3]")
{
    // v3 崩溃修复:任何失败路径(含早期参数校验)必须先清 header_/data_/几何,
    // isOpen()==false → 音频线程写读关断,杜绝悬挂。
    const u32 sr = 48000;
    AudioRing ring(kTestGroupRing, 15);
    REQUIRE(ring.createForInput(sr, 1u << 12, 1) == InitResult::kOk);
    REQUIRE(ring.isOpen());

    REQUIRE(ring.createForInput(sr, 1u << 12, 3) == InitResult::kFailed); // 非法声道数
    REQUIRE_FALSE(ring.isOpen());

    std::vector<float> block(64, 1.0f);
    ring.writeBlock(0, block.data(), 64); // isOpen false → 早退,不崩溃、不写
    REQUIRE(ring.header() == nullptr);
    REQUIRE(ring.data() == nullptr);
}
