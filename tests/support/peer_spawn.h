// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// peer_spawn —— 双进程测试的对端进程 spawn / 等待 / 杀进程 + CSV 回收 helper。
//
// 为什么是一份**新**头而不是把 test_ipc_contract.cpp 里的同名 helper 抽出来共用:
// 那个文件正被另一路(DS 侧 IPC-16 flake 修复)系统性改动,任何抽取都会造出大片合并冲突。
// 本头是新文件、零冲突面;`test_ipc_contract.cpp` 里那份等价的匿名命名空间副本**保持不动**
// (它是内部链接,与本头的 `scvb::ipctest::peer` 命名空间不打架)。
// **待办**:IPC-16 那路合并后,把 test_ipc_contract.cpp 改为包含本头、删掉它的本地副本。
//
// 与 test_ipc_contract.cpp 那份的唯一实质差别:`peerExe()` 接受对端 exe 名并把候选链放宽到
// 「测试 exe 目录的若干相对位置」—— 因为消费方不止 `<build>/tests/ipc/`(scvb_ipc_tests),
// 还有 `<build>/tests/`(scvb_monitor_tests)。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace scvb::ipctest::peer
{

// 运行期解析对端 exe 路径:不编译期烘焙 $<TARGET_FILE>(VS 生成器渲染反斜杠路径会触发
// MSVC C4129 或损坏转义,PR#46 复审)。从测试 exe 自身路径出发试若干相对位置。
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

// spawnErr:0 = 成功;ERROR_FILE_NOT_FOUND = 找不到对端 exe(多半是 -DSCVB_BUILD_TOOLS=OFF)。
inline PROCESS_INFORMATION spawnPeer(const std::wstring& exeName, const std::vector<std::string>& args, int* spawnErr)
{
    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    const std::wstring exe = peerExe(exeName);
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

// 返回对端退出码;超时或句柄无效返回 -1。
inline int waitPeer(PROCESS_INFORMATION& pi, DWORD timeoutMs)
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

inline void killPeer(PROCESS_INFORMATION& pi)
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
    PeerGuard() = default;
    ~PeerGuard() { killPeer(pi); }
    PeerGuard(const PeerGuard&) = delete;
    PeerGuard& operator=(const PeerGuard&) = delete;
};

inline std::atomic<int>& csvCounter()
{
    static std::atomic<int> c{0};
    return c;
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

inline void deleteFile(const std::string& path)
{
    ::DeleteFileA(path.c_str());
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
