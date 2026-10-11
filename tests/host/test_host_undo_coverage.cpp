// SPDX-License-Identifier: GPL-3.0-or-later
// test_host_undo_coverage —— [SL-536 / J140] 轨道页开关 × 插件撤销栈(真 Processor,免 DAW)。
//
// 用户裁定 J140(2026-09-28):三类都做 ——
//   ① A1-A7(`setChannelConfig`,契约 §1.15)进撤销栈;
//   ② 自动化参数(冻结 P/V、每轨 W、Tab1 的 WIDTH / MS BALANCE / LEAD SELECT;契约 §1.12-§1.14)
//      与冻结通道的 PAN / 音量卡箍(§1.16 ①)进撤销栈,撤销 / 重做**经宿主参数通路**写回;
//   ③ 首次接管音量卡箍那一步的撤销连冻结位(与参数面)一起回滚(§1.16 ②)。
//
// 每个落点一格(本文件的 TEST_CASE 与落点一一对应,删除式验证按格记在 PR 描述里):
//   UNDO-A   A1-A7 七个字段,表驱动,每字段一个 SECTION;
//   UNDO-P   五类自动化参数,每类一个 SECTION;撤销 / 重做都断「宿主看见了成对的 begin/end、
//            且值是经 setValueNotifyingHost 这条通知通路到达的」(GestureSpy);
//   UNDO-F   冻结通道 PAN / VOL;
//   UNDO-T   首次接管:一次撤销 = 段表 + 参数面 + 冻结位;配对照臂(跟进写了别的位 ⇒ 另起一步);
//   UNDO-C   合并:连续量在窗内并、开关不并、出窗不并;配置的步进量并、开关不并;
//   UNDO-S   单栈:段表类 / 参数类 / 配置类交错,Ctrl+Z 按时间倒序弹;
//   UNDO-PRINT PRINT 态:用户编辑照常受理 ⇒ 撤销也照常受理(同一条规矩);车道参数的撤销
//            不被记成 hostEcho;宿主随后按自动化顶回来的那一下照常记 hostEcho、不进插件撤销栈。
//   UNDO-L   载入 state 清栈:按 blob 形态分格,每格对应 setStateInformation 里的一个清栈落点
//            (只带 PRMS 的预设走 CFGS 缺失早退,到不了函数末尾 —— #311 第 7 轮复审【重要】)。
//
// 为什么在 host 套件而不是 params 套件:要断的是**真 Processor + 真 APVTS + 真宿主通知通路**
// (AudioProcessorListener 收到的 begin/end/value),ParamWriteAction 那个零件本身另有
// test_segment_edit_service.cpp 的 PARAM-UNDO-1 守着 —— 这里守的是它**有没有被接上**。
//
// 组号:本文件只用一台 Output、不连 Input,组号取 kUndoGroup(4)。与 test_host_harness.cpp 的
// 组号纪律同口径(8 是保留组,不得分配)。同一可执行文件里各 TEST_CASE 串行,rig 随格析构、段随之释放。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "OutputProcessor.h"
#include "engine/AuthorityMode.h"
#include "state/StateCodec.h"
#include "state/StateMigration.h"

namespace
{

constexpr int kUndoGroup = 4;
constexpr double kUndoSr = 48000.0;
constexpr int kUndoBlock = 512;

struct UndoRig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScvbOutputAudioProcessor out;

    UndoRig()
    {
        out.setGroupId(kUndoGroup);
        REQUIRE(out.groupId() == kUndoGroup); // 组号越界会静默回落 g1(见 harness 的 kGroup 头注)
        out.prepareToPlay(kUndoSr, kUndoBlock);
        REQUIRE(out.isPrepared());
    }

    ~UndoRig() { out.releaseResources(); }

    juce::RangedAudioParameter& param(const juce::String& id)
    {
        auto* p = out.getAPVTS().getParameter(id);
        REQUIRE(p != nullptr);
        return *p;
    }

    float eng(const juce::String& id) { return param(id).convertFrom0to1(param(id).getValue()); }

