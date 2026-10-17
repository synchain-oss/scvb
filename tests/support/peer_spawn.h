// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// peer_spawn —— 双进程测试的对端进程 spawn / 等待 / 杀进程 + CSV 回收 helper。
//
// 消费方:scvb_ipc_tests(test_ipc_contract.cpp、test_ipc_viz.cpp、test_ipc_posix.cpp)与
// scvb_monitor_tests(test_monitor_harness.cpp)。[B 线 M12a] 之前 test_ipc_contract.cpp 留着一份
// 等价的匿名命名空间副本(当时是为了不和 IPC-16 flake 修复那一路抢同一个文件);那一路早已合入,
// M12a 把那份副本删掉、改为包含本头 —— 两份 spawn 实现各自 POSIX 化一遍,只会造出第二个分叉点。
//
// [B 线 M12a] 两个平台一份接口:
//   · Windows:CreateProcessW / WaitForSingleObject / TerminateProcess,代码与 M12a 之前逐行同义;
//   · POSIX  :posix_spawn / waitpid / kill(SIGKILL)。
//   `PeerProcess` 是平台句柄(Windows 上就是 PROCESS_INFORMATION),`peerPid()` 取对端 pid,
//   `peerRunning()` 非阻塞判「还活着」,`pollPeer()` 非阻塞收退出码。
//
// POSIX 上两条与 Windows 不同、但调用方必须知道的事实:
//   · **退出的子进程在被 waitpid 收走之前是僵尸,`kill(pid, 0)` 对僵尸照样成功** —— 也就是说
//     `scvb::isProcessAlive(pid)` 会把一个已经退出、还没被收走的对端判成「活着」。所以凡是
//     「对端已死」的断言(陈旧接管、J10 双条件)之前,对端必须先经 `waitPeer` / `killPeer` /
//     `pollPeer` 收走。本头的三个函数都在拿到退出状态的同一刻收走子进程。
//   · **对端退出码**:正常退出 = `exit()` 的值;被信号杀死 = 128 + 信号号(shell 口径)。
//     Windows 上 `killPeer` 用 TerminateProcess(…, 9),退出码 9;POSIX 上是 SIGKILL,128 + 9 = 137。
//     今天没有用例断言被杀对端的退出码。
//
// 对端 exe 路径运行期解析,不编译期烘焙 $<TARGET_FILE>(VS 生成器渲染反斜杠路径会触发
// MSVC C4129 或损坏转义,PR#46 复审):从测试 exe 自身路径出发试若干相对位置 —— 消费方不止
// `<build>/tests/ipc/`(scvb_ipc_tests),还有 `<build>/tests/`(scvb_monitor_tests)。

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
extern char** environ;
#endif

