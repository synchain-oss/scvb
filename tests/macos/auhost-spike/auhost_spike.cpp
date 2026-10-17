// SPDX-License-Identifier: GPL-3.0-or-later
// auhost_spike.cpp —— M01 进程外 shm spike 的宿主(纯 C++ + clang blocks + AudioToolbox/CoreFoundation,
// 不写 .mm:check-spdx / clang-format 只认 .cpp/.h)。
//
// 用法(四个互相独立的入口,CI 里各占一步,任何一步崩了都不会吞掉别的步的结果):
//   auhost_spike --mode inproc --out-dir DIR [--raw-paths]   进程内对照(AudioComponentInstantiate flags=0)
//   auhost_spike --mode oop    --out-dir DIR [--raw-paths]   进程外(kAudioComponentInstantiation_LoadOutOfProcess)
//   auhost_spike --judge       --out-dir DIR                 读 DIR/inproc.kv + DIR/oop.kv 出判定(退出码见
//   spike_judge.h) auhost_spike --self-test                                 用合成输入逐格断言判定表
// 其它:--subtype-a / --subtype-b / --manufacturer 改组件三元组(反向注入「写错 subtype」就用它),
//       --blocks N 渲染块数。
//
// 每种模式:实例化 A×2、B×1(依次 Initialize,探针在插件的 prepareToPlay 里跑),宿主自己再
// shm_open 同名段核对「插件建的段宿主看得见」,渲染若干块后看段里的计数有没有前进,然后经参数
// 通道读回探针结果,落盘 DIR/<mode>.kv(判定用)与 DIR/<mode>.json(人读用)。
//
// 隐私:默认把真实 home 前缀替换成 "~"(用户在自己的 Mac 上跑时不会带出真名);CI 上传
// runner 的原始值时显式加 --raw-paths(runner 的 home 是 /Users/runner)。
// 运行期字符串一律 ASCII。

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "spike_judge.h"
#include "spike_protocol.h"

namespace
{
// 默认三元组;必须与 CMakeLists.txt 的 SPIKE_SUBTYPE_A / SPIKE_SUBTYPE_B 默认值一致。
// 两边对不上的失效形态是「组件没注册上」= 负对照不成立 => inconclusive + 退出码 1,不会被读成任何结论。
constexpr const char* kDefaultSubtypeA = "Sxsa";
constexpr const char* kDefaultSubtypeB = "Sxsb";
constexpr const char* kDefaultManufacturer = "Snch";
constexpr UInt32 kBlockFrames = 512;
constexpr Float64 kSampleRate = 48000.0;
constexpr OSStatus kTimedOut = -42424; // 本工具自定义:实例化回调在超时内没回来

struct Options
{
    std::string mode;
    std::string outDir = "spike-out";
    std::string subtypeA = kDefaultSubtypeA;
    std::string subtypeB = kDefaultSubtypeB;
    std::string manufacturer = kDefaultManufacturer;
    int blocks = 16;
    double instantiateTimeoutSec = 30.0;
    double doneTimeoutSec = 15.0;
    bool rawPaths = false;
};

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 主线程等待时转 run loop:实例化回调可能被派到主队列,干等信号量会自锁。
void pump(double sec)
{
    const SInt32 r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, sec, false);
    if (r == kCFRunLoopRunFinished)
        usleep(static_cast<useconds_t>(sec * 1e6));
}

OSType fourcc(const std::string& s)
{
    if (s.size() != 4)
        return 0;
    return (static_cast<OSType>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<OSType>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<OSType>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<OSType>(static_cast<unsigned char>(s[3]));
}

std::string cfToString(CFStringRef s)
{
    if (s == nullptr)
        return {};
    std::array<char, 1024> buf{};
    if (CFStringGetCString(s, buf.data(), static_cast<CFIndex>(buf.size()), kCFStringEncodingUTF8))
        return buf.data();
    return {};
}

std::string realHome()
{
    struct passwd pw = {};
    struct passwd* res = nullptr;
    std::array<char, 4096> buf{};
    if (getpwuid_r(getuid(), &pw, buf.data(), buf.size(), &res) == 0 && res != nullptr && res->pw_dir != nullptr)
        return res->pw_dir;
    return {};
}

std::string sysctlString(const char* name)
{
    std::array<char, 256> buf{};
    std::size_t len = buf.size();
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0)
        return {};
    return std::string(buf.data());
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += ch;
        }
        else if (c < 0x20)
        {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c));
            o += b;
        }
        else
        {
            o += ch;
        }
    }
    return o;
}

