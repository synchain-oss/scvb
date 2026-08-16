// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike 诊断日志(v6):追加式文本日志,崩溃后现场留档(配合 collect-crash.ps1 收集)。
// 规则:只允许消息线程调用(§8:音频线程零文件 I/O,只做原子计数,由 timerCallback 落盘)。
//   默认开启;env SCVB_JOURNAL=0 关闭;SCVB_JOURNAL_PATH 覆盖路径(默认
//   %LOCALAPPDATA%\SynchainSCVB\logs\scvb-journal.txt)。
//   正式版 T06 由结构化诊断通道取代(本文件不进生产路径)。
#include <initializer_list>
#include <string>
#include <type_traits>

namespace scvb
{
namespace journal
{

// 进程启动标记(Input/Output 构造时各调一次,写 boot 行)。
void boot(const char* plugin, const char* version);

// 追加一行:HH:MM:SS.mmm|pid=..|tid=..|plugin=..|evt=..|fields
void log(const char* plugin, const char* evt, const std::string& fields);

// fields 构造辅助。
std::string kv(const std::string& key, unsigned long long value);
std::string kv(const std::string& key, long long value);

// 其它整型(u32/int/...):消歧模板(值域按有符号打印,诊断场景足够)。
template<typename T>
typename std::enable_if<std::is_integral<T>::value, std::string>::type kv(const std::string& key, T value)
{
    return key + "=" + std::to_string(static_cast<long long>(value));
}
std::string kv(const std::string& key, const char* value);
std::string kv(const std::string& key, const std::string& value);
std::string join(std::initializer_list<std::string> parts);

} // namespace journal
} // namespace scvb