    // UI 的一次性三段式(oneShotGesture):begin → set → end。
    void uiEdit(const juce::String& id, float engValue)
    {
        REQUIRE(out.uiBeginParamGesture(id));
        REQUIRE(out.uiSetParam(id, engValue));
        REQUIRE(out.uiEndParamGesture(id));
    }

    int segCount(int ch)
    {
        const auto crvs = out.crvsSnapshot();
        return static_cast<int>(crvs.versions[static_cast<std::size_t>(out.versionActive() - 1)]
                                    .tracks[static_cast<std::size_t>(ch - 1)]
                                    .segments.size());
    }
};

// 工程值比较的容差:工程值 ↔ 归一化往返不保证逐位复原(CI 实测 width 40.0f 差 1 ulp)。
// 1e-3 远小于任何参数的一步(最细的 ms_balance 一步 = 1,freeze / lead_select 为整数)。
bool closeTo(float a, float b)
{
    return std::abs(a - b) < 1.0e-3f;
}

// 宿主替身:AudioProcessorListener 就是 VST3 wrapper 收 begin/end/performEdit 的那一层。
// 只数「经通知通路到达」的东西 —— 裸 setValue(不通知宿主)在这里一次都看不见,
// 那正是本卡要排除的「插件内部静默写、宿主与插件不同步」。
struct HostSpy final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float v) override { last[index] = v; }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override
    {
        ++begins[index];
        if (open[index])
            ++doubleBegin;
        open[index] = true;
    }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override
    {
        ++ends[index];
        if (!open[index])
            ++orphanEnd;
        open[index] = false;
    }
    void reset()
    {
        last.clear();
        begins.clear();
        ends.clear();
        open.clear();
        doubleBegin = 0;
        orphanEnd = 0;
    }

    std::map<int, float> last;
    std::map<int, int> begins;
    std::map<int, int> ends;
    std::map<int, bool> open;
    int doubleBegin = 0;
    int orphanEnd = 0;
};

// 「经宿主通路写回了 id,值是 want(工程值)」—— 成对 begin/end 各一次、通知值对得上。
void checkHostSaw(HostSpy& spy, juce::RangedAudioParameter& p, float wantEng)
{
    const int idx = p.getParameterIndex();
    CHECK(spy.begins[idx] == 1);
    CHECK(spy.ends[idx] == 1);
    CHECK(spy.doubleBegin == 0);
    CHECK(spy.orphanEnd == 0);
    REQUIRE(spy.last.count(idx) == 1);
    CHECK(closeTo(p.convertFrom0to1(spy.last[idx]), wantEng));
}

