// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_bench —— SCVB 性能基准 + 内置最小离线 host(10-validation §5.4,J19 授权的第三个验证工具)。
//   由 T04(S3 spike)交付、T05(S4)复用。链接 scvb_core(纯 C++17,零 JUCE),可离线在任何机器运行。
//   两种模式:
//     --dsp    离线基准:Input(双 biquad + 拷贝)/ Output(15 轨 × 曲线求值 + pan + 求和)的
//              ns/block mean/p50/p95/p99/max + CPU%(PERF-1..4 判定依据;固定种子可跨 commit 对比)。
//     --render 最小离线 host:合成 15 轨(13 mono + 2 stereo)固定种子音频 → K-weighting → 每轨
//              pan/vol(PanMath + CurveEvaluator)→ 求和 → 写 wav。只做数学层端到端(PANLAW/L3 用途)。
//   ⚠ 本工具不能替代 DAW 验证:没有宿主参数系统、没有真实线程时序。跑了它 ≠ 跑了 DAW 矩阵。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "analysis/FeatureExtractor.h"
#include "dsp/KWeighting.h"
#include "dsp/PanMath.h"
#include "engine/CurveEvaluator.h"

namespace
{

// ---------------------------------------------------------------------------
// 命令行选项
// ---------------------------------------------------------------------------
struct Options
{
    bool help = false;
    bool dsp = false;
    bool render = false;
    int fs = 48000;
    int block = 512;
    int tracks = 15;
    int stereoTracks = 2;
    long long blocks = 100000;
    int seconds = 30;
    std::string jsonOut;
    std::string renderOut;
};

// ---------------------------------------------------------------------------
// 固定种子 PRNG(std::mt19937,结果跨 commit 可复现)
// ---------------------------------------------------------------------------
inline float frand(std::mt19937& gen)
{
    return std::uniform_real_distribution<float>(-1.0f, 1.0f)(gen);
}

// ---------------------------------------------------------------------------
// 极简 JSON(零依赖)
// ---------------------------------------------------------------------------
std::string jsonNum(const std::string& key, double v)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "\"%s\":%.3f", key.c_str(), v);
    return buf;
}

// ---------------------------------------------------------------------------
// 统计:mean / p50 / p95 / p99 / max(输入为 per-block 纳秒)
// ---------------------------------------------------------------------------
struct Stats
{
    double mean = 0.0;
    long long p50 = 0;
    long long p95 = 0;
    long long p99 = 0;
    long long max = 0;
};

Stats summarize(std::vector<long long>& ns)
{
    Stats s;
    if (ns.empty())
    {
        return s;
    }
    long long sum = 0;
    for (const long long v : ns)
    {
        sum += v;
        s.max = std::max(s.max, v);
    }
    s.mean = static_cast<double>(sum) / static_cast<double>(ns.size());
    std::sort(ns.begin(), ns.end());
    const auto q = [&](double p) {
        const std::size_t idx =
            static_cast<std::size_t>(std::llround((p / 100.0) * static_cast<double>(ns.size() - 1)));
        return ns[idx];
    };
    s.p50 = q(50.0);
    s.p95 = q(95.0);
    s.p99 = q(99.0);
    return s;
}

// ---------------------------------------------------------------------------
// WAV(float32)写入,供 --render 输出
// ---------------------------------------------------------------------------
void writeWavFloat(const std::string& path, int sampleRate, int channels, const std::vector<float>& samples,
                   std::string& err)
{
    auto put4 = [](std::ofstream& f, std::uint32_t v) {
        const char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                           static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff)};
        f.write(b, 4);
    };
    auto put2 = [](std::ofstream& f, std::uint16_t v) {
        const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
        f.write(b, 2);
    };

    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        err = "无法打开输出文件: " + path;
        return;
    }
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate * channels * 4);
    const std::uint32_t blockAlign = static_cast<std::uint32_t>(channels * 4);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * 4);

    f.write("RIFF", 4);
    put4(f, 36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put4(f, 16);
    put2(f, 3); // IEEE float
    put2(f, static_cast<std::uint16_t>(channels));
    put4(f, static_cast<std::uint32_t>(sampleRate));
    put4(f, byteRate);
    put2(f, static_cast<std::uint16_t>(blockAlign));
    put2(f, 32);
    f.write("data", 4);
    put4(f, dataBytes);
    for (const float s : samples)
    {
        std::uint32_t u = 0;
        std::memcpy(&u, &s, 4);
        put4(f, u);
    }
}

