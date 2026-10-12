// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_auhost.cpp —— 二进制级调度宿主(B 线 M13,由 M01 的 tests/macos/auhost-spike 演化而来)。
//
// 只通过 AudioComponent 加载**已构建、已安装**的三个真实 SCVB component(Input ×3、Output、Monitor),
// 进程内(AudioComponentInstantiate flags = 0)或进程外(kAudioComponentInstantiation_LoadOutOfProcess,
// 插件跑在 AUHostingService 里)两种模式,按 auhost_engine.h 的调度模型渲染,用 A-1 的在场判据
// (tests/support/presence_meter.h,经 auhost_judge.h)逐轨判 Output 输出上的在场 / 缺席 / 错音。
//
// 用法:
//   scvb_auhost --self-test                                     判据 + 调度模型自测(假插件,不碰 AU)
//   scvb_auhost --mode inproc|oop --scenario ls1|ls4 [--out-dir DIR] [--pace X] [--negative-control]
// 退出码:0 = 判据全过(负对照模式:负对照成立);1 = 判据不过(负对照模式:负对照不成立);
//         2 = 没测成(组件没注册 / 实例化 / 配置 / 预热失败、渲染出错);用法错也是 2。
//
// ## 宿主给每个实例的东西
//
//   · kAudioUnitProperty_HostCallbacks:每个实例一份独立的 transportState / beatAndTempo / musicalTimeLocation,
//     返回**该实例当前这一块**的时间线位置(currentSampleInTimeLine)、在放、Cycle 状态与循环点(拍)。
//   · AudioTimeStamp.mSampleTime = 该实例的渲染游标 + 一个**各实例互不相同**的大偏移(1e7 量级)。真实宿主的
//     mSampleTime 是渲染计数而不是时间线;偏移让「HostCallbacks 没传进插件、插件拿 mSampleTime 当时间线」
//     这种失效必然表现成各实例时间线对不上 ⇒ 在场判据红,而不是碰巧对齐。
//   · Input 的通道号经 kAudioUnitProperty_ClassInfo 的 jucePluginState(SCVB 自己的 state 容器 + CFGS,
//     用产品的 StateCodec / InputStateCodec 编码)在 Initialize 之前写进去,并读回核对。组号用默认 1。
//   · 输入经 kAudioUnitProperty_SetRenderCallback 供给;Input 是 mono→mono,Output / Monitor 是 stereo。
//
// ## 进程证据
//
// 插件建的 registry 段(/SynchainSCVB.v1.g1.registry,布局取自 src/core/ipc/SegmentLayout.h + Registry.h)
// 里有每个 Input / Output 实例写下的 pid(Registry::currentProcessId = 插件所在进程的 getpid)。宿主只读
// 映射它:预热判据看 Output 活跃、各 lane 的 InputSlot 活跃且 connected_mask 有这一位;判定时核
// 进程内 ⇒ pid 全等于宿主 pid,进程外 ⇒ pid 全不等于宿主 pid,并记下那个进程的名字。
//
// ## 线程
//
// 主线程:实例化 / 配置 / Initialize / 收尾,渲染期间一直转 CFRunLoop(进程内模式下 JUCE 的消息线程就是
// 宿主主线程,SCVB 的 25 Hz [M] 定时器靠它跑)。渲染在一条独立线程上(与真实宿主一样,[A] 与 [M] 真并发),
// 按墙钟实时节拍(--pace 1)推进:SCVB 有 500 ms 停滞一类的墙钟门限,实时节拍才与真实宿主同形。
//
// 运行期字符串一律 ASCII;不记录任何本机路径(只记组件名与版本、AUHostingService 的系统路径)。

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <libproc.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "auhost_engine.h"
#include "auhost_judge.h"
#include "ipc/Registry.h"
#include "ipc/SegmentLayout.h"
#include "state/InputStateCodec.h"
#include "state/StateCodec.h"

namespace
{
namespace ah = scvb::auhost;

// ADR-001 冻结的 AU 三元组(aufx / Scv? / Snch)。
constexpr const char* kManufacturer = "Snch";
constexpr const char* kSubtypeInput = "Scvi";
constexpr const char* kSubtypeOutput = "Scvo";
constexpr const char* kSubtypeMonitor = "Scvm";
constexpr std::uint32_t kGroup = 1; // Output / Input 的默认组
constexpr UInt32 kMaxFrames = 1024; // ≥ 场景里最大的块(预取 1024)
constexpr OSStatus kTimedOut = -42424; // 本工具自定义:实例化回调在超时内没回来
constexpr const char* kJuceStateKey = "jucePluginState"; // JUCE juce_AU_Shared.h 的 JUCE_STATE_DICTIONARY_KEY

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 主线程等待时转 run loop:实例化回调可能被派到主队列,进程内 JUCE 的消息也走这里。
void pump(double sec)
{
    const SInt32 r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, sec, false);
    if (r == kCFRunLoopRunFinished)
    {
        usleep(static_cast<useconds_t>(sec * 1e6));
    }
}

OSType fourcc(const char* s)
{
    return (static_cast<OSType>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<OSType>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<OSType>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<OSType>(static_cast<unsigned char>(s[3]));
}

std::string cfToString(CFStringRef s)
{
    if (s == nullptr)
    {
        return {};
    }
    std::array<char, 1024> buf{};
    if (CFStringGetCString(s, buf.data(), static_cast<CFIndex>(buf.size()), kCFStringEncodingUTF8))
    {
        return buf.data();
    }
    return {};
}

std::string sysctlString(const char* name)
{
    std::array<char, 256> buf{};
    std::size_t len = buf.size();
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0)
    {
        return {};
    }
    return std::string(buf.data());
}