bool readFile(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    out.clear();
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
        out.append(buf.data(), n);
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// ---- 结果收集 ---------------------------------------------------------------------------------
struct Results
{
    spike::KV kv;
    std::string redactFrom; // 真实 home;非空时写盘前替换成 "~"

    void set(const std::string& k, std::string v)
    {
        for (char& c : v)
            if (c == '\n' || c == '\r')
                c = ' ';
        kv[k] = std::move(v);
    }
    void seti(const std::string& k, long long v) { kv[k] = std::to_string(v); }
    std::string redact(const std::string& s) const { return redactFrom.empty() ? s : replaceAll(s, redactFrom, "~"); }
    std::string asKV() const
    {
        std::string o;
        for (const auto& [k, v] : kv)
            o += k + "=" + redact(v) + "\n";
        return o;
    }
    std::string asJSON() const
    {
        std::string o = "{\n";
        std::size_t n = 0;
        for (const auto& [k, v] : kv)
        {
            o += "  \"" + jsonEscape(k) + "\": \"" + jsonEscape(redact(v)) + "\"";
            o += ++n < kv.size() ? ",\n" : "\n";
        }
        o += "}\n";
        return o;
    }
};

// ---- 实例 -------------------------------------------------------------------------------------
struct Instance
{
    std::string key; // a1 / a2 / b1
    std::string subtype;
    int nonce = 0;
    AudioComponent comp = nullptr;
    AudioComponentInstance unit = nullptr;
    bool initialized = false;
    std::map<std::string, AudioUnitParameterID> params;
};

struct InstantiateWait
{
    std::atomic<bool> done{false};
    AudioComponentInstance inst = nullptr;
    OSStatus err = noErr;
};

bool instantiate(AudioComponent comp, bool oop, double timeoutSec, AudioComponentInstance& out, OSStatus& err,
                 double& ms)
{
    // 堆上分配;超时后不释放(回调可能晚到,写进已释放的栈会更糟)。spike 一次性进程,泄漏可接受。
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
        pump(0.02);
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

OSStatus silenceInput(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
        if (io->mBuffers[b].mData != nullptr)
            std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
    return noErr;
}

OSStatus configure(AudioUnit u, std::string& stage)
{
    AudioStreamBasicDescription f{};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = sizeof(float);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(float);
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    UInt32 maxFrames = kBlockFrames;
    AURenderCallbackStruct cb{&silenceInput, nullptr};
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
    };
    for (const Step& s : steps)
    {
        const OSStatus st = AudioUnitSetProperty(u, s.id, s.scope, 0, s.data, s.size);
        if (st != noErr)
        {
            stage = s.name;
            return st;
        }
    }
    stage = "ok";
    return noErr;
}

std::map<std::string, AudioUnitParameterID> mapParams(AudioUnit u)
{
    std::map<std::string, AudioUnitParameterID> out;
    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, &writable) !=
            noErr ||
        size == 0)
        return out;
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size) !=
        noErr)
        return out;
    for (const AudioUnitParameterID id : ids)
    {
        AudioUnitParameterInfo info{};
        UInt32 s = sizeof info;
        if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &s) != noErr)
            continue;
        std::string name;
        if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0 && info.cfNameString != nullptr)
        {
            name = cfToString(info.cfNameString);
            if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0)
                CFRelease(info.cfNameString);
        }
        else
        {
            name = info.name;
        }
        out[name] = id;
    }
    return out;
}

bool getParam(const Instance& in, spike::Param p, float& v)
{
    const auto it = in.params.find(spike::kParamNames[p]);
    if (it == in.params.end() || in.unit == nullptr)
        return false;
    AudioUnitParameterValue val = 0;
    if (AudioUnitGetParameter(in.unit, it->second, kAudioUnitScope_Global, 0, &val) != noErr)
        return false;
    v = val;
    return true;
}

OSStatus setParam(const Instance& in, spike::Param p, float v)
{
    const auto it = in.params.find(spike::kParamNames[p]);
    if (it == in.params.end())
        return kAudioUnitErr_InvalidParameter;
    return AudioUnitSetParameter(in.unit, it->second, kAudioUnitScope_Global, 0, v, 0);
}

