// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScvbJournal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace scvb
{
namespace journal
{
namespace
{
bool g_disabled = false;
bool g_booted = false;
HANDLE g_file = nullptr;
std::mutex g_mtx;

std::string resolvePath()
{
    const char* env = std::getenv("SCVB_JOURNAL_PATH");
    if (env != nullptr && *env != '\0')
    {
        return env;
    }
    const char* local = std::getenv("LOCALAPPDATA");
    const std::string base = (local != nullptr && *local != '\0') ? local : ".";
    return base + "\\SynchainSCVB\\logs\\scvb-journal.txt";
}

void ensureOpenLocked()
{
    if (g_file != nullptr)
    {
        return;
    }
    const std::string path = resolvePath();
    const std::size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
    {
        ::CreateDirectoryA(path.substr(0, slash).c_str(), nullptr); // 已存在 → 失败可忽略
    }
    g_file = ::CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
    {
        g_file = nullptr;
        g_disabled = true;
    }
}

std::string nowStr()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));
    return buf;
}

void writeLine(const std::string& line)
{
    if (g_file == nullptr)
    {
        return;
    }
    DWORD written = 0;
    ::WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
} // namespace

void boot(const char* plugin, const char* version)
{
    const char* dis = std::getenv("SCVB_JOURNAL");
    if (dis != nullptr && std::strcmp(dis, "0") == 0)
    {
        g_disabled = true;
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    ensureOpenLocked();
    if (g_file == nullptr)
    {
        return;
    }
    if (!g_booted)
    {
        char pidBuf[32];
        std::snprintf(pidBuf, sizeof(pidBuf), "%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
        writeLine(nowStr() + "|pid=" + pidBuf + "|tid=0|plugin=" + plugin + "|evt=boot|ver=" + version + "\n");
        g_booted = true;
    }
}

void log(const char* plugin, const char* evt, const std::string& fields)
{
    if (g_disabled)
    {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    ensureOpenLocked();
    if (g_file == nullptr)
    {
        return;
    }
    char pidBuf[32];
    std::snprintf(pidBuf, sizeof(pidBuf), "%lu", static_cast<unsigned long>(::GetCurrentProcessId()));
    char tidBuf[32];
    std::snprintf(tidBuf, sizeof(tidBuf), "%lu", static_cast<unsigned long>(::GetCurrentThreadId()));
    std::string line = nowStr() + "|pid=" + pidBuf + "|tid=" + tidBuf + "|plugin=" + plugin + "|evt=" + evt;
    if (!fields.empty())
    {
        line += "|" + fields;
    }
    line += "\n";
    writeLine(line);
}

std::string kv(const std::string& key, unsigned long long value)
{
    return key + "=" + std::to_string(value);
}

std::string kv(const std::string& key, long long value)
{
    return key + "=" + std::to_string(value);
}

std::string kv(const std::string& key, const char* value)
{
    return key + "=" + (value != nullptr ? value : "");
}

std::string kv(const std::string& key, const std::string& value)
{
    return key + "=" + value;
}

std::string join(std::initializer_list<std::string> parts)
{
    std::string out;
    for (const auto& p : parts)
    {
        if (!out.empty())
        {
            out += " ";
        }
        out += p;
    }
    return out;
}

} // namespace journal
} // namespace scvb