bool writeFile(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// ---- 结果(key=value) ---------------------------------------------------------------------------
struct Results
{
    std::map<std::string, std::string> kv;
    void set(const std::string& k, std::string v)
    {
        for (char& c : v)
        {
            if (c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        kv[k] = std::move(v);
    }
    void seti(const std::string& k, long long v) { kv[k] = std::to_string(v); }
    std::string get(const std::string& k) const
    {
        const auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    }
    std::string asKV() const
    {
        std::string o;
        for (const auto& [k, v] : kv)
        {
            o += k + "=" + v + "\n";
        }
        return o;
    }
};

// ---- 实例 -----------------------------------------------------------------------------------------
enum class Role
{
    Input,
    Output,
    Monitor
};

struct Instance
{
    std::string key; // in0 / in1 / in2 / out / mon
    Role role = Role::Input;
    int lane = -1; // Input 的 lane 号(通道 = lane + 1)
    const char* subtype = kSubtypeInput;
    UInt32 channels = 1;
    Float64 sampleTimeOffset = 0;
    AudioComponent comp = nullptr;
    AudioComponentInstance unit = nullptr;
    bool initialized = false;

    // HostCallbacks 读这几个量(渲染线程写、回调可能在别的线程读 ⇒ 原子)。
    std::atomic<std::int64_t> timeline{0};
    std::atomic<bool> cycling{false};
    std::atomic<std::int64_t> loopB{0};
    std::atomic<std::int64_t> loopE{0};
    std::atomic<bool> playingReported{false};
    std::atomic<long long> transportCalls{0};
    std::atomic<long long> beatCalls{0};
    std::atomic<long long> inputPulls{0};
    std::atomic<long long> inputShortPulls{0};

    // 渲染输入:渲染前拷进 scratch,输入回调从这里取(插件就地处理也伤不到引擎的缓冲)。
    std::array<std::vector<float>, 2> scratch;
    UInt32 pendingFrames = 0;
    long long renders = 0;
    long long renderErrors = 0;
    OSStatus lastRenderError = noErr;
};

OSStatus hostBeatAndTempo(void* ud, Float64* outBeat, Float64* outTempo)
{
    auto* in = static_cast<Instance*>(ud);
    in->beatCalls.fetch_add(1, std::memory_order_relaxed);
    const double t = static_cast<double>(in->timeline.load(std::memory_order_acquire));
    if (outBeat != nullptr)
    {
        *outBeat = ah::samplesToBeats(t);
    }
    if (outTempo != nullptr)
    {
        *outTempo = ah::kBpm;
    }
    return noErr;
}

OSStatus hostMusicalTime(void* ud, UInt32* outDeltaToNextBeat, Float32* outNum, UInt32* outDen, Float64* outDownBeat)
{
    auto* in = static_cast<Instance*>(ud);
    const double beat = ah::samplesToBeats(static_cast<double>(in->timeline.load(std::memory_order_acquire)));
    const double samplesPerBeat = ah::kSr * 60.0 / ah::kBpm;
    if (outDeltaToNextBeat != nullptr)
    {
        const double frac = beat - std::floor(beat);
        *outDeltaToNextBeat =
            static_cast<UInt32>(std::llround((1.0 - frac) * samplesPerBeat)) % static_cast<UInt32>(samplesPerBeat);
    }
    if (outNum != nullptr)
    {
        *outNum = 4.0f;
    }
    if (outDen != nullptr)
    {
        *outDen = 4;
    }
    if (outDownBeat != nullptr)
    {
        *outDownBeat = std::floor(beat / 4.0) * 4.0;
    }
    return noErr;
}

OSStatus hostTransportState(void* ud, Boolean* outIsPlaying, Boolean* outChanged, Float64* outSampleInTimeLine,
                            Boolean* outIsCycling, Float64* outCycleStartBeat, Float64* outCycleEndBeat)
{
    auto* in = static_cast<Instance*>(ud);
    in->transportCalls.fetch_add(1, std::memory_order_relaxed);
    if (outIsPlaying != nullptr)
    {
        *outIsPlaying = true;
    }
    if (outChanged != nullptr)
    {
        *outChanged = in->playingReported.exchange(true, std::memory_order_relaxed) ? false : true;
    }
    if (outSampleInTimeLine != nullptr)
    {
        *outSampleInTimeLine = static_cast<Float64>(in->timeline.load(std::memory_order_acquire));
    }
    const bool cyc = in->cycling.load(std::memory_order_acquire);
    if (outIsCycling != nullptr)
    {
        *outIsCycling = cyc;
    }
    if (outCycleStartBeat != nullptr)
    {
        *outCycleStartBeat = cyc ? ah::samplesToBeats(static_cast<double>(in->loopB.load())) : 0.0;
    }
    if (outCycleEndBeat != nullptr)
    {
        *outCycleEndBeat = cyc ? ah::samplesToBeats(static_cast<double>(in->loopE.load())) : 0.0;
    }
    return noErr;
}

OSStatus inputProc(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                   AudioBufferList* io)
{
    auto* in = static_cast<Instance*>(refCon);
    in->inputPulls.fetch_add(1, std::memory_order_relaxed);
    if (frames > in->pendingFrames)
    {
        in->inputShortPulls.fetch_add(1, std::memory_order_relaxed);
    }
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        std::vector<float>& src = in->scratch[std::min<std::size_t>(b, 1)];
        AudioBuffer& dst = io->mBuffers[b];
        if (dst.mData == nullptr)
        {
            dst.mData = src.data(); // 插件没给缓冲:直接借 scratch(渲染返回前一直有效)
            dst.mDataByteSize = frames * sizeof(float);
            continue;
        }
        const UInt32 n = std::min(frames, in->pendingFrames);
        std::memcpy(dst.mData, src.data(), n * sizeof(float));
        if (n < frames)
        {
            std::memset(static_cast<float*>(dst.mData) + n, 0, (frames - n) * sizeof(float));
        }
    }
    return noErr;
}

struct InstantiateWait
{
    std::atomic<bool> done{false};
    AudioComponentInstance inst = nullptr;
    OSStatus err = noErr;
};

bool instantiate(AudioComponent comp, bool oop, double timeoutSec, AudioComponentInstance& out, OSStatus& err,
                 double& ms)
{
    // 堆上分配;超时后不释放(回调可能晚到,写进已释放的内存会更糟)。一次性进程,泄漏可接受。
    auto* w = new InstantiateWait();
    const double t0 = nowSec();
    const AudioComponentInstantiationOptions opts =
        oop ? static_cast<AudioComponentInstantiationOptions>(kAudioComponentInstantiation_LoadOutOfProcess)
            : static_cast<AudioComponentInstantiationOptions>(0);
    AudioComponentInstantiate(comp, opts, ^(AudioComponentInstance inst, OSStatus e) {
      w->inst = inst;
      w->err = e;
      w->done.store(true, std::memory_order_release);
    });
    while (!w->done.load(std::memory_order_acquire) && nowSec() - t0 < timeoutSec)
    {
        pump(0.02);
    }
    ms = (nowSec() - t0) * 1000.0;
    if (!w->done.load(std::memory_order_acquire))
    {
        err = kTimedOut;
        return false;
    }
    out = w->inst;
    err = w->err;
    delete w;
    return err == noErr && out != nullptr;
}

OSStatus configure(Instance& in, std::string& stage)
{
    AudioStreamBasicDescription f{};
    f.mSampleRate = ah::kSr;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = sizeof(float);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(float);
    f.mChannelsPerFrame = in.channels;
    f.mBitsPerChannel = 32;
    UInt32 maxFrames = kMaxFrames;
    AURenderCallbackStruct cb{&inputProc, &in};
    HostCallbackInfo hc{};
    hc.hostUserData = &in;
    hc.beatAndTempoProc = &hostBeatAndTempo;
    hc.musicalTimeLocationProc = &hostMusicalTime;
    hc.transportStateProc = &hostTransportState;
    struct Step
    {
        const char* name;
        AudioUnitPropertyID id;
        AudioUnitScope scope;
        const void* data;
        UInt32 size;
    };
    const Step steps[] = {
        {"stream_format_input", kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, &f, sizeof f},
        {"stream_format_output", kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, &f, sizeof f},
        {"max_frames", kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, &maxFrames, sizeof maxFrames},
        {"render_callback", kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, &cb, sizeof cb},
        {"host_callbacks", kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, &hc, sizeof hc},
    };
    for (const Step& s : steps)
    {
        const OSStatus st = AudioUnitSetProperty(in.unit, s.id, s.scope, 0, s.data, s.size);
        if (st != noErr)
        {
            stage = s.name;
            return st;
        }
    }
    stage = "ok";
    return noErr;
}

// ---- Input 的通道号:经 ClassInfo 的 jucePluginState 写 SCVB state ------------------------------------
std::vector<std::uint8_t> inputStateBlob(std::uint32_t channel)
{
    scvb::state::InputState s;
    s.channelId = channel;
    s.groupId = kGroup;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> blob;
    if (!scvb::state::encodeInputState(s, payload))
    {
        return blob;
    }
    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(scvb::state::kFourccCfgs, std::move(payload));
    if (!scvb::state::encodeContainer(chunks, blob))
    {
        blob.clear();
    }
    return blob;
}

// 读回 jucePluginState 并解出 channelId;-1 = 读不到 / 解不开。
int readBackChannel(AudioUnit u)
{
    CFPropertyListRef plist = nullptr;
    UInt32 size = sizeof plist;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr ||
        plist == nullptr)
    {
        return -1;
    }
    int ch = -1;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID())
    {
        CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, kJuceStateKey, kCFStringEncodingUTF8);
        const void* v = nullptr;
        if (CFDictionaryGetValueIfPresent(static_cast<CFDictionaryRef>(plist), key, &v) && v != nullptr &&
            CFGetTypeID(v) == CFDataGetTypeID())
        {
            const auto data = static_cast<CFDataRef>(v);
            scvb::state::StateChunks chunks;
            if (scvb::state::decodeContainer(CFDataGetBytePtr(data), static_cast<std::size_t>(CFDataGetLength(data)),
                                             chunks) == scvb::state::DecodeStatus::Ok)
            {
                const scvb::state::Chunk* cfg = chunks.find(scvb::state::kFourccCfgs);
                scvb::state::InputState s;
                if (cfg != nullptr && scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), s))
                {
                    ch = static_cast<int>(s.channelId);
                }
            }
        }
        CFRelease(key);
    }
    CFRelease(plist);
    return ch;
}