namespace scvb::ipctest::peer
{

// 对端可执行文件的基本名(不带扩展名;Windows 上 spawnPeer 自己补 ".exe")。
inline constexpr const char* kIpcPeerName = "scvb_ipc_peer";

inline std::atomic<int>& csvCounter()
{
    static std::atomic<int> c{0};
    return c;
}

#ifdef _WIN32

using PeerProcess = PROCESS_INFORMATION;

inline std::wstring peerExe(const std::wstring& exeName)
{
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

    std::vector<std::wstring> candidates;
    // Ninja 单配置:<build>/tests/ipc/ 或 <build>/tests/ → tools 在同级或上一级。
    candidates.push_back(dir + L"/../tools/" + exeName); // <build>/tests/ipc → <build>/tests/tools
    candidates.push_back(dir + L"/tools/" + exeName); // <build>/tests      → <build>/tests/tools
    // VS 多配置:目录末段是 <Config>,tools 产物在 <build>/tests/tools/<Config>/。
    const std::size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
    {
        const std::wstring config = dir.substr(lastSlash + 1);
        candidates.push_back(dir + L"/../../tools/" + config + L"/" + exeName); // <build>/tests/ipc/<Config>
        candidates.push_back(dir + L"/../tools/" + config + L"/" + exeName); // <build>/tests/<Config>
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

inline std::wstring wide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

inline std::string quote(const std::string& s)
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

// spawnErr:0 = 成功;ERROR_FILE_NOT_FOUND(2)= 找不到对端 exe(多半是 -DSCVB_BUILD_TOOLS=OFF)。
inline PeerProcess spawnPeer(const std::string& exeBaseName, const std::vector<std::string>& args, int* spawnErr)
{
    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    const std::wstring exe = peerExe(wide(exeBaseName) + L".exe");
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

inline std::uint32_t peerPid(const PeerProcess& p)
{
    return static_cast<std::uint32_t>(p.dwProcessId);
}

// 返回对端退出码;超时或句柄无效返回 -1。
inline int waitPeer(PeerProcess& pi, std::uint32_t timeoutMs)
{
    if (pi.hProcess == nullptr)
    {
        return -1;
    }
    const DWORD r = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));
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

// 非阻塞:对端已退出 → 收走、*exitCode 写退出码、返回 true;仍在跑(或句柄无效)→ false。
inline bool pollPeer(PeerProcess& pi, int* exitCode)
{
    if (pi.hProcess == nullptr || ::WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0)
    {
        return false;
    }
    const int code = waitPeer(pi, 0);
    if (exitCode != nullptr)
    {
        *exitCode = code;
    }
    return true;
}

// 非阻塞:对端此刻是否仍在运行(不收走、不改句柄)。
// 参数故意是非 const 引用,与 POSIX 分支同一个签名(那边会顺手收走已退出的子进程,必须能改句柄):
// 签名不一致时,对 const 对象的调用在 Windows 本地 gates 上编得过、到 mac 才红(#375 评审第 1 轮)。
inline bool peerRunning(PeerProcess& pi)
{
    return pi.hProcess != nullptr && ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
}

inline void killPeer(PeerProcess& pi)
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

inline std::string tempCsvPath()
{
    char buf[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, buf);
    std::string p = std::string(buf, n);
    p += "scvb_peer_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(csvCounter().fetch_add(1)) +
         ".csv";
    return p;
}

inline void deleteFile(const std::string& path)
{
    ::DeleteFileA(path.c_str());
}

#else // !_WIN32

// POSIX 句柄:pid 与「已经收走时的退出码」(peerRunning 可能先一步收走子进程,结果要留给 waitPeer)。
struct PeerProcess
{
    pid_t pid = -1;
    bool exited = false; // 已被 waitpid 收走,exitCode 有效
    int exitCode = -1;
};

// waitpid 状态 → 退出码(正常退出 = exit 值;被信号杀死 = 128 + 信号号)。
inline int peerExitCodeFromStatus(int status)
{
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

// 非阻塞收一次:已退出 → 记下退出码并返回 true。子进程已被别人收走(ECHILD)也按「已退出」处理,
// 退出码记 -1(本头之外没有人 waitpid 这些 pid,正常不会走到)。
inline bool reapPeerNoHang(PeerProcess& p)
{
    if (p.exited)
    {
        return true;
    }
    if (p.pid <= 0)
    {
        return false;
    }
    int status = 0;
    pid_t r = 0;
    do
    {
        r = ::waitpid(p.pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == p.pid)
    {
        p.exited = true;
        p.exitCode = peerExitCodeFromStatus(status);
        return true;
    }
    if (r < 0)
    {
        p.exited = true;
        p.exitCode = -1;
        return true;
    }
    return false;
}

// 本进程可执行文件的绝对路径(符号链接已解开);取不到返回空串。
inline std::string selfExePath()
{
    std::string raw;
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size); // 只问长度
    std::vector<char> buf(static_cast<std::size_t>(size) + 1, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) != 0)
    {
        return "";
    }
    raw = buf.data();
#else
    std::vector<char> buf(PATH_MAX + 1, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), PATH_MAX);
    if (n <= 0)
    {
        return "";
    }
    raw.assign(buf.data(), static_cast<std::size_t>(n));
#endif
    char resolved[PATH_MAX];
    if (::realpath(raw.c_str(), resolved) == nullptr)
    {
        return "";
    }
    return std::string(resolved);
}

inline std::string peerExe(const std::string& exeName)
{
    std::string dir = selfExePath();
    const std::size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos)
    {
        return "";
    }
    dir.resize(slash); // 测试 exe 目录

    // 与 Windows 分支同一条候选链(单配置生成器;多配置时目录末段是 <Config>)。
    std::vector<std::string> candidates;
    candidates.push_back(dir + "/../tools/" + exeName); // <build>/tests/ipc → <build>/tests/tools
    candidates.push_back(dir + "/tools/" + exeName); // <build>/tests      → <build>/tests/tools
    const std::size_t lastSlash = dir.find_last_of('/');
    if (lastSlash != std::string::npos)
    {
        const std::string config = dir.substr(lastSlash + 1);
        candidates.push_back(dir + "/../../tools/" + config + "/" + exeName);
        candidates.push_back(dir + "/../tools/" + config + "/" + exeName);
    }

    for (const auto& cand : candidates)
    {
        char resolved[PATH_MAX];
        if (::realpath(cand.c_str(), resolved) != nullptr && ::access(resolved, X_OK) == 0)
        {
            return std::string(resolved);
        }
    }
    return "";
}

// 按绝对路径拉起一个进程(环境原样继承 —— 锁目录覆盖 SCVB_IPC_LOCK_DIR 必须两边一致,
// 见 SegmentBackendPosix.h「测试钩子」)。spawnErr:0 = 成功;否则是 posix_spawn 的错误码。
inline PeerProcess spawnProcess(const std::string& exe, const std::vector<std::string>& args, int* spawnErr)
{
    PeerProcess p;
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage)
    {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawnErr != nullptr)
    {
        *spawnErr = rc;
    }
    if (rc == 0)
    {
        p.pid = pid;
    }
    return p;
}

