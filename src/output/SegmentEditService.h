// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentEditService —— CRVS 变更事务(全量快照 + UndoManager + 重建回调)。JUCE 依赖(UndoManager),
// 故不落 scvb_core(ADR-011 JUCE-free);落 src/output/ 供 OutputEditor 与 scvb_params_tests 共用。
// 关键修复(PR#55 缺陷3):editSegmentTransactional 先跑 editTrackSegments 判结果,仅 Ok 才压入
// undo 事务并重建;失败(BadArg/NotAdjacent)不改原表、不碰 undo、不重建 —— 避免无效 undo 步。

#include <algorithm>
#include <cmath> // std::isfinite(clampManualValue 拦 NaN/Inf)
#include <functional>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "state/SegmentEdit.h"
#include "state/StateCodec.h"

namespace scvb::output
{

// CRVS 全量快照 undo 事务:perform 置 newData + 重建,undo 置 oldData + 重建。
class CrvsTransactionAction final : public juce::UndoableAction
{
public:
    CrvsTransactionAction(scvb::state::CrvsData& crvs, scvb::state::CrvsData oldData, scvb::state::CrvsData newData,
                          std::function<void()> rebuild)
        : crvs_(crvs), oldData_(std::move(oldData)), newData_(std::move(newData)), rebuild_(std::move(rebuild))
    {
    }

    bool perform() override
    {
        crvs_ = newData_;
        if (rebuild_)
            rebuild_();
        return true;
    }

    bool undo() override
    {
        crvs_ = oldData_;
        if (rebuild_)
            rebuild_();
        return true;
    }