OSStatus setInputChannel(AudioUnit u, std::uint32_t channel, std::string& stage)
{
    CFPropertyListRef plist = nullptr;
    UInt32 size = sizeof plist;
    OSStatus st = AudioUnitGetProperty(u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size);
    if (st != noErr || plist == nullptr || CFGetTypeID(plist) != CFDictionaryGetTypeID())
    {
        stage = "get_class_info";
        if (plist != nullptr)
        {
            CFRelease(plist);
        }
        return st != noErr ? st : kAudioUnitErr_InvalidPropertyValue;
    }
    const std::vector<std::uint8_t> blob = inputStateBlob(channel);
    if (blob.empty())
    {
        CFRelease(plist);
        stage = "encode_state";
        return kAudioUnitErr_InvalidPropertyValue;
    }
    CFMutableDictionaryRef dict =
        CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, static_cast<CFDictionaryRef>(plist));
    CFRelease(plist);
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, kJuceStateKey, kCFStringEncodingUTF8);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, blob.data(), static_cast<CFIndex>(blob.size()));
    CFDictionarySetValue(dict, key, data);
    CFPropertyListRef out = dict;
    st = AudioUnitSetProperty(u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &out, sizeof out);
    stage = st == noErr ? "ok" : "set_class_info";
    CFRelease(data);
    CFRelease(key);
    CFRelease(dict);
    return st;
}

