// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/SidecarStore.h"

#include "state/FeaturesCodec.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// T21:sidecar 读写 + owner.lock + copy-on-write(04 §5.4/§5.5/§5.6)。JUCE-free。
namespace scvb::state
{
namespace
{

// ---- SHA-256(FIPS 180-4)----
constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

void sha256Compress(std::array<std::uint32_t, 8>& h, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<std::uint32_t>(block[4u * i]) << 24) |
               (static_cast<std::uint32_t>(block[4u * i + 1u]) << 16) |
               (static_cast<std::uint32_t>(block[4u * i + 2u]) << 8) | static_cast<std::uint32_t>(block[4u * i + 3u]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = hh + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

std::string toHex(const std::array<std::uint8_t, 32>& d)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (std::size_t i = 0; i < 32u; ++i)
    {
        s[2u * i] = kHex[d[i] >> 4];
        s[2u * i + 1u] = kHex[d[i] & 0xfu];
    }
    return s;
}

// ---- 文本/JSON 辅助 ----
std::string escapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
    {
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '\"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out += ch;
            }
        }
    }
    return out;
}

// 原子写:先写 <path>.tmp 再 rename 覆盖目标(MSVC MoveFileExW REPLACE_EXISTING / POSIX rename)。
// 崩溃/失败只可能残留 .tmp 文件,绝不会留下半写的 features.bin.gz / manifest.json / owner.lock。
bool atomicWriteBytes(const std::filesystem::path& p, const void* data, std::size_t size)
{
    const std::filesystem::path tmp = std::filesystem::path(p.string() + ".tmp");
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        if (size > 0u)
            ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!ofs.good())
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec); // 原子覆盖,不先 remove(防 remove↔rename 间隙丢失旧文件)
    return !ec;
}

bool atomicWriteText(const std::filesystem::path& p, const std::string& s)
{
    return atomicWriteBytes(p, s.data(), s.size());
}

