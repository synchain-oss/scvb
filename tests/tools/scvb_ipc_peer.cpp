// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_ipc_peer —— T07b L1 双进程 IPC 契约测试的对端进程(10-validation §2.1/§2.3)。
// 由 test_ipc_contract.cpp 经 CreateProcessW 拉起;argv 决定角色,共享 tests/ipc/ipc_contract_harness.h
// 的环读写语义。所有角色只做一件事并退出(holder 除外:持 slot + 心跳直到被 TerminateProcess)。
//
// 角色:
//   writer       写音频环斜坡(--blocks/--blocksize/--channels/--seek-at/--seek-to/--negative-start)
//   reader       读音频环 → CSV(--out;--decode-channels 触发 channels 不匹配拒读;--wrong-units 错位解码)
//   claimer      抢一个 Input channel(IPC-7;--kind=output 改抢 Output;退出码 0=成功/2=冲突/3=不可用/4=abi)
//   holder       claim Input(--kind=input)或 Output(--kind=output)+ 250ms 心跳,持住直到被杀
//   ctrl-writer  Input [M] 向命令环 enqueue(IPC-13/13b)
//   ctrl-reader  Output [M] 从命令环 dequeue(IPC-13/13b)
//   globalinfo-writer / globalinfo-reader  ctrl 全局小节(IPC-16;reader 握手超时退出码 5)
//   feat-writer  Input 侧写特征环 run 协议(IPC-14)
//   feat-reader  Output 侧增量拉取特征环 → CSV(IPC-14)
//   viz-writer   [T44] Output [M] 建 viz 段并发布一帧(--linger-ms 持段供读方 attach)
//   viz-reader   [T44] Monitor 侧只读 attach viz 段 + 一致性读 → CSV(--out)
//   viz-publisher[T44] 走**真 VizPublisher**(CRVS 分段 + CurveEvaluator 求值)发布,而非手搓快照

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ipc/CtrlPlane.h"
#include "ipc/FeatRing.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"
#include "output/VizPublisher.h"
#include "ipc_contract_harness.h"

using namespace scvb;
using scvb::ipctest::AudioGeometry;
using scvb::ipctest::fillBlockSamples;
using scvb::ipctest::initAudioHeader;
using scvb::ipctest::ringReadBlock;
using scvb::ipctest::RingReaderState;
using scvb::ipctest::ringWriteBlock;
using scvb::ipctest::RingWriterState;
using scvb::ipctest::sampleAt;
using scvb::ipctest::TimelineModel;

namespace
{

struct Args
{
    std::string role;
    // 通用
    int group = 1;
    int ch = 1;
    // audio
    int channels = 1;
    int sr = 48000;
    int ringFrames = static_cast<int>(kDefaultRingFrames);
    long long blocks = 0;
    int blocksize = 64;
    long long startT0 = 0;
    long long seekAt = -1;
    long long seekTo = 0;
    bool negativeStart = false;
    int decodeChannels = -1; // -1 = 用段头 channels
    bool wrongUnits = false;
    std::string out;
    int dieAt = -1;
    // registry
    int kindInput = 1; // 1=input 0=output
    long long holdMs = -1;
    // ctrl
    long long enqueueCount = 0;
    unsigned long long valueBase = 0;
    int opValue = 0;
    // globalinfo
    unsigned long long epochBase = 0;
    // feat
    long long featHops = 0;
    long long featStartSample = 0;
    int featChannels = 1;
    // 生产方写完后的保活时间(ms):保持段句柄打开,让消费方有时间 attach(命名段在最后句柄关闭时销毁)。
    long long lingerMs = 0;
    // 读方读多少帧(覆盖 blocks*blocksize 的默认值;reader 与 writer 块长不同时由父进程显式传入)。
    long long totalFrames = 0;
    // 读前显式等写方静止(write_head 到达该值,超时 10s)。用于「错误口径」类用例:让覆盖判定在
    // 确定性时机触发,消除「读方追平写方 → 无 gap → 周期性 ramp 使错位内容哈希恰等于期望」的 flake。
    long long waitWriteHead = 0;
};

bool hasArg(int argc, char** argv, const char* key)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key, std::strlen(key)) == 0)
        {
            return true;
        }
    }
    return false;
}

std::string argValue(int argc, char** argv, const char* key, const std::string& def = "")
{
    const std::size_t kl = std::strlen(key);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key, kl) == 0)
        {
            const char* p = argv[i] + kl;
            if (*p == '=')
            {
                return std::string(p + 1);
            }
            if (*p == '\0' && i + 1 < argc)
            {
                return std::string(argv[i + 1]);
            }
        }
    }
    return def;
}

long long ll(const std::string& s, long long def = 0)
{
    if (s.empty())
    {
        return def;
    }
    return std::strtoll(s.c_str(), nullptr, 10);
}

