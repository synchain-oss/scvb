// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PlatformLog —— 插件侧的平台文件日志(B 线 M07)。
//
// 【为什么要有】WebViewHost::logDiag 走 juce::Logger::writeToLog,无 logger 时落 outputDebugString:
// Windows 上是 DebugView 看得见的那一条;mac 上是 stderr,在 Logic / LUNA / AUHostingService 下用户
// 基本拿不到。段后端(SegmentBackendPosix)的失败原因也只有 IpcDiag 这一个旁路出口,默认空操作。
// mac 上需要一个用户能直接打包发回来的文件。
//
// 【落点】mac = ~/Library/Logs/Synchain/SCVB/<role>.log(role = input / output / monitor;三个插件是
// 三个二进制,各写各的文件,彼此不加锁)。1 MB 轮转:写之前若会超过 kMaxLogBytes,先把当前文件改名为
// <role>.1.log(覆盖更旧的那一份)再新开 ⇒ 每个角色最多约 2 MB。
// Windows 与其它平台**不落文件**:defaultLogFile() 返回空 File ⇒ install() 是空操作 ⇒ write() 什么都不做;
// OutputDebugString 那条路(WebViewHost::logDiag 里的 juce::Logger)不变。
//
// 【多个写者】同一份 <role>.log 可能同时有几个写者:同一宿主里同角色的 AU 与 VST3(两个二进制,各有一把
// 进程内锁),以及同时开着的多个宿主进程。POSIX 上每行用 O_APPEND 一次 write 落盘,行与行不交错、不互相覆盖。
// 唯一的例外是短写重试:write 被信号打断、只写进半行时,剩下的半行另起一次追加,多写者下理论上可能夹进
// 别的写者的一行(本地普通文件上几乎碰不到)。
// **轮转不跨写者协调**:两个写者同时越过阈值时可能连着轮转两次,较旧的那份备份(至多约 1 MB)被覆盖。
// 对诊断日志可以接受,这里写明不保证。
//
// 【线程】**文件只在消息线程写。** write() 可在任何非实时线程调用:当前就是消息线程时就地写;不是就经
// MessageManager::callAsync 投递(行内容按值拷贝),没有 MessageManager 时丢弃。音频线程一律不许调
// (CLAUDE.md §8:文件 I/O、锁、堆分配)。全局状态由一把 std::mutex 守,只在非实时线程上取。
//
// 【IPC 诊断】install() 在有 POSIX 段后端的平台上(SCVB_HAS_POSIX_SHM,今天 = mac)把 IpcDiag 的 sink 接到
// 本日志。IpcDiag.h 的约定是「同一事件可能高频重复,sink 要落盘就得自己限流或去重」,所以 sink 先过
// DiagDeduper:同一 (op, error, segment) 在 kRepeatWindowMs 内只落一行,下一次落行时带上期间略去的
// 次数;另有全局每窗口 kMaxLinesPerWindow 行的上限,挡住大量不同键同时刷屏。
//
// 【viz 实得速率】(B 线 M07b)installAt() 同时把 scvb_core 的 viz-rate 槽(core/diag/VizRateSink.h)接到
// 本日志:Output / Monitor 的定时器回调每约 10 s 交一行 `viz-rate ...`(行的口径见 core/diag/VizRateMeter.h)。
// 源头本身就按窗口限了频(每实例每窗一行);sink 另有一道全局上限 LineBudget(每 kVizRateBudgetWindowMs
// 最多 kVizRateLinesPerWindow 行,任意实例合计),挡住「同一个二进制里开了一大把实例」把日志撑满,
// 被挡下的条数进下一条落行。uninstall() 摘掉。Windows 生产路径从不 install,槽一直是空的。
//
// 文案一律 ASCII(运行期字面量含非 ASCII 会在 CP936 机器上触发 MSVC C4819);中文只在注释里。

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scvb::platformlog
{

// 单个日志文件的轮转阈值(字节)。
inline constexpr juce::int64 kMaxLogBytes = 1024 * 1024;

// 单行上限(字符)。诊断行本来都很短;这条只防一条异常长的行(例如前端上报的错误串)把整份
// 日志撑过阈值。超出部分截掉并标 `...(truncated)`。
inline constexpr int kMaxLineChars = 2048;

// 角色的默认日志文件。mac = ~/Library/Logs/Synchain/SCVB/<role>.log;role 只认 input / output / monitor,
// 其它值返回空 File(不让任意字符串拼进路径)。非 mac 平台恒返回空 File = 不落文件。
juce::File defaultLogFile(const juce::String& role);

// 一个按大小轮转的追加式文本文件。**不加锁、不管线程**:调用方保证只在一个线程上用它
// (本文件的全局实例只在消息线程上写)。
class RotatingLogFile
{
public:
    explicit RotatingLogFile(juce::File file, juce::int64 maxBytes = kMaxLogBytes);

    // 追加一行(自动补 '\n',UTF-8,不做行尾转换)。写之前若「当前大小 + 本行」会超过 maxBytes,
    // 先把当前文件改名为 rotatedFile()(覆盖旧的那份)。父目录不存在时先建。
    // 返回 false = 这一行没写进去(目录建不出 / 文件打不开 / 写失败);不抛异常。
    bool append(const juce::String& line);

    const juce::File& file() const noexcept { return file_; }
    // <名>.1<扩展名>,例如 input.log → input.1.log(保留 .log 扩展名,控制台.app 照样能打开)。
    juce::File rotatedFile() const;

private:
    juce::File file_;
    juce::int64 maxBytes_;
};

// IpcDiag 事件的去重 / 限流(纯逻辑,时间由调用方传入,便于离线单测)。
// 键 = (op, error, segment)。时间差一律按 uint32 无符号相减(getMillisecondCounter 约 49 天回绕一次)。
class DiagDeduper
{
public:
    // 同一键两次落行之间至少隔这么久;全局上限也按这个窗口计。
    static constexpr std::uint32_t kRepeatWindowMs = 60000;
    // 全局:每个窗口最多落这么多行(任意键合计)。超出的只计数,并进下一条落行。
    static constexpr std::uint32_t kMaxLinesPerWindow = 64;
    // 记住的键数上限;满了按「最久没落过行」淘汰一个。
    static constexpr std::size_t kMaxKeys = 256;

    struct Verdict
    {
        bool emit = false; // true = 这一条落行
        std::uint32_t repeatsSuppressed = 0; // 同一键在它上一行之后被略去的次数(emit 时才有意义)
        std::uint32_t rateLimited = 0; // 全局上限在上一条落行之后挡掉的条数(任意键;emit 时才有意义)
    };

    Verdict onEvent(std::uint32_t op, int error, const juce::String& segment, std::uint32_t nowMs);

    std::size_t keyCount() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        std::uint32_t op = 0;
        int error = 0;
        juce::String segment;
        std::uint32_t lastEmitMs = 0;
        std::uint32_t suppressed = 0;
    };

    std::vector<Entry> entries_;
    bool windowStarted_ = false;
    std::uint32_t windowStartMs_ = 0;
    std::uint32_t linesInWindow_ = 0;
    std::uint32_t rateLimited_ = 0;
};

