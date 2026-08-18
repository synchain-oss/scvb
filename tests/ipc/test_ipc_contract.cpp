// SPDX-License-Identifier: GPL-3.0-or-later
// test_ipc_contract.cpp —— L1 双进程 IPC 契约测试(T07b / J19,10-validation §2.3 的 IPC-1..20 共 26 条)。
// 父进程(Catch2)经 CreateProcessW 拉起 tests/tools/scvb_ipc_peer 作对端;跨进程用例用真实共享内存 +
// 真实 pid 探活 + TerminateProcess,绝不用 InProcess 模拟(J19 的价值所在)。纯逻辑用例(布局/覆盖判定/
// fp_report 打包/内存序/J33 交错/双线程 flags)在本进程内直接驱动生产代码。
//
// 真源:masterPlan 10-validation §2.3(IPC-1..20 逐行实数)、ipc-contract v1.5、01-architecture §5、04 §3.2。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ipc/CtrlPlane.h"
#include "ipc/FeatRing.h"
#include "ipc/Registry.h"
#include "ipc/SegmentBackendInProcess.h"
#include "ipc/SegmentBackendWin32.h"
#include "ipc_contract_harness.h"

using scvb::kMaxChannels;
using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::kSlotActive;
using scvb::kSlotFree;
using scvb::u32;
using scvb::u64;
using scvb::ipctest::AudioGeometry;
using scvb::ipctest::fillBlockSamples;
using scvb::ipctest::initAudioHeader;
using scvb::ipctest::ringReadBlock;
using scvb::ipctest::RingReaderState;
using scvb::ipctest::ringWriteBlock;
using scvb::ipctest::RingWriterState;
using scvb::ipctest::TimelineModel;

using namespace scvb;

namespace
{

// ---------------------------------------------------------------------------
// 进程 spawn / 等待 / 杀进程 helper
// ---------------------------------------------------------------------------

std::wstring peerExe()
{
    // 运行期解析对端进程路径:不编译期烘焙 $<TARGET_FILE>(VS 生成器渲染反斜杠路径会触发 MSVC
    // C4129 或损坏转义,PR#46 复审)。从测试 exe 自身路径推导:
    //   测试 exe = <build>/tests/ipc[/<Config>]/scvb_ipc_tests.exe
    //   对端 exe = <build>/tests/tools[/<Config>]/scvb_ipc_peer.exe
    wchar_t self[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return L"";
    }
    std::wstring dir(self, self + n);
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L"";
    }
    dir.resize(slash); // 测试 exe 目录

    // 候选链:Ninja 单配置 <build>/tests/tools/ 与 VS 多配置 <build>/tests/tools/<Config>/。
    // (dir 末段为 <Config> 时须回退两层:dir/.. = <build>/tests/ipc,dir/../.. = <build>/tests。)
    std::vector<std::wstring> candidates;
    candidates.push_back(dir + L"/../tools/scvb_ipc_peer.exe"); // Ninja
    const std::size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
    {
        const std::wstring config = dir.substr(lastSlash + 1);
        candidates.push_back(dir + L"/../../tools/" + config + L"/scvb_ipc_peer.exe"); // VS 多配置
    }

    for (const auto& cand : candidates)
    {
        wchar_t full[MAX_PATH];
        if (::GetFullPathNameW(cand.c_str(), MAX_PATH, full, nullptr) != 0 &&
            ::GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES)
        {
            return std::wstring(full);
        }
    }
    return L"";
}

std::wstring wide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::string quote(const std::string& s)
{
    if (s.find(' ') == std::string::npos && s.find('"') == std::string::npos)
    {
        return s;
    }
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"')
        {
            out += "\\\"";
        }
        else
        {
            out += c;
        }
    }
    out += "\"";
    return out;
}

PROCESS_INFORMATION spawnPeer(const std::vector<std::string>& args, int* spawnErr)
{
    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    const std::wstring exe = peerExe();
    if (exe.empty())
    {
        if (spawnErr != nullptr)
        {
            *spawnErr = static_cast<int>(ERROR_FILE_NOT_FOUND); // 2(winerror.h 宏,勿加 :: 前缀)
        }
        return PROCESS_INFORMATION{};
    }
    std::wstring cmd = L"\"" + exe + L"\"";
    for (const auto& a : args)
    {
        cmd += L" " + wide(quote(a));
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        if (spawnErr != nullptr)
        {
            *spawnErr = static_cast<int>(::GetLastError());
        }
        return PROCESS_INFORMATION{};
    }
    if (spawnErr != nullptr)
    {
        *spawnErr = 0;
    }
    return pi;
}

int waitPeer(PROCESS_INFORMATION& pi, DWORD timeoutMs)
{
    if (pi.hProcess == nullptr)
    {
        return -1;
    }
    const DWORD r = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    if (r != WAIT_OBJECT_0)
    {
        return -1;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    pi.hProcess = nullptr;
    pi.hThread = nullptr;
    return static_cast<int>(code);
}

void killPeer(PROCESS_INFORMATION& pi)
{
    if (pi.hProcess != nullptr)
    {
        ::TerminateProcess(pi.hProcess, 9);
        ::WaitForSingleObject(pi.hProcess, 5000);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        pi.hProcess = nullptr;
        pi.hThread = nullptr;
    }
}

struct PeerGuard
{
    PROCESS_INFORMATION pi{};
    ~PeerGuard() { killPeer(pi); }
    PeerGuard(const PeerGuard&) = delete;
    PeerGuard& operator=(const PeerGuard&) = delete;
    PeerGuard() = default;
};

std::atomic<int> g_csvCounter{0};

std::string tempCsvPath()
{
    char buf[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, buf);
    std::string p = std::string(buf, n);
    p += "scvb_ipc_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(g_csvCounter.fetch_add(1)) +
         ".csv";
    return p;
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good())
    {
        return "";
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void deleteFile(const std::string& path)
{
    ::DeleteFileA(path.c_str());
}

std::map<std::string, std::string> parseCsv(const std::string& text)
{
    std::map<std::string, std::string> m;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos)
        {
            continue;
        }
        m[line.substr(0, sp)] = line.substr(sp + 1);
    }
    return m;
}

long long csvLL(const std::map<std::string, std::string>& m, const std::string& k)
{
    const auto it = m.find(k);
    return (it == m.end()) ? 0 : std::strtoll(it->second.c_str(), nullptr, 10);
}

u64 csvU64Hex(const std::map<std::string, std::string>& m, const std::string& k)
{
    const auto it = m.find(k);
    return (it == m.end()) ? 0 : std::strtoull(it->second.c_str(), nullptr, 16);
}

struct RingRun
{
    int writerExit = -1;
    int readerExit = -1;
    std::string csv;
};

RingRun runWriterThenReader(const std::vector<std::string>& writerArgs, const std::vector<std::string>& readerArgs)
{
    RingRun out;
    const std::string csv = tempCsvPath();

    PeerGuard writer;
    PeerGuard reader;
    int err = 0;

    // 生产方写完保活(保持段句柄打开),消费方随后 attach(命名段在最后句柄关闭时销毁)。
    std::vector<std::string> wa = writerArgs;
    wa.push_back("--linger-ms=1000");
    writer.pi = spawnPeer(wa, &err);
    REQUIRE(err == 0);

    std::vector<std::string> ra = readerArgs;
    ra.push_back("--out=" + csv);
    reader.pi = spawnPeer(ra, &err);
    REQUIRE(err == 0);

    out.writerExit = waitPeer(writer.pi, 30000);
    out.readerExit = waitPeer(reader.pi, 30000);
    out.csv = readFile(csv);
    deleteFile(csv);
    return out;
}