// 在 base 的容器上只留 keep 认的 chunk;cfgsPayload 非空 = 把 CFGS 的载荷换成它。
std::vector<std::uint8_t> reshapeBlob(const juce::MemoryBlock& base, const std::function<bool(std::uint32_t)>& keep,
                                      const std::vector<std::uint8_t>& cfgsPayload = {})
{
    scvb::state::StateChunks chunks;
    REQUIRE(scvb::state::loadState(static_cast<const std::uint8_t*>(base.getData()), base.getSize(), chunks).status ==
            scvb::state::StateLoadStatus::Ok);
    chunks.chunks.erase(std::remove_if(chunks.chunks.begin(), chunks.chunks.end(),
                                       [&keep](const scvb::state::Chunk& c) { return !keep(c.fourcc); }),
                        chunks.chunks.end());
    for (auto& c : chunks.chunks)
    {
        if (c.fourcc == scvb::state::kFourccCfgs && !cfgsPayload.empty())
            c.payload = cfgsPayload;
    }
    std::vector<std::uint8_t> out;
    REQUIRE(scvb::state::encodeContainer(chunks, out));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// UNDO-A:A1-A7(§1.15)。每个字段:改 → 撤销回旧值 → 重做回新值。
// participate 额外断「显式设置过」这一位也一并还原([J83]:未设置 = 按默认参与)。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-A:A1-A7 通道配置七项各自进插件撤销栈", "[host][sl536][undo]")
{
    UndoRig r;
    using Patch = ScvbOutputAudioProcessor::ChannelConfigPatch;

    struct Row
    {
        const char* name;
        int index; // 每个字段用不同的轨,互不串台
        Patch patch;
        std::function<bool(const OutputRuntimeState::Channel&)> isNew;
        std::function<bool(const OutputRuntimeState::Channel&)> isOld;
    };
    std::vector<Row> rows;
    {
        Patch p;
        p.enabled = false;
        rows.push_back(
            {"A enabled", 0, p, [](const auto& c) { return !c.enabled; }, [](const auto& c) { return c.enabled; }});
    }
    {
        Patch p;
        p.label = juce::String("Vox");
        rows.push_back({"label", 1, p, [](const auto& c) { return c.label == "Vox"; },
                        [](const auto& c) { return c.label.isEmpty(); }});
    }
    {
        Patch p;
        p.priority = 8;
        rows.push_back({"priority", 2, p, [](const auto& c) { return c.priority == 8; },
                        [](const auto& c) { return c.priority == 5; }});
    }
    {
        Patch p;
        p.leadLock = true;
        rows.push_back(
            {"lead_lock", 3, p, [](const auto& c) { return c.leadLock; }, [](const auto& c) { return !c.leadLock; }});
    }
    {
        Patch p;
        p.leadVolExempt = true;
        rows.push_back({"lead_vol_exempt", 4, p, [](const auto& c) { return c.leadVolExempt; },
                        [](const auto& c) { return !c.leadVolExempt; }});
    }
    {
        Patch p;
        p.participate = false;
        rows.push_back({"participate_in_auto_pan", 5, p,
                        [](const auto& c) { return c.participateAutoPanSet && !c.participatesInAutoPan(); },
                        [](const auto& c) { return !c.participateAutoPanSet && c.participatesInAutoPan(); }});
    }
    {
        Patch p;
        p.pairId = 3;
        rows.push_back({"pair_id", 6, p, [](const auto& c) { return c.pairId == 3; },
                        [](const auto& c) { return c.pairId == 0; }});
    }
    REQUIRE(rows.size() == 7);

    for (const auto& row : rows)
    {
        DYNAMIC_SECTION("字段 " << row.name)
        {
            const auto idx = static_cast<std::size_t>(row.index);
            REQUIRE(row.isOld(r.out.channelsSnapshot()[idx])); // 前提:出厂默认
            REQUIRE(r.out.bridgeApplyChannelConfig(row.index, row.patch));
            REQUIRE(row.isNew(r.out.channelsSnapshot()[idx]));

            CHECK(r.out.undo());
            CHECK(row.isOld(r.out.channelsSnapshot()[idx]));
            CHECK(r.out.redo());
            CHECK(row.isNew(r.out.channelsSnapshot()[idx]));
        }
    }

    SECTION("值没变不压步:同值再下发后,撤销弹的是更早那一步")
    {
        Patch a;
        a.priority = 9;
        REQUIRE(r.out.bridgeApplyChannelConfig(9, a));
        REQUIRE_FALSE(r.out.bridgeApplyChannelConfig(9, a)); // 同值:不算变化
        CHECK(r.out.undo());
        CHECK(r.out.channelsSnapshot()[9].priority == 5);
        CHECK_FALSE(r.out.undo()); // 栈里只有那一步
    }
}

// ---------------------------------------------------------------------------
// UNDO-P:五类自动化参数(§1.12-§1.14)。撤销 / 重做都经宿主 gesture 通路写回。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-P:自动化参数每类一格,撤销 / 重做经宿主 gesture 写回", "[host][sl536][undo]")
{
    HostSpy spy; // 须比 rig 活得久(同 harness 的 GestureSpy)
    UndoRig r;
    r.out.addListener(&spy);

    const int v = r.out.versionActive();
    struct Case
    {
        const char* name;
        juce::String id;
        float target;
    };
    const Case cases[] = {
        {"全局 WIDTH", "width", 40.0f},
        {"MS BALANCE", "ms_balance", -30.0f},
        {"LEAD SELECT", "lead_select", 5.0f},
        {"每轨 W", scvb::params::widthId(v, 2), 60.0f},
        {"冻结 P/V", scvb::params::freezeId(v, 2), 3.0f},
    };

    for (const auto& c : cases)
    {
        DYNAMIC_SECTION("参数 " << c.name)
        {
            auto& p = r.param(c.id);
            const float before = r.eng(c.id);
            REQUIRE(before != c.target);
            r.uiEdit(c.id, c.target);
            REQUIRE(closeTo(r.eng(c.id), c.target));

            spy.reset();
            CHECK(r.out.undo());
            CHECK(closeTo(r.eng(c.id), before));
            checkHostSaw(spy, p, before);

            spy.reset();
            CHECK(r.out.redo());
            CHECK(closeTo(r.eng(c.id), c.target));
            checkHostSaw(spy, p, c.target);
        }
    }

    SECTION("拖了又拖回原处:不压步")
    {
        const float before = r.eng("ms_balance");
        REQUIRE(r.out.uiBeginParamGesture("ms_balance"));
        REQUIRE(r.out.uiSetParam("ms_balance", 20.0f));
        REQUIRE(r.out.uiSetParam("ms_balance", before));
        REQUIRE(r.out.uiEndParamGesture("ms_balance"));
        CHECK_FALSE(r.out.undo());
    }

    SECTION("一次拖动 = 一步(中间多少次 setParam 都只压一步)")
    {
        const float before = r.eng("width");
        REQUIRE(r.out.uiBeginParamGesture("width"));
        for (float x = 100.0f; x <= 140.0f; x += 5.0f)
            REQUIRE(r.out.uiSetParam("width", x));
        REQUIRE(r.out.uiEndParamGesture("width"));
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng("width"), before));
        CHECK_FALSE(r.out.undo());
    }

    r.out.removeListener(&spy);
}