// ---- registry 段(只读) ---------------------------------------------------------------------------
std::string posixSegName(scvb::SegmentKind kind, std::uint32_t channel = 0)
{
    const std::wstring w = scvb::segmentLogicalName(kGroup, kind, channel);
    std::string s = "/";
    for (const wchar_t c : w)
    {
        s.push_back(static_cast<char>(c)); // 逻辑名全是 ASCII
    }
    return s;
}

class RegistryView
{
public:
    ~RegistryView()
    {
        if (base_ != nullptr)
        {
            munmap(const_cast<void*>(base_), scvb::kRegistrySegmentSize);
        }
    }
    bool open()
    {
        if (base_ != nullptr)
        {
            return true;
        }
        const std::string name = posixSegName(scvb::SegmentKind::kRegistry);
        const int fd = shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0)
        {
            lastErrno_ = errno;
            return false;
        }
        struct stat st = {};
        if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(scvb::kRegistrySegmentSize))
        {
            lastErrno_ = errno != 0 ? errno : EINVAL;
            close(fd);
            return false;
        }
        void* p = mmap(nullptr, scvb::kRegistrySegmentSize, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED)
        {
            lastErrno_ = errno;
            return false;
        }
        base_ = p;
        return true;
    }
    bool isOpen() const { return base_ != nullptr; }
    int lastErrno() const { return lastErrno_; }
    bool magicOk() const
    {
        return base_ != nullptr && static_cast<const scvb::RegistryHeader*>(base_)->magic.load(
                                       std::memory_order_acquire) == scvb::kScvbMagic;
    }
    const scvb::OutputSlot* output() const
    {
        return reinterpret_cast<const scvb::OutputSlot*>(static_cast<const char*>(base_) + scvb::kOutputSlotOffset);
    }
    const scvb::InputSlot* input(std::uint32_t channel) const
    {
        return reinterpret_cast<const scvb::InputSlot*>(static_cast<const char*>(base_) + scvb::kInputSlotOffset +
                                                        (channel - 1) * sizeof(scvb::InputSlot));
    }

private:
    const void* base_ = nullptr;
    int lastErrno_ = 0;
};

std::string processName(pid_t pid)
{
    std::array<char, 2 * MAXCOMLEN + 1> name{};
    if (proc_name(pid, name.data(), static_cast<uint32_t>(name.size())) > 0)
    {
        return name.data();
    }
    return {};
}

std::string processPath(pid_t pid)
{
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    if (proc_pidpath(pid, path.data(), static_cast<uint32_t>(path.size())) > 0)
    {
        return path.data();
    }
    return {};
}

// ---- 渲染 -----------------------------------------------------------------------------------------
bool renderUnit(Instance& in, std::int64_t cursor, const ah::TimePoint& tp, int n, const float* const* src, int srcCh,
                float* const* dst, int dstCh)
{
    in.timeline.store(tp.t, std::memory_order_release);
    in.cycling.store(tp.cycling, std::memory_order_release);
    in.loopB.store(tp.loopB, std::memory_order_relaxed);
    in.loopE.store(tp.loopE, std::memory_order_relaxed);
    for (int c = 0; c < 2; ++c)
    {
        const float* s = src[std::min(c, srcCh - 1)];
        std::memcpy(in.scratch[static_cast<std::size_t>(c)].data(), s, static_cast<std::size_t>(n) * sizeof(float));
    }
    in.pendingFrames = static_cast<UInt32>(n);
    // 两个声道的 AudioBufferList(系统头只声明了 mBuffers[1];同布局的结构体按需开两格,对齐也对)。
    struct StereoBufferList
    {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[2];
    } abl{};
    static_assert(offsetof(StereoBufferList, mBuffers) == offsetof(AudioBufferList, mBuffers),
                  "StereoBufferList must match the AudioBufferList layout");
    abl.mNumberBuffers = static_cast<UInt32>(dstCh);
    for (int c = 0; c < dstCh; ++c)
    {
        abl.mBuffers[c].mNumberChannels = 1;
        abl.mBuffers[c].mDataByteSize = static_cast<UInt32>(n) * sizeof(float);
        abl.mBuffers[c].mData = dst[c];
    }
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mSampleTime = static_cast<Float64>(cursor) + in.sampleTimeOffset;
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    const OSStatus st =
        AudioUnitRender(in.unit, &flags, &ts, 0, static_cast<UInt32>(n), reinterpret_cast<AudioBufferList*>(&abl));
    ++in.renders;
    if (st != noErr)
    {
        ++in.renderErrors;
        in.lastRenderError = st;
        return false;
    }
    // 插件若换了缓冲指针(AU 允许),从它给的地方拷回来。
    for (int c = 0; c < dstCh; ++c)
    {
        if (abl.mBuffers[c].mData != dst[c] && abl.mBuffers[c].mData != nullptr)
        {
            std::memcpy(dst[c], abl.mBuffers[c].mData, static_cast<std::size_t>(n) * sizeof(float));
        }
    }
    return true;
}