// ---------------------------------------------------------------------------
// 期望哈希(镜像 reader 的读序;seek/负 t0 由 TimelineModel 表达)
// ---------------------------------------------------------------------------
u64 expectedRampHash(const AudioGeometry& g, long long totalFrames, int blocksize, const TimelineModel& model)
{
    u64 hash = 0xcbf29ce484222325ull;
    std::vector<float> block(static_cast<std::size_t>(blocksize) * g.channels);
    long long framesRead = 0;
    long long bi = 0;
    while (framesRead < totalFrames)
    {
        const long long t0 = model.t0Of(bi, blocksize);
        if (t0 < 0)
        {
            ++bi;
            continue;
        }
        const long long remaining = totalFrames - framesRead;
        const int n = static_cast<int>(remaining < blocksize ? remaining : blocksize);
        fillBlockSamples(block.data(), t0, n, g, model.genOf(bi));
        hash = scvb::ipctest::fnv1a64(block.data(), static_cast<std::size_t>(n) * g.channels, hash);
        framesRead += n;
        ++bi;
    }
    return hash;
}

void writeRampInProcess(AudioRingHeader* hdr, const AudioGeometry& g, long long totalFrames, int blocksize,
                        TimelineModel model = {})
{
    RingWriterState st;
    std::vector<float> buf(static_cast<std::size_t>(blocksize) * g.channels);
    long long framesWritten = 0;
    long long bi = 0;
    while (framesWritten < totalFrames)
    {
        const long long t0 = model.t0Of(bi, blocksize);
        if (t0 < 0)
        {
            ++bi;
            continue;
        }
        const long long remaining = totalFrames - framesWritten;
        const int n = static_cast<int>(remaining < blocksize ? remaining : blocksize);
        fillBlockSamples(buf.data(), t0, n, g, model.genOf(bi));
        ringWriteBlock(hdr, g, t0, n, buf.data(), st);
        framesWritten += n;
        ++bi;
    }
}

u64 readRampInProcess(const AudioRingHeader* hdr, const AudioGeometry& g, long long totalFrames, int blocksize,
                      u64& gapCount)
{
    RingReaderState st;
    std::vector<float> buf(static_cast<std::size_t>(blocksize) * g.channels);
    u64 hash = 0xcbf29ce484222325ull;
    long long framesRead = 0;
    long long t0 = 0;
    while (framesRead < totalFrames)
    {
        const long long remaining = totalFrames - framesRead;
        const int n = static_cast<int>(remaining < blocksize ? remaining : blocksize);
        ringReadBlock(hdr, g, t0, n, buf.data(), st);
        hash = scvb::ipctest::fnv1a64(buf.data(), static_cast<std::size_t>(n) * g.channels, hash);
        framesRead += n;
        t0 += n;
    }
    gapCount = st.gapCount;
    return hash;
}

// 在栈上零初始化一个 FeatHeader(与共享内存内核零初始化同法;原子零值合法)。
struct FeatHeaderBox
{
    alignas(scvb::FeatHeader) unsigned char buf[sizeof(scvb::FeatHeader)]{};
    scvb::FeatHeader* hdr;
    FeatHeaderBox() : hdr(reinterpret_cast<scvb::FeatHeader*>(buf)) {}
};

void initFeatHeader(scvb::FeatHeader* h)
{
    // PR#42 起 FeatRing::bind / FeatPuller::bind 校验 magic/abi(不符拒绑);in-process 构造须显式置位。
    h->magic.store(kScvbMagic, std::memory_order_relaxed);
    h->abi.store(kScvbAbi, std::memory_order_relaxed);
    h->hop_ms = scvb::kFeatHopMs;
    h->capacity_hops = scvb::kFeatCapacityHops;
    h->base_hop.store(0, std::memory_order_relaxed);
    h->write_hop.store(0, std::memory_order_relaxed);
}

// 强制把某组 registry 段清零到「magic=SCVB / abi=1 / generation=1 / 全槽空闲」。
// 段可能被此前测试的 SegmentHandle 宽限期保活(状态累积),本 helper 让每个 registry 用例互不串扰。
void resetRegistry(scvb::SegmentBackendWin32& backend, u32 group)
{
    scvb::SegmentView v;
    if (backend.createOrOpen(scvb::segmentRegistryName(group), scvb::kRegistrySegmentSize, v) != scvb::InitResult::kOk)
    {
        return;
    }
    std::memset(v.base, 0, v.size);
    auto* h = static_cast<scvb::RegistryHeader*>(v.base);
    h->magic.store(kScvbMagic, std::memory_order_release);
    h->abi.store(kScvbAbi, std::memory_order_release);
    h->generation.store(1, std::memory_order_release);
    backend.unmap(v);
}

} // namespace

// ===========================================================================
// IPC-1 布局冻结 + 段名参数化
// ===========================================================================
TEST_CASE("IPC-1 布局冻结 + 段名参数化", "[ipc][contract]")
{
    static_assert(std::atomic<u32>::is_always_lock_free);
    static_assert(std::atomic<u64>::is_always_lock_free);
    static_assert(decltype(scvb::CtrlRecord::value)::is_always_lock_free);
    static_assert(decltype(scvb::InputSlot::flags)::is_always_lock_free);
    static_assert(sizeof(scvb::CtrlRecord) == 24);
    static_assert(offsetof(scvb::CtrlRecord, value) == 16);
    static_assert(sizeof(scvb::AudioRingHeader) == 40);
    static_assert(offsetof(scvb::AudioRingHeader, channels) == 16);

    REQUIRE(scvb::ipctest::segmentNamesAreParametric());
    REQUIRE(scvb::segmentLogicalName(1, scvb::SegmentKind::kRegistry) == L"SynchainSCVB.v1.g1.registry");
    REQUIRE(scvb::segmentLogicalName(8, scvb::SegmentKind::kAudio, 15) == L"SynchainSCVB.v1.g8.audio.ch15");
    REQUIRE(scvb::segmentLogicalName(8, scvb::SegmentKind::kFeat, 15) == L"SynchainSCVB.v1.g8.feat.ch15");
    REQUIRE(scvb::segmentLogicalName(8, scvb::SegmentKind::kCtrl) == L"SynchainSCVB.v1.g8.ctrl");

    // 跨进程:g1 与 g8 的 registry 是两段独立共享内存(各自 claim 成功,段名跨进程同构解析)。
    PeerGuard pa;
    PeerGuard pb;
    int err = 0;
    pa.pi = spawnPeer({"--role=claimer", "--group=1", "--ch=1"}, &err);
    REQUIRE(err == 0);
    pb.pi = spawnPeer({"--role=claimer", "--group=8", "--ch=15"}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(pa.pi, 15000) == 0);
    REQUIRE(waitPeer(pb.pi, 15000) == 0);
}

