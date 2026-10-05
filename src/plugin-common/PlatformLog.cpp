// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformLog.h"

#include <juce_events/juce_events.h> // MessageManager(write 的跨线程投递)

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#if SCVB_HAS_POSIX_SHM
#include "ipc/IpcDiag.h"
#endif

#if !JUCE_WINDOWS
#include <cerrno>
#include <fcntl.h> // open(O_APPEND)
#include <unistd.h> // write / close / getpid
#endif

namespace scvb::platformlog
{

namespace
{
std::uint32_t saturatingIncrement(std::uint32_t v) noexcept
{
    return v == 0xffffffffu ? v : v + 1u;
}

#if JUCE_MAC
bool isKnownRole(const juce::String& role)
{
    return role == "input" || role == "output" || role == "monitor";
}
#endif

// 全局(每个插件二进制一份)状态。**有意不析构**(new 出来不 delete):宿主在退出路径上的析构顺序
// 不由我们定 —— 段后端在 Processor 析构里还可能经 IpcDiag 报一条,那时若这把锁已随静态对象析构,
// 就是 use-after-destroy。泄漏的只有一个文件名和一张小表,进程退出时由系统收回。
struct State
{
    std::mutex mutex;
    std::unique_ptr<RotatingLogFile> log; // 非空 = 已装
    DiagDeduper deduper;
};

State& state()
{
    static State* s = new State();
    return *s;
}

// write() 的快路径:没装时连锁都不取。真值只由 installAt / uninstall 在持锁时改。
std::atomic<bool>& installedFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}

void writeOnMessageThread(const juce::String& line)
{
    juce::String text = line;
    if (text.length() > kMaxLineChars)
        text = text.substring(0, kMaxLineChars) + " ...(truncated)";
    const juce::String stamped = juce::Time::getCurrentTime().toISO8601(true) + " " + text;

    const std::lock_guard<std::mutex> lock(state().mutex);
    if (state().log != nullptr)
        state().log->append(stamped);
}

#if SCVB_HAS_POSIX_SHM
juce::String formatIpcDiagLine(const scvb::IpcDiagEvent& event, const DiagDeduper::Verdict& verdict)
{
    const juce::String segment = juce::String::fromUTF8(event.segment);
    juce::String line;
    line << "ipc " << scvb::ipcDiagOpName(event.op) << " errno " << juce::String(event.error) << " segment "
         << (segment.isEmpty() ? juce::String("-") : segment);
    if (event.path[0] != '\0')
        line << " path " << juce::String::fromUTF8(event.path);
    line << " -- " << juce::String::fromUTF8(event.detail);
    if (event.suppressed > 0)
        line << " (backend suppressed " << juce::String(static_cast<juce::int64>(event.suppressed)) << ")";
    if (verdict.repeatsSuppressed > 0)
        line << " (" << juce::String(static_cast<juce::int64>(verdict.repeatsSuppressed))
             << " identical reports suppressed since the last line)";
    if (verdict.rateLimited > 0)
        line << " (" << juce::String(static_cast<juce::int64>(verdict.rateLimited))
             << " ipc reports dropped by the rate limit)";
    return line;
}

// IpcDiag 的 sink(段后端在失败点同步调用;调用方线程 = 段后端的非实时线程,可能不是消息线程)。
// 先去重 / 限流,再交给 write()(不在消息线程时由它投递)。noexcept:吞掉一切异常(bad_alloc 等),
// 报告点在后端的失败清理路径里,不允许异常穿出(IpcDiag.h)。
void ipcDiagSink(const scvb::IpcDiagEvent& event) noexcept
{
    try
    {
        const juce::String segment = juce::String::fromUTF8(event.segment);
        DiagDeduper::Verdict verdict;
        {
            const std::lock_guard<std::mutex> lock(state().mutex);
            if (state().log == nullptr)
                return;
            verdict = state().deduper.onEvent(static_cast<std::uint32_t>(event.op), event.error, segment,
                                              juce::Time::getMillisecondCounter());
        }
        if (!verdict.emit)
            return;
        write(formatIpcDiagLine(event, verdict));
    }
    catch (...)
    {
    }
}
#endif

juce::String processIdForLog()
{
#if JUCE_MAC
    return juce::String(static_cast<int>(::getpid()));
#else
    return "n/a"; // 只有 mac 会真的 install,这一支只为让别的平台编得过
#endif
}
} // namespace