// ---- 一次运行 -------------------------------------------------------------------------------------
struct Options
{
    std::string mode;
    std::string scenario = "ls1";
    std::string outDir = "auhost-out";
    double pace = 1.0;
    bool negativeControl = false;
    double instantiateTimeoutSec = 30.0;
    double wallLimitSec = 180.0;
};

const char* roleName(Role r)
{
    switch (r)
    {
    case Role::Input:
        return "Input";
    case Role::Output:
        return "Output";
    case Role::Monitor:
        return "Monitor";
    }
    return "?";
}

int runMode(const Options& opt)
{
    const bool oop = opt.mode == "oop";
    const ah::ScenarioSpec spec = opt.scenario == "ls4" ? ah::makeLs4() : ah::makeLs1();
    const std::string tag = opt.mode + "-" + spec.name + (opt.negativeControl ? "-negctl" : "");
    Results r;
    r.set("mode", opt.mode);
    r.set("scenario", spec.name);
    r.seti("negative_control", opt.negativeControl ? 1 : 0);
    r.set("env.os_product_version", sysctlString("kern.osproductversion"));
    r.set("env.os_build", sysctlString("kern.osversion"));
    const pid_t hostPid = getpid();
    r.seti("host.pid", hostPid);
    std::printf("[auhost] %s: host_pid=%d\n", tag.c_str(), static_cast<int>(hostPid));
    std::fflush(stdout);

    // 实例:Output 先(与 SchedRig 同序),再 3 条 Input,最后 Monitor。
    std::vector<std::unique_ptr<Instance>> insts;
    auto add = [&insts](const char* key, Role role, int lane, const char* subtype, UInt32 ch, Float64 off) {
        auto in = std::make_unique<Instance>();
        in->key = key;
        in->role = role;
        in->lane = lane;
        in->subtype = subtype;
        in->channels = ch;
        in->sampleTimeOffset = off;
        for (auto& s : in->scratch)
        {
            s.assign(kMaxFrames, 0.0f);
        }
        insts.push_back(std::move(in));
    };
    add("out", Role::Output, -1, kSubtypeOutput, 2, 50.0e6);
    add("in0", Role::Input, 0, kSubtypeInput, 1, 10.0e6);
    add("in1", Role::Input, 1, kSubtypeInput, 1, 20.0e6);
    add("in2", Role::Input, 2, kSubtypeInput, 1, 30.0e6);
    add("mon", Role::Monitor, -1, kSubtypeMonitor, 2, 60.0e6);

    bool setupOk = true;
    std::string setupError;
    for (auto& p : insts)
    {
        Instance& in = *p;
        const std::string k = "inst." + in.key + ".";
        r.set(k + "role", roleName(in.role));
        r.set(k + "subtype", in.subtype);
        AudioComponentDescription d{};
        d.componentType = kAudioUnitType_Effect;
        d.componentSubType = fourcc(in.subtype);
        d.componentManufacturer = fourcc(kManufacturer);
        in.comp = AudioComponentFindNext(nullptr, &d);
        r.seti(k + "found", in.comp != nullptr ? 1 : 0);
        if (in.comp == nullptr)
        {
            setupOk = false;
            setupError = std::string("component aufx/") + in.subtype + "/" + kManufacturer + " is not registered";
            std::printf("[auhost] %s: NOT registered\n", in.key.c_str());
            continue;
        }
        CFStringRef name = nullptr;
        if (AudioComponentCopyName(in.comp, &name) == noErr && name != nullptr)
        {
            r.set(k + "component_name", cfToString(name));
            CFRelease(name);
        }
        UInt32 version = 0;
        if (AudioComponentGetVersion(in.comp, &version) == noErr)
        {
            r.seti(k + "component_version", version);
        }
        OSStatus st = noErr;
        double ms = 0;
        const bool ok = instantiate(in.comp, oop, opt.instantiateTimeoutSec, in.unit, st, ms);
        r.seti(k + "instantiated", ok ? 1 : 0);
        r.seti(k + "inst_status", st);
        r.seti(k + "inst_ms", static_cast<long long>(ms));
        std::printf("[auhost] %s: instantiate %s status=%d (%.0f ms)\n", in.key.c_str(), ok ? "ok" : "FAILED",
                    static_cast<int>(st), ms);
        if (!ok)
        {
            in.unit = nullptr;
            setupOk = false;
            setupError = in.key + ": instantiation failed (status " + std::to_string(st) + ")";
            continue;
        }
        std::string stage;
        st = configure(in, stage);
        r.seti(k + "configure_status", st);
        r.set(k + "configure_stage", stage);
        if (st != noErr)
        {
            setupOk = false;
            setupError = in.key + ": configure failed at " + stage + " (status " + std::to_string(st) + ")";
            std::printf("[auhost] %s: configure FAILED at %s status=%d\n", in.key.c_str(), stage.c_str(),
                        static_cast<int>(st));
            continue;
        }
        if (in.role == Role::Input)
        {
            const std::uint32_t ch = static_cast<std::uint32_t>(in.lane + 1);
            st = setInputChannel(in.unit, ch, stage);
            r.seti(k + "state_status", st);
            r.set(k + "state_stage", stage);
            const int back = readBackChannel(in.unit);
            r.seti(k + "state_readback_channel", back);
            if (st != noErr || back != static_cast<int>(ch))
            {
                setupOk = false;
                setupError = in.key + ": channel " + std::to_string(ch) + " did not stick (stage " + stage +
                             ", status " + std::to_string(st) + ", read back " + std::to_string(back) + ")";
                std::printf("[auhost] %s: %s\n", in.key.c_str(), setupError.c_str());
                continue;
            }
        }
        st = AudioUnitInitialize(in.unit);
        in.initialized = st == noErr;
        r.seti(k + "initialized", in.initialized ? 1 : 0);
        r.seti(k + "init_status", st);
        std::printf("[auhost] %s: initialize status=%d\n", in.key.c_str(), static_cast<int>(st));
        if (!in.initialized)
        {
            setupOk = false;
            setupError = in.key + ": AudioUnitInitialize failed (status " + std::to_string(st) + ")";
        }
    }
    std::fflush(stdout);

    Instance* out = insts[0].get();
    std::array<Instance*, ah::kLanes> lanes{{insts[1].get(), insts[2].get(), insts[3].get()}};
    Instance* mon = insts[4].get();

    RegistryView reg;
    ah::DriveResult dr;
    std::unique_ptr<ah::Engine> engine;
    std::array<long long, ah::kLanes + 1> regPid{};
    if (setupOk)
    {
        ah::Callbacks cb;
        cb.lane = [&lanes](int l, std::int64_t c, const ah::TimePoint& tp, int n, const float* in, float* o) {
            const float* src[1] = {in};
            float* dst[1] = {o};
            return renderUnit(*lanes[static_cast<std::size_t>(l)], c, tp, n, src, 1, dst, 1);
        };
        cb.output = [out](std::int64_t c, const ah::TimePoint& tp, int n, const float* inL, const float* inR, float* oL,
                          float* oR) {
            const float* src[2] = {inL, inR};
            float* dst[2] = {oL, oR};
            return renderUnit(*out, c, tp, n, src, 2, dst, 2);
        };
        // Monitor 接在 Output 后面看同一路(它只读 viz 段,不进判定);输出丢弃。
        std::vector<float> monL(kMaxFrames), monR(kMaxFrames);
        ah::Engine* ep = nullptr;
        cb.afterOutput = [mon, &ep, &monL, &monR, &spec](std::int64_t c0, int n) {
            std::int64_t c = c0;
            while (c < c0 + n)
            {
                const int m = static_cast<int>(std::min<std::int64_t>(c0 + n - c, ah::runLength(spec, c)));
                const ah::TimePoint tp = ah::timelineAt(spec, c);
                const float* src[2] = {&ep->outL[static_cast<std::size_t>(c)], &ep->outR[static_cast<std::size_t>(c)]};
                float* dst[2] = {monL.data(), monR.data()};
                if (!renderUnit(*mon, c, tp, m, src, 2, dst, 2))
                {
                    return false;
                }
                c += m;
            }
            return true;
        };
        engine = std::make_unique<ah::Engine>(spec, cb);
        ep = engine.get();
        if (opt.negativeControl)
        {
            engine->setSkipped(1, true); // 负对照:宿主从不调用 lane 1 的 Input
        }

        reg.open(); // Output 的 prepareToPlay 已建好段;打不开就在预热判据里重试
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        ah::DriveHooks hooks;
        const double wallStart = nowSec();
        hooks.pace = [&opt, wallStart](std::int64_t c) {
            if (opt.pace <= 0)
            {
                return;
            }
            const double target = wallStart + ah::samplesToSec(c) / opt.pace;
            const double wait = target - nowSec();
            if (wait > 0)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            }
        };
        hooks.externalSettled = [&reg, &engine]() {
            if (!reg.open() || !reg.magicOk())
            {
                return false;
            }
            const scvb::OutputSlot* o = reg.output();
            if (o->state.load(std::memory_order_acquire) != scvb::kSlotActive)
            {
                return false;
            }
            const std::uint32_t mask = o->connected_mask.load(std::memory_order_acquire);
            for (int l = 0; l < ah::kLanes; ++l)
            {
                if (engine->skipped(l))
                {
                    continue;
                }
                const std::uint32_t ch = static_cast<std::uint32_t>(l + 1);
                if (reg.input(ch)->state.load(std::memory_order_acquire) != scvb::kSlotActive ||
                    (mask & (1u << (ch - 1))) == 0)
                {
                    return false;
                }
            }
            return true;
        };
        hooks.shouldStop = [&stop]() { return stop.load(std::memory_order_acquire); };
        std::thread renderThread([&]() {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            dr = ah::drive(*engine, hooks);
            // 判定前趁实例都还活着把 registry 里的 pid 抄下来(收尾时段会被最后离开者 unlink)。
            if (reg.isOpen())
            {
                regPid[0] = reg.output()->pid;
                for (int l = 0; l < ah::kLanes; ++l)
                {
                    regPid[static_cast<std::size_t>(l + 1)] = reg.input(static_cast<std::uint32_t>(l + 1))->pid;
                }
            }
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire))
        {
            pump(0.01);
            if (nowSec() - wallStart > opt.wallLimitSec)
            {
                stop.store(true, std::memory_order_release);
            }
        }
        renderThread.join();
        r.seti("run.wall_ms", static_cast<long long>((nowSec() - wallStart) * 1000.0));
    }

    // ---- 记录 ----
    for (auto& p : insts)
    {
        const Instance& in = *p;
        const std::string k = "inst." + in.key + ".";
        r.seti(k + "renders", in.renders);
        r.seti(k + "render_errors", in.renderErrors);
        r.seti(k + "last_render_error", in.lastRenderError);
        r.seti(k + "hostcb_transport_calls", in.transportCalls.load());
        r.seti(k + "hostcb_beat_calls", in.beatCalls.load());
        r.seti(k + "input_pulls", in.inputPulls.load());
        r.seti(k + "input_short_pulls", in.inputShortPulls.load());
    }
    r.seti("registry.open", reg.isOpen() ? 1 : 0);
    r.seti("registry.open_errno", reg.lastErrno());

    std::vector<std::string> failures;
    ah::Judgement jd;
    bool processOk = false;
    std::string processLine = "-";
    if (setupOk && engine)
    {
        r.seti("drive.ok", dr.ok ? 1 : 0);
        r.set("drive.error", dr.error);
        r.seti("drive.settled", dr.settled ? 1 : 0);
        r.set("drive.settled_sec", std::to_string(ah::samplesToSec(dr.settledCursor)));
        r.seti("drive.end_cursor", dr.endCursor);
        r.seti("drive.cycles", engine->cycles());
        jd = ah::judge(*engine, dr, true);
        r.seti("judge.measured", jd.measured ? 1 : 0);
        r.seti("judge.pass", jd.pass ? 1 : 0);
        for (int l = 0; l < ah::kLanes; ++l)
        {
            const ah::LaneStats& st = jd.total[static_cast<std::size_t>(l)];
            const std::string k = "judge.lane" + std::to_string(l) + ".";
            r.seti(k + "pass", jd.lanePass[static_cast<std::size_t>(l)] ? 1 : 0);
            r.seti(k + "frames", st.frames);
            r.seti(k + "mix_present", st.mixPresent);
            r.seti(k + "raw_present", st.rawPresent);
            r.seti(k + "absent", st.absent);
            r.seti(k + "partial", st.partial);
            r.seti(k + "wrong", st.wrong);
            if (spec.id == ah::Scenario::Ls4 && l > 0)
            {
                const ah::LeadObserved lo = ah::observedLead(*engine, dr, l);
                r.seti(k + "lead_observed_min", lo.any ? lo.min : -1);
                r.seti(k + "lead_observed_max", lo.any ? lo.max : -1);
            }
        }
        // 进程:registry 里各实例的 pid。
        std::set<long long> pids;
        const std::array<const char*, ah::kLanes + 1> names{{"out", "in0", "in1", "in2"}};
        for (std::size_t i = 0; i < regPid.size(); ++i)
        {
            r.seti(std::string("registry.") + names[i] + ".pid", regPid[i]);
            pids.insert(regPid[i]);
        }
        r.seti("registry.distinct_pids", static_cast<long long>(pids.size()));
        if (!pids.empty() && *pids.begin() > 0)
        {
            const pid_t p0 = static_cast<pid_t>(*pids.begin());
            r.set("plugin_process.name", processName(p0));
            // 只记系统路径(AUHostingService 在 /System 下);进程内时那是宿主自己,路径在用户目录里,不记。
            const std::string path = processPath(p0);
            r.set("plugin_process.path", path.rfind("/System/", 0) == 0 ? path : std::string("(not a system path)"));
        }
        processOk = ah::processCheck(oop, hostPid, std::vector<long long>(regPid.begin(), regPid.end()));
        processLine = "host pid " + std::to_string(hostPid) +
                      "; registry pids out/in0/in1/in2 = " + std::to_string(regPid[0]) + "/" +
                      std::to_string(regPid[1]) + "/" + std::to_string(regPid[2]) + "/" + std::to_string(regPid[3]) +
                      "; plugin process '" + r.get("plugin_process.name") + "'" +
                      (oop ? " (out-of-process: every SCVB instance must NOT be in the host process)"
                           : " (in-process: every SCVB instance must be in the host process)");
        r.seti("process.ok", processOk ? 1 : 0);
        if (!dr.ok)
        {
            setupOk = false;
            setupError = "drive: " + dr.error;
        }
        if (dr.ok && !processOk)
        {
            failures.push_back("process check failed: " + processLine);
        }
        for (const std::string& f : jd.failures)
        {
            failures.push_back(f);
        }
    }

    // ---- 收尾(先落盘再拆,拆的时候崩了也不丢测量) ----
    auto verdict = [&]() -> int {
        if (!setupOk || !jd.measured)
        {
            return 2;
        }
        if (opt.negativeControl)
        {
            // 负对照成立 = 判据红在 lane 1 且只红在 lane 1(lane 0 / 2 照绿),进程判据照过。
            const bool held = !jd.pass && !jd.lanePass[1] && jd.lanePass[0] && jd.lanePass[2] && processOk;
            return held ? 0 : 1;
        }
        return (jd.pass && processOk) ? 0 : 1;
    };
    const int rc = verdict();
    r.seti("exit_code", rc);

    std::string md = "### scvb_auhost " + opt.mode + " " + spec.name +
                     (opt.negativeControl ? " (negative control: host never calls Input lane 1)" : "") + "\n\n";
    md += "macOS " + r.get("env.os_product_version") + " (" + r.get("env.os_build") + ")\n\n";
    md += "| instance | component | instantiated | init | renders (errors) | host transport cb calls |\n"
          "|---|---|---|---|---|---|\n";
    for (auto& p : insts)
    {
        const Instance& in = *p;
        const std::string k = "inst." + in.key + ".";
        md += "| " + in.key + " | " + r.get(k + "component_name") + " v" + r.get(k + "component_version") + " | " +
              r.get(k + "instantiated") + " (" + r.get(k + "inst_ms") + " ms) | " + r.get(k + "init_status") + " | " +
              std::to_string(in.renders) + " (" + std::to_string(in.renderErrors) + ") | " +
              std::to_string(in.transportCalls.load()) + " |\n";
    }
    md += "\nprocess: " + processLine + " -> **" + (processOk ? "ok" : "FAIL") + "**\n\n";
    if (engine && jd.measured)
    {
        md += ah::judgementMarkdown(*engine, dr, jd);
    }
    if (!setupOk)
    {
        md += "\n**not measured**: " + setupError + "\n";
    }
    md += "\nexit code " + std::to_string(rc) +
          (opt.negativeControl ? (rc == 0 ? " (negative control held: the criteria went red on lane 1 only)"
                                          : " (negative control did NOT hold)")
                               : (rc == 0 ? " (pass)" : (rc == 1 ? " (criteria failed)" : " (not measured)"))) +
          "\n";
    if (!failures.empty())
    {
        r.set("failures", [&failures] {
            std::string s;
            for (const std::string& f : failures)
            {
                s += (s.empty() ? "" : " | ") + f;
            }
            return s;
        }());
    }
    if (!setupError.empty())
    {
        r.set("setup_error", setupError);
    }
    mkdir(opt.outDir.c_str(), 0755);
    const std::string base = opt.outDir + "/" + tag;
    writeFile(base + ".kv", r.asKV());
    writeFile(base + ".md", md);
    if (jd.measured)
    {
        writeFile(base + "-frames.csv", ah::rowsCsv(jd));
    }
    std::fputs(md.c_str(), stdout);
    std::fflush(stdout);

    for (auto& p : insts)
    {
        if (p->initialized)
        {
            AudioUnitUninitialize(p->unit);
        }
        if (p->unit != nullptr)
        {
            AudioComponentInstanceDispose(p->unit);
        }
        p->unit = nullptr;
    }
    // 收尾后的残段(只记录,不判定):最后离开者应当已经 unlink。进程外要给服务一点时间拆实例。
    pump(1.0);
    int leftover = 0;
    std::string leftoverNames;
    std::vector<std::string> names = {posixSegName(scvb::SegmentKind::kRegistry),
                                      posixSegName(scvb::SegmentKind::kCtrl), posixSegName(scvb::SegmentKind::kViz)};
    for (std::uint32_t ch = 1; ch <= ah::kLanes; ++ch)
    {
        names.push_back(posixSegName(scvb::SegmentKind::kAudio, ch));
        names.push_back(posixSegName(scvb::SegmentKind::kFeat, ch));
    }
    for (const std::string& n : names)
    {
        const int fd = shm_open(n.c_str(), O_RDONLY, 0);
        if (fd >= 0)
        {
            close(fd);
            ++leftover;
            leftoverNames += (leftoverNames.empty() ? "" : " ") + n;
        }
    }
    r.seti("cleanup.leftover_segments", leftover);
    r.set("cleanup.leftover_names", leftoverNames);
    writeFile(base + ".kv", r.asKV());
    std::printf("\nafter dispose: %d SCVB segment(s) still present%s%s (recorded only)\n", leftover,
                leftover ? ": " : "", leftoverNames.c_str());
    std::fflush(stdout);
    return rc;
}