// 一类日志行的全局上限(纯逻辑,时间由调用方传入)。每个窗口最多 maxLines 行,超出的只计数,并进下一条
// 放行的那一行。时间差按 uint32 无符号相减(getMillisecondCounter 约 49 天回绕一次)。
// 被挡下的条数只能随「下一条放行的行」落盘:之后再也没有行进来(实例都关了、进程退出)时,最后那一批的
// 丢弃数不会出现在日志里。诊断用途可以接受,这里写明不保证。
class LineBudget
{
public:
    struct Verdict
    {
        bool emit = false; // true = 这一行落盘
        std::uint32_t dropped = 0; // 上一条放行之后被挡下的条数(emit 时才有意义)
    };

    LineBudget(std::uint32_t maxLines, std::uint32_t windowMs) noexcept : maxLines_(maxLines), windowMs_(windowMs) {}

    Verdict onLine(std::uint32_t nowMs) noexcept;

private:
    std::uint32_t maxLines_;
    std::uint32_t windowMs_;
    bool windowStarted_ = false;
    std::uint32_t windowStartMs_ = 0;
    std::uint32_t linesInWindow_ = 0;
    std::uint32_t dropped_ = 0;
};

// viz-rate 行的全局上限:每 60 s 最多 60 行 = 每个实例每 10 s 一行时可容 10 个实例同时报。
// 按每行约 200 字节算,封顶约 12 KB / 分钟,一份 1 MB 日志至少撑约 80 分钟才轮转。
inline constexpr std::uint32_t kVizRateBudgetWindowMs = 60000;
inline constexpr std::uint32_t kVizRateLinesPerWindow = 60;

// 生产入口(三个插件的 createPluginFilter 调):本角色的默认日志文件 + IPC 诊断 sink,再写一行开场
// (角色、版本、pid、宿主可执行名)。每个插件二进制只装一次,后续调用是空操作;非 mac 平台恒为空操作。
// 返回 true = 这次真的装上了。线程:任意非实时线程(内部有锁)。
bool install(const juce::String& role, const juce::String& version = {});

// 指定文件装上(install 的下半段;单测也走这里,把日志放进临时目录)。已装时不替换、返回 false。
// 装上时同时接上 IpcDiag sink(有 POSIX 段后端的平台)与 viz-rate sink(所有平台;生产上只有 mac 会走到这里)。
bool installAt(const juce::File& file);

// 卸下(只给单测用:恢复 IpcDiag 与 viz-rate 的空操作 sink,丢掉文件句柄)。生产代码从不调用 —— 日志与进程(准确说
// 与插件二进制)同寿。
void uninstall();

bool isInstalled() noexcept;
juce::File installedFile();

// 写一行(自动加本地时间戳前缀)。没装时什么都不做。消息线程上就地写;其它非实时线程上投递到消息线程。
void write(const juce::String& line);

} // namespace scvb::platformlog