// -----------------------------------------------------------------------------
// 路径
// -----------------------------------------------------------------------------
juce::File defaultLogFile(const juce::String& role)
{
#if JUCE_MAC
    if (!isKnownRole(role))
        return {};
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    if (home == juce::File()) // 拿不到 home 时不往下拼(空 File 拼出来会落在文件系统根下)
        return {};
    return home.getChildFile("Library")
        .getChildFile("Logs")
        .getChildFile("Synchain")
        .getChildFile("SCVB")
        .getChildFile(role + ".log");
#else
    // Windows 维持现状:诊断只走 OutputDebugString(WebViewHost::logDiag 的 juce::Logger),不落文件。
    juce::ignoreUnused(role);
    return {};
#endif
}

// -----------------------------------------------------------------------------
// RotatingLogFile
// -----------------------------------------------------------------------------
RotatingLogFile::RotatingLogFile(juce::File file, juce::int64 maxBytes) : file_(std::move(file)), maxBytes_(maxBytes) {}

juce::File RotatingLogFile::rotatedFile() const
{
    return file_.getSiblingFile(file_.getFileNameWithoutExtension() + ".1" + file_.getFileExtension());
}

bool RotatingLogFile::append(const juce::String& line)
{
    if (file_ == juce::File())
        return false;

    const auto bytes = static_cast<juce::int64>(line.getNumBytesAsUTF8()) + 1; // + '\n'
    if (file_.existsAsFile())
    {
        const auto size = file_.getSize();
        // 当前文件非空且这一行写进去会超阈值 ⇒ 先轮转。改名失败(例如被别的进程占着)就接着往当前文件写:
        // 宁可超一点,也不丢这一行。
        if (size > 0 && size + bytes > maxBytes_)
            file_.moveFileTo(rotatedFile()); // moveFileTo 会先删掉已存在的目标 = 只留一份旧的
    }

    const auto dir = file_.getParentDirectory();
    if (!dir.isDirectory() && dir.createDirectory().failed())
        return false;

#if 1 // [临时注入 R3] POSIX 也走 FileOutputStream(无 O_APPEND)
    // Windows 上本类只给单测用(不落文件日志,见 defaultLogFile),沿用 JUCE 的流。
    juce::FileOutputStream out(file_); // 已存在时写位置在文件尾 = 追加
    if (out.failedToOpen())
        return false;
    out.writeText(line, false, false, nullptr); // UTF-8、无 BOM、不做行尾转换
    out.writeByte('\n');
    out.flush();
    return out.getStatus().wasOk();
#else
    // POSIX:O_APPEND + 一行一次 write。同一份 <role>.log 可能有**多个写者**:同一宿主里同角色的 AU 与 VST3 是
    // 两个二进制(各有一把进程内锁),另有多个宿主进程 / AUHostingService 同时开着。juce::FileOutputStream 是
    // open + lseek 到文件尾 + 缓冲写,两个写者拿到同一个尾偏移就会互相覆盖整行;O_APPEND 让「移到文件尾 + 写」
    // 由内核原子完成,行与行不交错、不覆盖(判据 = test_plugin_common.cpp 的 [mac] 多写者格)。
    // 轮转那两步(查大小 → 改名)跨写者不协调,见 PlatformLog.h 头注。
    const juce::String text = line + "\n";
    const char* data = text.toRawUTF8();
    auto remaining = text.getNumBytesAsUTF8();
    const int fd = ::open(file_.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = true;
    while (remaining > 0)
    {
        const auto n = ::write(fd, data, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return ok;
#endif
}

// -----------------------------------------------------------------------------
// DiagDeduper
// -----------------------------------------------------------------------------
DiagDeduper::Verdict DiagDeduper::onEvent(std::uint32_t op, int error, const juce::String& segment, std::uint32_t nowMs)
{
    Verdict verdict;

    Entry* hit = nullptr;
    for (auto& e : entries_)
    {
        if (e.op == op && e.error == error && e.segment == segment)
        {
            hit = &e;
            break;
        }
    }

    // 同一键,窗口内:只计数。
    if (hit != nullptr && (nowMs - hit->lastEmitMs) < kRepeatWindowMs)
    {
        hit->suppressed = saturatingIncrement(hit->suppressed);
        return verdict;
    }

    // 要落行了:先过全局上限。被挡下的这一条不更新该键的状态(下次照样再试),只进全局计数。
    if (!windowStarted_ || (nowMs - windowStartMs_) >= kRepeatWindowMs)
    {
        windowStarted_ = true;
        windowStartMs_ = nowMs;
        linesInWindow_ = 0;
    }
    if (linesInWindow_ >= kMaxLinesPerWindow)
    {
        rateLimited_ = saturatingIncrement(rateLimited_);
        return verdict;
    }
    ++linesInWindow_;

    verdict.emit = true;
    verdict.rateLimited = std::exchange(rateLimited_, 0u);

    if (hit != nullptr)
    {
        verdict.repeatsSuppressed = std::exchange(hit->suppressed, 0u);
        hit->lastEmitMs = nowMs;
        return verdict;
    }

    if (entries_.size() >= kMaxKeys)
    {
        // 淘汰「最久没落过行」的那个键(无符号差值最大 = 最旧;回绕安全)。
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if ((nowMs - it->lastEmitMs) > (nowMs - oldest->lastEmitMs))
                oldest = it;
        entries_.erase(oldest);
    }
    Entry e;
    e.op = op;
    e.error = error;
    e.segment = segment;
    e.lastEmitMs = nowMs;
    entries_.push_back(std::move(e));
    return verdict;
}

// -----------------------------------------------------------------------------
// 全局实例
// -----------------------------------------------------------------------------
bool installAt(const juce::File& file)
{
    if (file == juce::File())
        return false;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        if (state().log != nullptr)
            return false; // 先装者为准:同一个二进制里第二个实例走到这里是正常的
        state().log = std::make_unique<RotatingLogFile>(file);
        state().deduper = DiagDeduper{};
        installedFlag().store(true, std::memory_order_release);
    }
#if SCVB_HAS_POSIX_SHM
    // 锁外设 sink:sink 自己会取同一把锁。IpcDiag 只有一个槽,默认空操作(M05),装上就是本日志。
    scvb::setIpcDiagSink(&ipcDiagSink);
#endif
    return true;
}

bool install(const juce::String& role, const juce::String& version)
{
    if (!installAt(defaultLogFile(role)))
        return false; // 非 mac(空 File)或已经装过
    juce::String opened = "SCVB " + role;
    if (version.isNotEmpty())
        opened << " " << version;
    opened << " log opened (pid " << processIdForLog() << ", host "
           << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << ")";
    write(opened);
    return true;
}

void uninstall()
{
#if SCVB_HAS_POSIX_SHM
    if (scvb::ipcDiagSink() == &ipcDiagSink)
        scvb::setIpcDiagSink(nullptr);
#endif
    const std::lock_guard<std::mutex> lock(state().mutex);
    installedFlag().store(false, std::memory_order_release);
    state().log.reset();
    state().deduper = DiagDeduper{};
}

bool isInstalled() noexcept
{
    return installedFlag().load(std::memory_order_acquire);
}

juce::File installedFile()
{
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().log != nullptr ? state().log->file() : juce::File();
}

void write(const juce::String& line)
{
    if (!isInstalled())
        return;
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        writeOnMessageThread(line);
        return;
    }
    // 不在消息线程:按值拷贝投递。没有 MessageManager 时 callAsync 返回 false,这一行丢弃。
    // 这一支**不是**「契约被违反时的兜底」:IpcDiag 的约定只保证报告在非实时线程上,而段后端会在
    // prepareToPlay / releaseResources 里被调到(例如 Input 的 claim),这两个回调跑在哪个线程由宿主定,
    // JUCE 不保证是消息线程。所以这里不加 jassert(合法宿主上也会触发)。
    juce::MessageManager::callAsync([line] { writeOnMessageThread(line); });
}

} // namespace scvb::platformlog