// ---------------------------------------------------------------------------
// 构造 15 轨固定种子曲线(每轨 5 段阶梯,段间 80ms ramp;S2 SpikeCurve 同构)
// ---------------------------------------------------------------------------
std::vector<scvb::CurveEvaluator> makeCurves(int tracks, double seconds)
{
    std::vector<scvb::CurveEvaluator> evals(static_cast<std::size_t>(tracks));
    const double step = seconds / 5.0;
    const double panLadder[5] = {-80.0, -40.0, 0.0, 40.0, 80.0};
    const double volLadder[5] = {-6.0, -3.0, 0.0, 3.0, 6.0};
    for (int t = 0; t < tracks; ++t)
    {
        std::vector<scvb::CurveSegment> segs;
        for (int i = 0; i < 5; ++i)
        {
            scvb::CurveSegment seg;
            seg.startSec = i * step;
            seg.endSec = (i + 1) * step;
            seg.pan = panLadder[(i + t) % 5];
            seg.volDb = volLadder[(i + t * 2) % 5];
            segs.push_back(seg);
        }
        evals[static_cast<std::size_t>(t)].build(segs, scvb::TransitionConfig{});
    }
    return evals;
}

// ---------------------------------------------------------------------------
// --dsp 基准:Input(双 biquad + 拷贝)与 Output(15 轨曲线求值 + pan + 求和)
// ---------------------------------------------------------------------------
struct DspResult
{
    std::vector<long long> inputNs;
    std::vector<long long> outputNs;
    double checksum = 0.0;
};

DspResult runDsp(const Options& o)
{
    DspResult r;
    r.inputNs.reserve(static_cast<std::size_t>(o.blocks));
    r.outputNs.reserve(static_cast<std::size_t>(o.blocks));

    // 固定种子输入数据
    std::mt19937 gen(12345u);
    const std::size_t n = static_cast<std::size_t>(o.block);
    std::vector<float> src(n);
    std::vector<float> dst(n);
    std::vector<float> accL(n);
    std::vector<float> accR(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        src[i] = frand(gen);
    }

    // 15 轨曲线(每条轨一个 CurveEvaluator)
    auto curves = makeCurves(o.tracks, 300.0); // 5 分钟(曲线求值成本与时长无关)

    // Input:KWeighting(两 biquad)per-sample × channels;stereo 轨跑两组。
    std::vector<scvb::dsp::KWeighting> kwInput(2);
    kwInput[0].prepare(static_cast<double>(o.fs));
    kwInput[1].prepare(static_cast<double>(o.fs));

    // Output:每轨固定 pan/vol 求值 + PanMath 增益 + 求和
    const double tSecPerBlock = static_cast<double>(o.block) / static_cast<double>(o.fs);
    double tSec = 0.0;

    const auto nowNs = []() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    for (long long b = 0; b < o.blocks; ++b)
    {
        // ---- Input 基准:2 组 biquad(KWeighting)+ 拷贝,模拟 mono/stereo 两路 ----
        {
            const auto t0 = nowNs();
            const bool stereo = ((b % o.tracks) >= o.tracks - o.stereoTracks);
            if (stereo)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    dst[i] = kwInput[0].process(src[i]) + kwInput[1].process(src[i]);
                }
            }
            else
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    dst[i] = kwInput[0].process(src[i]);
                }
            }
            const auto t1 = nowNs();
            r.inputNs.push_back(t1 - t0);
            r.checksum += dst[0];
        }

        // ---- Output 基准:15 轨 × (曲线求值 + pan + 求和) ----
        {
            const auto t0 = nowNs();
            std::fill(accL.begin(), accL.end(), 0.0f);
            std::fill(accR.begin(), accR.end(), 0.0f);
            for (int t = 0; t < o.tracks; ++t)
            {
                const double pan = curves[static_cast<std::size_t>(t)].panAt(tSec);
                const double volDb = curves[static_cast<std::size_t>(t)].volAt(tSec);
                const float gain = std::pow(10.0f, static_cast<float>(volDb / 20.0));
                const bool stereo = (t >= o.tracks - o.stereoTracks);
                if (stereo)
                {
                    const scvb::DualPanGains g = scvb::dualPanGains(static_cast<float>(pan), 100.0f);
                    const float gLL = g.gL_L * gain;
                    const float gRL = g.gR_L * gain;
                    const float gLR = g.gL_R * gain;
                    const float gRR = g.gR_R * gain;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float v = src[i];
                        accL[i] += v * gLL + v * gLR;
                        accR[i] += v * gRL + v * gRR;
                    }
                }
                else
                {
                    float gL = 0.0f;
                    float gR = 0.0f;
                    scvb::panGains(static_cast<float>(pan), gL, gR);
                    gL *= gain;
                    gR *= gain;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float v = src[i];
                        accL[i] += v * gL;
                        accR[i] += v * gR;
                    }
                }
            }
            const auto t1 = nowNs();
            r.outputNs.push_back(t1 - t0);
            r.checksum += accL[0] + accR[0];
        }

        tSec += tSecPerBlock;
    }
    return r;
}