OSStatus renderBlocks(AudioUnit u, int blocks, Float64& sampleTime, int& rendered)
{
    std::vector<float> left(kBlockFrames, 0.0f);
    std::vector<float> right(kBlockFrames, 0.0f);
    std::vector<std::uint8_t> storage(offsetof(AudioBufferList, mBuffers) + 2 * sizeof(AudioBuffer));
    auto* abl = reinterpret_cast<AudioBufferList*>(storage.data());
    AudioBuffer* bufs = abl->mBuffers;
    for (int i = 0; i < blocks; ++i)
    {
        abl->mNumberBuffers = 2;
        bufs[0].mNumberChannels = 1;
        bufs[0].mDataByteSize = kBlockFrames * sizeof(float);
        bufs[0].mData = left.data();
        bufs[1].mNumberChannels = 1;
        bufs[1].mDataByteSize = kBlockFrames * sizeof(float);
        bufs[1].mData = right.data();
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp ts{};
        ts.mSampleTime = sampleTime;
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        const OSStatus st = AudioUnitRender(u, &flags, &ts, 0, kBlockFrames, abl);
        if (st != noErr)
            return st;
        sampleTime += kBlockFrames;
        ++rendered;
    }
    return noErr;
}

int errCode(int e)
{
    return spike::kErrnoBase + e;
}