// ---------------------------------------------------------------------------
// UNDO-F:冻结通道的 PAN / 音量卡箍(§1.16 ①)。段表逐字节不动、参数面回到旧值。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-F:冻结通道 PAN / VOL 的手动值进撤销栈", "[host][sl536][undo]")
{
    HostSpy spy;
    UndoRig r;
    r.out.addListener(&spy);
    constexpr int kCh = 3;
    const int v = r.out.versionActive();
    r.uiEdit(scvb::params::freezeId(v, kCh), 3.0f); // 两维都冻(本身是一步)
    const int segsBefore = r.segCount(kCh);

    for (const bool isPan : {true, false})
    {
        DYNAMIC_SECTION((isPan ? "PAN" : "VOL"))
        {
            const juce::String id = isPan ? scvb::params::panId(v, kCh) : scvb::params::volId(v, kCh);
            const float before = r.eng(id);
            const float target = isPan ? 40.0f : -6.0f;
            int replaced = -1;
            int locked = -1;
            REQUIRE(r.out.setTrackManual(kCh, isPan, target, replaced, locked));
            REQUIRE(replaced == 0); // 冻结通道:一段都不替换
            REQUIRE(closeTo(r.eng(id), target));

            spy.reset();
            CHECK(r.out.undo());
            CHECK(closeTo(r.eng(id), before));
            checkHostSaw(spy, r.param(id), before);
            CHECK(r.segCount(kCh) == segsBefore); // 段表没动过,撤销也不碰
            CHECK(closeTo(r.eng(scvb::params::freezeId(v, kCh)), 3.0f)); // 只撤这一步,冻结那一步还在

            spy.reset();
            CHECK(r.out.redo());
            CHECK(closeTo(r.eng(id), target));
            checkHostSaw(spy, r.param(id), target);
        }
    }
    r.out.removeListener(&spy);
}