    // [SL-209 复审 S1 / #152 复审【重要】①②] 撤销栈的容量上限按「units」记。
    //
    // 本类把 **1 unit 定义为 1 字节**:恒回 1 时那个上限等价于「3 万条事务」—— 而本事务持有的是
    // **两份整个 `CrvsData` 快照**(2 版本 × 15 轨的全部段);分析回落一次就可能是几万段,
    // 几十条这样的事务就能把内存顶到几百 MB,上限却一次都不会触发。
    //
    // 口径:两份快照的总段数 × `sizeof(Segment)`。这不是「近似」—— `Segment` = t0/t1 各 8 +
    // pan/vol 各 4 + flags 4 = 28,按 8 对齐补到 32,与 vector 里逐元素的实际占用逐字节相等
    // (下面的 static_assert 钉住;真变了就红,而不是悄悄记错账)。只数段、不数
    // pan_curve/版本名 —— 段是唯一会上量级的那项。至少回 1:0 会让这条事务在容量账上「不存在」。
    //
    // ⚠ **JUCE 的裁剪语义**(juce_UndoManager.cpp `dropOldTransactionsIfTooLarge`,8.0.8 逐行核对):
    // ```
    // while (nextIndex > 0 && totalUnitsStored > maxNumUnitsToKeep
    //                      && transactions.size() > minimumTransactionsToKeep)
    //     { 丢栈底那条 }
    // ```
    // 三个条件是**与**,`minimumTransactionsToKeep` 的优先级高于 units 上限。由此两条结论:
    //   ① **单条超限事务永远不会被丢**,只要栈里的事务数还没超过 min —— 所以「一次分析 = 一条
    //      撤销步」这个承诺**不会**因为段多而静默失效(复审里「超限动作被丢/整个历史被清空」
    //      的说法与 8.0.8 源码不符,已按源码更正);
    //   ② 真正的后果是**撤销深度**塌到 min,而内存**没有**被按字节封住(min 条大快照照样全留)。
    //      默认参数 (30000, 30) 下:两份快照总段数 ≳ 938 就吃光整个 units 预算 ⇒ 深度恒 = 30,
    //      而 30 条大事务(密集工程一条 ≈ 3.8MB)≈ 115MB 仍稳稳挂在栈上。
    // 所以本卡不再吃默认值,在 `configureCrvsUndoBudget()` 里显式定参,推导见那里。
    int getSizeInUnits() override
    {
        static_assert(sizeof(scvb::state::Segment) == 32,
                      "Segment 布局变了:撤销预算的推导(kCrvsUndoBudgetBytes 注释)按 32B/段 标定,"
                      "请同步重算预算与 SERVICE-12 的期望深度");
        const auto segBytes = [](const scvb::state::CrvsData& d) {
            std::size_t n = 0;
            for (const auto& v : d.versions)
            {
                for (const auto& t : v.tracks)
                {
                    n += t.segments.size();
                }
            }
            return n * sizeof(scvb::state::Segment);
        };
        const std::size_t bytes = segBytes(oldData_) + segBytes(newData_);
        // 下限 1(0 会让本事务在容量账上「不存在」);上限夹到 2^30 防 int 溢出。
        return static_cast<int>(std::clamp<std::size_t>(bytes, std::size_t{1}, std::size_t{1} << 30));
    }

private:
    scvb::state::CrvsData& crvs_;
    scvb::state::CrvsData oldData_;
    scvb::state::CrvsData newData_;
    std::function<void()> rebuild_;
};

// ---------------------------------------------------------------------------
// [#152 复审【重要】①②] CRVS 撤销栈的显式预算 —— 不吃 juce::UndoManager 的默认 (30000, 30)。
//
// 单位:`CrvsTransactionAction::getSizeInUnits()` 把 **1 unit 定为 1 字节**,所以这两个数就是
// 「字节预算」与「无论如何都保住的最少步数」,不需要再换算。
//
// 一条事务的开销 = (旧快照段数 + 新快照段数) × 32B。稳态下新旧同量级,记工程总段数为 N
// (N = 2 版本 × 15 轨的段数之和),则 **一步 ≈ 64·N 字节**。
//
// 预算 = 64 MiB(67,108,864 units)。它买到的深度:
//   • 典型工程 N = 15 × 2 × 200 段  =  6,000 → 384 KB/步 → ≈ 174 步
//   • 密集工程 N = 15 × 2 × 2,000 段 = 60,000 → 3.84 MB/步 → ≈  17 步
//   (对照:默认 30000 units 在这两档下都被单条事务一口吃光,深度恒 = 30 而内存无封顶 ——
//    密集工程 30 × 3.84MB ≈ 115MB。本卡是拿深度换一个**真的存在**的内存天花板。)
// 64 MiB 的量级依据:插件自身的大头是 FrameStore 的特征页(一小时素材 × 15 轨为百 MB 级),
// 撤销栈压在它下面一个档位,既不会成为内存主项,又留得下上百步的编辑历史。
//
// 最少步数 = 16。JUCE 的 min 条件优先于 units 上限(见 getSizeInUnits 的裁剪语义注),所以这是
// **硬地板**:再密的工程也保 16 步可撤。取 16 而不是更大,是为了让地板在密集档上不越过预算 ——
// 16 × 3.84MB ≈ 61MB ≲ 64MiB,与上面算出的 ≈17 步正好同一量级,两个数不打架。
//
// ⚠ 这两个数是**用户可感知行为**的定参(撤销能回多少步),改动请连同
// `SERVICE-12` 的期望深度与变更文档 `20260827-sl209-analyze-undoable.md` 一起改。
inline constexpr int kCrvsUndoBudgetBytes = 64 * 1024 * 1024;
inline constexpr int kCrvsUndoMinTransactions = 16;

// 把上面的预算装到一个 UndoManager 上。生产侧唯一调用点 = `OutputAuthority` 的构造;
// 单测调同一个函数,才算真的钉住了**生产配置**(而不是测试自己另标一套数)。
inline void configureCrvsUndoBudget(juce::UndoManager& undo)
{
    undo.setMaxNumberOfStoredUnits(kCrvsUndoBudgetBytes, kCrvsUndoMinTransactions);
}

// 通用 CRVS 变更事务(无条件成功,如 setVersionName/copyVersion/setTrackManual/setPanCurve/
// **分析回落**([J89] 起分析可撤销,见 finishAnalysis))。
inline void commitCrvsTransaction(juce::UndoManager& undo, scvb::state::CrvsData& crvs, const juce::String& name,
                                  const std::function<void()>& mutator, const std::function<void()>& rebuild)
{
    const scvb::state::CrvsData oldData = crvs;
    mutator(); // 就地改 crvs
    const scvb::state::CrvsData newData = crvs;
    undo.beginNewTransaction(name);
    undo.perform(new CrvsTransactionAction(crvs, oldData, newData, rebuild));
}

// setTrackManual(契约 §1.16 / 04 §1.5 方案 A)的产物:覆盖全时间线的单段 user_edited 常值。
//
// **pan 与 vol 是同一条常值段上的两个独立维度**,而 UI 的读回值对两维读的是同一段
// (tab-tracks.js manualConstantOf)。所以写一维时必须原样保留另一维 —— 早先的实现把另一维
// 硬写成默认值(pan 写 0、vol 写 0dB),于是「先调 vol 再调 pan」会把 vol 打回 0dB、反过来
// 把 pan 打回居中,两个维度互相冲掉(T37 三轮 D 族)。
//
// existing = 该轨替换前的段表:非空则从首段继承另一维;空表(从未编辑/分析过)才落各自默认。
// 纯函数,可离线断言。

// 手动值的值域钳制(pan −100..+100 / vol −24..+12 dB,契约 §1.16 的 `value` 域)。
// [J85] 冻结通道**不写曲线**,于是「钳制」不能再只藏在建段函数里 —— 参数面那一路也要用同一
// 把尺子,否则两条通道对同一个越界输入会给出两个不同的落地值。
//
// **非有限值必须在这里就死掉**(#106 复审重要4):`std::clamp(NaN, lo, hi)` 两次比较全 false,
// 原样把 NaN 吐回来。冻结维度上这个值不是显示用的数字,它**就是音频目标值** ——
// `convertTo0to1(NaN)` → `rawPan/rawVol` = NaN → DspArbiter 冻结分支直接拿去喂平滑器与增益,
// 一次畸形桥调用就能让整条总线出 NaN。回落 0(pan 居中 / vol 0dB)= 该维度的中性值,
// 与「参数未接线」的默认同款。桥面 `handleSetTrackManual` 另有一道 badArg 拒绝,这里是兜底:
// 桥不是唯一调用方(harness / 单测 / 将来的 native 调用者都走这条路)。
inline float clampManualValue(bool isPan, float value)
{
    if (!std::isfinite(value))
        return 0.0f;
    return isPan ? std::clamp(value, -100.0f, 100.0f) : std::clamp(value, -24.0f, 12.0f);
}

inline scvb::state::Segment makeManualConstantSegment(const std::vector<scvb::state::Segment>& existing, bool isPan,
                                                      float value)
{
    float keepPan = 0.0f;
    float keepVolDb = 0.0f;
    if (!existing.empty())
    {
        keepPan = existing.front().pan;
        keepVolDb = existing.front().volDb;
    }

    scvb::state::Segment seg;
    seg.t0 = 0;
    seg.t1 = static_cast<std::int64_t>(1) << 40; // 覆盖全时间线近似(真末端由宿主时间线提供)
    seg.pan = isPan ? clampManualValue(true, value) : keepPan;
    seg.volDb = isPan ? keepVolDb : clampManualValue(false, value);
    seg.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::UserEdited, false);
    return seg;
}

// 结果门控的段编辑事务:先 editTrackSegments 判结果,仅 Ok 才压入 undo 事务并重建;
// 失败(BadArg/NotAdjacent)直接返回,不改原表、不碰 undo、不重建(PR#55 缺陷3)。
inline scvb::state::SegmentEditResult editSegmentTransactional(juce::UndoManager& undo, scvb::state::CrvsData& crvs,
                                                               int version, int track,
                                                               const scvb::state::SegmentEditArgs& args,
                                                               const std::function<void()>& rebuild)
{
    const scvb::state::CrvsData oldData = crvs;
    auto& segments =
        crvs.versions[static_cast<std::size_t>(version - 1)].tracks[static_cast<std::size_t>(track)].segments;
    const scvb::state::SegmentEditResult result = scvb::state::editTrackSegments(segments, args);
    if (result != scvb::state::SegmentEditResult::Ok)
        return result; // 强异常安全:失败不改 segments

    const scvb::state::CrvsData newData = crvs;
    undo.beginNewTransaction("Edit segment");
    undo.perform(new CrvsTransactionAction(crvs, oldData, newData, rebuild));
    return scvb::state::SegmentEditResult::Ok;
}

} // namespace scvb::output
