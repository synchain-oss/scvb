// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_stress —— S1 免 DAW 压测宿主(v6 交付)。链接 s1_core,在命令行复现宿主编排:
//   Cubase 14/15 的 freeze / Render-in-Place / 导出会反复以 mono⇄stereo 重建音频环段;
//   段无法在存活视图下扩容,旧版把「请求大小」当「实际大小」发布几何 → 小视图 + 大几何 →
//   写环越界 0xC0000005(实锤:工作目录下全部 dmp 同址)。本工具把该序列 1:1 重放。
// 用法:scvb_stress <mono2stereo|flip|outputcheck|churn>;每场景独立进程运行,
//   崩溃由 WER LocalDumps 自动转储(collect-crash.ps1 -Setup 配置)。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "AudioRing.h"
#include "Registry.h"
#include "SegmentBackendWin32.h"
#include "SegmentLayout.h"

using namespace scvb;

namespace
{
int fail(const char* msg)
{
    std::printf("FAIL: %s\n", msg);
    return 1;
}

void fillSine(float* p, int frames, int channels, float freq)
{
    for (int i = 0; i < frames; ++i)
    {
        for (int c = 0; c < channels; ++c)
        {
            p[static_cast<std::size_t>(i) * channels + c] =
                0.25f * std::sin(2.0f * 3.14159265f * freq * static_cast<float>(i) / 48000.0f);
        }
    }
}

// v5 崩溃序列:mono 段先建 → 写 → 宿主切 stereo(冻结/导出路径)→ 写。
// v5:第二次 createForInput attach 到旧 mono 段却发布 stereo 几何 → 越界崩溃;
// v6:段恒按 stereo 容量创建 → 全程安全,几何随请求切换。
int scenarioMono2Stereo()
{
    const u32 frames = ringFramesFromEnv();
    AudioRing ring(1, 1);
    if (ring.createForInput(48000, frames, 1) != InitResult::kOk)
    {
        return fail("mono create");
    }
    std::printf("mono ring created: frames=%u channels=%u\n", ring.ringFrames(), ring.channels());
    std::vector<float> buf(static_cast<std::size_t>(512) * 2);
    fillSine(buf.data(), 512, 2, 440.0f);
    for (int b = 0; b < 200; ++b)
    {
        ring.writeBlock(static_cast<i64>(b) * 512, buf.data(), 512);
    }

    std::printf("switching to stereo (Cubase freeze/export re-creation path)...\n");
    if (ring.createForInput(48000, frames, 2) != InitResult::kOk)
    {
        return fail("stereo create");
    }
    std::printf("stereo ring: frames=%u channels=%u (header chans=%u)\n", ring.ringFrames(), ring.channels(),
                ring.header() != nullptr ? ring.header()->channels : 0);
    if (ring.channels() != 2)
    {
        return fail("geometry not stereo after re-creation");
    }
    // 越过 v5 崩溃点(stereo 几何写满超过旧 mono 段容量的位置)。
    for (int b = 0; b < 400; ++b)
    {
        ring.writeBlock(static_cast<i64>(200 + b) * 512, buf.data(), 512);
    }
    std::printf("mono2stereo: PASS\n");
    return 0;
}

// 反复 mono⇄stereo ×8(含写),对齐「冻结/取消冻结」风暴。
int scenarioFlip()
{
    const u32 frames = ringFramesFromEnv();
    AudioRing ring(1, 1);
    std::vector<float> buf(static_cast<std::size_t>(512) * 2);
    fillSine(buf.data(), 512, 2, 440.0f);
    i64 t0 = 0;
    for (int k = 0; k < 8; ++k)
    {
        const u32 chans = (k % 2 == 0) ? 1 : 2;
        if (ring.createForInput(48000, frames, chans) != InitResult::kOk)
        {
            return fail("flip create");
        }
        if (ring.channels() != chans)
        {
            return fail("flip geometry mismatch");
        }
        for (int b = 0; b < 100; ++b)
        {
            ring.writeBlock(t0, buf.data(), 512);
            t0 += 512;
        }
    }
    std::printf("flip: PASS (8 geometry flips)\n");
    return 0;
}

// Output 侧防护:v5 时代 Input 可能留下「mono 尺寸段 + 头里 stereo」的坏几何;
// v6 openForOutput 必须拒绝(静默断连),绝不越界读。
int scenarioOutputCheck()
{
    const u32 frames = ringFramesFromEnv();
    SegmentView v;
    const std::size_t monoBytes = sizeof(AudioRingHeader) + static_cast<std::size_t>(frames) * sizeof(float);
    if (SegmentBackendWin32::map(segmentAudioName(1, 1), monoBytes, v) != InitResult::kOk)
    {
        return fail("map mono segment");
    }
    auto* h = static_cast<AudioRingHeader*>(v.base);
    h->sample_rate = 48000;
    h->ring_frames = frames;
    h->channels = 2; // 篡改:mono 尺寸段上宣称 stereo(旧版 Input 坏几何残留)
    h->write_head_samples.store(0, std::memory_order_relaxed);
    h->epoch.store(0, std::memory_order_relaxed);
    h->abi.store(kScvbAbi, std::memory_order_relaxed);
    h->magic.store(kScvbMagic, std::memory_order_release);

    AudioRing ring(1, 1);
    const auto r = ring.openForOutput();
    if (r == InitResult::kOk || ring.isOpen())
    {
        return fail("tampered geometry was accepted");
    }
    std::printf("outputcheck: PASS (oversized geometry rejected)\n");
    return 0;
}

// 几何 + 块长 churn 长跑。
int scenarioChurn()
{
    const u32 frames = ringFramesFromEnv();
    AudioRing ring(1, 1);
    std::vector<float> buf(static_cast<std::size_t>(1024) * 2);
    fillSine(buf.data(), 1024, 2, 220.0f);
    i64 t0 = 0;
    for (int b = 0; b < 2000; ++b)
    {
        if (b % 300 == 0)
        {
            const u32 chans = ((b / 300) % 2 == 0) ? 1 : 2;
            if (ring.createForInput(48000, frames, chans) != InitResult::kOk)
            {
                return fail("churn create");
            }
            if (ring.channels() != chans)
            {
                return fail("churn geometry mismatch");
            }
        }
        const int n = 128 + (b % 3) * 256;
        ring.writeBlock(t0, buf.data(), n);
        t0 += n;
    }
    std::printf("churn: PASS\n");
    return 0;
}

// v8 认领塌缩回归:15 个实例(同 pid)按 InputProcessor 自动认领循环选槽,
// 必须落在 15 个不同 channel;v2..v7 的 claimInput 同 pid 刷新会全部挤在 ch1。
int scenarioMulticlaim()
{
    Registry reg(8); // 组 8:避开 DAW 实测组 g1/g2,测试进程退出后段随进程销毁
    if (reg.open() != Registry::ClaimResult::kClaimed)
    {
        return fail("registry open");
    }
    const u32 pid = static_cast<u32>(::GetCurrentProcessId());
    const u64 now = steadyNowMs();
    u32 got[kMaxChannels + 1] = {};
    u32 claimed = 0;
    for (int i = 0; i < static_cast<int>(kMaxChannels); ++i)
    {
        u32 ch = 0;
        for (u32 c = 1; c <= kMaxChannels; ++c)
        {
            if (reg.claimInputAuto(c, pid, 48000, 512, now) == Registry::ClaimResult::kClaimed)
            {
                ch = c;
                break;
            }
        }
        if (ch == 0)
        {
            return fail("instance could not claim any channel");
        }
        if (got[ch] != 0)
        {
            std::printf("FAIL: duplicate channel ch%u (instance %d)\n", ch, i);
            return 1;
        }
        got[ch] = 1;
        ++claimed;
    }
    u32 mask = 0;
    for (u32 c = 1; c <= kMaxChannels; ++c)
    {
        if (got[c] != 0)
        {
            mask |= (1u << (c - 1));
        }
    }
    std::printf("multiclaim: PASS (%u instances on %u distinct channels, mask=%u)\n", claimed, claimed, mask);
    return mask == 0x7FFFu ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    // 抑制 WER 交互弹窗(CI/无人值守下崩溃直接失败,WER LocalDumps 转储不受影响)。
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // 崩溃瞬间输出必须落盘(取证)
    // 快速触发:小环帧数让越界位置几步之内到达(环帧数必须 2^k)。
    _putenv_s("SCVB_RING_FRAMES", "8192");
    const std::string scenario = (argc > 1) ? argv[1] : "mono2stereo";
    std::printf("scvb_stress scenario=%s ringFrames=%u\n", scenario.c_str(), ringFramesFromEnv());
    if (scenario == "mono2stereo")
    {
        return scenarioMono2Stereo();
    }
    if (scenario == "flip")
    {
        return scenarioFlip();
    }
    if (scenario == "outputcheck")
    {
        return scenarioOutputCheck();
    }
    if (scenario == "churn")
    {
        return scenarioChurn();
    }
    if (scenario == "multiclaim")
    {
        return scenarioMulticlaim();
    }
    std::printf("unknown scenario: %s\n", scenario.c_str());
    return 2;
}