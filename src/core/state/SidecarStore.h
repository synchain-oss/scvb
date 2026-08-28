// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T21:sidecar 读写 + owner.lock + copy-on-write + 8MB/6MB 回滞(04 §5.4/§5.5/§5.6)。
// 纯 C++17 + std::filesystem,无 JUCE(ADR-011)。目录布局 <base>/sessions/<GUID>/,
// 含 features.bin.gz / manifest.json / owner.lock。

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scvb::state
{

// ADR-007 冻结阈值 + 04 §5.3 回滞(压缩后字节)。
inline constexpr std::uint64_t kSidecarThresholdBytes = 8ull * 1024u * 1024u; // >8MB → sidecar
inline constexpr std::uint64_t kReembedThresholdBytes = 6ull * 1024u * 1024u; // <6MB → 收回内嵌

// owner.lock 判活(04 §5.5):pid 存在 ∧ 心跳 < 30s。
inline constexpr std::int64_t kOwnerLockAliveHeartbeatMs = 30000;

// [SL-233] owner.lock 心跳刷新周期(STATE_SCHEMA §4.3「每 10s 由消息线程刷新」)。
// 与判活窗口的 3 倍余量:漏掉两拍(GUI 卡顿 / 磁盘忙)仍判活,不会把自己的租约丢掉。
inline constexpr std::int64_t kOwnerLockRefreshIntervalMs = 10000;
static_assert(kOwnerLockRefreshIntervalMs * 2 < kOwnerLockAliveHeartbeatMs,
              "刷新周期必须留够重试余量,否则单次刷新失败就被别的实例判死");

inline constexpr std::string_view kSidecarFeaturesFilename = "features.bin.gz";
inline constexpr std::string_view kSidecarManifestFilename = "manifest.json";
inline constexpr std::string_view kSidecarOwnerLockFilename = "owner.lock";

// ---- 通用工具(JUCE-free)----
// SHA-256(FIPS 180-4,自实现)。返回 32 字节摘要。
std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);

// UUID v4(36 字符 dashed)。测试可注入固定值以保确定性。
std::string generateSessionGuid();

// 当前 epoch 毫秒 / UTC ISO8601(心跳时间戳;std::chrono::system_clock,免 Win32)。
std::int64_t epochMsNow();
std::string iso8601UtcNow();

// 进程身份(owner.lock / copy-on-write 判定)。
struct ProcessIdentity
{
    std::uint64_t pid = 0;
    std::uint64_t processStartEpochMs = 0;
    std::string hostName; // manifest hostName 元数据
};
// 当前进程身份(Windows 用 GetCurrentProcessId/GetProcessTimes/GetComputerNameA;非 Windows 退化为 pid=0)。
ProcessIdentity currentProcessIdentity();

// owner.lock(04 §5.5):{pid, processStartTime, heartbeatIso8601}。
// heartbeatEpochMs 与 heartbeatIso8601 并存:前者供判活(免解析 ISO8601),后者满足规格/可读。
struct OwnerLock
{
    std::uint64_t pid = 0;
    std::uint64_t processStartEpochMs = 0;
    std::uint64_t heartbeatEpochMs = 0;
    std::string heartbeatIso8601;
};
// 判活:pid != 0 ∧ (nowEpochMs - heartbeatEpochMs) < kOwnerLockAliveHeartbeatMs。
bool isOwnerLockAlive(const OwnerLock& lock, std::int64_t nowEpochMs);

class SidecarStore
{
public:
    // baseDir = <appdata>/Synchain/SCVB(Windows: %APPDATA%\Synchain\SCVB)。
    explicit SidecarStore(std::filesystem::path baseDir);

    const std::filesystem::path& baseDir() const noexcept { return baseDir_; }

    // sidecar 目录:<base>/sessions/<GUID>/。
    std::filesystem::path sessionDir(const std::string& guid) const;
    std::filesystem::path featuresFile(const std::string& guid) const;

    // 原子写 sidecar(04 §5.4)。写时序约定:features.bin.gz → manifest.json → owner.lock,
    // 每个文件均 .tmp→rename 原子替换(崩溃/失败只残留 .tmp,绝不留下半写文件);
    // 调用方必须先 copyOnWriteIfNeeded() 确认无他人活锁再写本 GUID。返回 false = 写失败(特征丢弃,04 §5.3)。
    bool write(const std::string& guid, const std::uint8_t* gzData, std::size_t gzSize, std::uint32_t channelCount,
               std::uint32_t codecVer, const ProcessIdentity& self);

    // 读 sidecar 文件(gzip 字节);缺失/失败返回 false。
    bool read(const std::string& guid, std::vector<std::uint8_t>& gzOut) const;

    // 删除 sidecar 目录(04 §5.3:数据已随工程内嵌,防双源分叉)。
    void remove(const std::string& guid);

    // owner.lock 读写。
    bool writeOwnerLock(const std::string& guid, const OwnerLock& lock);
    bool readOwnerLock(const std::string& guid, OwnerLock& lock) const;

    // [SL-233] owner.lock 续租(STATE_SCHEMA §4.3:sidecar 模式下每 10s 由消息线程刷新)。
    // 只改心跳时间戳,pid / processStartEpochMs 原样留在盘上。三道闸,任一不过即返回 false 且**不写盘**:
    //   ① guid 非法 → 防目录穿越(与本类其余入口同一道);
    //   ② 盘上没有 owner.lock → **不新建**。没写过 sidecar 就没有租约可续;新建等于凭空宣示占有
    //      一份自己没写过的外部特征,而 copyOnWriteIfNeeded 正是靠「谁写谁持锁」判他人的;
    //   ③ 锁不归本进程(pid + processStartEpochMs 双元组,与 CoW 同口径防 PID 复用)→ **绝不覆盖**。
    //      那是别人的活租约,也是 CoW 唯一的证据;把它改成自己的就等于把别人的 sidecar 抢过来。
    // 过期但仍归本进程的锁**允许**续租:所有权没变(能抢走它的只有「别人写了一次 sidecar」,
    // 那种情况 pid 已经不是自己,走 ③ 拒掉)。
    // 返回 true = 心跳已落盘。调用方 = 消息线程(小文件、0.1Hz;音频线程一律禁止)。
    bool refreshOwnerLock(const std::string& guid, const ProcessIdentity& self, std::int64_t nowEpochMs);

    // copy-on-write(04 §5.6):owner.lock 活 ∧ 非己(pid + processStartEpochMs 双元组,防 PID 复用)→
    // 生成 newGuid、复制目录。返回 true = 已 CoW(newGuid 已设);false = 无需 CoW(无活锁/锁为己有/复制失败)。
    bool copyOnWriteIfNeeded(const std::string& guid, const ProcessIdentity& self, std::int64_t nowEpochMs,
                             std::string& newGuid);

    // 回滞判定(04 §5.3):当前是否走 sidecar。
    //   未走 sidecar:gz > 8MB → sidecar;已走 sidecar:gz < 6MB → 收回内嵌。
    static bool shouldUseSidecar(std::uint64_t gzBytes, bool currentlySidecar);

private:
    std::filesystem::path baseDir_;
};

} // namespace scvb::state
