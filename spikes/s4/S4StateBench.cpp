// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike 验收跑分:满配 state 的 save/load 往返一致性、耗时、gzip 压缩率、abi+1 拒载、sidecar 删除。
//   计时方法学复用 T04 tests/tools/scvb_bench(steady_clock ns + mean/p50/p95/p99/max 统计);
//   gzip 计时必须在链接 juce_core 的本目标内完成(scvb_bench 为「零 JUCE」定位,不链 GZIPCompressorOutputStream)。

#include "StateCodec.h"
#include "StateFixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// 统计(逐字复用 T04 scvb_bench 的 summarize)。
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
        return s;
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

struct Options
{
    bool quick = false;
    int seconds = 300;
    int tracks = 15;
    int iterations = 10;
    std::string sidecarDir;
    std::string jsonOut;
};

// ---------------------------------------------------------------------------
// 结构相等(用于断言「曲线完好」;f32 经 memcpy 往返逐位一致,== 即逐位比较)。
// ---------------------------------------------------------------------------
bool versionsEqual(const std::vector<scvb::s4::VersionCurves>& a, const std::vector<scvb::s4::VersionCurves>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t v = 0; v < a.size(); ++v)
    {
        const auto& av = a[v];
        const auto& bv = b[v];
        if (av.name != bv.name || av.tracks.size() != bv.tracks.size())
            return false;
        for (std::size_t t = 0; t < av.tracks.size(); ++t)
        {
            const auto& at = av.tracks[t];
            const auto& bt = bv.tracks[t];
            if (at.segments.size() != bt.segments.size() || at.excludedRanges.size() != bt.excludedRanges.size())
                return false;
            for (std::size_t i = 0; i < at.segments.size(); ++i)
            {
                const auto& as = at.segments[i];
                const auto& bs = bt.segments[i];
                if (as.t0 != bs.t0 || as.t1 != bs.t1 || as.pan != bs.pan || as.volDb != bs.volDb ||
                    as.flags != bs.flags)
                    return false;
            }
            for (std::size_t i = 0; i < at.excludedRanges.size(); ++i)
            {
                if (at.excludedRanges[i].t0 != bt.excludedRanges[i].t0 ||
                    at.excludedRanges[i].t1 != bt.excludedRanges[i].t1)
                    return false;
            }
        }
    }
    return true;
}

void printUsage()
{
    std::cout << "scvb_s4_state —— S4 状态体量 spike 验收跑分(ADR-014)\n"
              << "用法:\n"
              << "  scvb_s4_state [--quick] [--seconds N] [--tracks N] [--iterations N] [--sidecar-dir PATH] "
                 "[--json out.json]\n"
              << "  --quick     小夹具(2 轨 × 5s)快速回环,不断言耗时/体量阈值\n"
              << "  默认         满配(15 轨 × 300s)全量跑分并断言验收阈值\n";
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
            printUsage();
            std::exit(0);
        }
        else if (a == "--quick")
        {
            o.quick = true;
        }
        else if (a == "--seconds" || a == "--tracks" || a == "--iterations")
        {
            const char* v = needValue(a);
            if (v == nullptr)
                return false;
            const long n = std::strtol(v, nullptr, 10);
            if (n <= 0)
            {
                err = a + " 需要正整数";
                return false;
            }
            if (a == "--seconds")
                o.seconds = static_cast<int>(n);
            else if (a == "--tracks")
                o.tracks = static_cast<int>(n);
            else
                o.iterations = static_cast<int>(n);
        }
        else if (a == "--sidecar-dir" || a == "--json")
        {
            const char* v = needValue(a);
            if (v == nullptr)
                return false;
            if (a == "--sidecar-dir")
                o.sidecarDir = v;
            else
                o.jsonOut = v;
        }
        else
        {
            err = "未知选项: " + a;
            return false;
        }
    }
    return true;
}