// ---------------------------------------------------------------------------
// UNDO-T:首次接管(§1.16 ②)。UI 的顺序:setTrackManual 落地 → 收到回执 → 把该维度冻结位置 1
// (tab-tracks.js sendManual 的 oneShotGesture)。一次撤销必须把三样一起回滚。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-T:首次接管的撤销连段表、参数面、冻结位一起回滚", "[host][sl536][undo]")
{
    UndoRig r;
    constexpr int kCh = 5;
    const int v = r.out.versionActive();
    const juce::String volId = scvb::params::volId(v, kCh);
    const juce::String frzId = scvb::params::freezeId(v, kCh);
    const float volBefore = r.eng(volId);
    REQUIRE(closeTo(r.eng(frzId), 0.0f));
    REQUIRE(r.segCount(kCh) == 0); // 从没分析过:接管前段表为空

    int replaced = 0;
    int locked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/false, -6.0f, replaced, locked));
    REQUIRE(r.segCount(kCh) == 1); // 接管通道:写了全时限常值段

    SECTION("UI 跟进置 vol 位 ⇒ 并进接管那一步")
    {
        r.uiEdit(frzId, 2.0f); // bit1 = vol
        REQUIRE(closeTo(r.eng(frzId), 2.0f));

        CHECK(r.out.undo()); // **一次**
        CHECK(r.segCount(kCh) == 0);
        CHECK(closeTo(r.eng(volId), volBefore));
        CHECK(closeTo(r.eng(frzId), 0.0f)); // 冻结位跟着回去 —— 旋钮与声音回到拖之前
        CHECK_FALSE(r.out.undo()); // 跟进没有另起一步

        CHECK(r.out.redo());
        CHECK(r.segCount(kCh) == 1);
        CHECK(closeTo(r.eng(volId), -6.0f));
        CHECK(closeTo(r.eng(frzId), 2.0f));
    }

    SECTION("对照臂:跟进置的不是本维度那一位 ⇒ 另起一步(判据真的在看位)")
    {
        r.uiEdit(frzId, 3.0f); // 两位都置 —— 不是「起点 | vol 位」
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng(frzId), 0.0f));
        CHECK(r.segCount(kCh) == 1); // 接管那一步还在
        CHECK(r.out.undo());
        CHECK(r.segCount(kCh) == 0);
        CHECK(closeTo(r.eng(volId), volBefore));
    }

    SECTION("对照臂:跟进之前插进了别的一步 ⇒ 不并(栈顶已不是接管那一步)")
    {
        r.uiEdit("ms_balance", 10.0f);
        r.uiEdit(frzId, 2.0f);
        CHECK(r.out.undo()); // 冻结
        CHECK(closeTo(r.eng(frzId), 0.0f));
        CHECK(closeTo(r.eng("ms_balance"), 10.0f));
        CHECK(r.out.undo()); // ms
        CHECK(r.out.undo()); // 接管(段表 + vol;冻结占位没被填,不写)
        CHECK(r.segCount(kCh) == 0);
        CHECK(closeTo(r.eng(volId), volBefore));
    }
}