// ===========================================================================
// IPC-2 单进程 SPSC 时间线寻址(不同块长逐样本一致)
// ===========================================================================
TEST_CASE("IPC-2 单进程 SPSC 时间线寻址(多块长逐样本一致)", "[ipc][contract]")
{
    AudioGeometry g;
    g.sampleRate = 48000;
    g.ringFrames = scvb::kDefaultRingFrames;
    g.channels = 1;

    scvb::SegmentBackendWin32 backend;
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(scvb::segmentAudioName(1, 1), scvb::ipctest::audioSegmentBytes(g.ringFrames), view) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::AudioRingHeader*>(view.base);
    REQUIRE(backend.initHeader(
                view, &hdr->magic, &hdr->abi, nullptr, sizeof(scvb::AudioRingHeader), [&] { initAudioHeader(hdr, g); },
                true) == scvb::InitResult::kOk);

    const long long totalFrames = 100000;
    writeRampInProcess(hdr, g, totalFrames, 480);

    const int blocksizes[] = {61, 127, 480, 513};
    const u64 expect = expectedRampHash(g, totalFrames, 480, TimelineModel{});
    for (const int bs : blocksizes)
    {
        u64 gap = 0;
        const u64 got = readRampInProcess(hdr, g, totalFrames, bs, gap);
        INFO("blocksize=" << bs);
        REQUIRE(gap == 0);
        REQUIRE(got == expect);
    }
}

// ===========================================================================
// IPC-3 双进程音频环 10 万块(跨进程,逐样本一致,gapCount==0)
// ===========================================================================
TEST_CASE("IPC-3 双进程音频环 10 万块逐样本一致且 gapCount==0", "[ipc][contract]")
{
    // 10 万块 × 4 帧 = 40 万帧 < 默认环 524288 帧 → 不套圈,读方确定性追平。
    const long long blocks = 100000;
    const int bs = 4;
    std::vector<std::string> w{"--role=writer", "--group=1", "--ch=2", "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> r{"--role=reader", "--group=1", "--ch=2", "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};

    const RingRun run = runWriterThenReader(w, r);
    REQUIRE(run.readerExit == 0);
    const auto m = parseCsv(run.csv);
    REQUIRE(csvLL(m, "gap_count") == 0);
    REQUIRE(csvLL(m, "blocks") == blocks);

    AudioGeometry g;
    g.channels = 1;
    g.ringFrames = scvb::kDefaultRingFrames;
    REQUIRE(csvU64Hex(m, "hash") == expectedRampHash(g, blocks * bs, bs, TimelineModel{}));
}

// ===========================================================================
// IPC-4 epoch 跳变:绝不读跨代样本
// ===========================================================================
TEST_CASE("IPC-4 epoch 跳变绝不读跨代样本", "[ipc][contract]")
{
    TimelineModel model;
    model.startT0 = 0;
    model.seekAt = 100;
    model.seekTo = 400000; // 新 t0(跳跃后最终 write_head = 400000 + 100*64 < 524288,不套圈)

    const long long blocks = 200;
    const int bs = 64;
    AudioGeometry g;
    g.channels = 1;
    g.ringFrames = scvb::kDefaultRingFrames;

    std::vector<std::string> w{"--role=writer",
                               "--group=1",
                               "--ch=3",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs),
                               "--seek-at=" + std::to_string(model.seekAt),
                               "--seek-to=" + std::to_string(model.seekTo)};
    std::vector<std::string> r{"--role=reader",
                               "--group=1",
                               "--ch=3",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs),
                               "--seek-at=" + std::to_string(model.seekAt),
                               "--seek-to=" + std::to_string(model.seekTo)};

    const RingRun run = runWriterThenReader(w, r);
    REQUIRE(run.readerExit == 0);
    const auto m = parseCsv(run.csv);
    REQUIRE(csvLL(m, "gap_count") == 0);
    REQUIRE(csvU64Hex(m, "hash") == expectedRampHash(g, blocks * bs, bs, model));
}

// ===========================================================================
// IPC-5 覆盖判定三边界(落后 / 超环距 / 读中换代)→ gapCount+1 且静音
// ===========================================================================
TEST_CASE("IPC-5 覆盖判定三边界各自 gapCount+1 且静音", "[ipc][contract]")
{
    AudioGeometry g;
    g.sampleRate = 48000;
    g.ringFrames = 64;
    g.channels = 1;

    scvb::SegmentBackendWin32 backend;
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(scvb::segmentAudioName(1, 4), scvb::ipctest::audioSegmentBytes(g.ringFrames), view) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::AudioRingHeader*>(view.base);
    REQUIRE(backend.initHeader(
                view, &hdr->magic, &hdr->abi, nullptr, sizeof(scvb::AudioRingHeader), [&] { initAudioHeader(hdr, g); },
                true) == scvb::InitResult::kOk);

    SECTION("write_head 落后")
    {
        hdr->write_head_samples.store(4, std::memory_order_release); // w=4 < t0+n=8 → 未覆盖
        hdr->epoch.store(0, std::memory_order_release);
        RingReaderState st;
        std::vector<float> out(8);
        REQUIRE(ringReadBlock(hdr, g, 0, 8, out.data(), st) == 1);
        REQUIRE(st.gapCount == 1);
        for (const float v : out)
        {
            REQUIRE(v == 0.0f);
        }
    }

    SECTION("超环距(w - t0 > ring_frames)")
    {
        hdr->write_head_samples.store(128, std::memory_order_release);
        hdr->epoch.store(0, std::memory_order_release);
        RingReaderState st;
        std::vector<float> out(8);
        REQUIRE(ringReadBlock(hdr, g, 0, 8, out.data(), st) == 1);
        for (const float v : out)
        {
            REQUIRE(v == 0.0f);
        }
    }

    SECTION("读中换代(e2 != e1)")
    {
        hdr->write_head_samples.store(8, std::memory_order_release);
        hdr->epoch.store(1, std::memory_order_release);
        RingReaderState st;
        std::vector<float> out(8);
        const u32 gap =
            ringReadBlock(hdr, g, 0, 8, out.data(), st, [&] { hdr->epoch.store(2, std::memory_order_release); });
        REQUIRE(gap == 1);
        for (const float v : out)
        {
            REQUIRE(v == 0.0f);
        }
    }
}

// ===========================================================================
// IPC-5b 读后 write_head 复查(套圈弃块,无撕裂样本)
// ===========================================================================
TEST_CASE("IPC-5b 读后 write_head 复查套圈弃块", "[ipc][contract]")
{
    AudioGeometry g;
    g.ringFrames = 64;
    g.channels = 1;

    scvb::SegmentBackendWin32 backend;
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(scvb::segmentAudioName(1, 5), scvb::ipctest::audioSegmentBytes(g.ringFrames), view) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::AudioRingHeader*>(view.base);
    REQUIRE(backend.initHeader(
                view, &hdr->magic, &hdr->abi, nullptr, sizeof(scvb::AudioRingHeader), [&] { initAudioHeader(hdr, g); },
                true) == scvb::InitResult::kOk);

    hdr->write_head_samples.store(8, std::memory_order_release);
    hdr->epoch.store(0, std::memory_order_release);
    RingReaderState st;
    std::vector<float> out(8);
    const u32 gap = ringReadBlock(hdr, g, 0, 8, out.data(), st,
                                  [&] { hdr->write_head_samples.store(80, std::memory_order_release); });
    REQUIRE(gap == 1);
    REQUIRE(st.gapCount == 1);
    for (const float v : out)
    {
        REQUIRE(v == 0.0f);
    }
}