Args parse(int argc, char** argv)
{
    Args a;
    a.role = argValue(argc, argv, "--role");
    a.group = static_cast<int>(ll(argValue(argc, argv, "--group"), 1));
    a.ch = static_cast<int>(ll(argValue(argc, argv, "--ch"), 1));
    a.channels = static_cast<int>(ll(argValue(argc, argv, "--channels"), 1));
    a.sr = static_cast<int>(ll(argValue(argc, argv, "--sr"), 48000));
    a.ringFrames = static_cast<int>(ll(argValue(argc, argv, "--ring-frames"), kDefaultRingFrames));
    a.blocks = ll(argValue(argc, argv, "--blocks"), 0);
    a.blocksize = static_cast<int>(ll(argValue(argc, argv, "--blocksize"), 64));
    a.startT0 = ll(argValue(argc, argv, "--start-t0"), 0);
    a.seekAt = ll(argValue(argc, argv, "--seek-at"), -1);
    a.seekTo = ll(argValue(argc, argv, "--seek-to"), 0);
    a.negativeStart = hasArg(argc, argv, "--negative-start");
    a.decodeChannels = static_cast<int>(ll(argValue(argc, argv, "--decode-channels"), -1));
    a.wrongUnits = hasArg(argc, argv, "--wrong-units");
    a.out = argValue(argc, argv, "--out");
    a.dieAt = static_cast<int>(ll(argValue(argc, argv, "--die-at"), -1));
    a.kindInput = (argValue(argc, argv, "--kind", "input") == "output") ? 0 : 1;
    a.holdMs = ll(argValue(argc, argv, "--hold-ms"), -1);
    a.enqueueCount = ll(argValue(argc, argv, "--enqueue-count"), 0);
    a.valueBase = static_cast<unsigned long long>(ll(argValue(argc, argv, "--value-base"), 0));
    a.opValue = static_cast<int>(ll(argValue(argc, argv, "--op"), 1));
    a.epochBase = static_cast<unsigned long long>(ll(argValue(argc, argv, "--epoch-base"), 0));
    a.featHops = ll(argValue(argc, argv, "--feat-hops"), 0);
    a.featStartSample = ll(argValue(argc, argv, "--feat-start-sample"), 0);
    a.featChannels = static_cast<int>(ll(argValue(argc, argv, "--feat-channels"), 1));
    a.lingerMs = ll(argValue(argc, argv, "--linger-ms"), 0);
    a.totalFrames = ll(argValue(argc, argv, "--total-frames"), 0);
    a.waitWriteHead = ll(argValue(argc, argv, "--wait-write-head"), 0);
    return a;
}