std::string jsonNum(const std::string& key, double v)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), R"("%s":%.3f)", key.c_str(), v);
    return buf;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace scvb::s4;

    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, err))
    {
        std::cerr << "参数错误: " << err << "\n\n";
        printUsage();
        return 1;
    }

    if (o.quick)
    {
        o.seconds = 5;
        o.tracks = 2;
        o.iterations = 3;
    }

    bool allPass = true;
    const auto check = [&](bool ok, const std::string& label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok)
            allPass = false;
    };

    const double kMaxSaveMs = 200.0;
    const double kMaxLoadMs = 1000.0;
    const std::uint64_t kMaxGzBytes = static_cast<std::uint64_t>(2.25 * 1024.0 * 1024.0);

    // ---- 夹具 ----
    FixtureOptions fo;
    fo.tracks = static_cast<std::size_t>(o.tracks);
    fo.seconds = o.seconds;
    const FullState fixture = buildFullState(fo);
    const std::uint64_t totalHops = static_cast<std::uint64_t>(o.seconds) * (1000u / fixture.hopMs);

    const juce::File sidecarBase =
        o.sidecarDir.empty()
            ? juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("scvb-s4-spike-bench")
            : juce::File(o.sidecarDir);

    std::cout << "=== S4 满配 state 夹具 ===\n"
              << "  轨数 " << o.tracks << " × 时长 " << o.seconds << "s × hop 10ms = " << totalHops << " hop/轨\n"
              << "  raw 特征 " << (static_cast<std::uint64_t>(o.tracks) * totalHops * 5u) << " B(5 B/hop)\n";

    // ---- 验收①:save→load→save 逐字节一致 ----
    std::cout << "\n=== 验收① 往返逐字节一致 ===\n";
    const EncodeResult enc1 = encode(fixture, EncodeOptions{});
    const LoadResult load1 = decode(enc1.bytes.data(), enc1.bytes.size(), sidecarBase);
    check(load1.status == LoadStatus::Ok, "save→load 状态 = Ok");
    check(!load1.featuresMissing, "特征内嵌加载,无 sidecar 缺失");
    check(load1.state.features.size() == fixture.features.size(), "特征轨数一致");
    const EncodeResult enc2 = encode(load1.state, EncodeOptions{});
    check(enc1.bytes == enc2.bytes,
          "save→load→save 两次 chunk 逐字节一致(" + std::to_string(enc1.bytes.size()) + " B)");

    // ---- 验收②/③:耗时与压缩率 ----
    std::cout << "\n=== 验收②③ 耗时 / gzip 压缩率 ===\n";
    const double rawFeat = static_cast<double>(enc1.rawFeatBytes);
    const double gzFeat = static_cast<double>(enc1.gzFeatBytes);
    const double ratio = rawFeat / gzFeat;
    std::cout << "  FEAT 压缩前 " << enc1.rawFeatBytes << " B,压缩后 " << enc1.gzFeatBytes << " B,压缩率 " << ratio
              << "×\n"
              << "  state chunk 总 " << enc1.totalBytes << " B\n";

    std::vector<long long> saveNs;
    std::vector<long long> loadNs;
    saveNs.reserve(static_cast<std::size_t>(o.iterations));
    loadNs.reserve(static_cast<std::size_t>(o.iterations));
    {
        // 预热一次(去冷缓存)。
        (void)encode(fixture, EncodeOptions{});
        const std::vector<std::uint8_t> warm = enc1.bytes;
        (void)decode(warm.data(), warm.size(), sidecarBase);
    }
    const auto nowNs = []() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    for (int i = 0; i < o.iterations; ++i)
    {
        const auto t0 = nowNs();
        const EncodeResult e = encode(fixture, EncodeOptions{});
        const auto t1 = nowNs();
        const LoadResult l = decode(e.bytes.data(), e.bytes.size(), sidecarBase);
        const auto t2 = nowNs();
        (void)l;
        saveNs.push_back(t1 - t0);
        loadNs.push_back(t2 - t1);
    }
    Stats saveStats = summarize(saveNs);
    Stats loadStats = summarize(loadNs);
    std::cout << "  [save(含 gzip)] mean " << saveStats.mean / 1e6 << " ms  p50 " << saveStats.p50 / 1e6 << " ms  p95 "
              << saveStats.p95 / 1e6 << " ms  p99 " << saveStats.p99 / 1e6 << " ms  max " << saveStats.max / 1e6
              << " ms\n";
    std::cout << "  [load]          mean " << loadStats.mean / 1e6 << " ms  p50 " << loadStats.p50 / 1e6 << " ms  p95 "
              << loadStats.p95 / 1e6 << " ms  p99 " << loadStats.p99 / 1e6 << " ms  max " << loadStats.max / 1e6
              << " ms\n";

    if (!o.quick)
    {
        check(loadStats.max / 1e6 < kMaxLoadMs, "加载 max < 1s");
        check(saveStats.max / 1e6 < kMaxSaveMs, "保存(含 gzip)max < 200ms");
        check(enc1.gzFeatBytes <= kMaxGzBytes,
              "15 轨 × 5 分钟特征 gzip ≤ 2.25 MB(" + std::to_string(enc1.gzFeatBytes) + " B)");
    }

    // ---- 验收④:abi+1 拒载 + 原样回写 ----
    std::cout << "\n=== 验收④ abi+1 拒载并原样回写 ===\n";
    std::vector<std::uint8_t> abi2 = enc1.bytes;
    abi2[4] = 2; // abi 字段(offset 4,little-endian u32)= 2
    abi2[5] = 0;
    abi2[6] = 0;
    abi2[7] = 0;
    ProcessorState ps;
    ps.load(abi2.data(), abi2.size(), sidecarBase);
    check(ps.status() == LoadStatus::RejectedNewer, "abi=2 blob 被拒载(RejectedNewer)");
    check(ps.lastResult().preservedOriginal == abi2, "保留原 blob 逐字节一致");
    const std::vector<std::uint8_t> reSaved = ps.save(EncodeOptions{});
    check(reSaved == abi2, "拒载后 save 原样回写,不毁高版本数据");

    // ---- 验收⑤:sidecar 删除后工程可开、曲线完好 ----
    std::cout << "\n=== 验收⑤ sidecar 删除后工程可开、曲线完好 ===\n";
    FullState sideState = fixture;
    sideState.sessionGuid = "11111111-2222-3333-4444-555555555555";
    EncodeOptions sideOpts;
    sideOpts.forceSidecar = true;
    sideOpts.sidecarBaseDir = sidecarBase;
    const EncodeResult sideEnc = encode(sideState, sideOpts);
    check(!sideEnc.embedded, "强制 sidecar 生效(embedded=false)");
    const juce::File sideDir = sidecarDirectoryFor(sidecarBase, sideState.sessionGuid);
    check(sideDir.getChildFile("features.bin.gz").existsAsFile(), "sidecar 目录/文件已写");

    const LoadResult loadSide = decode(sideEnc.bytes.data(), sideEnc.bytes.size(), sidecarBase);
    check(loadSide.status == LoadStatus::Ok && !loadSide.featuresMissing, "sidecar 存在:加载特征成功");
    check(loadSide.state.features.size() == fixture.features.size(), "sidecar 读回特征轨数一致");

    sideDir.deleteRecursively();
    const LoadResult loadSideMissing = decode(sideEnc.bytes.data(), sideEnc.bytes.size(), sidecarBase);
    check(loadSideMissing.status == LoadStatus::Ok, "sidecar 删除后:工程可开(Ok)");
    check(loadSideMissing.featuresMissing, "sidecar 删除后:标记特征缺失");
    check(loadSideMissing.state.features.empty(), "sidecar 删除后:特征清空");
    check(versionsEqual(loadSideMissing.state.versions, fixture.versions), "sidecar 删除后:曲线完好");

    // 收尾清理。
    sidecarBase.deleteRecursively();

    std::cout << "\n=== 汇总 ===\n" << (allPass ? "全部 PASS" : "存在 FAIL") << '\n';

    if (!o.jsonOut.empty())
    {
        std::string json = "{";
        json += jsonNum("raw_feat_bytes", rawFeat) + ",";
        json += jsonNum("gz_feat_bytes", gzFeat) + ",";
        json += jsonNum("ratio", ratio) + ",";
        json += jsonNum("total_bytes", static_cast<double>(enc1.totalBytes)) + ",";
        json += jsonNum("save_mean_ms", saveStats.mean / 1e6) + ",";
        json += jsonNum("save_p50_ms", static_cast<double>(saveStats.p50) / 1e6) + ",";
        json += jsonNum("save_max_ms", static_cast<double>(saveStats.max) / 1e6) + ",";
        json += jsonNum("load_mean_ms", loadStats.mean / 1e6) + ",";
        json += jsonNum("load_p50_ms", static_cast<double>(loadStats.p50) / 1e6) + ",";
        json += jsonNum("load_max_ms", static_cast<double>(loadStats.max) / 1e6) + ",";
        json += "\"tracks\":" + std::to_string(o.tracks) + ",\"seconds\":" + std::to_string(o.seconds) + "}";
        juce::File(o.jsonOut).replaceWithText(json);
        std::cout << "已写 " << o.jsonOut << '\n';
    }

    return allPass ? 0 : 1;
}