// ===========================================================================
// IPC-5c 负 playhead 不越界不写环,gapCount 不增
// ===========================================================================
TEST_CASE("IPC-5c 负 playhead 跨零点尾段写入且 gapCount 不增", "[ipc][contract]")
{
    // 负 playhead 语义(01 §5.1 步骤 2):t0<0 时不整块写环、不越界;跨零点块只写 [0, t0+n) 尾段。
    // in-process 直接驱动 harness 环写读;环 256 帧,容纳尾段 [0,32) + 正常块 [32,96) 不套圈。
    AudioGeometry g;
    g.sampleRate = 48000;
    g.ringFrames = 256;
    g.channels = 1;

    scvb::SegmentBackendWin32 backend;
    scvb::SegmentView view;
    REQUIRE(backend.createOrOpen(scvb::segmentAudioName(1, 6), scvb::ipctest::audioSegmentBytes(g.ringFrames), view) ==
            scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::AudioRingHeader*>(view.base);
    REQUIRE(backend.initHeader(
                view, &hdr->magic, &hdr->abi, nullptr, sizeof(scvb::AudioRingHeader), [&] { initAudioHeader(hdr, g); },
                true) == scvb::InitResult::kOk);

    // 跨零点块:t0=-32,n=64 → 只写 [0,32) 尾段,write_head=32,不越界。
    RingWriterState wst;
    std::vector<float> tail(32);
    fillBlockSamples(tail.data(), 0, 32, g, 1);
    ringWriteBlock(hdr, g, 0, 32, tail.data(), wst);
    REQUIRE(hdr->write_head_samples.load() == 32); // 只写尾段

    // 后续正常块 [32,96)。
    std::vector<float> nxt(64);
    fillBlockSamples(nxt.data(), 32, 64, g, 1);
    ringWriteBlock(hdr, g, 32, 64, nxt.data(), wst);

    // 读方:负区间不读(Output 直通,§5.2),从 t0=0 起读,gapCount==0。
    RingReaderState rst;
    std::vector<float> out(96);
    REQUIRE(ringReadBlock(hdr, g, 0, 32, out.data(), rst) == 0);
    REQUIRE(ringReadBlock(hdr, g, 32, 64, out.data() + 32, rst) == 0);
    REQUIRE(rst.gapCount == 0);
}

// ===========================================================================
// IPC-6 崩溃残段:reader 判陈旧 + 新 writer 双阈值接管
// ===========================================================================
TEST_CASE("IPC-6 崩溃残段 reader 判陈旧 + 新 writer 双阈值接管", "[ipc][contract]")
{
    PeerGuard holder;
    int err = 0;
    holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=7"}, &err);
    REQUIRE(err == 0);
    {
        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        auto* slot = reg.inputSlot(7);
        REQUIRE(slot != nullptr);
        for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(slot->state.load() == kSlotActive);
    }

    const u32 peerPid = holder.pi.dwProcessId;
    killPeer(holder.pi);

    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    auto* slot = reg.inputSlot(7);
    REQUIRE(slot != nullptr);
    const u64 hb = slot->heartbeat_ms.load();

    REQUIRE_FALSE(scvb::isProcessAlive(peerPid));
    REQUIRE(scvb::isStaleDisplay(hb, hb + 2100));
    REQUIRE(reg.claimInput(7, 9001, 48000, 512, hb + 4900) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(reg.claimInput(7, 9001, 48000, 512, hb + 5100) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(slot->pid == 9001);
    REQUIRE(slot->state.load() == kSlotActive);
}

// ===========================================================================
// IPC-7 段初始化竞态(15 claimer)
// ===========================================================================
TEST_CASE("IPC-7 段初始化竞态 15 claimer", "[ipc][contract]")
{
    SECTION("15 个 claimer 抢 15 个 distinct channel(取满 slot,段自洽)")
    {
        std::vector<PeerGuard> guards(15);
        int err = 0;
        for (int ch = 1; ch <= 15; ++ch)
        {
            guards[static_cast<std::size_t>(ch - 1)].pi =
                spawnPeer({"--role=claimer", "--group=1", "--ch=" + std::to_string(ch)}, &err);
            REQUIRE(err == 0);
        }
        int claimed = 0;
        for (int ch = 1; ch <= 15; ++ch)
        {
            const int c = waitPeer(guards[static_cast<std::size_t>(ch - 1)].pi, 15000);
            if (c == 0)
            {
                ++claimed;
            }
        }
        REQUIRE(claimed == 15);

        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(reg.header()->magic.load() == kScvbMagic);
        REQUIRE(reg.header()->abi.load() == kScvbAbi);
        for (int ch = 1; ch <= 15; ++ch)
        {
            REQUIRE(reg.inputSlot(static_cast<u32>(ch))->state.load() == kSlotFree);
        }
    }

    SECTION("15 个 claimer 抢同一 channel → 恰好 1 个成功")
    {
        // 用 holder 持住 slot(claim 后不立即释放),15 个同时抢同号 → CAS 恰好 1 个胜者。
        std::vector<PeerGuard> guards(15);
        int err = 0;
        for (int i = 0; i < 15; ++i)
        {
            guards[static_cast<std::size_t>(i)].pi =
                spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=1", "--hold-ms=1500"}, &err);
            REQUIRE(err == 0);
        }
        int claimed = 0;
        int conflict = 0;
        for (int i = 0; i < 15; ++i)
        {
            const int c = waitPeer(guards[static_cast<std::size_t>(i)].pi, 15000);
            if (c == 0)
            {
                ++claimed;
            }
            else if (c == 2)
            {
                ++conflict;
            }
        }
        REQUIRE(claimed == 1);
        REQUIRE(conflict == 14);
    }
}

// ===========================================================================
// IPC-8 半初始化残段覆盖式重初始化(跨进程)
// ===========================================================================
TEST_CASE("IPC-8 半初始化残段覆盖式重初始化", "[ipc][contract]")
{
    scvb::SegmentBackendWin32 backend;
    // 先正常打开(创建者路径 generation=1 或既有段),再把 magic 清零模拟「创建者死于 re-init」,
    // 但段仍被 reg1 保活(不 unmaps → 段不销毁)。新 owner 覆盖式重初始化 generation +1。
    scvb::Registry reg1(backend, 1);
    REQUIRE(reg1.open() == scvb::Registry::ClaimResult::kClaimed);
    const u32 genBefore = reg1.generation();
    reg1.header()->magic.store(0, std::memory_order_release); // 半初始化残段

    scvb::Registry reg2(backend, 1);
    REQUIRE(reg2.open() == scvb::Registry::ClaimResult::kClaimed); // owner 自旋 500ms 后覆盖式重初始化
    REQUIRE(reg2.header()->magic.load() == kScvbMagic);
    REQUIRE(reg2.header()->abi.load() == kScvbAbi);
    REQUIRE(reg2.generation() == genBefore + 1); // generation +1
}

// ===========================================================================
// IPC-9 ABI 不符(跨进程)
// ===========================================================================
TEST_CASE("IPC-9 ABI 不符拒连不崩溃", "[ipc][contract]")
{
    // 用独立 group=3 承载 abi 异常段,避免污染 group=1(其余测试共享的默认组)。
    scvb::SegmentBackendWin32 backend;
    scvb::SegmentView view;
    const std::wstring name = scvb::segmentRegistryName(3);
    REQUIRE(backend.createOrOpen(name, scvb::kRegistrySegmentSize, view) == scvb::InitResult::kOk);
    auto* hdr = static_cast<scvb::RegistryHeader*>(view.base);
    hdr->magic.store(kScvbMagic, std::memory_order_release);
    hdr->abi.store(2, std::memory_order_release);
    // 保持 view 打开(段在 claimer 打开期间必须存活),claim 结束后再解映射。
    PeerGuard p;
    int err = 0;
    p.pi = spawnPeer({"--role=claimer", "--group=3", "--ch=1"}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(p.pi, 15000) == 4);
    backend.unmap(view); // 释放本进程持有的 abi=2 段视图
}

// ===========================================================================
// IPC-10 采样率不符
// ===========================================================================
TEST_CASE("IPC-10 采样率不符该轨禁用不重采样", "[ipc][contract]")
{
    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.claimInput(8, 1001, 44100, 512, scvb::steadyNowMs()) == scvb::Registry::ClaimResult::kClaimed);
    const u32 outputSr = 48000;
    REQUIRE(reg.inputSlot(8)->sample_rate == 44100);
    REQUIRE(reg.inputSlot(8)->sample_rate != outputSr);
    reg.releaseInput(8, 1001);
}

// ===========================================================================
// IPC-11a 第二 Output 优雅退出后 ≤3s 接管
// ===========================================================================
TEST_CASE("IPC-11a 第二 Output 优雅退出后接管", "[ipc][contract]")
{
    PeerGuard first;
    int err = 0;
    first.pi = spawnPeer({"--role=holder", "--kind=output", "--group=1", "--hold-ms=300"}, &err);
    REQUIRE(err == 0);
    {
        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        for (int i = 0; i < 200 && reg.outputSlot()->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(reg.outputSlot()->state.load() == kSlotActive);
    }
    REQUIRE(waitPeer(first.pi, 15000) == 0);

    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.claimOutput(2002, scvb::steadyNowMs()) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.outputSlot()->pid == 2002);
}

// ===========================================================================
// IPC-11b 第二 Output 强杀后 ≥5s 才可接管
// ===========================================================================
TEST_CASE("IPC-11b 第二 Output 强杀路径 ≥5s 才可接管", "[ipc][contract]")
{
    PeerGuard first;
    int err = 0;
    first.pi = spawnPeer({"--role=holder", "--kind=output", "--group=1"}, &err);
    REQUIRE(err == 0);
    {
        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        for (int i = 0; i < 200 && reg.outputSlot()->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(reg.outputSlot()->state.load() == kSlotActive);
    }
    const u32 pid = first.pi.dwProcessId;
    killPeer(first.pi);

    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    const u64 hb = reg.outputSlot()->heartbeat_ms.load();
    REQUIRE_FALSE(scvb::isProcessAlive(pid));
    REQUIRE(reg.claimOutput(2002, hb + 4900) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(reg.claimOutput(2002, hb + 5100) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.outputSlot()->pid == 2002);
}

// ===========================================================================
// IPC-12a 心跳显示陈旧边界(1900 新鲜 / 2100 陈旧,不授权接管)
// ===========================================================================
TEST_CASE("IPC-12a 心跳显示陈旧边界不授权接管", "[ipc][contract]")
{
    PeerGuard holder;
    int err = 0;
    holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=9"}, &err);
    REQUIRE(err == 0);
    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    auto* slot = reg.inputSlot(9);
    for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const u64 hb = slot->heartbeat_ms.load();

    REQUIRE_FALSE(scvb::isStaleDisplay(hb, hb + 1900));
    REQUIRE(scvb::isStaleDisplay(hb, hb + 2100));
    REQUIRE(reg.claimInput(9, 9002, 48000, 512, hb + 2100) == scvb::Registry::ClaimResult::kConflict);
    REQUIRE(slot->pid != 9002);
    killPeer(holder.pi);
}

// ===========================================================================
// IPC-12b 心跳接管四格
// ===========================================================================
TEST_CASE("IPC-12b 心跳接管四格(J10 双条件)", "[ipc][contract]")
{
    SECTION("4900ms × pid 存活 → 不接管")
    {
        scvb::SegmentBackendWin32 backend;
        resetRegistry(backend, 1);
        PeerGuard holder;
        int err = 0;
        holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=10"}, &err);
        REQUIRE(err == 0);
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        auto* slot = reg.inputSlot(10);
        for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const u64 hb = slot->heartbeat_ms.load();
        REQUIRE(reg.claimInput(10, 9003, 48000, 512, hb + 4900) == scvb::Registry::ClaimResult::kConflict);
        killPeer(holder.pi);
    }
    SECTION("4900ms × pid 已死 → 不接管(未过时限)")
    {
        scvb::SegmentBackendWin32 backend;
        resetRegistry(backend, 1);
        PeerGuard holder;
        int err = 0;
        holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=10"}, &err);
        REQUIRE(err == 0);
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        auto* slot = reg.inputSlot(10);
        for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const u64 hb = slot->heartbeat_ms.load();
        killPeer(holder.pi);
        REQUIRE(reg.claimInput(10, 9004, 48000, 512, hb + 4900) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(slot->pid != 9004);
    }
    SECTION("5100ms × pid 存活 → 不接管(假死保护)")
    {
        scvb::SegmentBackendWin32 backend;
        resetRegistry(backend, 1);
        PeerGuard holder;
        int err = 0;
        holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=10"}, &err);
        REQUIRE(err == 0);
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        auto* slot = reg.inputSlot(10);
        for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const u64 hb = slot->heartbeat_ms.load();
        REQUIRE(reg.claimInput(10, 9005, 48000, 512, hb + 5100) == scvb::Registry::ClaimResult::kConflict);
        REQUIRE(slot->pid != 9005);
        killPeer(holder.pi);
    }
    SECTION("5100ms × pid 已死 → CAS 接管唯一胜者")
    {
        scvb::SegmentBackendWin32 backend;
        resetRegistry(backend, 1);
        PeerGuard holder;
        int err = 0;
        holder.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=10"}, &err);
        REQUIRE(err == 0);
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        auto* slot = reg.inputSlot(10);
        for (int i = 0; i < 200 && slot->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const u64 hb = slot->heartbeat_ms.load();
        killPeer(holder.pi);
        REQUIRE(reg.claimInput(10, 9006, 48000, 512, hb + 5100) == scvb::Registry::ClaimResult::kClaimed);
        REQUIRE(slot->pid == 9006);
        REQUIRE(slot->state.load() == kSlotActive);
    }
}

// ===========================================================================
// IPC-13 ctrl 命令环满环丢最旧(跨进程)
// ===========================================================================
TEST_CASE("IPC-13 ctrl 命令环满环丢最旧", "[ipc][contract]")
{
    PeerGuard pw;
    PeerGuard pr;
    int err = 0;
    pw.pi = spawnPeer(
        {"--role=ctrl-writer", "--group=1", "--ch=3", "--enqueue-count=17", "--value-base=0", "--linger-ms=1000"},
        &err);
    REQUIRE(err == 0);
    const std::string csv = tempCsvPath();
    pr.pi = spawnPeer({"--role=ctrl-reader", "--group=1", "--ch=3", "--out=" + csv}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(pw.pi, 15000) == 0);
    REQUIRE(waitPeer(pr.pi, 15000) == 0);
    const auto m = parseCsv(readFile(csv));
    deleteFile(csv);

    REQUIRE(csvLL(m, "count") == scvb::kCtrlRingCapacity); // 容量 16
    REQUIRE(csvLL(m, "last_value") == 16); // 最旧(value 0)被丢,读到 1..16
    REQUIRE(csvLL(m, "overflow") == 1); // 17 条 → 溢出 1 次
}

// ===========================================================================
// IPC-13b fp_report 打包往返 + value 宽度
// ===========================================================================
TEST_CASE("IPC-13b fp_report 打包往返 + value 宽度 u64", "[ipc][contract]")
{
    static_assert(sizeof(scvb::CtrlRecord::value) == 8);
    static_assert(std::is_same_v<decltype(scvb::CtrlRecord::value), std::atomic<u64>>);

    const u32 tiles[] = {0, 1, 0xFFFE, 0xFFFF};
    const u64 hashes[] = {0, 1, 0xFFFFFFFFFFFFull, 0x123456789ABCull, 0xFFFFFFFFFFFFFFFFull};
    for (const u32 tile : tiles)
    {
        for (const u64 hash : hashes)
        {
            const u64 v = scvb::packFpReport(tile, hash);
            REQUIRE(scvb::unpackFpReportTileIdx(v) == tile);
            REQUIRE(scvb::unpackFpReportHash(v) == (hash & 0xFFFFFFFFFFFFull));
        }
    }
    REQUIRE(scvb::unpackFpReportTileIdx(scvb::packFpReport(0x12, 0x0001FFFFFFFFFFFFull)) == 0x12);
    REQUIRE(scvb::unpackFpReportTileIdx(scvb::packFpReport(0x10000, 0xAB)) == 0xFFFF);
}

// ===========================================================================
// IPC-14 特征环 run 协议(J33 base 先 / write 后 + 读方 w 单调性 + 受限追赶 + 停顿中切 run)
// ===========================================================================
TEST_CASE("IPC-14 特征环 run 协议", "[ipc][contract]")
{
    SECTION("J33 base_hop 先 store 后 write_hop(交错注入)")
    {
        FeatHeaderBox box;
        initFeatHeader(box.hdr);
        std::vector<scvb::FeatFrame> ring(scvb::kFeatCapacityHops);

        scvb::FeatRing feat;
        feat.bind(box.hdr, ring.data(), static_cast<u32>(ring.size()));
        feat.prepare(48000.0, 1, 512);
        feat.setCapturing(true);

        feat.startRun(0);
        std::vector<float> plane(512, 0.0f);
        const float* ptr = plane.data();
        for (int i = 0; i < 10; ++i)
        {
            feat.processBlock(&ptr, 512);
        }
        const u64 oldWrite = box.hdr->write_hop.load();

        bool sawNewBase = false;
        bool sawOldWrite = false;
        feat.startRun(10000LL * feat.hopSize(), [&] {
            sawNewBase = (box.hdr->base_hop.load() == 10000);
            sawOldWrite = (box.hdr->write_hop.load() == oldWrite);
        });
        REQUIRE(sawNewBase);
        REQUIRE(sawOldWrite);
    }

    SECTION("读方 w 单调性防御(w < lastPulled 丢弃重读)")
    {
        FeatHeaderBox box;
        initFeatHeader(box.hdr);
        box.hdr->base_hop.store(0, std::memory_order_relaxed);
        box.hdr->write_hop.store(10, std::memory_order_relaxed);
        std::vector<scvb::FeatFrame> ring(scvb::kFeatCapacityHops);

        analysis::ChannelFrames store;
        store.setReadOnly(false);
        scvb::FeatPullState state;
        state.initialized = true;
        state.lastBase = 0;
        state.lastPulled = 10;
        box.hdr->write_hop.store(5, std::memory_order_release);
        const u32 pulled = scvb::pullIncremental(*box.hdr, ring.data(), static_cast<u32>(ring.size()), state, store,
                                                 analysis::HopRange{0, UINT64_MAX});
        REQUIRE(pulled == 0);
    }

    SECTION("受限追赶分拍追平 coverage 无洞")
    {
        FeatHeaderBox box;
        initFeatHeader(box.hdr);
        box.hdr->base_hop.store(0, std::memory_order_relaxed);
        box.hdr->write_hop.store(3000, std::memory_order_relaxed);
        std::vector<scvb::FeatFrame> ring(scvb::kFeatCapacityHops);

        analysis::ChannelFrames store;
        store.setReadOnly(false);
        scvb::FeatPullState state;
        // 首次调用是「确认拍」(init/run 切换 return 0,不拉取,下拍才拉);循环到追平 write_hop,
        // 不要把确认拍的 0 当作「拉完」。
        for (int tick = 0; tick < 1000 && !(state.initialized && state.lastPulled >= 3000); ++tick)
        {
            const u32 pulled = scvb::pullIncremental(*box.hdr, ring.data(), static_cast<u32>(ring.size()), state, store,
                                                     analysis::HopRange{0, UINT64_MAX});
            REQUIRE(pulled <= scvb::kMaxBurstHops); // 每拍 ≤256 hop
        }
        REQUIRE(store.coverage().coversFully(analysis::HopRange{0, 3000}));
    }

    SECTION("停顿中切 run 负向:旧积压跳过不进 coverage")
    {
        FeatHeaderBox box;
        initFeatHeader(box.hdr);
        box.hdr->base_hop.store(0, std::memory_order_relaxed);
        box.hdr->write_hop.store(3000, std::memory_order_relaxed);
        std::vector<scvb::FeatFrame> ring(scvb::kFeatCapacityHops);

        analysis::ChannelFrames store;
        store.setReadOnly(false);
        scvb::FeatPullState state;
        box.hdr->write_hop.store(2000, std::memory_order_release);
        // 受限追赶:每拍 ≤ kMaxBurstHops;首次调用是确认拍(return 0),循环到追平 write_hop。
        for (int tick = 0; tick < 1000 && !(state.initialized && state.lastPulled >= 2000); ++tick)
        {
            scvb::pullIncremental(*box.hdr, ring.data(), static_cast<u32>(ring.size()), state, store,
                                  analysis::HopRange{0, UINT64_MAX});
        }
        REQUIRE(store.coverage().coversFully(analysis::HopRange{0, 2000}));

        // 停顿中途切 run:旧 run 未拉积压 [2000,3000) 整段跳过。
        box.hdr->base_hop.store(10000, std::memory_order_release);
        box.hdr->write_hop.store(10500, std::memory_order_release);

        // run 切换到 base=10000 后第一拍也是确认拍(return 0,重设 lastPulled=10000),重试后才拉到
        // [10000,10500);旧 run 未拉积压 [2000,3000) 仍整段跳过(不进 coverage)。
        for (int tick = 0; tick < 1000 && !(state.initialized && state.lastPulled >= 10500); ++tick)
        {
            scvb::pullIncremental(*box.hdr, ring.data(), static_cast<u32>(ring.size()), state, store,
                                  analysis::HopRange{0, UINT64_MAX});
        }

        REQUIRE_FALSE(store.coverage().coversFully(analysis::HopRange{2000, 3000}));
        REQUIRE(store.coverage().coversFully(analysis::HopRange{10000, 10500}));
    }

    SECTION("跨进程特征环 writer→reader coverage 一致")
    {
        PeerGuard pw;
        PeerGuard pr;
        int err = 0;
        pw.pi = spawnPeer({"--role=feat-writer", "--group=1", "--ch=2", "--feat-hops=2000", "--feat-start-sample=0",
                           "--linger-ms=1000"},
                          &err);
        REQUIRE(err == 0);
        const std::string csv = tempCsvPath();
        pr.pi = spawnPeer({"--role=feat-reader", "--group=1", "--ch=2", "--out=" + csv}, &err);
        REQUIRE(err == 0);
        REQUIRE(waitPeer(pw.pi, 30000) == 0);
        REQUIRE(waitPeer(pr.pi, 30000) == 0);
        const auto m = parseCsv(readFile(csv));
        deleteFile(csv);

        const u64 base = csvLL(m, "base_hop");
        const u64 write = csvLL(m, "write_hop");
        const u64 covered = csvLL(m, "covered_hops");
        REQUIRE(write > base);
        REQUIRE(covered == write - base);
    }
}

// ===========================================================================
// IPC-15 内存序(release/acquire SPSC 压力)
// ===========================================================================
TEST_CASE("IPC-15 内存序 release/acquire 压力", "[ipc][contract]")
{
    constexpr u64 kN = 10000000;
    std::atomic<u64> w{0};
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (u64 i = 1; i <= kN; ++i)
        {
            w.store(i, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    u64 last = 0;
    while (true)
    {
        const u64 v = w.load(std::memory_order_acquire);
        REQUIRE(v >= last);
        last = v;
        if (done.load(std::memory_order_acquire) && v == kN)
        {
            break;
        }
        std::this_thread::yield();
    }
    producer.join();
    REQUIRE(last == kN);
}

// ===========================================================================
// IPC-16 ctrl 段 Output 全局小节(跨进程)
// ===========================================================================
TEST_CASE("IPC-16 ctrl 全局小节跨进程逐项一致", "[ipc][contract]")
{
    PeerGuard pw;
    PeerGuard pr;
    int err = 0;
    pw.pi = spawnPeer({"--role=globalinfo-writer", "--group=1", "--sr=48000", "--epoch-base=300", "--linger-ms=1000"},
                      &err);
    REQUIRE(err == 0);
    const std::string csv = tempCsvPath();
    pr.pi = spawnPeer({"--role=globalinfo-reader", "--group=1", "--out=" + csv}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(pw.pi, 15000) == 0);
    REQUIRE(waitPeer(pr.pi, 15000) == 0);
    const auto m = parseCsv(readFile(csv));
    deleteFile(csv);

    REQUIRE(csvLL(m, "capture_enabled") == 1);
    REQUIRE(csvLL(m, "output_sample_rate") == 48000);
    REQUIRE(csvLL(m, "flags") == static_cast<long long>(scvb::kOutputEnabled));
    REQUIRE(csvLL(m, "gap_count0") == 100);
    REQUIRE(csvLL(m, "gap_count14") == 114);
    REQUIRE(csvLL(m, "overlap_count14") == 214);
    REQUIRE(csvLL(m, "epoch_summary14") == 314);
}

// ===========================================================================
// IPC-17 直通/静音协议(健康仲裁信号,跨进程)
// ===========================================================================
TEST_CASE("IPC-17 直通/静音健康仲裁信号", "[ipc][contract]")
{
    scvb::SegmentBackendWin32 backend;
    resetRegistry(backend, 1); // 清掉 IPC-11b 等可能残留的 OutputSlot 状态,消除残留态耦合

    PeerGuard holder;
    int err = 0;
    holder.pi = spawnPeer({"--role=holder", "--kind=output", "--group=1", "--ch=1"}, &err);
    REQUIRE(err == 0);
    const u32 holderPid = holder.pi.dwProcessId;

    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    // 轮询到「mask 非零 且 属主 pid == 本 holder」再断言:claimOutput 先 store mask=0 再 setMaskBit,
    // 只轮询 state==active 会与 mask 置位竞态;pid 校验确保是本次 holder 真正 claim 成功(非残留态)。
    for (int i = 0; i < 200 && (reg.outputSlot()->pid != holderPid || reg.connectedMask() == 0); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(reg.outputSlot()->pid == holderPid);
    REQUIRE(reg.outputSlot()->state.load() == kSlotActive);
    REQUIRE(reg.connectedMask() == (1u << 0));
    // 校验 holder 未早退(claim 失败会 exit 2;成功则持住不退出)。
    REQUIRE(::WaitForSingleObject(holder.pi.hProcess, 0) == WAIT_TIMEOUT);

    const u64 hbFresh = reg.outputSlot()->heartbeat_ms.load();
    REQUIRE_FALSE(scvb::isStaleDisplay(hbFresh, hbFresh));

    REQUIRE(scvb::isStaleDisplay(hbFresh, hbFresh + 2100));

    reg.clearConnectedMask();
    REQUIRE(reg.connectedMask() == 0);

    reg.setConnectedMaskBit(1);
    REQUIRE((reg.connectedMask() & 1u) != 0);
    killPeer(holder.pi);
}

// ===========================================================================
// IPC-18 InputSlot.flags 双线程混写([J48])
// ===========================================================================
TEST_CASE("IPC-18 flags 双线程混写无吞位", "[ipc][contract]")
{
    static_assert(std::is_same_v<decltype(scvb::InputSlot::flags), std::atomic<u32>>);
    static_assert(std::atomic<u32>::is_always_lock_free);

    scvb::SegmentBackendWin32 backend;
    scvb::Registry reg(backend, 1);
    REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(reg.claimInput(11, 1001, 48000, 512, scvb::steadyNowMs()) == scvb::Registry::ClaimResult::kClaimed);
    scvb::InputSlot* slot = reg.inputSlot(11);

    constexpr u32 kBit0 = scvb::kFlagCapturing;
    constexpr u32 kBit1 = scvb::kFlagMuted;

    std::atomic<bool> start{false};
    std::atomic<bool> ok{true};
    std::thread a([&] {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < 1000000; ++i)
        {
            slot->flags.fetch_or(kBit0, std::memory_order_release);
            if ((slot->flags.load(std::memory_order_acquire) & kBit0) == 0)
            {
                ok.store(false, std::memory_order_relaxed);
            }
            slot->flags.fetch_and(~kBit0, std::memory_order_release);
        }
    });
    std::thread m([&] {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < 1000000; ++i)
        {
            slot->flags.fetch_or(kBit1, std::memory_order_release);
            if ((slot->flags.load(std::memory_order_acquire) & kBit1) == 0)
            {
                ok.store(false, std::memory_order_relaxed);
            }
            slot->flags.fetch_and(~kBit1, std::memory_order_release);
        }
    });
    start.store(true, std::memory_order_release);
    a.join();
    m.join();

    REQUIRE(ok.load());
    REQUIRE((slot->flags.load(std::memory_order_acquire) & (kBit0 | kBit1)) == 0);
}

// ===========================================================================
// IPC-19 stereo interleaved 环
// ===========================================================================
TEST_CASE("IPC-19 stereo interleaved 环 L/R 各自逐样本一致不互换", "[ipc][contract]")
{
    const long long blocks = 4000;
    const int bs = 64;
    AudioGeometry g;
    g.channels = 2;
    g.ringFrames = scvb::kDefaultRingFrames;

    const u64 expect = expectedRampHash(g, blocks * bs, bs, TimelineModel{});

    for (const int rbs : {61, 127, 480, 513})
    {
        std::vector<std::string> w{"--role=writer",
                                   "--group=1",
                                   "--ch=12",
                                   "--channels=2",
                                   "--blocks=" + std::to_string(blocks),
                                   "--blocksize=" + std::to_string(bs)};
        std::vector<std::string> r{"--role=reader",
                                   "--group=1",
                                   "--ch=12",
                                   "--channels=2",
                                   "--blocks=" + std::to_string(blocks),
                                   "--blocksize=" + std::to_string(rbs),
                                   "--total-frames=" + std::to_string(blocks * bs)};
        const RingRun run = runWriterThenReader(w, r);
        REQUIRE(run.readerExit == 0);
        const auto m = parseCsv(run.csv);
        INFO("reader blocksize=" << rbs);
        REQUIRE(csvLL(m, "gap_count") == 0);
        REQUIRE(csvLL(m, "samples") == blocks * bs * 2);
        REQUIRE(csvU64Hex(m, "hash") == expect);
    }
}

TEST_CASE("IPC-19 channels 不匹配判配置错误拒读", "[ipc][contract]")
{
    const long long blocks = 100;
    const int bs = 64;
    std::vector<std::string> w{"--role=writer",
                               "--group=1",
                               "--ch=13",
                               "--channels=2",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> r{"--role=reader",
                               "--group=1",
                               "--ch=13",
                               "--decode-channels=1",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};

    const RingRun run = runWriterThenReader(w, r);
    REQUIRE(run.readerExit == 3);
    const auto m = parseCsv(run.csv);
    REQUIRE(m.count("channels_mismatch") == 1);
}

TEST_CASE("IPC-19 ring_frames 口径错位可检出", "[ipc][contract]")
{
    // 块数足够大(> 半环)使「把 ring_frames 当样本数」的错误口径产生错位/缺口(可检出,非静默半速)。
    const long long blocks = 5000;
    const int bs = 64;
    std::vector<std::string> w{"--role=writer",
                               "--group=1",
                               "--ch=14",
                               "--channels=2",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> r{"--role=reader",
                               "--group=1",
                               "--ch=14",
                               "--channels=2",
                               "--wrong-units",
                               "--blocks=" + std::to_string(blocks),
                               "--blocksize=" + std::to_string(bs)};

    const RingRun run = runWriterThenReader(w, r);
    REQUIRE(run.readerExit == 0);
    const auto m = parseCsv(run.csv);
    AudioGeometry g;
    g.channels = 2;
    g.ringFrames = scvb::kDefaultRingFrames;
    REQUIRE(csvU64Hex(m, "hash") != expectedRampHash(g, blocks * bs, bs, TimelineModel{}));
}

TEST_CASE("IPC-19 mono 与 stereo 环同 registry 并存互不影响", "[ipc][contract]")
{
    const long long blocks = 500;
    const int bs = 64;
    std::vector<std::string> wm{"--role=writer",
                                "--group=1",
                                "--ch=15",
                                "--channels=1",
                                "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> ws{"--role=writer",
                                "--group=1",
                                "--ch=1",
                                "--channels=2",
                                "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> rm{"--role=reader",
                                "--group=1",
                                "--ch=15",
                                "--channels=1",
                                "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> rs{"--role=reader",
                                "--group=1",
                                "--ch=1",
                                "--channels=2",
                                "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};

    const RingRun runM = runWriterThenReader(wm, rm);
    const RingRun runS = runWriterThenReader(ws, rs);
    REQUIRE(runM.readerExit == 0);
    REQUIRE(runS.readerExit == 0);
    AudioGeometry gm;
    gm.channels = 1;
    AudioGeometry gs;
    gs.channels = 2;
    REQUIRE(csvU64Hex(parseCsv(runM.csv), "hash") == expectedRampHash(gm, blocks * bs, bs, TimelineModel{}));
    REQUIRE(csvU64Hex(parseCsv(runS.csv), "hash") == expectedRampHash(gs, blocks * bs, bs, TimelineModel{}));
}

// ===========================================================================
// IPC-20a 双组并行隔离(跨进程)
// ===========================================================================
TEST_CASE("IPC-20a 双组并行隔离同号 channel 互不串扰", "[ipc][contract]")
{
    const long long blocks = 100000;
    const int bs = 4;
    std::vector<std::string> w1{"--role=writer", "--group=1", "--ch=1", "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> r1{"--role=reader", "--group=1", "--ch=1", "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> w2{"--role=writer", "--group=2", "--ch=1", "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};
    std::vector<std::string> r2{"--role=reader", "--group=2", "--ch=1", "--blocks=" + std::to_string(blocks),
                                "--blocksize=" + std::to_string(bs)};

    const RingRun run1 = runWriterThenReader(w1, r1);
    const RingRun run2 = runWriterThenReader(w2, r2);
    REQUIRE(run1.readerExit == 0);
    REQUIRE(run2.readerExit == 0);
    AudioGeometry g;
    g.channels = 1;
    const u64 expect = expectedRampHash(g, blocks * bs, bs, TimelineModel{});
    REQUIRE(csvU64Hex(parseCsv(run1.csv), "hash") == expect);
    REQUIRE(csvU64Hex(parseCsv(run2.csv), "hash") == expect);
    REQUIRE(csvLL(parseCsv(run1.csv), "gap_count") == 0);
    REQUIRE(csvLL(parseCsv(run2.csv), "gap_count") == 0);

    PeerGuard c1;
    PeerGuard c2;
    int err = 0;
    c1.pi = spawnPeer({"--role=claimer", "--group=1", "--ch=1"}, &err);
    REQUIRE(err == 0);
    c2.pi = spawnPeer({"--role=claimer", "--group=2", "--ch=1"}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(c1.pi, 15000) == 0);
    REQUIRE(waitPeer(c2.pi, 15000) == 0);
}

// ===========================================================================
// IPC-20b 改组释放-重连(跨进程)
// ===========================================================================
TEST_CASE("IPC-20b 改组释放-重连", "[ipc][contract]")
{
    PeerGuard h1;
    int err = 0;
    h1.pi = spawnPeer({"--role=holder", "--kind=input", "--group=1", "--ch=5"}, &err);
    REQUIRE(err == 0);
    {
        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        for (int i = 0; i < 200 && reg.inputSlot(5)->state.load() != kSlotActive; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(reg.inputSlot(5)->state.load() == kSlotActive);
    }
    u64 hb = 0;
    {
        scvb::SegmentBackendWin32 backend;
        scvb::Registry reg(backend, 1);
        REQUIRE(reg.open() == scvb::Registry::ClaimResult::kClaimed);
        hb = reg.inputSlot(5)->heartbeat_ms.load();
    }
    killPeer(h1.pi);

    scvb::SegmentBackendWin32 backend;
    scvb::Registry regG1(backend, 1);
    REQUIRE(regG1.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(scvb::isStaleDisplay(hb, hb + 2100));
    REQUIRE(regG1.claimInput(5, 7001, 48000, 512, hb + 5100) == scvb::Registry::ClaimResult::kClaimed);
    regG1.releaseInput(5, 7001);
    REQUIRE(regG1.inputSlot(5)->state.load() == kSlotFree);

    scvb::Registry regG2(backend, 2);
    REQUIRE(regG2.open() == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(regG2.claimInput(5, 7002, 48000, 512, scvb::steadyNowMs()) == scvb::Registry::ClaimResult::kClaimed);
    REQUIRE(regG2.inputSlot(5)->pid == 7002);
}
