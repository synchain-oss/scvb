// SPDX-License-Identifier: GPL-3.0-or-later
// SpikeProcessor.cpp —— M01 探针插件。同一份源码编出两个 AU(subtype Sxsa / Sxsb,厂商码 Snch),
// 模拟 SCVB「三个独立二进制」:A 编两份实例、B 一份,看它们在宿主进程内 / AUHostingService 里
// 能不能经 POSIX 共享内存互通。
//
// 探针全部在 prepareToPlay(= AudioUnitInitialize)里跑一次,结果走两条互相独立的通道:
//   ① AU 参数(spike_protocol.h 的 Param 表):不依赖文件系统,沙箱再严也回得来;
//   ② JSON 文件(真实 home 下):写不出来本身就是结论(参数 out.json 记录 errno)。
// processBlock 只做一件事:在段里给自己的槽位计数 +1(纯内存原子操作,不做系统调用),
// 让宿主判断「渲染期间两边映射的是同一块物理内存」。
//
// 这是 spike,不进根工程、不进任何发行包;运行期字符串一律 ASCII(本机 CP936 的 C4819 纪律)。

#include <juce_audio_processors/juce_audio_processors.h>

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <libproc.h>
#include <objc/runtime.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "spike_protocol.h"

// Foundation 的 C 函数(返回 NSString*,与 CFStringRef 免费桥接)。只在这里声明,避免本 TU 变成 ObjC++。
extern "C" CFStringRef NSHomeDirectory(void);
// clang 的 @autoreleasepool 就是这两个函数;NSHomeDirectory 返回 autorelease 对象,探针线程未必有池。
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

#ifndef SPIKE_ROLE_A
#error "SPIKE_ROLE_A must be defined (1 for the A component, 0 for B)"
#endif

namespace
{
constexpr std::uint32_t kCode = static_cast<std::uint32_t>(JucePlugin_PluginCode);

std::string fourccString(std::uint32_t c)
{
    std::string s(4, ' ');
    s[0] = static_cast<char>((c >> 24) & 0xff);
    s[1] = static_cast<char>((c >> 16) & 0xff);
    s[2] = static_cast<char>((c >> 8) & 0xff);
    s[3] = static_cast<char>(c & 0xff);
    return s;
}

int errCode(int e)
{
    return spike::kErrnoBase + e;
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

std::string stripSlash(std::string s)
{
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

std::string cfToString(CFStringRef s)
{
    if (s == nullptr)
        return {};
    std::array<char, 2048> buf{};
    if (CFStringGetCString(s, buf.data(), static_cast<CFIndex>(buf.size()), kCFStringEncodingUTF8))
        return buf.data();
    return {};
}

std::string cfHomeUrlPath()
{
    CFURLRef url = CFCopyHomeDirectoryURL();
    if (url == nullptr)
        return {};
    std::array<char, 2048> buf{};
    std::string out;
    if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buf.data()),
                                         static_cast<CFIndex>(buf.size())))
        out = buf.data();
    CFRelease(url);
    return out;
}