void reportBench(const std::string& label, std::vector<long long>& ns, double deadlineNs, const Options& o,
                 std::ostream& os, std::string& json)
{
    Stats s = summarize(ns);
    const double cpuPct = s.mean / deadlineNs * 100.0;
    os << "[" << label << "]\n"
       << "  ns/block  mean=" << s.mean << "  p50=" << s.p50 << "  p95=" << s.p95 << "  p99=" << s.p99
       << "  max=" << s.max << "\n"
       << "  CPU%      mean=" << cpuPct << "  (block 期限 " << deadlineNs / 1e6 << " ms)\n\n";
    if (!o.jsonOut.empty())
    {
        json += "\"" + label + "\":{";
        json += jsonNum("mean_ns", s.mean) + ",";
        json += jsonNum("p50_ns", static_cast<double>(s.p50)) + ",";
        json += jsonNum("p95_ns", static_cast<double>(s.p95)) + ",";
        json += jsonNum("p99_ns", static_cast<double>(s.p99)) + ",";
        json += jsonNum("max_ns", static_cast<double>(s.max)) + ",";
        json += jsonNum("cpu_pct", cpuPct) + "}";
    }
}

// ---------------------------------------------------------------------------
// --render:最小离线 host。合成 15 轨固定种子音频 → K-weighting → pan/vol → 求和 → wav。
// ---------------------------------------------------------------------------
int runRender(const Options& o)
{
    std::string err;
    const int totalSamples = o.fs * o.seconds;
    const int channels = 2; // 输出 stereo 总线
    std::vector<float> out(static_cast<std::size_t>(totalSamples) * channels, 0.0f);

    std::mt19937 gen(999u);
    auto curves = makeCurves(o.tracks, static_cast<double>(o.seconds));
    std::vector<scvb::dsp::KWeighting> kw(static_cast<std::size_t>(o.tracks));
    for (auto& k : kw)
    {
        k.prepare(static_cast<double>(o.fs));
    }

    for (int t = 0; t < o.tracks; ++t)
    {
        const bool stereo = (t >= o.tracks - o.stereoTracks);
        for (int s = 0; s < totalSamples; ++s)
        {
            const double tSec = static_cast<double>(s) / static_cast<double>(o.fs);
            const double pan = curves[static_cast<std::size_t>(t)].panAt(tSec);
            const double volDb = curves[static_cast<std::size_t>(t)].volAt(tSec);
            const float gain = static_cast<float>(std::pow(10.0, volDb / 20.0));
            const float v = frand(gen) * 0.1f;
            const float kwv = kw[static_cast<std::size_t>(t)].process(v);

            if (stereo)
            {
                const scvb::DualPanGains g = scvb::dualPanGains(static_cast<float>(pan), 100.0f);
                const float vr = frand(gen) * 0.1f;
                const float kwr = kw[static_cast<std::size_t>(t)].process(vr);
                out[static_cast<std::size_t>(s) * 2] += kwv * g.gL_L * gain + kwr * g.gL_R * gain;
                out[static_cast<std::size_t>(s) * 2 + 1] += kwv * g.gR_L * gain + kwr * g.gR_R * gain;
            }
            else
            {
                float gL = 0.0f;
                float gR = 0.0f;
                scvb::panGains(static_cast<float>(pan), gL, gR);
                out[static_cast<std::size_t>(s) * 2] += kwv * gL * gain;
                out[static_cast<std::size_t>(s) * 2 + 1] += kwv * gR * gain;
            }
        }
    }
    writeWavFloat(o.renderOut, o.fs, channels, out, err);
    if (!err.empty())
    {
        std::cerr << err << '\n';
        return 1;
    }
    std::cout << "scvb_bench --render: 已写 " << o.renderOut << " (" << o.tracks << " 轨, " << o.seconds << "s, "
              << o.fs << " Hz, stereo)\n";
    return 0;
}