void writeCsv(const std::string& path, const std::string& content)
{
    if (path.empty())
    {
        std::fputs(content.c_str(), stdout);
        std::fflush(stdout);
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr)
    {
        return;
    }
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

void linger(long long ms)
{
    if (ms > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

std::string hex64(u64 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

// ---- 音频环 attach ----
InitResult openAudio(u32 group, u32 ch, const AudioGeometry& g, bool create, SegmentBackendWin32& backend,
                     SegmentView& view, AudioRingHeader*& hdr)
{
    hdr = nullptr;
    const std::wstring name = segmentAudioName(group, ch);
    if (create)
    {
        if (backend.createOrOpen(name, scvb::ipctest::audioSegmentBytes(g.ringFrames), view) != InitResult::kOk)
        {
            return InitResult::kFailed;
        }
        hdr = static_cast<AudioRingHeader*>(view.base);
        const auto r = backend.initHeader(
            view, &hdr->magic, &hdr->abi, nullptr, sizeof(AudioRingHeader), [&] { initAudioHeader(hdr, g); }, true);
        if (r == InitResult::kOk)
        {
            // attach(段已存在)路径 initHeader 不回调 initData → 强制重置几何 + write_head/epoch,
            // 消除上次运行残留的陈旧计数(否则 reader 会读到上一代 write_head/epoch)。
            initAudioHeader(hdr, g);
        }
        return r;
    }
    // 只读 attach(Output 侧):openExisting + allowOverwrite=false;段未就绪则重试。
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        if (backend.openExisting(name, view) == InitResult::kOk)
        {
            hdr = static_cast<AudioRingHeader*>(view.base);
            const auto r =
                backend.initHeader(view, &hdr->magic, &hdr->abi, nullptr, sizeof(AudioRingHeader), {}, false);
            if (r == InitResult::kOk || r == InitResult::kAbiMismatch)
            {
                return r;
            }
            // magic 未就绪(创建者仍在初始化)→ 重试
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return InitResult::kFailed;
}

int runWriter(const Args& a)
{
    AudioGeometry g;
    g.sampleRate = static_cast<u32>(a.sr);
    g.ringFrames = static_cast<u32>(a.ringFrames);
    g.channels = static_cast<u32>(a.channels);

    SegmentBackendWin32 backend;
    SegmentView view;
    AudioRingHeader* hdr = nullptr;
    if (openAudio(static_cast<u32>(a.group), static_cast<u32>(a.ch), g, true, backend, view, hdr) != InitResult::kOk)
    {
        return 3;
    }

    TimelineModel model;
    model.startT0 = a.startT0;
    model.seekAt = a.seekAt;
    model.seekTo = a.seekTo;

    RingWriterState st;
    std::vector<float> buf(static_cast<std::size_t>(a.blocksize) * g.channels);

    for (long long bi = 0; bi < a.blocks; ++bi)
    {
        const long long t0 = model.t0Of(bi, a.blocksize);
        const u64 gen = model.genOf(bi);

        if (t0 < 0)
        {
            // 负 playhead(IPC-5c):不整块写环;跨零点块只写 t0>=0 尾段(01 §5.1 步骤 2)。
            const long long tailEnd = t0 + a.blocksize;
            if (tailEnd > 0)
            {
                const int n = static_cast<int>(tailEnd);
                fillBlockSamples(buf.data(), 0, n, g, gen);
                ringWriteBlock(hdr, g, 0, n, buf.data(), st);
            }
            // 整块仍为负:不写环、不越界。
        }
        else
        {
            fillBlockSamples(buf.data(), t0, a.blocksize, g, gen);
            ringWriteBlock(hdr, g, t0, a.blocksize, buf.data(), st);
        }

        if (a.dieAt >= 0 && bi == static_cast<long long>(a.dieAt))
        {
            // 写满 K 块后自杀(模拟崩溃,无清理机会;01 §5.2 / IPC-6)。
            ::TerminateProcess(::GetCurrentProcess(), 7);
        }
    }
    linger(a.lingerMs); // 保活:让 reader 有时间 attach(段在最后句柄关闭时销毁)
    return 0;
}

int runReader(const Args& a)
{
    AudioGeometry g;
    g.sampleRate = static_cast<u32>(a.sr);
    g.ringFrames = static_cast<u32>(a.ringFrames);
    g.channels = static_cast<u32>(a.channels);

    SegmentBackendWin32 backend;
    SegmentView view;
    AudioRingHeader* hdr = nullptr;
    const auto r = openAudio(static_cast<u32>(a.group), static_cast<u32>(a.ch), g, false, backend, view, hdr);
    if (r == InitResult::kAbiMismatch)
    {
        writeCsv(a.out, "abi_mismatch 1\n");
        return 4;
    }
    if (r != InitResult::kOk)
    {
        writeCsv(a.out, "unavailable 1\n");
        return 3;
    }

    // 几何快照纪律:attach 时读一次段头几何,之后只读本地快照。
    AudioGeometry decode;
    decode.sampleRate = hdr->sample_rate;
    decode.ringFrames = hdr->ring_frames;
    decode.channels = hdr->channels;

    // IPC-19③:channels 不匹配 → 配置错误拒读(不得静默半速)。
    if (a.decodeChannels >= 1 && static_cast<u32>(a.decodeChannels) != decode.channels)
    {
        writeCsv(a.out, "channels_mismatch 1\n");
        return 3;
    }
    if (a.decodeChannels >= 1)
    {
        decode.channels = static_cast<u32>(a.decodeChannels);
    }
    if (a.wrongUnits)
    {
        // IPC-19②:按「另一种口径」解码(把 ring_frames 当样本数)→ 可检出错位。
        if (decode.ringFrames >= 2)
        {
            decode.ringFrames = decode.ringFrames / decode.channels;
        }
    }

    TimelineModel model;
    model.startT0 = a.startT0;
    model.seekAt = a.seekAt;
    model.seekTo = a.seekTo;

    const long long totalFrames = (a.totalFrames > 0) ? a.totalFrames : (a.blocks * a.blocksize);
    RingReaderState st;
    std::vector<float> block(static_cast<std::size_t>(a.blocksize) * decode.channels);
    u64 hash = 0xcbf29ce484222325ull;
    long long framesRead = 0;
    long long blockIndex = 0;
    long long gapTimeout = 0;
    long long gapReadBlock = 0;

    // 可选:读前显式等写方静止(见 Args::waitWriteHead)。覆盖判定(写方超过半环)由此在确定性
    // 时机触发,而不是依赖「写方是否恰好先写完」的进程调度时序。
    if (a.waitWriteHead > 0)
    {
        const u64 deadline = ::GetTickCount64() + 10000;
        while (hdr->write_head_samples.load(std::memory_order_acquire) < static_cast<u64>(a.waitWriteHead))
        {
            if (::GetTickCount64() >= deadline)
            {
                break;
            }
            ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让)
        }
    }

    // 从第一个非负块开始读(负 t0 区间 Output 直通不读环,§5.2)。
    while (framesRead < totalFrames)
    {
        const long long t0 = model.t0Of(blockIndex, a.blocksize);
        if (t0 < 0)
        {
            ++blockIndex;
            continue;
        }
        const long long remaining = totalFrames - framesRead;
        const int n = static_cast<int>(remaining < a.blocksize ? remaining : a.blocksize);

        // 等写方覆盖本块(超时 10s 判 gap)。
        bool timedOut = false;
        const u64 deadline = ::GetTickCount64() + 10000;
        while (hdr->write_head_samples.load(std::memory_order_acquire) < static_cast<u64>(t0 + n))
        {
            if (::GetTickCount64() >= deadline)
            {
                timedOut = true;
                break;
            }
            ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让) //
                        // 让出时间片给写方进程(跨进程比 SwitchToThread 更可靠)
        }
        if (timedOut)
        {
            std::memset(block.data(), 0, block.size() * sizeof(float));
            ++st.gapCount;
            ++gapTimeout;
        }
        else
        {
            const u64 before = st.gapCount;
            ringReadBlock(hdr, decode, t0, n, block.data(), st);
            if (st.gapCount > before)
            {
                ++gapReadBlock;
            }
        }
        hash = scvb::ipctest::fnv1a64(block.data(), static_cast<std::size_t>(n) * decode.channels, hash);
        framesRead += n;
        ++blockIndex;
    }

    std::string csv;
    csv += "blocks " + std::to_string(blockIndex) + "\n";
    csv += "gap_count " + std::to_string(st.gapCount) + "\n";
    csv += "gap_timeout " + std::to_string(gapTimeout) + "\n";
    csv += "gap_readblock " + std::to_string(gapReadBlock) + "\n";
    csv += "hash " + hex64(hash) + "\n";
    csv += "samples " + std::to_string(framesRead * decode.channels) + "\n";
    writeCsv(a.out, csv);
    return 0;
}

int runClaimer(const Args& a)
{
    SegmentBackendWin32 backend;
    Registry reg(backend, static_cast<u32>(a.group));
    const auto open = reg.open();
    if (open != Registry::ClaimResult::kClaimed)
    {
        return (open == Registry::ClaimResult::kAbiMismatch) ? 4 : 3;
    }
    const u32 pid = ::GetCurrentProcessId();
    const u64 now = steadyNowMs();
    Registry::ClaimResult r;
    if (a.kindInput == 0)
    {
        r = reg.claimOutput(pid, now);
    }
    else
    {
        r = reg.claimInput(static_cast<u32>(a.ch), pid, static_cast<u32>(a.sr), 512, now);
    }
    if (r == Registry::ClaimResult::kClaimed)
    {
        return 0;
    }
    return (r == Registry::ClaimResult::kConflict) ? 2 : 3;
}

int runHolder(const Args& a)
{
    SegmentBackendWin32 backend;
    Registry reg(backend, static_cast<u32>(a.group));
    if (reg.open() != Registry::ClaimResult::kClaimed)
    {
        return 3;
    }
    const u32 pid = ::GetCurrentProcessId();
    if (a.kindInput == 0)
    {
        if (reg.claimOutput(pid, steadyNowMs()) != Registry::ClaimResult::kClaimed)
        {
            return 2;
        }
        reg.setConnectedMaskBit(static_cast<u32>(a.ch));
    }
    else
    {
        if (reg.claimInput(static_cast<u32>(a.ch), pid, static_cast<u32>(a.sr), 512, steadyNowMs()) !=
            Registry::ClaimResult::kClaimed)
        {
            return 2;
        }
    }

    const long long deadline = (a.holdMs >= 0) ? static_cast<long long>(steadyNowMs()) + a.holdMs : -1;
    while (deadline < 0 || static_cast<long long>(steadyNowMs()) < deadline)
    {
        if (a.kindInput == 0)
        {
            reg.heartbeatOutput(steadyNowMs());
        }
        else
        {
            reg.heartbeatInput(static_cast<u32>(a.ch), steadyNowMs());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}

int runCtrlWriter(const Args& a)
{
    SegmentBackendWin32 backend;
    CtrlPlane plane(backend, static_cast<u32>(a.group));
    if (plane.open() != InitResult::kOk)
    {
        return 3;
    }
    const auto op = static_cast<CtrlOp>(a.opValue);
    for (long long i = 0; i < a.enqueueCount; ++i)
    {
        if (!plane.enqueue(static_cast<u32>(a.ch), op, a.valueBase + static_cast<unsigned long long>(i)))
        {
            return 3;
        }
    }
    linger(a.lingerMs);
    return 0;
}

int runCtrlReader(const Args& a)
{
    SegmentBackendWin32 backend;
    CtrlPlane plane(backend, static_cast<u32>(a.group));
    if (plane.open() != InitResult::kOk)
    {
        return 3;
    }
    long long count = 0;
    unsigned long long last = 0;
    u32 lastSeq = 0;
    CtrlRecord rec;

    // 握手(IPC-13):writer 先 enqueue;reader 带超时轮询到第一条记录再继续,避免 writer 被抢占时
    // 读到空环(count==0 误判)。
    bool got = false;
    const u64 deadline = ::GetTickCount64() + 10000;
    for (;;)
    {
        got = plane.dequeue(static_cast<u32>(a.ch), rec);
        if (got || ::GetTickCount64() >= deadline)
        {
            break;
        }
        ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让)
    }

    while (got)
    {
        const u32 seq = rec.seq.load(std::memory_order_relaxed);
        const unsigned long long val = rec.value.load(std::memory_order_relaxed);
        if (count > 0 && seq <= lastSeq)
        {
            writeCsv(a.out, "seq_regression 1\n");
            return 2;
        }
        lastSeq = seq;
        last = val;
        ++count;
        got = plane.dequeue(static_cast<u32>(a.ch), rec);
    }
    std::string csv;
    csv += "count " + std::to_string(count) + "\n";
    csv += "last_value " + std::to_string(last) + "\n";
    csv += "overflow " + std::to_string(plane.overflowCount(static_cast<u32>(a.ch))) + "\n";
    writeCsv(a.out, csv);
    return 0;
}

int runGlobalInfoWriter(const Args& a)
{
    SegmentBackendWin32 backend;
    CtrlPlane plane(backend, static_cast<u32>(a.group));
    if (plane.open() != InitResult::kOk)
    {
        return 3;
    }
    OutputGlobalInfoSnapshot s;
    s.capture_enabled = 1;
    s.output_sample_rate = static_cast<u32>(a.sr);
    s.flags = kOutputEnabled;
    for (u32 i = 0; i < kMaxChannels; ++i)
    {
        s.gap_count[i] = 100 + i;
        s.overlap_count[i] = 200 + i;
        s.epoch_summary[i] = a.epochBase + i;
    }
    plane.refreshGlobalInfo(s);
    linger(a.lingerMs);
    return 0;
}

int runGlobalInfoReader(const Args& a)
{
    SegmentBackendWin32 backend;
    CtrlPlane plane(backend, static_cast<u32>(a.group));
    if (plane.open() != InitResult::kOk)
    {
        return 3;
    }
    // 握手(IPC-16):writer refreshGlobalInfo 前 capture_enabled 保持 0;带超时轮询到非零再读全量,
    // 避免 writer 被抢占时读到全零快照。
    OutputGlobalInfoSnapshot s;
    const u64 deadline = ::GetTickCount64() + 10000;
    for (;;)
    {
        s = plane.readGlobalInfo();
        if (s.capture_enabled != 0 || ::GetTickCount64() >= deadline)
        {
            break;
        }
        ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让)
    }

    // 握手超时(capture_enabled 仍 0)→ 不写 CSV,返回独立退出码 5:writer 未在 10s 内发布快照
    // (被抢占/段异常/被截杀)。父进程 REQUIRE(waitPeer(pr.pi, 15000) == 0) 据此判失败,而非读全零 CSV。
    if (s.capture_enabled == 0)
    {
        return 5;
    }

    std::string csv;
    csv += "capture_enabled " + std::to_string(s.capture_enabled) + "\n";
    csv += "output_sample_rate " + std::to_string(s.output_sample_rate) + "\n";
    csv += "flags " + std::to_string(s.flags) + "\n";
    csv += "gap_count0 " + std::to_string(s.gap_count[0]) + "\n";
    csv += "gap_count14 " + std::to_string(s.gap_count[14]) + "\n";
    csv += "overlap_count14 " + std::to_string(s.overlap_count[14]) + "\n";
    csv += "epoch_summary14 " + std::to_string(s.epoch_summary[14]) + "\n";
    writeCsv(a.out, csv);
    return 0;
}

// ---- 特征环(IPC-14)----
bool openFeat(u32 group, u32 ch, bool create, SegmentBackendWin32& backend, SegmentView& view, FeatHeader*& hdr,
              FeatFrame*& ring, u32& mappedCapacity)
{
    hdr = nullptr;
    ring = nullptr;
    mappedCapacity = 0;
    const std::wstring name = segmentFeatName(group, ch);
    const std::size_t segBytes = sizeof(FeatHeader) + static_cast<std::size_t>(kFeatCapacityHops) * sizeof(FeatFrame);
    if (create)
    {
        if (backend.createOrOpen(name, segBytes, view) != InitResult::kOk)
        {
            return false;
        }
        hdr = static_cast<FeatHeader*>(view.base);
        const auto r = backend.initHeader(
            view, &hdr->magic, &hdr->abi, nullptr, sizeof(FeatHeader),
            [&] {
                hdr->hop_ms = kFeatHopMs;
                hdr->capacity_hops = kFeatCapacityHops;
                hdr->base_hop.store(0, std::memory_order_relaxed);
                hdr->write_hop.store(0, std::memory_order_relaxed);
            },
            true);
        if (r != InitResult::kOk)
        {
            return false;
        }
    }
    else
    {
        // 只读 attach 重试(生产方可能在竞态窗口内尚未创建段)。
        bool attached = false;
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            if (backend.openExisting(name, view) == InitResult::kOk)
            {
                hdr = static_cast<FeatHeader*>(view.base);
                const auto r = backend.initHeader(view, &hdr->magic, &hdr->abi, nullptr, sizeof(FeatHeader), {}, false);
                if (r == InitResult::kOk)
                {
                    attached = true;
                    break;
                }
                if (r == InitResult::kAbiMismatch)
                {
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!attached)
        {
            return false;
        }
    }
    const std::size_t frameBytes = view.size - sizeof(FeatHeader);
    mappedCapacity = static_cast<u32>(frameBytes / sizeof(FeatFrame));
    ring = reinterpret_cast<FeatFrame*>(static_cast<char*>(view.base) + sizeof(FeatHeader));
    return true;
}

int runFeatWriter(const Args& a)
{
    SegmentBackendWin32 backend;
    SegmentView view;
    FeatHeader* hdr = nullptr;
    FeatFrame* ring = nullptr;
    u32 mappedCapacity = 0;
    if (!openFeat(static_cast<u32>(a.group), static_cast<u32>(a.ch), true, backend, view, hdr, ring, mappedCapacity))
    {
        return 3;
    }

    FeatRing feat;
    feat.bind(hdr, ring, mappedCapacity);
    feat.prepare(static_cast<double>(a.sr), a.featChannels, 512);
    feat.setCapturing(true);
    feat.startRun(a.featStartSample);

    const int hopSize = feat.hopSize();
    const int samplesPerPush = hopSize;
    std::vector<std::vector<float>> plane(static_cast<std::size_t>(a.featChannels),
                                          std::vector<float>(static_cast<std::size_t>(samplesPerPush), 0.0f));
    std::vector<const float*> ptrs(static_cast<std::size_t>(a.featChannels));
    for (long long hop = 0; hop < a.featHops; ++hop)
    {
        const u64 timelineSample = static_cast<u64>(a.featStartSample) + static_cast<u64>(hop) * hopSize;
        for (int c = 0; c < a.featChannels; ++c)
        {
            for (int s = 0; s < samplesPerPush; ++s)
            {
                plane[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)] =
                    sampleAt(timelineSample + static_cast<u64>(s), static_cast<u32>(c), 1, 1);
            }
            ptrs[static_cast<std::size_t>(c)] = plane[static_cast<std::size_t>(c)].data();
        }
        feat.processBlock(ptrs.data(), samplesPerPush);
    }
    linger(a.lingerMs);
    return 0;
}

int runFeatReader(const Args& a)
{
    SegmentBackendWin32 backend;
    SegmentView view;
    FeatHeader* hdr = nullptr;
    FeatFrame* ring = nullptr;
    u32 mappedCapacity = 0;
    if (!openFeat(static_cast<u32>(a.group), static_cast<u32>(a.ch), false, backend, view, hdr, ring, mappedCapacity))
    {
        return 3;
    }

    FeatPuller puller;
    puller.bind(static_cast<u32>(a.ch), hdr, ring, mappedCapacity);
    analysis::FrameStore store(kMaxChannels);
    store.channel(static_cast<u32>(a.ch)).setReadOnly(false); // ChannelFrames 默认只读,须显式放行写入
    const u32 activeMask = 1u << (a.ch - 1);
    const analysis::HopRange gate{0, std::numeric_limits<uint64_t>::max()};

    // 握手(建议 4):先带超时等 writer 开写(write_hop > base_hop),再进入稳定收敛计数;否则 writer
    // 尚未开写时连续 3 拍无进展会提前退出(空 coverage)。
    {
        const u64 deadline = ::GetTickCount64() + 10000;
        while (hdr->write_hop.load(std::memory_order_acquire) <= hdr->base_hop.load(std::memory_order_acquire))
        {
            if (::GetTickCount64() >= deadline)
            {
                break;
            }
            ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让)
        }
    }

    // 拉到 lastPulled 不再推进(写方已退出且全部数据已拉取;连续 3 拍无进展即停)。
    int stableTicks = 0;
    u64 lastPulled = 0;
    for (int tick = 0; tick < 10000; ++tick)
    {
        puller.pullTick(store, gate, 0, activeMask);
        const u64 lp = puller.state(static_cast<u32>(a.ch)).lastPulled;
        if (lp == lastPulled)
        {
            if (++stableTicks >= 3)
            {
                break;
            }
        }
        else
        {
            stableTicks = 0;
            lastPulled = lp;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto& cov = store.channel(static_cast<u32>(a.ch)).coverage();
    const auto ranges = cov.intersect(gate);
    std::string csv;
    csv += "base_hop " + std::to_string(hdr->base_hop.load(std::memory_order_acquire)) + "\n";
    csv += "write_hop " + std::to_string(hdr->write_hop.load(std::memory_order_acquire)) + "\n";
    csv += "covered_hops " + std::to_string(cov.coveredHops(gate)) + "\n";
    csv += "coverage_ranges " + std::to_string(ranges.size()) + "\n";
    csv += "coverage_text ";
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        if (i > 0)
        {
            csv += ",";
        }
        csv += std::to_string(ranges[i].begin) + "-" + std::to_string(ranges[i].end);
    }
    csv += "\n";
    writeCsv(a.out, csv);
    return 0;
}

} // namespace

// ---- [T44/J75] viz 段(VIZ-1/2/3)----

int runVizWriter(const Args& a)
{
    SegmentBackendWin32 backend;
    VizPlane plane(backend, static_cast<u32>(a.group));
    if (plane.open() != InitResult::kOk)
    {
        return 3;
    }
    auto snap = std::make_unique<VizSnapshot>();
    snap->publishMs = 123456;
    snap->windowStartSamples = 0;
    snap->windowSpanSamples = static_cast<u64>(a.sr) * 120u; // 120s 窗口
    snap->playheadSamples = 48000;
    snap->loopStartSamples = 96000;
    snap->loopEndSamples = 192000;
    snap->sampleRate = static_cast<u32>(a.sr);
    snap->versionActive = 2;
    snap->playheadFlags = kVizPlaying | kVizLooping | kVizLoopValid;
    snap->playheadEpoch = 7;
    snap->onlineMask = 0x7FFFu;
    snap->coveredMask = 0x0003u;
    snap->stereoMask = 0x0002u;
    snap->leadMask = 0x0004u; // 轨3 = lead_lock
    snap->laneRevision = 42;
    // 每轨当前值三件套(分布图数据面);轨3 起保持哨兵 = 无数据。
    snap->panNow[0] = vizPackPan(-12.5);
    snap->volDb[0] = vizPackFixed(-6.25, kVizVolDbMin, kVizVolDbMax);
    snap->widthPct[0] = vizPackFixed(80.0, kVizWidthMin, kVizWidthMax);
    snap->panNow[1] = vizPackPan(25.0);
    snap->volDb[1] = vizPackFixed(0.0, kVizVolDbMin, kVizVolDbMax);
    snap->widthPct[1] = vizPackFixed(100.0, kVizWidthMin, kVizWidthMax);
    // 轨名:ASCII + 多字节 UTF-8(截断必须落在字符边界)。
    snap->label[0] = "Lead";
    snap->label[1] = "\xE4\xB8\xBB\xE5\x94\xB1"; // 主唱
    for (u32 t = 0; t < kMaxChannels; ++t)
    {
        snap->trackColor[t] = t + 1;
    }
    // 轨1:全覆盖、pan 沿列线性上升;轨2:只覆盖前半段(断线场景);其余轨保持哨兵。
    for (u32 i = 0; i < kVizColumns; ++i)
    {
        snap->pan[0][i] = vizPackPan(-100.0 + 200.0 * static_cast<double>(i) / static_cast<double>(kVizColumns - 1));
        snap->setCovered(0, i);
        if (i < kVizColumns / 2)
        {
            snap->pan[1][i] = vizPackPan(25.0);
            snap->setCovered(1, i);
        }
    }
    plane.publish(*snap, /*writeLanes=*/true);
    linger(a.lingerMs);
    return 0;
}

// 真 VizPublisher 发布(VIZ-4):轨1 [0,30s) pan=-50;轨2 [60,90s) pan=+40;其余轨无分段。
// 与手搓快照的 viz-writer 互补 —— 这条路把「引擎曲线 → 降采样 → 段」整段接线也验了。
int runVizPublisher(const Args& a)
{
    SegmentBackendWin32 backend;
    scvb::output::VizPublisher pub(backend, static_cast<u32>(a.group));
    if (pub.open() != InitResult::kOk)
    {
        return 3;
    }
    const double sr = static_cast<double>(a.sr);

    auto crvs = std::make_unique<scvb::state::CrvsData>();
    const auto seg = [sr](double t0Sec, double t1Sec, float pan) {
        scvb::state::Segment x;
        x.t0 = static_cast<std::int64_t>(t0Sec * sr);
        x.t1 = static_cast<std::int64_t>(t1Sec * sr);
        x.pan = pan;
        return x;
    };
    crvs->versions[0].tracks[0].segments = {seg(0.0, 30.0, -50.0f)};
    crvs->versions[0].tracks[1].segments = {seg(60.0, 90.0, 40.0f)};

    scvb::CurveEvaluator c0;
    scvb::CurveEvaluator c1;
    const auto build = [&](scvb::CurveEvaluator& ev, const std::vector<scvb::state::Segment>& segs) {
        std::vector<scvb::CurveSegment> cs;
        for (const auto& x : segs)
        {
            scvb::CurveSegment c;
            c.startSec = static_cast<double>(x.t0) / sr;
            c.endSec = static_cast<double>(x.t1) / sr;
            c.pan = x.pan;
            cs.push_back(c);
        }
        ev.build(cs, scvb::TransitionConfig{});
    };
    build(c0, crvs->versions[0].tracks[0].segments);
    build(c1, crvs->versions[0].tracks[1].segments);

    scvb::output::VizPublishInput in;
    in.crvs = crvs.get();
    in.curves[0] = &c0;
    in.curves[1] = &c1;
    in.versionActive = 1;
    in.enabledMask = 0x7FFF;
    in.sampleRate = sr;
    in.crvsRevision = 1;
    in.playhead.timeSamples = 0;
    in.playhead.sampleRate = sr;
    in.label[0] = "Lead";
    in.leadMask = 0x0001;
    in.widthPct[0] = 80.0f;

    // 至少发一帧;linger 期间按**生产同款节拍**续发,读方随时 attach 都能拿到一致帧。
    // 时钟用 steadyNowMs()(与生产路径 OutputProcessor 的 vizTimer_ 同源)——
    // 用从 0 起的假时钟会让 publish_ms 落在读方的「没动过」判据上。
    //
    // [SL-192] 驱动改成「**1ms 细粒度轮询 + 让发布器自己的闸门分频**」,不再是写死的 50ms。
    //
    // 为什么不按 kPublishTimerHz 算一个 ~16ms 的 Sleep:Windows 的默认定时器分辨率是
    // ~15.6ms,`Sleep(16)` 实得约 31ms,配 33ms 闸门就变成每两拍才发一帧 = ~16Hz ——
    // 实测就是这么来的(读方 1.5s 只看到 25 帧)。**驱动被分辨率抬慢、于是与闸门同频异相**,
    // 又是 SL-192 那个坑的第三种长相。
    // `Sleep(1)` 同样受分辨率影响,但它远小于闸门,即使被抬到 15.6ms 也是每两三拍一帧,
    // 分频权仍在闸门手里 —— 对端因此忠实于生产路径(那边是 JUCE 的高分辨率 Timer)。
    constexpr u32 driveMs = 1;
    // 播放头**逐帧推进**:这是给读方的跨字段不变式 —— 帧头里的 playhead_samples 与
    // publish_ms 必须同步前进。撕裂读(新旧两帧拼接)会让它们对不上或倒退,而
    // 「所有字段都恒定」的对端根本测不出撕裂:拼接出来的帧与正确帧逐字节相同。
    const auto step = static_cast<std::int64_t>(sr / 30.0); // 每帧约 1/30 秒
    pub.tick(scvb::steadyNowMs(), in);
    const u64 deadline = ::GetTickCount64() + static_cast<u64>(a.lingerMs > 0 ? a.lingerMs : 0);
    while (::GetTickCount64() < deadline)
    {
        ::Sleep(driveMs);
        in.playhead.timeSamples += step;
        pub.tick(scvb::steadyNowMs(), in);
    }
    return 0;
}

int runVizReader(const Args& a)
{
    SegmentBackendWin32 backend;
    VizPlane plane(backend, static_cast<u32>(a.group));
    // 握手:writer 可能尚未建段/发布,带超时轮询到 attach 成功且 lane_revision 非零。
    const u64 deadline = ::GetTickCount64() + 10000;
    auto snap = std::make_unique<VizSnapshot>();
    InitResult ir = InitResult::kFailed;
    bool got = false;
    for (;;)
    {
        if (!plane.isOpen())
        {
            ir = plane.attachReadOnly();
        }
        else
        {
            ir = InitResult::kOk;
        }
        if (ir == InitResult::kOk && plane.read(*snap) && snap->laneRevision != 0)
        {
            got = true;
            break;
        }
        if (::GetTickCount64() >= deadline)
        {
            break;
        }
        ::Sleep(1); // 让出时间片而不是忙等(::Sleep(0) 只在同优先级就绪队列非空时才让)
    }

    std::string csv;
    csv += "attach " + std::to_string(static_cast<int>(ir)) + "\n";
    csv += "read_ok " + std::to_string(got ? 1 : 0) + "\n";
    csv += "read_only " + std::to_string(plane.isReadOnly() ? 1 : 0) + "\n";
    csv += "geometry_ok " + std::to_string(plane.geometryMatches() ? 1 : 0) + "\n";
    csv += "columns " + std::to_string(kVizColumns) + "\n";
    csv += "window_span " + std::to_string(snap->windowSpanSamples) + "\n";
    csv += "playhead " + std::to_string(snap->playheadSamples) + "\n";
    csv += "loop_start " + std::to_string(snap->loopStartSamples) + "\n";
    csv += "loop_end " + std::to_string(snap->loopEndSamples) + "\n";
    csv += "playhead_flags " + std::to_string(snap->playheadFlags) + "\n";
    csv += "playhead_epoch " + std::to_string(snap->playheadEpoch) + "\n";
    csv += "version_active " + std::to_string(snap->versionActive) + "\n";
    csv += "online_mask " + std::to_string(snap->onlineMask) + "\n";
    csv += "covered_mask " + std::to_string(snap->coveredMask) + "\n";
    csv += "stereo_mask " + std::to_string(snap->stereoMask) + "\n";
    csv += "lead_mask " + std::to_string(snap->leadMask) + "\n";
    csv += "seq " + std::to_string(snap->seq) + "\n";
    csv += "generation " + std::to_string(snap->generation) + "\n";
    csv += "lane_revision " + std::to_string(snap->laneRevision) + "\n";
    csv += "color1 " + std::to_string(snap->trackColor[0]) + "\n";
    csv += "color15 " + std::to_string(snap->trackColor[14]) + "\n";
    csv += "pan_t1_first " + std::to_string(static_cast<long long>(snap->pan[0][0])) + "\n";
    csv += "pan_t1_last " + std::to_string(static_cast<long long>(snap->pan[0][kVizColumns - 1])) + "\n";
    csv += "pan_t2_mid " + std::to_string(static_cast<long long>(snap->pan[1][10])) + "\n";
    csv += "pan_t2_tail " + std::to_string(static_cast<long long>(snap->pan[1][kVizColumns - 1])) + "\n";
    csv += "pan_t3_any " + std::to_string(static_cast<long long>(snap->pan[2][0])) + "\n";
    csv += "now_pan1 " + std::to_string(static_cast<long long>(snap->panNow[0])) + "\n";
    csv += "now_vol1 " + std::to_string(static_cast<long long>(snap->volDb[0])) + "\n";
    csv += "now_width1 " + std::to_string(static_cast<long long>(snap->widthPct[0])) + "\n";
    csv += "now_pan3 " + std::to_string(static_cast<long long>(snap->panNow[2])) + "\n";
    csv += "label1 " + snap->label[0] + "\n";
    csv += "label2 " + snap->label[1] + "\n";
    csv += "cov_t1_0 " + std::to_string(snap->covered(0, 0) ? 1 : 0) + "\n";
    csv += "cov_t1_last " + std::to_string(snap->covered(0, kVizColumns - 1) ? 1 : 0) + "\n";
    csv += "cov_t2_0 " + std::to_string(snap->covered(1, 0) ? 1 : 0) + "\n";
    csv += "cov_t2_last " + std::to_string(snap->covered(1, kVizColumns - 1) ? 1 : 0) + "\n";
    csv += "cov_t3_0 " + std::to_string(snap->covered(2, 0) ? 1 : 0) + "\n";
    // 位序字边界(LSB 起、每字 32 格):写反了图照画,只是断线整体错开 32 的倍数,肉眼查不出来。
    csv += "cov_bit31 " + std::to_string(snap->covered(0, 31) ? 1 : 0) + "\n";
    csv += "cov_bit32 " + std::to_string(snap->covered(0, 32) ? 1 : 0) + "\n";
    csv += "cov_word0 " + std::to_string(snap->coverage[0][0]) + "\n";
    writeCsv(a.out, csv);
    return 0;
}

int main(int argc, char** argv)
{
    const Args a = parse(argc, argv);
    if (a.role == "writer")
    {
        return runWriter(a);
    }
    if (a.role == "reader")
    {
        return runReader(a);
    }
    if (a.role == "claimer")
    {
        return runClaimer(a);
    }
    if (a.role == "holder")
    {
        return runHolder(a);
    }
    if (a.role == "ctrl-writer")
    {
        return runCtrlWriter(a);
    }
    if (a.role == "ctrl-reader")
    {
        return runCtrlReader(a);
    }
    if (a.role == "globalinfo-writer")
    {
        return runGlobalInfoWriter(a);
    }
    if (a.role == "globalinfo-reader")
    {
        return runGlobalInfoReader(a);
    }
    if (a.role == "feat-writer")
    {
        return runFeatWriter(a);
    }
    if (a.role == "feat-reader")
    {
        return runFeatReader(a);
    }
    if (a.role == "viz-writer")
    {
        return runVizWriter(a);
    }
    if (a.role == "viz-reader")
    {
        return runVizReader(a);
    }
    if (a.role == "viz-publisher")
    {
        return runVizPublisher(a);
    }
    return 99;
}
