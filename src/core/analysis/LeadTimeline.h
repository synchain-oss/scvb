// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LeadTimeline —— `lead_select` 在时间线上的取值记录([SL-216] / J136)。
//
// 为什么要它:`lead_select` 是宿主可自动化的全局参数(J58「每句换主唱」),插件拿不到宿主的
// 自动化曲线本身,只能在**播放经过时**看到它当下的值。J136 要求主唱居中进入分析的槽位/平衡计算
// (而不只是播放期把那一轨强制拉回中间),分析就得知道「哪一段时间谁是主唱」—— 于是 Output 在
// 走带播放时逐块记下 `lead_select` 的值,分析按区间取这里的记录。
//
// 四件东西:
//   · `LeadTimeline`:样本域的分段常值记录([t0,t1) → 0..15 + 来源),后写覆盖先写,相邻同值同来源合并。
//     只在消息线程上用(Output 持 lifecycleMutex_ 访问)。
//   · `LeadRecorder`:音频线程 → 消息线程的单生产者单消费者队列,音频线程每块 push 一条,
//     消息线程每拍排干进 `LeadTimeline`。音频线程侧无分配、无锁。
//   · `majorityLead`:分析取值 —— 一个区间里**已记录**的样本中占比最大的那个值
//     (整段没有记录 → 调用方给的回落值,[SL-545 / J143])。
//   · [SL-545 / J143b] `LeadWriteOrigin`:lead_select 最近一次改值是谁写的(插件自己 / 宿主)。音频线程
//     每块连同值一起记下它(`LeadRun::automated`);分析只把**宿主写的**记录当作自动化(`automatedLeadRuns`),
//     插件界面上改的值、撤销重做写回的值、载入工程时恢复的值一律不看:宿主记录盖到的区间按记录,其余区间
//     (一条宿主记录都没有时就是整窗)取点分析那一刻的值。
//
// 纯 C++17、JUCE-free,可在 scvb_tests 里直接测。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace scvb::analysis
{

inline constexpr int kLeadMaxValue = 15; // lead_select 值域 0..15(0 = 无主唱 / 遵循分析)

// 一段记录:样本半开区间 [t0, t1) 上 lead_select 恒为 `lead`。
// `automated`([SL-545 / J143b]):这段值是**宿主**写进来的(自动化回放 / 宿主自己的参数面板或控制器)。
// false = 插件自己写的(插件界面、撤销 / 重做、载入工程),或写入之后值没再变过(见 `LeadWriteOrigin`)。
struct LeadRun
{
    std::int64_t t0 = 0;
    std::int64_t t1 = 0;
    int lead = 0;
    bool automated = false;
};

class LeadTimeline
{
public:
    // 段数上限。正常用法(一首歌里每句换一次主唱)是几十到几百段;
    // 到顶之后新的写入整条丢弃并计数(见 droppedWrites),已有记录不动。
    static constexpr std::size_t kMaxRuns = 65536;

    // 用 (`lead`, `automated`) 覆盖 [t0, t1)。t1 <= t0、t0 < 0、lead 越界 → 忽略(返回 false)。
    // 到段数上限 → 丢弃并计数(返回 false)。相邻两段只有值**与来源都相同**才合并。
    bool write(std::int64_t t0, std::int64_t t1, int lead, bool automated);

    void clear() noexcept { runs_.clear(); }
    std::size_t size() const noexcept { return runs_.size(); }
    bool empty() const noexcept { return runs_.empty(); }
    std::uint64_t droppedWrites() const noexcept { return dropped_; }

    // 全部记录,按 t0 升序、互不重叠。
    std::vector<LeadRun> runs() const;
    // 与 [t0, t1) 有交集的记录(不裁剪)。
    std::vector<LeadRun> runsOverlapping(std::int64_t t0, std::int64_t t1) const;

    // 整份替换(state 回灌用)。调用方保证 runs 已通过 decodeLeadChunk 的校验。
    void assign(const std::vector<LeadRun>& runs);

private:
    struct Run
    {
        std::int64_t t1 = 0;
        int lead = 0;
        bool automated = false;
    };
    std::map<std::int64_t, Run> runs_; // key = t0
    std::uint64_t dropped_ = 0;
};

// 区间 [t0, t1) 的主唱:在**已记录**的样本里按覆盖样本数取最多的那个值;
// 平局取较小的值;整段一个已记录样本都没有 → `fallback`([SL-545 / J143]:分析传的是点分析
// 那一刻的 lead_select;缺省 0 = 无主唱)。只要有一个已记录样本,`fallback` 就不参与(记录优先)。
// 0 也是一个值(「这段没有选主唱」),参与计数 —— 未记录 ≠ 记录了 0:前者得 `fallback`,后者得 0。
// 不看 `automated`:分析传进来的是 `automatedLeadRuns` 滤过的那份。
int majorityLead(const std::vector<LeadRun>& runs, std::int64_t t0, std::int64_t t1, int fallback = 0);

// [SL-545 / J143b] 只留 `automated` 的记录(顺序不变)。分析拿它取多数值:一条都没有 ⇒ 每个区间都取回落值
// (= 整窗按点分析那一刻的 lead_select);有 ⇒ 被它盖到的区间按它,盖不到的区间取回落值。
std::vector<LeadRun> automatedLeadRuns(const std::vector<LeadRun>& runs);

// [SL-545 / J143b] lead_select 最近一次**改值**是谁写的。
//
// 判法:插件自己的每一次写(插件界面 `uiSetParam`、撤销 / 重做 `paramWriter`、载入工程 `replaceState`)都包在
// `ScopedPluginWrite` 里;挂在 lead_select 上的参数监听器(JUCE 只在值**真的变了**时调它,宿主那条路在音频
// 线程、插件那条路在消息线程)调 `noteValueChanged`:作用域里 ⇒ 插件,作用域外 ⇒ 宿主。音频线程每块读
// `byHost()`,连同值一起记进 `LeadRecorder`。
//
// 两条已知的边(照实写,不是判据漏洞):
//   · 宿主写进来的值与此刻的值**相同**时 JUCE 不调监听器,来源保持上一次改值时的那个 —— 自动化的第一个值恰好
//     等于播放前的值,那一截就记成「插件」(不算自动化证据),直到自动化第一次改值;
//   · 插件写的那几微秒里宿主恰好也在音频线程上改它,宿主那一笔会被记成「插件」(与 AutomationPrinter 的自写位
//     同一个窗口);下一次宿主改值即纠正。
class LeadWriteOrigin
{
public:
    // RAII。`active = false` 时什么都不做(调用方按参数 id 决定要不要标,见 OutputProcessor)。
    // 构造时就把来源**预置**成「插件」:值落地(音频线程看得见)先于监听器被调,中间那一瞬音频线程若读到
    // (新值, 旧来源) 而旧来源恰是宿主,就会凭空记下一截「宿主写的」新值 —— 一截自动化证据。预置之后
    // 最坏读到 (旧值, 插件):那一截只是不算证据。代价:插件写一次**同值**(监听器不来)也把来源改成插件。
    class ScopedPluginWrite
    {
    public:
        ScopedPluginWrite(LeadWriteOrigin& origin, bool active) noexcept : origin_(active ? &origin : nullptr)
        {
            if (origin_ != nullptr)
            {
                origin_->pluginWriting_.fetch_add(1);
                origin_->byHost_.store(false);
            }
        }
        ~ScopedPluginWrite() noexcept
        {
            if (origin_ != nullptr)
            {
                origin_->pluginWriting_.fetch_sub(1);
            }
        }
        ScopedPluginWrite(const ScopedPluginWrite&) = delete;
        ScopedPluginWrite& operator=(const ScopedPluginWrite&) = delete;

    private:
        LeadWriteOrigin* origin_;
    };

    // 参数监听器:lead_select 的值刚变了(任何线程)。
    void noteValueChanged() noexcept { byHost_.store(pluginWriting_.load() == 0); }

    // 音频线程:此刻的值是不是宿主写的。
    bool byHost() const noexcept { return byHost_.load(); }

private:
    std::atomic<int> pluginWriting_{0}; // 计数而非布尔:作用域套作用域时,内层退出不会把外层提前清掉
    std::atomic<bool> byHost_{false}; // 起始 = 插件:还没人改过它,默认值 / 刚构造不是自动化
};
static_assert(std::atomic<int>::is_always_lock_free, "LeadWriteOrigin 计数必须 lock-free(音频线程读)");
static_assert(std::atomic<bool>::is_always_lock_free, "LeadWriteOrigin 来源位必须 lock-free(音频线程读)");

// 音频线程 → 消息线程的记录队列(SPSC,定长环,满则丢并计数)。
class LeadRecorder
{
public:
    static constexpr std::uint32_t kCapacity = 4096; // 2 的幂

    // 音频线程:记一块。不分配、不加锁。`automated` = 这一块的值是宿主写的(`LeadWriteOrigin::byHost`)。
    void record(std::int64_t t0, std::int64_t t1, int lead, bool automated) noexcept;

    // 消息线程:排干进 timeline,返回取出的条数。
    std::size_t drainInto(LeadTimeline& timeline);
    // 消息线程:排干并丢弃(载入另一份工程时,队列里是旧工程那次播放的记录)。
    std::size_t discard();

    std::uint32_t droppedRecords() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Rec
    {
        std::int64_t t0 = 0;
        std::int64_t t1 = 0;
        std::int32_t lead = 0;
        bool automated = false;
    };
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "SPSC 索引必须 lock-free");
    std::array<Rec, kCapacity> ring_{};
    std::atomic<std::uint32_t> writePos_{0};
    std::atomic<std::uint32_t> readPos_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

// ---- state 编解码(LEAD chunk,docs/STATE_SCHEMA.md §三)----
// 载荷(小端):u16 minor | u16 reserved(0) | u32 runCount | runCount × 记录。
//   · minor 2([SL-545 / J143b],本构建写):记录 = { i64 t0 | i64 t1 | u32 lead | u32 flags }(24 字节);
//     flags bit0 = `automated`,其余位必须为 0(否则整块不用)。
//   · minor 1([SL-216],只读):记录 = { i64 t0 | i64 t1 | u32 lead }(20 字节),那时还不分来源 ⇒ 一律按
//     `automated = false` 读(不算自动化证据)。这一档从未随正式版发出,读它只为开发期存过的工程不整块丢。
inline constexpr std::uint16_t kLeadChunkMinor = 2;
inline constexpr std::size_t kLeadChunkHeaderBytes = 8;
inline constexpr std::size_t kLeadChunkRecordBytes = 24; // minor 2
inline constexpr std::size_t kLeadChunkRecordBytesMinor1 = 20;
inline constexpr std::uint32_t kLeadRunFlagAutomated = 1u;

enum class LeadDecodeStatus
{
    Ok,
    Malformed, // 长度/顺序/值域不合法 → 整块不用
    NewerMinor, // 更高 minor → 本构建不解,调用方原样保留字节
};

void encodeLeadChunk(const std::vector<LeadRun>& runs, std::vector<std::uint8_t>& out);
LeadDecodeStatus decodeLeadChunk(const std::uint8_t* data, std::size_t size, std::vector<LeadRun>& out);

} // namespace scvb::analysis