void printUsage()
{
    std::cout << "scvb_bench —— SCVB 性能基准 + 最小离线 host(10-validation §5.4)\n"
              << "用法:\n"
              << "  scvb_bench --dsp [--fs 48000] [--block 512] [--tracks 15] [--stereo-tracks 2] [--blocks 100000] "
                 "[--json out.json]\n"
              << "    离线基准:Input/Output ns/block mean/p50/p95/p99/max + CPU%(固定种子,可跨 commit 对比)。\n"
              << "  scvb_bench --render --out <wav> [--fs 48000] [--seconds 30] [--tracks 15] [--stereo-tracks 2]\n"
              << "    最小离线 host:合成 15 轨 → K-weighting → pan/vol → 求和 → 写 wav。\n"
              << "⚠ 本工具不能替代 DAW 验证(无宿主参数系统、无真实线程时序)。跑了它 ≠ 跑了 DAW 矩阵。\n";
}

bool parseArgs(int argc, char** argv, Options& o, std::string& err)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto needValue = [&](const std::string& key) -> const char* {
            if (i + 1 >= argc)
            {
                err = key + " 需要一个参数";
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h")
        {
            o.help = true;
            return true;
        }
        if (a == "--dsp")
        {
            o.dsp = true;
        }
        else if (a == "--render")
        {
            o.render = true;
        }
        else if (a == "--fs" || a == "--block" || a == "--tracks" || a == "--stereo-tracks" || a == "--blocks" ||
                 a == "--seconds")
        {
            const char* v = needValue(a);
            if (v == nullptr)
            {
                return false;
            }
            const long n = std::strtol(v, nullptr, 10);
            if (n <= 0)
            {
                err = a + " 需要正整数";
                return false;
            }
            if (a == "--fs")
                o.fs = static_cast<int>(n);
            else if (a == "--block")
                o.block = static_cast<int>(n);
            else if (a == "--tracks")
                o.tracks = static_cast<int>(n);
            else if (a == "--stereo-tracks")
                o.stereoTracks = static_cast<int>(n);
            else if (a == "--blocks")
                o.blocks = n;
            else
                o.seconds = static_cast<int>(n);
        }
        else if (a == "--json" || a == "--out")
        {
            const char* v = needValue(a);
            if (v == nullptr)
            {
                return false;
            }
            if (a == "--json")
                o.jsonOut = v;
            else
                o.renderOut = v;
        }
        else
        {
            err = "未知选项: " + a;
            return false;
        }
    }
    if (o.help)
    {
        return true;
    }
    if (!o.dsp && !o.render)
    {
        err = "需要 --dsp 或 --render 之一";
        return false;
    }
    if (o.render && o.renderOut.empty())
    {
        err = "--render 需要 --out <wav>";
        return false;
    }
    if (o.stereoTracks > o.tracks)
    {
        err = "--stereo-tracks 不得大于 --tracks";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, err))
    {
        std::cerr << "参数错误: " << err << "\n\n";
        printUsage();
        return 1;
    }
    if (o.help)
    {
        printUsage();
        return 0;
    }

    if (o.render)
    {
        return runRender(o);
    }

    // --dsp
    const double deadlineNs = static_cast<double>(o.block) / static_cast<double>(o.fs) * 1e9;
    DspResult r = runDsp(o);

    std::string json = "{";
    reportBench("input", r.inputNs, deadlineNs, o, std::cout, json);
    json += ",";
    reportBench("output", r.outputNs, deadlineNs, o, std::cout, json);
    json += ",";
    json += jsonNum("checksum", r.checksum);
    json += ",\"block\":" + std::to_string(o.block) + ",\"fs\":" + std::to_string(o.fs) +
            ",\"tracks\":" + std::to_string(o.tracks) + ",\"blocks\":" + std::to_string(o.blocks) + "}";

    if (!o.jsonOut.empty())
    {
        std::ofstream f(o.jsonOut);
        if (!f)
        {
            std::cerr << "无法打开 JSON 输出: " << o.jsonOut << '\n';
            return 1;
        }
        f << json << '\n';
        std::cout << "已写 " << o.jsonOut << '\n';
    }
    return 0;
}