// ---- 一种模式跑一遍 ----------------------------------------------------------------------------
int runMode(const Options& opt)
{
    const bool oop = opt.mode == "oop";
    Results r;
    const std::string home = realHome();
    if (!opt.rawPaths)
        r.redactFrom = home;
    r.set("mode", opt.mode);
    r.set("env.os_product_version", sysctlString("kern.osproductversion"));
    r.set("env.os_build", sysctlString("kern.osversion"));
    struct utsname un = {};
    if (uname(&un) == 0)
        r.set("env.machine", un.machine);
    const char* envHome = std::getenv("HOME");
    r.set("host.home_env", envHome != nullptr ? envHome : "<unset>");
    r.set("host.pw_dir", home);
    r.set("host.paths_redacted", opt.rawPaths ? "0" : "1");

    const pid_t hostPid = getpid();
    const int token = 1 + static_cast<int>(arc4random_uniform(131000));
    r.seti("host.pid", hostPid);
    r.seti("host.token", token);
    std::printf("[spike] mode=%s host_pid=%d token=%d\n", opt.mode.c_str(), static_cast<int>(hostPid), token);

    // 清掉上一轮可能残留的段:否则第一个实例会「附着」到旧段,三个实例全成附着方 => 判定拒绝读成 A。
    r.seti("host.preclean_seg_errno", shm_unlink(spike::kSegName) == 0 ? 0 : errno);

    // 宿主建自己的段,写 magic + pid + token(宿主 -> 插件方向)。
    shm_unlink(spike::kHostSegName);
    void* hostMap = nullptr;
    {
        int code = spike::kOk;
        const int fd = shm_open(spike::kHostSegName, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
            code = errCode(errno);
        else
        {
            if (ftruncate(fd, static_cast<off_t>(spike::kHostSegBytes)) != 0)
                code = errCode(errno);
            else
            {
                void* p = mmap(nullptr, spike::kHostSegBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED)
                    code = errCode(errno);
                else
                {
                    hostMap = p;
                    auto* h = static_cast<spike::HostHeader*>(p);
                    h->hostPid.store(static_cast<std::int32_t>(hostPid), std::memory_order_relaxed);
                    h->token.store(static_cast<std::uint32_t>(token), std::memory_order_relaxed);
                    h->magic.store(spike::kHostMagic, std::memory_order_release);
                }
            }
            close(fd);
        }
        r.seti("host.seg_create", code);
    }

    std::vector<Instance> insts = {{"a1", opt.subtypeA, 1}, {"a2", opt.subtypeA, 2}, {"b1", opt.subtypeB, 3}};
    for (Instance& in : insts)
    {
        const std::string p = in.key + ".";
        r.set(p + "subtype", in.subtype);
        r.seti(p + "nonce", in.nonce);
        AudioComponentDescription d{};
        d.componentType = kAudioUnitType_Effect;
        d.componentSubType = fourcc(in.subtype);
        d.componentManufacturer = fourcc(opt.manufacturer);
        in.comp = AudioComponentFindNext(nullptr, &d);
        r.seti(p + "found", in.comp != nullptr ? 1 : 0);
        if (in.comp == nullptr)
        {
            std::printf("[spike] %s: component aufx/%s/%s NOT registered\n", in.key.c_str(), in.subtype.c_str(),
                        opt.manufacturer.c_str());
            continue;
        }
        CFStringRef name = nullptr;
        if (AudioComponentCopyName(in.comp, &name) == noErr && name != nullptr)
        {
            r.set(p + "component_name", cfToString(name));
            CFRelease(name);
        }
        UInt32 version = 0;
        if (AudioComponentGetVersion(in.comp, &version) == noErr)
            r.seti(p + "component_version", version);

        OSStatus st = noErr;
        double ms = 0;
        const bool ok = instantiate(in.comp, oop, opt.instantiateTimeoutSec, in.unit, st, ms);
        r.seti(p + "instantiated", ok ? 1 : 0);
        r.seti(p + "inst_status", st);
        r.seti(p + "inst_ms", static_cast<long long>(ms));
        std::printf("[spike] %s: instantiate %s status=%d (%.0f ms)\n", in.key.c_str(), ok ? "ok" : "FAILED",
                    static_cast<int>(st), ms);
        if (!ok)
        {
            in.unit = nullptr;
            continue;
        }
        std::string stage;
        st = configure(in.unit, stage);
        r.seti(p + "configure_status", st);
        r.set(p + "configure_stage", stage);
        in.params = mapParams(in.unit);
        int mapped = 0;
        for (int i = 0; i < spike::pCount; ++i)
            mapped += in.params.count(spike::kParamNames[i]) != 0 ? 1 : 0;
        r.seti(p + "params_total", static_cast<long long>(in.params.size()));
        r.seti(p + "params_mapped", mapped);
        r.seti(p + "set_hostpid_status", setParam(in, spike::pHostPid, spike::encodePid(hostPid)));
        r.seti(p + "set_token_status", setParam(in, spike::pRunToken, spike::encodePid(token)));
        r.seti(p + "set_nonce_status", setParam(in, spike::pNonce, spike::encodeCode(in.nonce)));
        st = AudioUnitInitialize(in.unit);
        in.initialized = st == noErr;
        r.seti(p + "initialized", in.initialized ? 1 : 0);
        r.seti(p + "init_status", st);
        std::printf("[spike] %s: initialize status=%d params_mapped=%d/%d\n", in.key.c_str(), static_cast<int>(st),
                    mapped, static_cast<int>(spike::pCount));
    }

    // 宿主核对插件建的段(插件 -> 宿主方向)。
    spike::SegHeader* seg = nullptr;
    {
        int openCode = spike::kOk;
        int magicCode = spike::kSkipped;
        const int fd = shm_open(spike::kSegName, O_RDWR, 0);
        if (fd < 0)
            openCode = errCode(errno);
        else
        {
            struct stat st = {};
            const long long size = fstat(fd, &st) == 0 ? static_cast<long long>(st.st_size) : -1;
            r.seti("host.plugin_seg_size", size);
            if (size >= static_cast<long long>(sizeof(spike::SegHeader)))
            {
                void* p = mmap(nullptr, sizeof(spike::SegHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED)
                    openCode = errCode(errno);
                else
                {
                    seg = static_cast<spike::SegHeader*>(p);
                    magicCode =
                        seg->magic.load(std::memory_order_acquire) == spike::kSegMagic ? spike::kOk : spike::kMismatch;
                    r.seti("host.plugin_seg_creator_pid", seg->creatorPid.load(std::memory_order_relaxed));
                    r.seti("host.plugin_seg_next_slot", seg->nextSlot.load(std::memory_order_relaxed));
                }
            }
            else
            {
                magicCode = spike::kMismatch;
            }
            close(fd);
        }
        r.seti("host.plugin_seg_open", openCode);
        r.seti("host.plugin_seg_magic", magicCode);
    }
    auto snapshotSlots = [&](const char* suffix) {
        if (seg == nullptr)
            return;
        for (std::uint32_t k = 0; k < spike::kMaxSlots; ++k)
        {
            const spike::Slot& s = seg->slots[k];
            const std::uint32_t nonce = s.nonce.load(std::memory_order_acquire);
            if (nonce == 0)
                continue;
            const std::string p = "host.slot" + std::to_string(k) + ".";
            r.seti(p + "nonce", nonce);
            r.seti(p + "pid", s.pid.load(std::memory_order_relaxed));
            r.seti(p + suffix, static_cast<long long>(s.renders.load(std::memory_order_relaxed)));
        }
    };
    snapshotSlots("renders_before");

    // 宿主对同一个锁文件试 EX:插件持 SH 时应得 EWOULDBLOCK(=2);文件不存在说明插件没建出来。
    const std::string lockFile = home.empty() ? std::string() : spike::lockPath(home, token);
    {
        int code = spike::kSkipped;
        if (!lockFile.empty())
        {
            const int fd = open(lockFile.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0)
                code = errCode(errno);
            else
            {
                if (flock(fd, LOCK_EX | LOCK_NB) == 0)
                {
                    code = spike::kOk;
                    flock(fd, LOCK_UN);
                }
                else
                {
                    code = errno == EWOULDBLOCK ? spike::kAlt : errCode(errno);
                }
                close(fd);
            }
        }
        r.seti("host.lock_probe", code);
    }

    // 渲染。
    for (Instance& in : insts)
    {
        if (!in.initialized)
            continue;
        Float64 t = 0;
        int rendered = 0;
        const OSStatus st = renderBlocks(in.unit, opt.blocks, t, rendered);
        r.seti(in.key + ".render_status", st);
        r.seti(in.key + ".render_blocks", rendered);
    }
    pump(0.1);
    snapshotSlots("renders_after");

    // 经参数通道读回探针结果:等到每个已初始化实例都报 done=1(进程外时值可能经 XPC 晚到)。
    const double t0 = nowSec();
    bool allDone = false;
    while (!allDone && nowSec() - t0 < opt.doneTimeoutSec)
    {
        allDone = true;
        for (const Instance& in : insts)
        {
            float v = 0;
            if (in.initialized && !(getParam(in, spike::pDone, v) && spike::decodeCode(v) == spike::kOk))
                allDone = false;
        }
        if (!allDone)
            pump(0.05);
    }
    r.seti("host.params_wait_ms", static_cast<long long>((nowSec() - t0) * 1000.0));
    for (const Instance& in : insts)
    {
        if (!in.initialized)
            continue;
        for (int i = spike::pDone; i < spike::pCount; ++i)
        {
            float v = 0;
            const std::string k = in.key + "." + spike::kParamNames[i];
            if (!getParam(in, static_cast<spike::Param>(i), v))
            {
                r.set(k, "");
                continue;
            }
            r.seti(k, i == spike::pPid ? spike::decodePid(v) : spike::decodeCode(v));
        }
    }

    // 第二条通道:插件写进真实 home 的 JSON。宿主只看在不在、多大,并拷进输出目录(默认脱敏)。
    for (const Instance& in : insts)
    {
        const std::string p = in.key + ".";
        if (home.empty())
        {
            r.seti(p + "json_found", 0);
            continue;
        }
        const std::string path = spike::jsonPath(home, token, in.nonce, in.subtype);
        std::string text;
        const bool found = readFile(path, text);
        r.seti(p + "json_found", found ? 1 : 0);
        r.seti(p + "json_bytes", found ? static_cast<long long>(text.size()) : 0);
        if (found)
            writeFile(opt.outDir + "/" + opt.mode + "-" + in.key + "-plugin.json", r.redact(text));
    }

    r.seti("run.complete", 1);
    const std::string kvPath = opt.outDir + "/" + opt.mode + ".kv";
    const std::string jsonPath = opt.outDir + "/" + opt.mode + ".json";
    const bool wrote = writeFile(kvPath, r.asKV()) && writeFile(jsonPath, r.asJSON());
    std::printf("[spike] results %s -> %s\n", wrote ? "written" : "NOT written", kvPath.c_str());
    std::fflush(stdout);

    // 收尾(结果已落盘,收尾里崩了也不丢测量)。
    for (Instance& in : insts)
    {
        if (in.initialized)
            AudioUnitUninitialize(in.unit);
        if (in.unit != nullptr)
            AudioComponentInstanceDispose(in.unit);
        in.unit = nullptr;
    }
    if (seg != nullptr)
        munmap(seg, sizeof(spike::SegHeader));
    if (hostMap != nullptr)
        munmap(hostMap, spike::kHostSegBytes);
    r.seti("cleanup.unlink_seg_errno", shm_unlink(spike::kSegName) == 0 ? 0 : errno);
    r.seti("cleanup.unlink_host_seg_errno", shm_unlink(spike::kHostSegName) == 0 ? 0 : errno);
    if (!lockFile.empty())
        r.seti("cleanup.unlink_lock_errno", unlink(lockFile.c_str()) == 0 ? 0 : errno);
    writeFile(kvPath, r.asKV());
    writeFile(jsonPath, r.asJSON());
    return wrote ? 0 : 2;
}

// ---- 判定 -------------------------------------------------------------------------------------
const char* yesNo(bool b)
{
    return b ? "yes" : "no";
}

std::string flagRows(const spike::ModeEval& i, const spike::ModeEval& o)
{
    struct Row
    {
        const char* name;
        bool spike::ModeEval::*field;
    };
    static const Row rows[] = {
        {"result present", &spike::ModeEval::present},
        {"components registered (A, B)", &spike::ModeEval::allFound},
        {"all instantiated", &spike::ModeEval::allInstantiated},
        {"all initialized", &spike::ModeEval::allInitialized},
        {"probes done (param channel)", &spike::ModeEval::allDone},
        {"shm create + attach ok", &spike::ModeEval::shmOk},
        {"shm denied (EPERM/EACCES)", &spike::ModeEval::shmDenied},
        {"32-char name -> ENAMETOOLONG", &spike::ModeEval::longNameOk},
        {"A1/A2/B1 same pid", &spike::ModeEval::samePid},
        {"instances inside the host process", &spike::ModeEval::inHostProcess},
        {"instances outside the host process", &spike::ModeEval::outOfHostProcess},
        {"host sees plugin segment", &spike::ModeEval::hostSeesPlugin},
        {"plugin sees host segment", &spike::ModeEval::pluginSeesHost},
        {"render counters advance", &spike::ModeEval::liveRender},
        {"lock file EX/EWOULDBLOCK/SH", &spike::ModeEval::lockOk},
        {"HOME/NSHomeDirectory containerized", &spike::ModeEval::homeContained},
        {"ObjC rendezvous visible to B", &spike::ModeEval::objcOk},
    };
    std::string o2;
    for (const Row& row : rows)
        o2 += std::string("| ") + row.name + " | " + yesNo(i.*row.field) + " | " + yesNo(o.*row.field) + " |\n";
    return o2;
}

std::string rawRows(const spike::KV* in, const spike::KV* oop)
{
    static const char* keys[] = {"out.pid",     "out.shmOpen", "out.shmTrunc", "out.shmMap",   "out.shmPeer",
                                 "out.shmLong", "out.shm31",   "out.hostSeg",  "out.lockOpen", "out.lockEx",
                                 "out.lockSh",  "out.homeEnv", "out.nsHome",   "out.kill",     "out.sysctlHost",
                                 "out.objc",    "out.json",    "out.jsonTmp",  "out.sandbox",  "json_found"};
    std::string o = "| key | inproc a1 / a2 / b1 | oop a1 / a2 / b1 |\n|---|---|---|\n";
    auto cell = [](const spike::KV* kv, const std::string& k) {
        if (kv == nullptr)
            return std::string("-");
        std::string s;
        for (const char* i : spike::kInstances)
        {
            const auto it = kv->find(std::string(i) + "." + k);
            if (!s.empty())
                s += " / ";
            s += it == kv->end() || it->second.empty() ? "-" : it->second;
        }
        return s;
    };
    for (const char* k : keys)
        o += std::string("| ") + k + " | " + cell(in, k) + " | " + cell(oop, k) + " |\n";
    return o;
}

int runJudge(const Options& opt)
{
    spike::KV in;
    spike::KV oop;
    std::string text;
    const bool hasIn = readFile(opt.outDir + "/inproc.kv", text) && spike::parseKV(text, in);
    const bool hasOop = readFile(opt.outDir + "/oop.kv", text) && spike::parseKV(text, oop);
    const spike::Verdict v = spike::judge(hasIn ? &in : nullptr, hasOop ? &oop : nullptr);

    std::string j = "{\n  \"verdict\": \"" + v.verdict + "\",\n  \"exit_code\": " + std::to_string(v.exitCode) +
                    ",\n  \"controls\": {\n";
    for (std::size_t k = 0; k < v.controls.size(); ++k)
        j += "    \"" + v.controls[k].first + "\": " + (v.controls[k].second ? "true" : "false") +
             (k + 1 < v.controls.size() ? ",\n" : "\n");
    j += "  },\n  \"notes\": [";
    for (std::size_t k = 0; k < v.notes.size(); ++k)
        j += std::string(k == 0 ? "\n" : ",\n") + "    \"" + jsonEscape(v.notes[k]) + "\"";
    j += v.notes.empty() ? "]\n}\n" : "\n  ]\n}\n";

    std::string md = "## M01 AUHostingService OOP shm spike\n\n";
    md += "**verdict: `" + v.verdict + "`** (judge exit code " + std::to_string(v.exitCode) + ")\n\n";
    if (hasIn)
        md += "macOS " + in["env.os_product_version"] + " (" + in["env.os_build"] + ", " + in["env.machine"] + ")\n\n";
    md += "### Negative controls (in-process run)\n\n| control | held |\n|---|---|\n";
    for (const auto& c : v.controls)
        md += "| " + c.first + " | " + yesNo(c.second) + " |\n";
    md += "\n### Evaluated flags\n\n| flag | inproc | oop |\n|---|---|---|\n" + flagRows(v.inproc, v.oop);
    md += "\n### Raw probe codes\n\n" + rawRows(hasIn ? &in : nullptr, hasOop ? &oop : nullptr);
    md += "\nCodes: 1 ok, 2 alt-ok (attached / EWOULDBLOCK / differs), 3 unexpected, 4 skipped, 5 mismatch, "
          "6 not found, 7 register failed, 100+errno failed (macOS errno: 1 EPERM, 2 ENOENT, 13 EACCES, "
          "35 EWOULDBLOCK, 63 ENAMETOOLONG).\n";
    if (!v.notes.empty())
    {
        md += "\n### Notes\n\n";
        for (const auto& n : v.notes)
            md += "- " + n + "\n";
    }
    writeFile(opt.outDir + "/verdict.json", j);
    // 单独一行纯文本给 workflow 读(macos-oop.yml 的反向注入步),免得脚本去解析上面手拼的 JSON 排版。
    writeFile(opt.outDir + "/verdict.txt", v.verdict + "\n");
    writeFile(opt.outDir + "/summary.md", md);
    std::fputs(md.c_str(), stdout);
    return v.exitCode;
}

void usage()
{
    std::fputs("usage: auhost_spike --mode inproc|oop [--out-dir DIR] [--raw-paths] [--blocks N]\n"
               "                    [--subtype-a XXXX] [--subtype-b XXXX] [--manufacturer XXXX]\n"
               "       auhost_spike --judge [--out-dir DIR]\n"
               "       auhost_spike --self-test\n",
               stderr);
}
} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc)
                return false;
            dst = argv[++i];
            return true;
        };
        std::string tmp;
        if (a == "--mode" && next(tmp) && (tmp == "inproc" || tmp == "oop"))
            opt.mode = tmp;
        else if (a == "--judge")
            opt.mode = "judge";
        else if (a == "--self-test")
            opt.mode = "self-test";
        else if (a == "--out-dir" && next(opt.outDir))
            continue;
        else if (a == "--subtype-a" && next(opt.subtypeA))
            continue;
        else if (a == "--subtype-b" && next(opt.subtypeB))
            continue;
        else if (a == "--manufacturer" && next(opt.manufacturer))
            continue;
        else if (a == "--blocks" && next(tmp))
            opt.blocks = std::atoi(tmp.c_str());
        else if (a == "--raw-paths")
            opt.rawPaths = true;
        else
        {
            usage();
            return 2;
        }
    }
    if (opt.mode == "self-test")
        return spike::selfTest();
    if (opt.mode == "judge")
        return runJudge(opt);
    if (opt.mode != "inproc" && opt.mode != "oop")
    {
        usage();
        return 2;
    }
    if (fourcc(opt.subtypeA) == 0 || fourcc(opt.subtypeB) == 0 || fourcc(opt.manufacturer) == 0 || opt.blocks <= 0)
    {
        usage();
        return 2;
    }
    mkdir(opt.outDir.c_str(), 0755);
    const int rc = runMode(opt);
    std::fflush(nullptr);
    // 进程内模式下两个 JUCE 插件镜像还挂在本进程里;跳过静态析构,别让退出阶段的崩溃把这一步染红。
    _exit(rc);
}