bool parseU64(const std::string& s, std::uint64_t& out)
{
    if (s.empty())
        return false;
    std::uint64_t v = 0;
    for (char ch : s)
    {
        if (ch < '0' || ch > '9')
            return false;
        v = v * 10u + static_cast<std::uint64_t>(ch - '0');
    }
    out = v;
    return true;
}

} // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size)
{
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    if (p == nullptr)
        p = reinterpret_cast<const std::uint8_t*>(""); // 空输入的非空占位指针,避免 nullptr 算术
    const std::uint64_t bitLen = static_cast<std::uint64_t>(size) * 8u;

    std::size_t pos = 0;
    while (pos + 64u <= size)
    {
        sha256Compress(h, p + pos);
        pos += 64u;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = size - pos;
    std::memcpy(tail.data(), p + pos, rem);
    tail[rem] = 0x80;
    const std::size_t tailLen = (rem < 56u) ? 64u : 128u;
    for (int i = 0; i < 8; ++i)
        tail[tailLen - 1u - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((bitLen >> (8u * i)) & 0xffu);

    for (std::size_t block = 0; block < tailLen; block += 64u)
        sha256Compress(h, tail.data() + block);

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
    {
        out[4u * i] = static_cast<std::uint8_t>((h[i] >> 24) & 0xffu);
        out[4u * i + 1u] = static_cast<std::uint8_t>((h[i] >> 16) & 0xffu);
        out[4u * i + 2u] = static_cast<std::uint8_t>((h[i] >> 8) & 0xffu);
        out[4u * i + 3u] = static_cast<std::uint8_t>(h[i] & 0xffu);
    }
    return out;
}

std::string generateSessionGuid()
{
    std::array<std::uint8_t, 16> b{};
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (auto& v : b)
        v = static_cast<std::uint8_t>(gen() & 0xffu);

    b[6] = static_cast<std::uint8_t>((b[6] & 0x0fu) | 0x40u); // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3fu) | 0x80u); // variant 10xx

    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(36, '-');
    std::size_t o = 0;
    for (std::size_t i = 0; i < 16u; ++i)
    {
        if (i == 4u || i == 6u || i == 8u || i == 10u)
            ++o;
        s[o++] = kHex[b[i] >> 4];
        s[o++] = kHex[b[i] & 0xfu];
    }
    return s;
}

std::int64_t epochMsNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string iso8601UtcFromEpochMs(std::int64_t epochMs)
{
    // [SL-233] 由**同一个**时间戳格式化,而不是各自取一次「现在」:owner.lock 的 heartbeatEpochMs
    // 与 heartbeatIso8601 是同一个心跳的两种写法(前者判活、后者可读),分头取会在注入时钟的
    // 单测里写出一份自相矛盾的锁文件(PR #154 复审【建议】3)。
    const std::time_t t = static_cast<std::time_t>(epochMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string iso8601UtcNow()
{
    return iso8601UtcFromEpochMs(epochMsNow());
}

ProcessIdentity currentProcessIdentity()
{
    ProcessIdentity id;
#ifdef _WIN32
    id.pid = static_cast<std::uint64_t>(::GetCurrentProcessId());
    FILETIME creation{};
    FILETIME exitT{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exitT, &kernel, &user) != 0)
    {
        ULARGE_INTEGER ul;
        ul.LowPart = creation.dwLowDateTime;
        ul.HighPart = creation.dwHighDateTime;
        // FILETIME = 100ns 自 1601-01-01;减 1601→1970 偏移再 /10000 → epoch ms。
        constexpr std::uint64_t kEpochOffset100ns = 116444736000000000ull;
        id.processStartEpochMs = (ul.QuadPart - kEpochOffset100ns) / 10000ull;
    }
    char host[256]{};
    DWORD hostLen = static_cast<DWORD>(sizeof(host));
    if (::GetComputerNameA(host, &hostLen) != 0)
        id.hostName.assign(host, hostLen);
#endif
    return id;
}

bool isOwnerLockAlive(const OwnerLock& lock, std::int64_t nowEpochMs)
{
    if (lock.pid == 0u)
        return false;
    return (nowEpochMs - static_cast<std::int64_t>(lock.heartbeatEpochMs)) < kOwnerLockAliveHeartbeatMs;
}

SidecarStore::SidecarStore(std::filesystem::path baseDir) : baseDir_(std::move(baseDir)) {}

std::filesystem::path SidecarStore::sessionDir(const std::string& guid) const
{
    return baseDir_ / "sessions" / guid;
}

std::filesystem::path SidecarStore::featuresFile(const std::string& guid) const
{
    return sessionDir(guid) / std::string(kSidecarFeaturesFilename);
}

bool SidecarStore::write(const std::string& guid, const std::uint8_t* gzData, std::size_t gzSize,
                         std::uint32_t channelCount, std::uint32_t codecVer, const ProcessIdentity& self)
{
    if (gzData == nullptr && gzSize != 0u)
        return false;
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越

    const auto dir = sessionDir(guid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;

    // 写时序约定(04 §5.4):features.bin.gz → manifest.json → owner.lock,每个文件都 .tmp→rename 原子替换;
    // 崩溃只可能残留 .tmp 文件,旧文件在 rename 前始终完好。调用方必须先 copyOnWriteIfNeeded()
    // 确认无他人活锁,再写本 GUID(CoW 正确性依赖该调用时序)。
    const auto featPath = dir / std::string(kSidecarFeaturesFilename);
    if (!atomicWriteBytes(featPath, gzData, gzSize))
        return false;

    const auto digest = sha256(gzData, gzSize);
    const std::string now = iso8601UtcNow();
    std::string manifest = "{\"schemaVersion\":1,\"codecVer\":" + std::to_string(codecVer);
    manifest += ",\"createdAt\":\"" + now + "\",\"savedAt\":\"" + now + "\"";
    manifest += ",\"sha256\":\"" + toHex(digest) + "\"";
    manifest += ",\"bytes\":" + std::to_string(gzSize);
    manifest += ",\"channelCount\":" + std::to_string(channelCount);
    manifest += ",\"hostName\":\"" + escapeJson(self.hostName) + "\"}";
    if (!atomicWriteText(dir / std::string(kSidecarManifestFilename), manifest))
        return false;

    OwnerLock lock;
    lock.pid = self.pid;
    lock.processStartEpochMs = self.processStartEpochMs;
    lock.heartbeatEpochMs = epochMsNow();
    lock.heartbeatIso8601 = iso8601UtcNow();
    return writeOwnerLock(guid, lock);
}

bool SidecarStore::read(const std::string& guid, std::vector<std::uint8_t>& gzOut) const
{
    gzOut.clear();
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越
    std::ifstream ifs(featuresFile(guid), std::ios::binary | std::ios::ate);
    if (!ifs)
        return false;
    const std::streamoff end = ifs.tellg();
    if (end < 0)
        return false;
    ifs.seekg(0, std::ios::beg);
    const std::size_t sz = static_cast<std::size_t>(end);
    gzOut.resize(sz);
    if (sz > 0u)
        ifs.read(reinterpret_cast<char*>(gzOut.data()), static_cast<std::streamsize>(sz));
    return !ifs.fail();
}

void SidecarStore::remove(const std::string& guid)
{
    if (!isValidSessionGuid(guid))
        return; // 防目录穿越
    std::error_code ec;
    std::filesystem::remove_all(sessionDir(guid), ec);
}

bool SidecarStore::writeOwnerLock(const std::string& guid, const OwnerLock& lock)
{
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越
    const auto dir = sessionDir(guid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;

    std::string s;
    s += "pid=" + std::to_string(lock.pid) + "\n";
    s += "processStartEpochMs=" + std::to_string(lock.processStartEpochMs) + "\n";
    s += "heartbeatEpochMs=" + std::to_string(lock.heartbeatEpochMs) + "\n";
    s += "heartbeatIso8601=" + lock.heartbeatIso8601 + "\n";
    return atomicWriteText(dir / std::string(kSidecarOwnerLockFilename), s);
}

bool SidecarStore::readOwnerLock(const std::string& guid, OwnerLock& lock) const
{
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越
    std::ifstream ifs(sessionDir(guid) / std::string(kSidecarOwnerLockFilename), std::ios::binary);
    if (!ifs)
        return false;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::istringstream lines(content);
    std::string line;
    bool any = false;
    while (std::getline(lines, line))
    {
        if (line.empty())
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string field = line.substr(0, eq);
        const std::string val = line.substr(eq + 1u);
        if (field == "pid")
            any = parseU64(val, lock.pid) || any;
        else if (field == "processStartEpochMs")
            parseU64(val, lock.processStartEpochMs);
        else if (field == "heartbeatEpochMs")
            parseU64(val, lock.heartbeatEpochMs);
        else if (field == "heartbeatIso8601")
            lock.heartbeatIso8601 = val;
    }
    return any;
}

bool SidecarStore::refreshOwnerLock(const std::string& guid, const ProcessIdentity& self, std::int64_t nowEpochMs)
{
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越

    OwnerLock lock;
    if (!readOwnerLock(guid, lock))
        return false; // 盘上没有租约 → 不新建(见头文件闸②)

    // 归属判定与 copyOnWriteIfNeeded 逐字同口径:pid + 进程启动时间双元组,防 Windows PID 复用。
    // pid==0 是「拿不到进程身份」的退化值(非 Windows),那种进程不该宣示占有任何 sidecar。
    if (self.pid == 0u || lock.pid != self.pid || lock.processStartEpochMs != self.processStartEpochMs)
        return false; // 锁归他人(或身份未知)→ 绝不覆盖(闸③)

    lock.heartbeatEpochMs = static_cast<std::uint64_t>(nowEpochMs < 0 ? 0 : nowEpochMs);
    lock.heartbeatIso8601 = iso8601UtcFromEpochMs(nowEpochMs < 0 ? 0 : nowEpochMs);
    return writeOwnerLock(guid, lock);
}

bool SidecarStore::claimOwnerLockIfUnheld(const std::string& guid, const ProcessIdentity& self, std::int64_t nowEpochMs)
{
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越
    if (self.pid == 0u)
        return false; // 拿不到进程身份的退化值不得宣示占有(与 refreshOwnerLock 同口径)
    if (!std::filesystem::exists(sessionDir(guid) / std::string(kSidecarFeaturesFilename)))
        return false; // 没有特征文件就没有可占的 sidecar(别凭空造出一个只有锁的空目录)

    OwnerLock existing;
    if (readOwnerLock(guid, existing))
    {
        // 已归本进程 → 无需重复宣示,心跳留给 refreshOwnerLock 去推(认领只负责「拿到所有权」)。
        // 「归自己但已判死」也走这一支:tick 的分频计时那时必然已过一个周期,下一拍 40ms 就补上心跳。
        if (existing.pid == self.pid && existing.processStartEpochMs == self.processStartEpochMs)
            return true;
        // **活锁归他人 → 绝不覆盖**。那是 copy-on-write 的判据,抢过来就等于把别人正开着的
        // sidecar 认成自己的。这一条与 refreshOwnerLock 的闸③、copyOnWriteIfNeeded 的判定同口径。
        if (isOwnerLockAlive(existing, nowEpochMs))
            return false;
        // 落到这里 = 锁存在但已判死(上一会话留下的)→ 按定义无人持有,可以接手。
    }

    OwnerLock mine;
    mine.pid = self.pid;
    mine.processStartEpochMs = self.processStartEpochMs;
    mine.heartbeatEpochMs = static_cast<std::uint64_t>(nowEpochMs < 0 ? 0 : nowEpochMs);
    mine.heartbeatIso8601 = iso8601UtcFromEpochMs(nowEpochMs < 0 ? 0 : nowEpochMs);
    return writeOwnerLock(guid, mine);
}

bool SidecarStore::copyOnWriteIfNeeded(const std::string& guid, const ProcessIdentity& self, std::int64_t nowEpochMs,
                                       std::string& newGuid)
{
    if (!isValidSessionGuid(guid))
        return false; // 防目录穿越

    OwnerLock lock;
    if (!readOwnerLock(guid, lock))
        return false; // 无锁 → 无需 CoW
    if (!isOwnerLockAlive(lock, nowEpochMs))
        return false; // 锁过期 → 无人持有,可安全复用
    // 活锁为己有:pid + 进程启动时间双元组(防 Windows PID 复用误判为己有而跳过 CoW)。
    if (self.pid != 0u && lock.pid == self.pid && lock.processStartEpochMs == self.processStartEpochMs)
        return false;

    // 活锁非己 → copy-on-write(04 §5.6):生成 newGuid、复制目录、写自己的 owner.lock。
    const auto oldDir = sessionDir(guid);
    const auto oldFeatures = oldDir / std::string(kSidecarFeaturesFilename);
    if (!std::filesystem::exists(oldFeatures))
        return false;

    newGuid = generateSessionGuid();
    const auto newDir = sessionDir(newGuid);
    std::error_code ec;
    std::filesystem::create_directories(newDir, ec);
    if (ec)
        return false;

    std::filesystem::copy_file(oldFeatures, newDir / std::string(kSidecarFeaturesFilename),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return false;

    const auto oldManifest = oldDir / std::string(kSidecarManifestFilename);
    if (std::filesystem::exists(oldManifest))
    {
        std::filesystem::copy_file(oldManifest, newDir / std::string(kSidecarManifestFilename),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
    }

    OwnerLock mine;
    mine.pid = self.pid;
    mine.processStartEpochMs = self.processStartEpochMs;
    mine.heartbeatEpochMs = nowEpochMs;
    mine.heartbeatIso8601 = iso8601UtcNow();
    return writeOwnerLock(newGuid, mine);
}

bool SidecarStore::shouldUseSidecar(std::uint64_t gzBytes, bool currentlySidecar)
{
    if (currentlySidecar)
        return gzBytes >= kReembedThresholdBytes; // 一旦转 sidecar,<6MB 才收回内嵌(04 §5.3)
    return gzBytes > kSidecarThresholdBytes; // 压缩后 >8MB 转 sidecar(ADR-007)
}

} // namespace scvb::state