// ---------------------------------------------------------------------------
// UNDO-C:合并。连续量 / 步进量在 kUndoCoalesceMs 窗内并成一步;开关不并;出窗不并。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-C:键盘 / 滚轮连按合并成一步,开关不合并", "[host][sl536][undo]")
{
    UndoRig r;
    using Patch = ScvbOutputAudioProcessor::ChannelConfigPatch;

    SECTION("MS BALANCE 三下方向键(窗内)= 一步")
    {
        const float before = r.eng("ms_balance");
        r.uiEdit("ms_balance", 1.0f);
        r.uiEdit("ms_balance", 2.0f);
        r.uiEdit("ms_balance", 3.0f);
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng("ms_balance"), before));
        CHECK_FALSE(r.out.undo());
        CHECK(r.out.redo());
        CHECK(closeTo(r.eng("ms_balance"), 3.0f)); // 重做到这一串的末值
    }

    SECTION("出窗 = 两步")
    {
        const float before = r.eng("ms_balance");
        r.uiEdit("ms_balance", 1.0f);
        juce::Thread::sleep(static_cast<int>(ScvbOutputAudioProcessor::kUndoCoalesceMs) + 150);
        r.uiEdit("ms_balance", 2.0f);
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng("ms_balance"), 1.0f));
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng("ms_balance"), before));
    }

    SECTION("冻结开关两下(窗内)= 两步")
    {
        const juce::String id = scvb::params::freezeId(r.out.versionActive(), 1);
        r.uiEdit(id, 1.0f);
        r.uiEdit(id, 0.0f);
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng(id), 1.0f));
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng(id), 0.0f));
    }

    SECTION("优先级 ± 连点(窗内)= 一步;开关连点 = 两步")
    {
        Patch a;
        a.priority = 6;
        Patch b;
        b.priority = 7;
        REQUIRE(r.out.bridgeApplyChannelConfig(0, a));
        REQUIRE(r.out.bridgeApplyChannelConfig(0, b));
        CHECK(r.out.undo());
        CHECK(r.out.channelsSnapshot()[0].priority == 5);
        CHECK_FALSE(r.out.undo());

        Patch off;
        off.leadLock = true;
        Patch on;
        on.leadLock = false;
        REQUIRE(r.out.bridgeApplyChannelConfig(1, off));
        REQUIRE(r.out.bridgeApplyChannelConfig(1, on));
        CHECK(r.out.undo());
        CHECK(r.out.channelsSnapshot()[1].leadLock);
        CHECK(r.out.undo());
        CHECK_FALSE(r.out.channelsSnapshot()[1].leadLock);
    }

    SECTION("不同参数不并:WIDTH 与 MS BALANCE 交替 = 各自一步")
    {
        r.uiEdit("width", 90.0f);
        r.uiEdit("ms_balance", 5.0f);
        CHECK(r.out.undo());
        CHECK(closeTo(r.eng("ms_balance"), 0.0f));
        CHECK(closeTo(r.eng("width"), 90.0f));
    }
}

// ---------------------------------------------------------------------------
// UNDO-S:单栈。段表类(接管)/ 参数类 / 配置类交错,撤销按时间倒序。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-S:段表 / 参数 / 配置同一条撤销栈,按时间倒序弹", "[host][sl536][undo]")
{
    UndoRig r;
    constexpr int kCh = 7;
    int replaced = 0;
    int locked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/false, -3.0f, replaced, locked)); // ① 段表类
    r.uiEdit("lead_select", 2.0f); // ② 参数类
    ScvbOutputAudioProcessor::ChannelConfigPatch lbl;
    lbl.label = juce::String("Lead");
    REQUIRE(r.out.bridgeApplyChannelConfig(0, lbl)); // ③ 配置类

    CHECK(r.out.undo());
    CHECK(r.out.channelsSnapshot()[0].label.isEmpty());
    CHECK(closeTo(r.eng("lead_select"), 2.0f));
    CHECK(r.segCount(kCh) == 1);

    CHECK(r.out.undo());
    CHECK(closeTo(r.eng("lead_select"), 0.0f));
    CHECK(r.segCount(kCh) == 1);

    CHECK(r.out.undo());
    CHECK(r.segCount(kCh) == 0);
    CHECK_FALSE(r.out.undo());

    // 重做同序前进,且中途插一笔新编辑会清掉剩下的重做(与段编辑同一条 JUCE 语义)。
    CHECK(r.out.redo());
    CHECK(r.segCount(kCh) == 1);
    r.uiEdit("ms_balance", 12.0f);
    CHECK_FALSE(r.out.redo());
}

