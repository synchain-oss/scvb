// SPDX-License-Identifier: GPL-3.0-or-later
// spike_protocol.h —— M01 进程外 shm spike:宿主(auhost_spike.cpp)与探针插件(SpikeProcessor.cpp)共用的常量与布局。
//
// 这是 spike,不是产品代码:段布局只在本目录内有效,与 docs/IPC_CONTRACT.md 的冻结布局无关。
// 段名借用冻结前缀 `SynchainSCVB.v1.` 只为让名字长度与将来 POSIX 后端的真实名字同量级
// (macOS 的 PSHMNAMLEN = 31,含前导 '/';30 字符是计划给产品名留的上限)。
// 本文件只放两侧必须逐字一致的东西;两侧各写一份的话,改一边忘一边的失效形态是「探针全部
// 读不到」,会被误判成 OOP 被拒 —— 所以集中在这里。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace spike
{
// ---- 段名 -------------------------------------------------------------------------------------
// 30 字符:探针插件创建 / 附着、宿主只读核对的那一段。
inline constexpr const char* kSegName = "/SynchainSCVB.v1.g8.spike.ch15";
// 32 字符:负对照,必须得到 ENAMETOOLONG(否则说明「名字上限」这条前提不成立,M05 的设计要重看)。
inline constexpr const char* kSegNameTooLong = "/SynchainSCVB.v1.g8.spike.ch15xy";
// 31 字符:边界值,只记录不判定。
inline constexpr const char* kSegName31 = "/SynchainSCVB.v1.g8.spike.ch15x";
// 30 字符:宿主创建、插件只读打开的那一段(宿主 -> 插件方向的可见性)。
inline constexpr const char* kHostSegName = "/SynchainSCVB.v1.g8.spike.host";

constexpr std::size_t constLen(const char* s)
{
    std::size_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}
static_assert(constLen(kSegName) == 30, "kSegName must be 30 chars");
static_assert(constLen(kSegNameTooLong) == 32, "kSegNameTooLong must be 32 chars");
static_assert(constLen(kSegName31) == 31, "kSegName31 must be 31 chars");
static_assert(constLen(kHostSegName) == 30, "kHostSegName must be 30 chars");

inline constexpr std::size_t kSegBytes = 4u * 1024u * 1024u; // 约 4 MB,与产品 audio ring 同量级
inline constexpr std::size_t kHostSegBytes = 16384u; // arm64 一页
inline constexpr std::uint64_t kSegMagic = 0x314B505342564353ull; // "SCVBSPK1"(小端)
inline constexpr std::uint64_t kHostMagic = 0x3154534842564353ull; // "SCVBHST1"(小端)
inline constexpr std::uint64_t kRendezvousMagic = 0x3156524842564353ull; // "SCVBHRV1"
inline constexpr std::uint32_t kMaxSlots = 16;

// ObjC 运行时会合类名(方案 B 的前置条件:A 注册、B 查找)。
inline constexpr const char* kRendezvousClass = "SynchainSCVBSpikeRendezvousV1";
inline constexpr const char* kRendezvousSelector = "spikeRendezvous";

// ---- 段布局 -----------------------------------------------------------------------------------
// ftruncate 出来的段全零,所以「全零 = 还没人写过」;创建者最后才以 release 写 magic。
struct Slot
{
    std::atomic<std::int32_t> pid;
    std::atomic<std::uint32_t> code; // 插件 subtype 四字码
    std::atomic<std::uint32_t> nonce; // 宿主给每个实例的编号(1/2/3)
    std::atomic<std::uint32_t> reserved;
    std::atomic<std::uint64_t> renders; // processBlock 计数:宿主据此判断「渲染期间两边是同一块物理内存」
};

struct SegHeader
{
    std::atomic<std::uint64_t> magic;
    std::atomic<std::int32_t> creatorPid;
    std::atomic<std::uint32_t> creatorCode;
    std::atomic<std::uint32_t> nextSlot;
    std::atomic<std::uint32_t> reserved;
    Slot slots[kMaxSlots];
};

struct HostHeader
{
    std::atomic<std::uint64_t> magic;
    std::atomic<std::int32_t> hostPid;
    std::atomic<std::uint32_t> token;
};

struct Rendezvous
{
    std::uint64_t magic;
    std::int32_t pid;
    std::uint32_t code;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free");
static_assert(std::atomic<std::int32_t>::is_always_lock_free, "32-bit atomics must be lock-free");
static_assert(sizeof(SegHeader) <= kSegBytes, "header must fit");
static_assert(sizeof(HostHeader) <= kHostSegBytes, "host header must fit");

// ---- 参数通道(不依赖文件系统)---------------------------------------------------------------
// 全部是 0..1 的连续参数(JUCE AU wrapper 对连续参数 maxValue = 1,GetParameter 原样回传归一化值)。
// 编码:状态码 = round(v * 1024);pid / token = round(v * 131072)(macOS pid 上限 99998 < 2^17,
// 两种缩放都是 2 的幂,float 能精确表示,不经任何舍入)。
inline constexpr float kCodeScale = 1024.0f;
inline constexpr float kPidScale = 131072.0f;

enum Param : int
{
    pHostPid = 0, // 宿主写入:宿主 pid
    pRunToken, // 宿主写入:本次运行的随机 token(文件名与锁名用)
    pNonce, // 宿主写入:实例编号 1/2/3
    pDone, // 插件写:全部探针跑完 = 1(最后写)
    pPid, // 插件写:getpid()
    pShmOpen, // ① shm_open:1 创建 / 2 附着已有 / 100+errno
    pShmTrunc, // ① ftruncate:1 成功 / 4 附着方不做 / 100+errno
    pShmMap, // ① mmap:1 成功 / 4 没走到 / 5 段尺寸不对 / 100+errno
    pShmPeer, // ① 段头 magic 可读:1 是 / 4 没走到 / 5 不对
    pShmLong, // ② 32 字符名:1 = ENAMETOOLONG(期望)/ 3 竟然成功 / 100+errno
    pShm31, // ② 31 字符名(只记录):1 成功 / 100+errno
    pHostSeg, // 宿主段可读且 magic+token 对得上:1 / 5 对不上 / 100+errno
    pLockOpen, // ③ 真实 home 下建锁文件:1 / 100+errno
    pLockEx, // ③ flock(LOCK_EX|LOCK_NB):1 拿到 / 2 EWOULDBLOCK(别的实例持有)/ 4 / 100+errno
    pLockSh, // ③ flock(LOCK_SH|LOCK_NB):1 持有 / 4 / 100+errno
    pHomeEnv, // ③ HOME 环境变量:1 = 真实 home / 2 不同(容器化)/ 3 未设 / 4 取不到真实 home
    pNsHome, // ③ NSHomeDirectory():1 = 真实 home / 2 不同 / 3 nil / 4 取不到真实 home
    pKill, // ⑤ kill(宿主 pid, 0):1 = 0 / 4 不知道宿主 pid / 100+errno
    pObjc, // ⑥ A:1 注册了 / 2 已被别的 A 实例注册 / 7 注册失败;B:1 找到且可调用 / 6 找不到 / 5 不对
    pJson, // JSON 文件写进真实 home:1 / 100+errno
    pJsonTmp, // 写不进真实 home 时改写 $TMPDIR:1 / 4 不需要 / 100+errno
    pSandboxEnv, // APP_SANDBOX_CONTAINER_ID:1 有 / 2 没有
    pSysctlHost, // sysctl(KERN_PROC_PID, 宿主 pid):1 / 4 / 100+errno(只记录)
    pSlot, // 段里分到的槽位 + 1(0 = 没分到)
    pCount
};

inline constexpr const char* kParamNames[pCount] = {
    "in.hostPid",   "in.runToken", "in.nonce",    "out.done",    "out.pid",        "out.shmOpen",
    "out.shmTrunc", "out.shmMap",  "out.shmPeer", "out.shmLong", "out.shm31",      "out.hostSeg",
    "out.lockOpen", "out.lockEx",  "out.lockSh",  "out.homeEnv", "out.nsHome",     "out.kill",
    "out.objc",     "out.json",    "out.jsonTmp", "out.sandbox", "out.sysctlHost", "out.slot",
};

// 状态码
inline constexpr int kNotRun = 0;
inline constexpr int kOk = 1;
inline constexpr int kAlt = 2;
inline constexpr int kUnexpected = 3;
inline constexpr int kSkipped = 4;
inline constexpr int kMismatch = 5;
inline constexpr int kNotFound = 6;
inline constexpr int kRegisterFailed = 7;
inline constexpr int kErrnoBase = 100;

inline float encodeCode(int c)
{
    return static_cast<float>(c) / kCodeScale;
}
inline int decodeCode(float v)
{
    return static_cast<int>(v * kCodeScale + 0.5f);
}
inline float encodePid(int pid)
{
    return static_cast<float>(pid) / kPidScale;
}
inline int decodePid(float v)
{
    return static_cast<int>(v * kPidScale + 0.5f);
}

// 文件位置(都相对 getpwuid 拿到的真实 home;不读 HOME,HOME 本身就是被测对象)。
inline std::string lockDir(const std::string& realHome)
{
    return realHome + "/Library/Application Support/Synchain/SCVB/ipc";
}
inline std::string lockPath(const std::string& realHome, int token)
{
    return lockDir(realHome) + "/spike-" + std::to_string(token) + ".lock";
}
inline std::string jsonDir(const std::string& realHome)
{
    return realHome + "/Library/Logs/Synchain/SCVB-spike";
}
inline std::string jsonPath(const std::string& realHome, int token, int nonce, const std::string& subtype)
{
    return jsonDir(realHome) + "/spike-" + std::to_string(token) + "-" + std::to_string(nonce) + "-" + subtype +
           ".json";
}
} // namespace spike