void usage()
{
    std::fputs("usage: scvb_auhost --self-test\n"
               "       scvb_auhost --mode inproc|oop --scenario ls1|ls4 [--out-dir DIR] [--pace X]\n"
               "                   [--negative-control]\n",
               stderr);
}
} // namespace

int main(int argc, char** argv)
{
    Options opt;
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc)
            {
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string tmp;
        if (a == "--self-test")
        {
            selfTest = true;
        }
        else if (a == "--mode" && next(tmp) && (tmp == "inproc" || tmp == "oop"))
        {
            opt.mode = tmp;
        }
        else if (a == "--scenario" && next(tmp) && (tmp == "ls1" || tmp == "ls4"))
        {
            opt.scenario = tmp;
        }
        else if (a == "--out-dir" && next(opt.outDir))
        {
            continue;
        }
        else if (a == "--pace" && next(tmp))
        {
            opt.pace = std::atof(tmp.c_str());
        }
        else if (a == "--negative-control")
        {
            opt.negativeControl = true;
        }
        else
        {
            usage();
            return 2;
        }
    }
    if (selfTest)
    {
        return scvb::auhost::selfTest();
    }
    if (opt.mode.empty())
    {
        usage();
        return 2;
    }
    const int rc = runMode(opt);
    std::fflush(nullptr);
    // 进程内模式下 JUCE 插件镜像还挂在本进程里;跳过静态析构,别让退出阶段的崩溃把这一步染红。
    _exit(rc);
}