// ---------------------------------------------------------------------------
// UNDO-PRINT:PRINT 态。契约 §1.12-§1.14 / §1.15 / §1.16 在 PRINT 下都**不拒绝**用户编辑
// (「这是宿主可录的用户操作面」),所以撤销同样受理 —— 同一条规矩,不另立。
// 车道参数(pan/vol)的撤销带自写位 ⇒ 不记 hostEcho;随后宿主按自动化把值顶回来(Read 档)
// 是一次**非自写**写入 ⇒ 照常记 hostEcho(UI 的「宿主正在写」如实亮),且**不进插件撤销栈**。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-PRINT:PRINT 态下撤销与用户编辑同规矩;撤销不冒充宿主回吐", "[host][sl536][undo][print]")
{
    HostSpy spy;
    UndoRig r;
    r.out.addListener(&spy);
    constexpr int kCh = 9;
    const int v = r.out.versionActive();
    const juce::String panId = scvb::params::panId(v, kCh);
    const juce::String frzId = scvb::params::freezeId(v, kCh);

    r.out.getPrinter().setMode(scvb::engine::AuthorityMode::Print);
    REQUIRE(r.out.getPrinter().mode() == scvb::engine::AuthorityMode::Print);

    // 用户编辑在 PRINT 下受理(前提:否则「撤销同规矩」无从谈起)。
    r.uiEdit(frzId, 1.0f); // 冻 pan
    const float panBefore = r.eng(panId);
    int replaced = 0;
    int locked = 0;
    REQUIRE(r.out.setTrackManual(kCh, /*isPan=*/true, 55.0f, replaced, locked));
    REQUIRE(closeTo(r.eng(panId), 55.0f));

    const int echoBefore = r.out.getPrinter().hostEchoCount();
    spy.reset();
    CHECK(r.out.undo()); // 受理
    CHECK(closeTo(r.eng(panId), panBefore));
    checkHostSaw(spy, r.param(panId), panBefore);
    CHECK(r.out.getPrinter().hostEchoCount() == echoBefore); // 撤销不是宿主回吐

    // 宿主 Read 档按车道数据把值顶回来:宿主那一下照常记 hostEcho,插件撤销栈不动
    // (仍可重做 = 宿主写没有被记成新的一步、也没把重做栈冲掉)。
    auto& pan = r.param(panId);
    pan.beginChangeGesture();
    pan.setValueNotifyingHost(pan.convertTo0to1(-20.0f));
    pan.endChangeGesture();
    CHECK(r.out.getPrinter().hostEchoCount() == echoBefore + 1);
    CHECK(closeTo(r.eng(panId), -20.0f)); // 宿主赢(与用户拖旋钮之后被顶回去是同一个结局)
    CHECK(r.out.redo());
    CHECK(closeTo(r.eng(panId), 55.0f));
    CHECK(r.out.getPrinter().hostEchoCount() == echoBefore + 1); // 重做同样不冒充

    r.out.getPrinter().setMode(scvb::engine::AuthorityMode::Follow);
    r.out.removeListener(&spy);
}