// mkdir -p;返回 0 或第一个真实错误的 errno(已存在的目录不算错,沙箱下 mkdir 已存在目录可能报 EPERM,
// 所以失败后再 stat 一次确认)。
int mkdirs(const std::string& path)
{
    std::size_t pos = 1;
    while (true)
    {
        pos = path.find('/', pos);
        const std::string part = path.substr(0, pos);
        if (!part.empty() && mkdir(part.c_str(), 0700) != 0)
        {
            const int e = errno;
            struct stat st = {};
            if (!(stat(part.c_str(), &st) == 0 && S_ISDIR(st.st_mode)))
                return e;
        }
        if (pos == std::string::npos)
            return 0;
        ++pos;
    }
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
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

// 方案 B 前置:A 在 ObjC 运行时里注册一个类,类方法返回指向 A 镜像内静态数据的指针;
// B 只凭类名找到它并调用。能调通 = 同进程 + 跨二进制可会合。
spike::Rendezvous gRendezvous{};

void* rendezvousImp(id, SEL)
{
    return &gRendezvous;
}

class SpikeProcessor final : public juce::AudioProcessor
{
public:
    SpikeProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        for (int i = 0; i < spike::pCount; ++i)
        {
            auto* p = new juce::AudioParameterFloat(juce::ParameterID{spike::kParamNames[i], 1}, spike::kParamNames[i],
                                                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f);
            params_[static_cast<std::size_t>(i)] = p;
            addParameter(p);
        }
    }

    ~SpikeProcessor() override { teardown(); }

    const juce::String getName() const override { return "SCVB Spike"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) &&
               layouts.getMainInputChannelSet() == out;
    }

    void prepareToPlay(double, int) override
    {
        if (probed_)
            return;
        probed_ = true;
        void* pool = objc_autoreleasePoolPush();
        runProbes();
        objc_autoreleasePoolPop(pool);
    }

    void releaseResources() override {}

    using AudioProcessor::processBlock;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        // 直通(缓冲原样留着);只给自己的槽位计数。
        if (slot_ != nullptr)
            slot_->renders.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::array<juce::AudioParameterFloat*, spike::pCount> params_{};
    bool probed_ = false;
    void* seg_ = nullptr;
    void* hostSeg_ = nullptr;
    int lockFd_ = -1;
    spike::Slot* slot_ = nullptr;
    std::array<int, spike::pCount> codes_{};

    float in(spike::Param p) const { return params_[static_cast<std::size_t>(p)]->getValue(); }

    void put(spike::Param p, int code) { codes_[static_cast<std::size_t>(p)] = code; }

    void publish()
    {
        // 先发全部结果,最后才发 done:宿主以 done=1 作为「其余参数已就位」的信号。
        for (int i = spike::pDone + 1; i < spike::pCount; ++i)
        {
            const auto idx = static_cast<std::size_t>(i);
            const float v = i == spike::pPid ? spike::encodePid(codes_[idx]) : spike::encodeCode(codes_[idx]);
            params_[idx]->setValueNotifyingHost(v);
        }
        params_[static_cast<std::size_t>(spike::pDone)]->setValueNotifyingHost(spike::encodeCode(spike::kOk));
    }

    void teardown()
    {
        slot_ = nullptr;
        if (seg_ != nullptr)
        {
            munmap(seg_, spike::kSegBytes);
            seg_ = nullptr;
        }
        if (hostSeg_ != nullptr)
        {
            munmap(hostSeg_, spike::kHostSegBytes);
            hostSeg_ = nullptr;
        }
        if (lockFd_ >= 0)
        {
            close(lockFd_);
            lockFd_ = -1;
        }
    }

    void runProbes()
    {
        const int hostPidParam = spike::decodePid(in(spike::pHostPid));
        const int token = spike::decodePid(in(spike::pRunToken));
        const int nonce = spike::decodeCode(in(spike::pNonce));
        const pid_t pid = getpid();
        put(spike::pPid, static_cast<int>(pid));
        const std::string home = realHome();

        // ---- ① 30 字符名:创建或附着,映射,读段头 ----
        int shmErrno = 0;
        bool creator = false;
        int fd = shm_open(spike::kSegName, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
        {
            creator = true;
            put(spike::pShmOpen, spike::kOk);
        }
        else if (errno == EEXIST)
        {
            fd = shm_open(spike::kSegName, O_RDWR, 0600);
            if (fd >= 0)
                put(spike::pShmOpen, spike::kAlt);
            else
                put(spike::pShmOpen, errCode(shmErrno = errno));
        }
        else
        {
            put(spike::pShmOpen, errCode(shmErrno = errno));
        }
        long long segSize = -1;
        if (fd >= 0)
        {
            bool sizeOk = true;
            if (creator)
            {
                if (ftruncate(fd, static_cast<off_t>(spike::kSegBytes)) == 0)
                    put(spike::pShmTrunc, spike::kOk);
                else
                {
                    put(spike::pShmTrunc, errCode(errno));
                    sizeOk = false;
                }
            }
            else
            {
                put(spike::pShmTrunc, spike::kSkipped); // 附着方绝不 ftruncate
            }
            struct stat st = {};
            if (fstat(fd, &st) == 0)
                segSize = static_cast<long long>(st.st_size);
            if (sizeOk && segSize < static_cast<long long>(spike::kSegBytes))
            {
                put(spike::pShmMap, spike::kMismatch); // 映射越过对象尾部会 SIGBUS,不碰
                sizeOk = false;
            }
            if (sizeOk)
            {
                void* p = mmap(nullptr, spike::kSegBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED)
                    put(spike::pShmMap, errCode(errno));
                else
                {
                    seg_ = p;
                    put(spike::pShmMap, spike::kOk);
                }
            }
            else if (codes_[spike::pShmMap] == spike::kNotRun)
            {
                put(spike::pShmMap, spike::kSkipped);
            }
            close(fd);
        }
        else
        {
            put(spike::pShmMap, spike::kSkipped);
            put(spike::pShmPeer, spike::kSkipped);
        }
        std::uint64_t seenMagic = 0;
        int seenCreatorPid = 0;
        if (seg_ != nullptr)
        {
            auto* h = static_cast<spike::SegHeader*>(seg_);
            if (creator)
            {
                h->creatorPid.store(static_cast<std::int32_t>(pid), std::memory_order_relaxed);
                h->creatorCode.store(kCode, std::memory_order_relaxed);
                h->magic.store(spike::kSegMagic, std::memory_order_release);
            }
            seenMagic = h->magic.load(std::memory_order_acquire);
            seenCreatorPid = h->creatorPid.load(std::memory_order_relaxed);
            put(spike::pShmPeer, seenMagic == spike::kSegMagic ? spike::kOk : spike::kMismatch);
            const std::uint32_t idx = h->nextSlot.fetch_add(1, std::memory_order_acq_rel);
            if (idx < spike::kMaxSlots)
            {
                spike::Slot& s = h->slots[idx];
                s.pid.store(static_cast<std::int32_t>(pid), std::memory_order_relaxed);
                s.code.store(kCode, std::memory_order_relaxed);
                s.nonce.store(static_cast<std::uint32_t>(nonce), std::memory_order_release);
                slot_ = &s;
                put(spike::pSlot, static_cast<int>(idx) + 1);
            }
        }
        else if (codes_[spike::pShmPeer] == spike::kNotRun)
        {
            put(spike::pShmPeer, spike::kSkipped);
        }

        // ---- ② 负对照:32 字符名必须 ENAMETOOLONG;31 字符只记录 ----
        const int fdLong = shm_open(spike::kSegNameTooLong, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fdLong >= 0)
        {
            close(fdLong);
            shm_unlink(spike::kSegNameTooLong);
            put(spike::pShmLong, spike::kUnexpected);
        }
        else
        {
            put(spike::pShmLong, errno == ENAMETOOLONG ? spike::kOk : errCode(errno));
        }
        const int fd31 = shm_open(spike::kSegName31, O_CREAT | O_RDWR, 0600);
        if (fd31 >= 0)
        {
            close(fd31);
            shm_unlink(spike::kSegName31);
            put(spike::pShm31, spike::kOk);
        }
        else
        {
            put(spike::pShm31, errCode(errno));
        }

        // ---- 宿主建的段(宿主 -> 插件方向)----
        int hostPidFromSeg = 0;
        const int hfd = shm_open(spike::kHostSegName, O_RDONLY, 0);
        if (hfd < 0)
        {
            put(spike::pHostSeg, errCode(errno));
        }
        else
        {
            void* p = mmap(nullptr, spike::kHostSegBytes, PROT_READ, MAP_SHARED, hfd, 0);
            if (p == MAP_FAILED)
                put(spike::pHostSeg, errCode(errno));
            else
            {
                hostSeg_ = p;
                const auto* hh = static_cast<const spike::HostHeader*>(p);
                const bool magicOk = hh->magic.load(std::memory_order_acquire) == spike::kHostMagic;
                hostPidFromSeg = hh->hostPid.load(std::memory_order_relaxed);
                const bool tokenOk = static_cast<int>(hh->token.load(std::memory_order_relaxed)) == token;
                put(spike::pHostSeg, magicOk && tokenOk ? spike::kOk : spike::kMismatch);
            }
            close(hfd);
        }

        // ---- ③ 真实 home 下的锁文件 + flock;HOME / NSHomeDirectory 是否被容器化 ----
        std::string lockFile;
        if (home.empty())
        {
            put(spike::pLockOpen, spike::kSkipped);
            put(spike::pLockEx, spike::kSkipped);
            put(spike::pLockSh, spike::kSkipped);
        }
        else
        {
            lockFile = spike::lockPath(home, token);
            const int me = mkdirs(spike::lockDir(home));
            if (me != 0)
            {
                put(spike::pLockOpen, errCode(me));
            }
            else
            {
                lockFd_ = open(lockFile.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
                put(spike::pLockOpen, lockFd_ >= 0 ? spike::kOk : errCode(errno));
            }
            if (lockFd_ >= 0)
            {
                if (flock(lockFd_, LOCK_EX | LOCK_NB) == 0)
                    put(spike::pLockEx, spike::kOk);
                else
                    put(spike::pLockEx, errno == EWOULDBLOCK ? spike::kAlt : errCode(errno));
                // 拿到 EX 的降成 SH;没拿到的直接要 SH(BSD flock 的转换不是原子的,spike 只看结果)。
                put(spike::pLockSh, flock(lockFd_, LOCK_SH | LOCK_NB) == 0 ? spike::kOk : errCode(errno));
            }
            else
            {
                put(spike::pLockEx, spike::kSkipped);
                put(spike::pLockSh, spike::kSkipped);
            }
        }
        const char* envHome = std::getenv("HOME");
        const std::string nsHome = stripSlash(cfToString(NSHomeDirectory()));
        const std::string cfHome = stripSlash(cfHomeUrlPath());
        const std::string pwHome = stripSlash(home);
        if (pwHome.empty())
            put(spike::pHomeEnv, spike::kSkipped);
        else if (envHome == nullptr)
            put(spike::pHomeEnv, spike::kUnexpected);
        else
            put(spike::pHomeEnv, stripSlash(envHome) == pwHome ? spike::kOk : spike::kAlt);
        if (pwHome.empty())
            put(spike::pNsHome, spike::kSkipped);
        else if (nsHome.empty())
            put(spike::pNsHome, spike::kUnexpected);
        else
            put(spike::pNsHome, nsHome == pwHome ? spike::kOk : spike::kAlt);

        // ---- ④ 进程身份 ----
        std::array<char, PROC_PIDPATHINFO_MAXSIZE> exe{};
        if (proc_pidpath(pid, exe.data(), static_cast<std::uint32_t>(exe.size())) <= 0)
            exe[0] = '\0';
        const char* prog = getprogname();
        const char* sandboxId = std::getenv("APP_SANDBOX_CONTAINER_ID");
        put(spike::pSandboxEnv, sandboxId != nullptr ? spike::kOk : spike::kAlt);

        // ---- ⑤ 宿主探活 ----
        const int hostPid = hostPidParam > 0 ? hostPidParam : hostPidFromSeg;
        if (hostPid <= 0)
        {
            put(spike::pKill, spike::kSkipped);
            put(spike::pSysctlHost, spike::kSkipped);
        }
        else
        {
            put(spike::pKill, kill(static_cast<pid_t>(hostPid), 0) == 0 ? spike::kOk : errCode(errno));
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, hostPid};
            struct kinfo_proc kp = {};
            std::size_t len = sizeof kp;
            if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0)
                put(spike::pSysctlHost, errCode(errno));
            else
                put(spike::pSysctlHost,
                    len == sizeof kp && kp.kp_proc.p_pid == hostPid ? spike::kOk : spike::kMismatch);
        }

        // ---- ⑥ ObjC 运行时会合(方案 B 前置)----
        int rvPid = 0;
        std::uint32_t rvCode = 0;
#if SPIKE_ROLE_A
        if (objc_lookUpClass(spike::kRendezvousClass) != nullptr)
        {
            put(spike::pObjc, spike::kAlt); // 同进程里另一个 A 实例已注册
        }
        else
        {
            Class base = objc_lookUpClass("NSObject");
            Class cls = base != nullptr ? objc_allocateClassPair(base, spike::kRendezvousClass, 0) : nullptr;
            if (cls == nullptr)
            {
                put(spike::pObjc, spike::kRegisterFailed);
            }
            else
            {
                gRendezvous = spike::Rendezvous{spike::kRendezvousMagic, static_cast<std::int32_t>(pid), kCode};
                class_addMethod(object_getClass(reinterpret_cast<id>(cls)),
                                sel_registerName(spike::kRendezvousSelector), reinterpret_cast<IMP>(&rendezvousImp),
                                "^v@:");
                objc_registerClassPair(cls);
                put(spike::pObjc, spike::kOk);
            }
        }
#else
        if (Class cls = objc_lookUpClass(spike::kRendezvousClass); cls == nullptr)
        {
            put(spike::pObjc, spike::kNotFound);
        }
        else
        {
            Method m = class_getClassMethod(cls, sel_registerName(spike::kRendezvousSelector));
            if (m == nullptr)
            {
                put(spike::pObjc, spike::kMismatch);
            }
            else
            {
                using Fn = void* (*)(id, SEL);
                auto fn = reinterpret_cast<Fn>(method_getImplementation(m));
                const auto* rv = static_cast<const spike::Rendezvous*>(
                    fn(reinterpret_cast<id>(cls), sel_registerName(spike::kRendezvousSelector)));
                if (rv != nullptr && rv->magic == spike::kRendezvousMagic)
                {
                    rvPid = rv->pid;
                    rvCode = rv->code;
                    put(spike::pObjc, spike::kOk);
                }
                else
                {
                    put(spike::pObjc, spike::kMismatch);
                }
            }
        }
#endif

        // ---- JSON(第二条通道)----
        std::string j = "{\n";
        auto kvs = [&j](const char* k, const std::string& v, bool last = false) {
            j += "  \"" + std::string(k) + "\": \"" + jsonEscape(v) + "\"" + (last ? "\n" : ",\n");
        };
        auto kvi = [&j](const char* k, long long v) {
            j += "  \"" + std::string(k) + "\": " + std::to_string(v) + ",\n";
        };
        kvs("subtype", fourccString(kCode));
        kvs("role", SPIKE_ROLE_A ? "A" : "B");
        kvi("nonce", nonce);
        kvi("token", token);
        kvi("pid", static_cast<long long>(pid));
        kvi("ppid", static_cast<long long>(getppid()));
        kvi("uid", static_cast<long long>(getuid()));
        kvs("progname", prog != nullptr ? prog : "");
        kvs("exe_path", exe.data());
        kvs("env_HOME", envHome != nullptr ? envHome : "<unset>");
        kvs("pw_dir", home);
        kvs("NSHomeDirectory", nsHome);
        kvs("CFCopyHomeDirectoryURL", cfHome);
        const char* tmpdir = std::getenv("TMPDIR");
        kvs("env_TMPDIR", tmpdir != nullptr ? tmpdir : "<unset>");
        kvs("env_APP_SANDBOX_CONTAINER_ID", sandboxId != nullptr ? sandboxId : "<unset>");
        kvi("host_pid_param", hostPidParam);
        kvi("host_pid_from_segment", hostPidFromSeg);
        kvs("shm_name", spike::kSegName);
        kvi("shm_creator", creator ? 1 : 0);
        kvi("shm_errno", shmErrno);
        kvi("shm_size_fstat", segSize);
        kvi("shm_seen_creator_pid", seenCreatorPid);
        kvs("shm_seen_magic_ok", seenMagic == spike::kSegMagic ? "yes" : "no");
        kvs("lock_file", lockFile);
        kvi("rendezvous_pid", rvPid);
        kvs("rendezvous_code", rvCode != 0 ? fourccString(rvCode) : "");
        for (int i = spike::pDone + 1; i < spike::pCount; ++i)
            if (i != spike::pJson && i != spike::pJsonTmp)
                kvi(spike::kParamNames[i], codes_[static_cast<std::size_t>(i)]);
        kvs("codes_legend",
            "1 ok, 2 alt-ok, 3 unexpected, 4 skipped, 5 mismatch, 6 not found, 7 register failed, "
            "100+errno failed",
            true);
        j += "}\n";

        auto writeFile = [&j](const std::string& path) -> int {
            FILE* f = std::fopen(path.c_str(), "w");
            if (f == nullptr)
                return errno;
            const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
            const int e = ok ? 0 : errno;
            if (std::fclose(f) != 0 && e == 0)
                return errno != 0 ? errno : EIO;
            return ok ? 0 : (e != 0 ? e : EIO);
        };
        int je = home.empty() ? ENOENT : mkdirs(spike::jsonDir(home));
        if (je == 0)
            je = writeFile(spike::jsonPath(home, token, nonce, fourccString(kCode)));
        put(spike::pJson, je == 0 ? spike::kOk : errCode(je));
        if (je == 0)
        {
            put(spike::pJsonTmp, spike::kSkipped);
        }
        else
        {
            const std::string tdir = tmpdir != nullptr ? stripSlash(tmpdir) : std::string("/tmp");
            const int te =
                writeFile(tdir + "/scvb-spike-" + std::to_string(token) + "-" + std::to_string(nonce) + ".json");
            put(spike::pJsonTmp, te == 0 ? spike::kOk : errCode(te));
        }

        publish();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpikeProcessor)
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpikeProcessor();
}