// spawnErr:0 = 成功;ENOENT(2,与 Windows 的 ERROR_FILE_NOT_FOUND 同值)= 找不到对端 exe
// (多半是 -DSCVB_BUILD_TOOLS=OFF);其余是 posix_spawn 的错误码。
inline PeerProcess spawnPeer(const std::string& exeBaseName, const std::vector<std::string>& args, int* spawnErr)
{
    const std::string exe = peerExe(exeBaseName);
    if (exe.empty())
    {
        if (spawnErr != nullptr)
        {
            *spawnErr = ENOENT;
        }
        return PeerProcess{};
    }
    return spawnProcess(exe, args, spawnErr);
}

inline std::uint32_t peerPid(const PeerProcess& p)
{
    return p.pid > 0 ? static_cast<std::uint32_t>(p.pid) : 0u;
}

// 返回对端退出码;超时或句柄无效返回 -1。拿到退出码的同时收走子进程(不留僵尸)。
inline int waitPeer(PeerProcess& p, std::uint32_t timeoutMs)
{
    if (p.pid <= 0)
    {
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!reapPeerNoHang(p))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int code = p.exitCode;
    p = PeerProcess{};
    return code;
}

// 非阻塞:对端已退出 → 收走、*exitCode 写退出码、返回 true;仍在跑(或句柄无效)→ false。
inline bool pollPeer(PeerProcess& p, int* exitCode)
{
    if (p.pid <= 0 || !reapPeerNoHang(p))
    {
        return false;
    }
    if (exitCode != nullptr)
    {
        *exitCode = p.exitCode;
    }
    p = PeerProcess{};
    return true;
}

// 非阻塞:对端此刻是否仍在运行。已退出的会被顺手收走(结果留给随后的 waitPeer / pollPeer)。
inline bool peerRunning(PeerProcess& p)
{
    return p.pid > 0 && !reapPeerNoHang(p);
}

// 强杀(模拟崩溃:对端没有任何清理机会,与 Windows 的 TerminateProcess 同一语义)并收走。
inline void killPeer(PeerProcess& p)
{
    if (p.pid <= 0)
    {
        return;
    }
    if (!reapPeerNoHang(p))
    {
        ::kill(p.pid, SIGKILL);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (!reapPeerNoHang(p) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    p = PeerProcess{};
}

inline std::string tempCsvPath()
{
    const char* tmp = std::getenv("TMPDIR");
    std::string p = (tmp != nullptr && tmp[0] != '\0') ? std::string(tmp) : std::string("/tmp");
    if (p.back() != '/')
    {
        p += '/';
    }
    p += "scvb_peer_" + std::to_string(::getpid()) + "_" + std::to_string(csvCounter().fetch_add(1)) + ".csv";
    return p;
}

inline void deleteFile(const std::string& path)
{
    ::unlink(path.c_str());
}

#endif // _WIN32

struct PeerGuard
{
    PeerProcess pi{};
    PeerGuard() = default;
    ~PeerGuard() { killPeer(pi); }
    PeerGuard(const PeerGuard&) = delete;
    PeerGuard& operator=(const PeerGuard&) = delete;
};

inline std::string readFile(const std::string& path)
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

// 「key value」逐行;# 开头为注释。
inline std::map<std::string, std::string> parseCsv(const std::string& text)
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

inline long long csvLL(const std::map<std::string, std::string>& m, const std::string& k)
{
    const auto it = m.find(k);
    return (it == m.end()) ? 0 : std::strtoll(it->second.c_str(), nullptr, 10);
}

inline std::string csvStr(const std::map<std::string, std::string>& m, const std::string& k)
{
    const auto it = m.find(k);
    return (it == m.end()) ? std::string{} : it->second;
}

} // namespace scvb::ipctest::peer