// ---------------------------------------------------------------------------
// UNDO-L:载入 state 后撤销 / 重做两边都空(#311 第 7 轮复审【重要】)。setStateInformation 有两个
// 清栈落点,按 blob 形态分格,每格只靠其中一处:
//   · 只带 PRMS(轨道 / 参数预设,CFGS 缺失早退)、PRMS + 解不开的 CFGS(解不开早退)—— 都走不到
//     函数末尾,靠 PRMS 读回处那一处;拖动中途载入那格另钉同一处里的 `uiGestures_.clear()`;
//   · 带 CFGS、不带 PRMS —— PRMS 那处不执行,靠末尾那一处。
// 清栈三行里 `resetUndoTracking()` 本格**钉不住**(单删全绿):合并与接管占位都先核「栈顶事务名」,
// 清栈后恒不中,它是纵深防御。
// 栈里压的是参数步:撤销时写回的正是被 PRMS 覆盖的那个参数,所以除了「撤销 / 重做都返回 false」,
// 还断 width 仍是预设值(改前:撤销把它写回载入前的出厂值,预设里的值被悄悄撤掉)。
// ---------------------------------------------------------------------------
TEST_CASE("SL-536 UNDO-L:载入 state 清空撤销 / 重做栈(含只带 PRMS 的预设)", "[host][sl536][undo]")
{
    UndoRig r;
    auto& width = r.param("width");
    const float w0 = r.eng("width"); // 出厂值
    constexpr float kPresetW = 70.0f;
    REQUIRE_FALSE(closeTo(w0, kPresetW));
    REQUIRE_FALSE(closeTo(w0, 40.0f));

    // 预设里 width = 70。经宿主通路写(不进插件撤销栈,见 UNDO-PRINT),存下后写回出厂值。
    width.setValueNotifyingHost(width.convertTo0to1(kPresetW));
    juce::MemoryBlock snap;
    r.out.getStateInformation(snap);
    width.setValueNotifyingHost(width.convertTo0to1(w0));
    REQUIRE(closeTo(r.eng("width"), w0));
    REQUIRE(closeTo(r.eng("ms_balance"), 0.0f));
    REQUIRE_FALSE(r.out.undo()); // 前提:到这里栈是空的(宿主写不压步)

    const auto isPrms = [](std::uint32_t f) { return f == scvb::state::kFourccPrms; };
    const auto load = [&r](const std::vector<std::uint8_t>& blob) {
        r.out.setStateInformation(blob.data(), static_cast<int>(blob.size()));
    };
    // 载入前的栈:可撤销一步(width 出厂值 → 40)+ 可重做一步(ms_balance 0 → -30)。
    const auto seedStack = [&r] {
        r.uiEdit("width", 40.0f);
        r.uiEdit("ms_balance", -30.0f);
        REQUIRE(r.out.undo());
        REQUIRE(closeTo(r.eng("ms_balance"), 0.0f));
    };

    SECTION("只带 PRMS 的预设(CFGS 缺失早退)")
    {
        seedStack();
        load(reshapeBlob(snap, isPrms));
        REQUIRE((r.out.stateNotRestoredMask() & scvb::output::kNotRestoredCfgsMissing) != 0); // 前提:走的是缺失那支
        REQUIRE(closeTo(r.eng("width"), kPresetW)); // 前提:PRMS 确实被采用

        CHECK_FALSE(r.out.undo());
        CHECK(closeTo(r.eng("width"), kPresetW)); // ★ 改前:回到出厂值
        CHECK_FALSE(r.out.redo());
        CHECK(closeTo(r.eng("ms_balance"), 0.0f));
    }

    SECTION("PRMS + 解不开的 CFGS(解不开早退)")
    {
        seedStack();
        const std::vector<std::uint8_t> badCfgs(12, std::uint8_t{0xEE});
        load(reshapeBlob(snap, [](std::uint32_t) { return true; }, badCfgs));
        REQUIRE((r.out.stateNotRestoredMask() & scvb::output::kNotRestoredCfgsRejected) != 0); // 前提:走的是解不开那支
        REQUIRE(closeTo(r.eng("width"), kPresetW));

        CHECK_FALSE(r.out.undo());
        CHECK(closeTo(r.eng("width"), kPresetW));
        CHECK_FALSE(r.out.redo());
        CHECK(closeTo(r.eng("ms_balance"), 0.0f));
    }

    SECTION("拖动中途载入只带 PRMS 的预设:松手不按载入前的起点压步")
    {
        REQUIRE(r.out.uiBeginParamGesture("width"));
        REQUIRE(r.out.uiSetParam("width", 40.0f));
        load(reshapeBlob(snap, isPrms));
        REQUIRE(closeTo(r.eng("width"), kPresetW));
        REQUIRE(r.out.uiEndParamGesture("width"));

        CHECK_FALSE(r.out.undo()); // ★ 不清 uiGestures_ ⇒ 松手按「出厂值 → 40」压一步
        CHECK(closeTo(r.eng("width"), kPresetW));
    }

    SECTION("带 CFGS、不带 PRMS(走到末尾那处)")
    {
        seedStack();
        load(reshapeBlob(snap, [isPrms](std::uint32_t f) { return !isPrms(f); }));
        REQUIRE((r.out.stateNotRestoredMask() &
                 (scvb::output::kNotRestoredCfgsMissing | scvb::output::kNotRestoredCfgsRejected)) == 0);
        REQUIRE(closeTo(r.eng("width"), 40.0f)); // 前提:PRMS 不在,参数没被覆盖

        CHECK_FALSE(r.out.undo());
        CHECK(closeTo(r.eng("width"), 40.0f));
        CHECK_FALSE(r.out.redo());
        CHECK(closeTo(r.eng("ms_balance"), 0.0f));
    }
}
