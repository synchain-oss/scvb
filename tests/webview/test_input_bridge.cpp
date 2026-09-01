// SPDX-License-Identifier: GPL-3.0-or-later
// test_input_bridge —— T30 Input 桥 L0 单测:claim 六值映射([R3 收口])/srMismatch 推导/
// remoteSetPriority 拒绝语义/五个事件与首帧快照的载荷形状(键名逐字对契约 §3/§4/§5)。
// 只测 InputBridgeLogic 纯函数与 InputBridgeApi 常量表;InputEditor 依赖真 WebView2,留待
// gate 8 真机 GUI pluginval。

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <limits>

#include <juce_core/juce_core.h>

#include "InputBridgeApi.h"
#include "InputBridgeLogic.h"

namespace
{
using scvb::input::InputClaimState;
using scvb::input::InputConnSnapshot;
using scvb::input::bridge::advanceConfigSeq;
using scvb::input::bridge::advanceEmitCache;
using scvb::input::bridge::buildConfigPayload;
using scvb::input::bridge::buildConnPayload;
using scvb::input::bridge::buildErrorPayload;
using scvb::input::bridge::buildGroupsPayload;
using scvb::input::bridge::buildInputSnapshot;
using scvb::input::bridge::buildPriorityResponse;
using scvb::input::bridge::buildStatePayload;
using scvb::input::bridge::claimEdgeConsumed;
using scvb::input::bridge::claimErrorEdgeChanged;
using scvb::input::bridge::claimValue;
using scvb::input::bridge::ConfigSnapshot;
using scvb::input::bridge::conflictResponse;
using scvb::input::bridge::parseIntArg;
using scvb::input::bridge::PriorityReject;
using scvb::input::bridge::priorityRejection;
using scvb::input::bridge::priorityRejectReason;
using scvb::input::bridge::srMismatch;

juce::DynamicObject::Ptr obj(const juce::var& v)
{
    auto* o = v.getDynamicObject();
    REQUIRE(o != nullptr);
    return o;
}

InputConnSnapshot connSnapshot(bool outputOnline = false, bool maskBit = false, bool capturing = false,
                               bool passthrough = true, bool passthroughPending = false, int occupiedMask = 0)
{
    InputConnSnapshot s;
    s.outputOnline = outputOnline;
    s.maskBit = maskBit;
    s.capturing = capturing;
    s.passthrough = passthrough;
    s.passthroughPending = passthroughPending;
    s.occupiedMask = static_cast<std::uint16_t>(occupiedMask);
    return s;
}
} // namespace

TEST_CASE("T30 InputBridgeApi 名表与契约 §7 逐项一致(7 函数 / 5 事件)")
{
    using namespace scvb::input::bridge;
    CHECK(juce::String(kFnRequestInitialState) == "requestInitialState");
    CHECK(juce::String(kFnSetChannelId) == "setChannelId");
    CHECK(juce::String(kFnSetGroupId) == "setGroupId");
    CHECK(juce::String(kFnRemoteSetPriority) == "remoteSetPriority");
    CHECK(juce::String(kFnSetUiScale) == "setUiScale");
    CHECK(juce::String(kFnCommitUiScale) == "commitUiScale");
    CHECK(juce::String(kFnSetLang) == "setLang");
    CHECK(juce::String(kEvState) == "scvb.state");
    CHECK(juce::String(kEvConn) == "scvb.conn");
    CHECK(juce::String(kEvConfig) == "scvb.config");
    CHECK(juce::String(kEvGroups) == "scvb.groups");
    CHECK(juce::String(kEvError) == "scvb.error");
}

TEST_CASE("T30 claim 六值映射列全([R3 收口] unassigned|idle|active|conflict|abiMismatch|srMismatch)")
{
    CHECK(claimValue(InputClaimState::kUnassigned, true, true) == "unassigned");
    CHECK(claimValue(InputClaimState::kConflict, true, true) == "conflict");
    CHECK(claimValue(InputClaimState::kAbiMismatch, true, true) == "abiMismatch");
    CHECK(claimValue(InputClaimState::kUnavailable, true, true) == "idle"); // I0 段未打开 → idle
    CHECK(claimValue(InputClaimState::kActive, true, false) == "active");
    CHECK(claimValue(InputClaimState::kActive, false, false) == "idle"); // 已 claim 但 Output 未健康读取
    // srMismatch 只作用于 kActive 且优先于 active/idle(§5.2)。
    CHECK(claimValue(InputClaimState::kActive, true, true) == "srMismatch");
    CHECK(claimValue(InputClaimState::kActive, false, true) == "srMismatch");
    // 非 kActive 下 sr 推导不生效。
    CHECK(claimValue(InputClaimState::kConflict, false, true) == "conflict");
}

TEST_CASE("T30 srMismatch 推导:仅 claim active ∧ Output SR 非零 ∧ ≠ 本机 SR(§4.1)")
{
    CHECK(srMismatch(InputClaimState::kActive, 44100, 48000));
    CHECK_FALSE(srMismatch(InputClaimState::kActive, 48000, 48000)); // 同 SR
    CHECK_FALSE(srMismatch(InputClaimState::kActive, 0, 48000)); // Output 未报 SR(离线/未接线)
    CHECK_FALSE(srMismatch(InputClaimState::kUnassigned, 44100, 48000));
    CHECK_FALSE(srMismatch(InputClaimState::kConflict, 44100, 48000));
    CHECK_FALSE(srMismatch(InputClaimState::kAbiMismatch, 44100, 48000));
}

TEST_CASE("T30 remoteSetPriority 拒绝语义与优先级:unassigned > outputOffline > notActive > ringFull(§3.4/§5.6)")
{
    CHECK(priorityRejection(0, true, false, true) == PriorityReject::kUnassigned);
    CHECK(priorityRejection(0, false, true, false) == PriorityReject::kUnassigned); // channel=0 最优先
    CHECK(priorityRejection(3, false, true, false) == PriorityReject::kOutputOffline); // 离线优先于满环/非活跃
    CHECK(priorityRejection(3, false, false, false) == PriorityReject::kOutputOffline);
    CHECK(priorityRejection(3, true, false, false) ==
          PriorityReject::kNotActive); // conflict/abiMismatch/unavailable 非持有者
    CHECK(priorityRejection(3, true, true, false) == PriorityReject::kNotActive); // 非活跃优先于满环(不写环)
    CHECK(priorityRejection(3, true, true, true) == PriorityReject::kRingFull);
    CHECK(priorityRejection(3, true, false, true) == PriorityReject::kNone);

    CHECK(priorityRejectReason(PriorityReject::kUnassigned) == "unassigned");
    CHECK(priorityRejectReason(PriorityReject::kNotActive) == "unassigned"); // §5.6 闭集内最近似:未持有 slot
    CHECK(priorityRejectReason(PriorityReject::kOutputOffline) == "outputOffline");
    CHECK(priorityRejectReason(PriorityReject::kRingFull) == "ringFull");
    CHECK(priorityRejectReason(PriorityReject::kNone).isEmpty());
}

TEST_CASE("T30 advanceEmitCache:不可见不推进缓存,恢复可见重发(PR#54 R4)")
{
    juce::String last;

    // 不可见:json 变了也不推进缓存、返回不 emit(隐藏期事件不被吞)。
    CHECK_FALSE(advanceEmitCache("a", last, false));
    CHECK(last.isEmpty());

    // 恢复可见:缓存仍是旧值 → 同一 json 推进缓存并返回 emit(事件重发)。
    CHECK(advanceEmitCache("a", last, true));
    CHECK(last == "a");

    // 已发过(缓存 == json):不再重发,且不受可见性影响。
    CHECK_FALSE(advanceEmitCache("a", last, true));
    CHECK_FALSE(advanceEmitCache("a", last, false));

    // 新值不可见 → 不推进;恢复可见 → 重发新值(缓存最终对齐已发出值)。
    CHECK_FALSE(advanceEmitCache("b", last, false));
    CHECK(last == "a");
    CHECK(advanceEmitCache("b", last, true));
    CHECK(last == "b");
}

TEST_CASE("T30 claimEdgeConsumed:隐藏 + error 边沿不消费,恢复可见消费(PR#54 R5)")
{
    // 无 error 边沿(非 conflict/srMismatch 边沿)恒消费,不受可见性影响。
    CHECK(claimEdgeConsumed(false, false));
    CHECK(claimEdgeConsumed(false, true));

    // 有 error 边沿(needsError=true):隐藏 → 不消费(基线不推进);恢复可见 → 消费(error 重发)。
    CHECK_FALSE(claimEdgeConsumed(true, false));
    CHECK(claimEdgeConsumed(true, true));
}

TEST_CASE("T30 claimErrorEdgeChanged:五分量边沿键任一变化即重发(PR#54 R6)")
{
    // 同 claim/channel 换组 → 有边沿(重发,groupId 新值)。
    CHECK(claimErrorEdgeChanged("conflict", 3, 2, 48000, 48000, "conflict", 3, 1, 48000, 48000));

    // 同 srMismatch 改 outputSr → 有边沿(重发,outputSr 新值)。
    CHECK(claimErrorEdgeChanged("srMismatch", 3, 1, 48000, 48000, "srMismatch", 3, 1, 48000, 44100));

    // 同 srMismatch 改 inputSr → 有边沿。
    CHECK(claimErrorEdgeChanged("srMismatch", 3, 1, 44100, 48000, "srMismatch", 3, 1, 48000, 48000));

    // 五分量全同 → 无边沿(不重发)。
    CHECK_FALSE(claimErrorEdgeChanged("conflict", 3, 1, 48000, 48000, "conflict", 3, 1, 48000, 48000));
}

TEST_CASE("T30 advanceConfigSeq:隐藏不推进基线,恢复可见重发(PR#54 R7)")
{
    scvb::u32 last = 0;

    // 隐藏(未发出)→ 基线不推进(seq 仍 != 基线,恢复可见后下一 tick 重发)。
    CHECK_FALSE(advanceConfigSeq(7, false, last));
    CHECK(last == 0);

    // 恢复可见(已发出)→ 推进基线(seq 新值)。
    CHECK(advanceConfigSeq(7, true, last));
    CHECK(last == 7);
}

TEST_CASE("T30 parseIntArg:类型不符/越界 → 空(回 badArg);数值截断(§0.8.2)")
{
    CHECK_FALSE(parseIntArg({}).hasValue()); // 缺参
    CHECK_FALSE(parseIntArg({juce::var("oops")}).hasValue()); // 字符串
    CHECK_FALSE(parseIntArg({juce::var()}).hasValue()); // void/null
    CHECK_FALSE(parseIntArg({juce::var(juce::String("3"))}).hasValue()); // 数字串也是类型不符

    // double 越界/NaN:cast 前挡下(直接 static_cast<int> 是 UB;处理器 clamp 在 cast 后跑不到)。
    CHECK_FALSE(parseIntArg({juce::var(1e300)}).hasValue());
    CHECK_FALSE(parseIntArg({juce::var(-1e300)}).hasValue());
    CHECK_FALSE(parseIntArg({juce::var(std::numeric_limits<double>::quiet_NaN())}).hasValue());
    CHECK_FALSE(parseIntArg({juce::var(2147483648.0)}).hasValue()); // INT_MAX+1

    CHECK(*parseIntArg({juce::var(0)}) == 0); // 0 是合法业务值(setChannelId 释放/优先级 0),不夹取为拒绝
    CHECK(*parseIntArg({juce::var(7)}) == 7);
    CHECK(*parseIntArg({juce::var(3.9)}) == 3); // JS number 走 double,截断
    CHECK(*parseIntArg({juce::var(-1)}) == -1); // int 域内越界归处理器 clamp(§0.8.2 夹取路径)
    CHECK(*parseIntArg({juce::var(2147483647.0)}) == 2147483647); // INT_MAX 边界值本身合法
    CHECK(*parseIntArg({juce::var(99), juce::var(1)}) == 99); // 多余参数忽略(取 args[0])
}

TEST_CASE("T30 buildStatePayload:键名 + abi_remote 条件存在(§4.1)")
{
    const auto withRemote =
        obj(buildStatePayload(3, 2, "abiMismatch", 1, juce::Optional<scvb::u32>(7), 1.25f, "fr", true));
    CHECK(static_cast<int>(withRemote->getProperty("channel_id")) == 3);
    CHECK(static_cast<int>(withRemote->getProperty("group_id")) == 2);
    CHECK(withRemote->getProperty("claim").toString() == "abiMismatch");
    CHECK(static_cast<int>(withRemote->getProperty("abi")) == 1);
    CHECK(static_cast<int>(withRemote->getProperty("abi_remote")) == 7); // 探测到 → 存在
    const auto ui = obj(withRemote->getProperty("ui"));
    CHECK(static_cast<float>(ui->getProperty("scale")) == 1.25f);
    CHECK(ui->getProperty("language").toString() == "fr");
    // [SL-258] §4.1 载荷行的 ui 第三字段:setGuideSeen 写入后的回推路径。
    CHECK(static_cast<bool>(ui->getProperty("guide_seen")) == true);

    // 探测不到 → abi_remote 字段不存在(§4.1 字段纪律)。
    const auto noRemote = obj(buildStatePayload(3, 2, "active", 1, juce::Optional<scvb::u32>(), 1.0f, "zh", false));
    CHECK_FALSE(noRemote->hasProperty("abi_remote"));
    CHECK(noRemote->getProperty("claim").toString() == "active");
    CHECK(static_cast<bool>(obj(noRemote->getProperty("ui"))->getProperty("guide_seen")) == false);
    // 全局位**不进** scvb.state:§3.1 语义行明写它只读、不属工程 state,只在首帧快照顶层。
    CHECK_FALSE(noRemote->hasProperty("guide_seen_global"));
}

TEST_CASE("T30 buildConnPayload 六字段(§4.2)")
{
    const auto c = obj(buildConnPayload(connSnapshot(true, true, true, false, true, 0b101001)));
    CHECK(static_cast<bool>(c->getProperty("outputOnline")) == true);
    CHECK(static_cast<bool>(c->getProperty("maskBit")) == true);
    CHECK(static_cast<bool>(c->getProperty("capturing")) == true);
    CHECK(static_cast<bool>(c->getProperty("passthrough")) == false);
    CHECK(static_cast<bool>(c->getProperty("passthroughPending")) == true);
    CHECK(static_cast<int>(c->getProperty("occupiedMask")) == 0b101001);
}

TEST_CASE("T37-C buildConfigPayload:读到广播区时逐字段取 Output 实况(§4.3)", "[input][config][t37]")
{
    // T37 三轮 C 族回归:Input 侧 label/priority/lead_lock/pair_id/freeze 曾是硬编码常量
    // (priority 恒 0,而 Output 侧默认 5),Output 改什么 Input 都看不见。
    ConfigSnapshot s;
    s.sourceChannels = 1;
    s.configSeq = 7;
    s.broadcastValid = true;
    s.channelId = 3; // 本实例是 ch3 → 取 channels[2]
    s.broadcast.config_seq = 7;
    s.broadcast.channels[2].priority = 6; // 真机场景:Output 把优先级从 5 改成 6
    s.broadcast.channels[2].pair_id = 2;
    s.broadcast.channels[2].freeze = 3; // pan + vol 双冻
    s.broadcast.channels[2].flags = scvb::kCfgFlagEnabled | scvb::kCfgFlagLeadLock | scvb::kCfgFlagParticipateAutoPan;
    std::snprintf(s.broadcast.labels[2], scvb::kCtrlLabelBytes, "%s", "Lead Vox");
    std::snprintf(s.broadcast.labels[0], scvb::kCtrlLabelBytes, "%s", "Ch1");

    const auto c = obj(buildConfigPayload(s));
    CHECK(c->getProperty("label").toString() == "Lead Vox");
    CHECK(static_cast<int>(c->getProperty("priority")) == 6); // ← 修复前恒 0
    CHECK(static_cast<bool>(c->getProperty("lead_lock")) == true);
    // §4.3 的载荷是逐字冻结的九键,**不含** lead_vol_exempt —— 它属于 Output 侧的
    // scvb.state.channels[](§2.1/§4.1),Input 页没有消费面。这里反向钉住:别再漏进来。
    CHECK_FALSE(c->hasProperty("lead_vol_exempt"));
    CHECK(static_cast<int>(c->getProperty("pair_id")) == 2);
    CHECK(static_cast<int>(c->getProperty("freeze")) == 3);
    CHECK(static_cast<bool>(c->getProperty("participate_in_auto_pan")) == true);
    CHECK(static_cast<int>(c->getProperty("config_seq")) == 7);
    // source_channels 恒本机实测,不吃广播区回镜像。
    CHECK(static_cast<int>(c->getProperty("source_channels")) == 1);
    // A-32:15 张卡 label 镜像全组。
    const auto labels = c->getProperty("channelLabels").getArray();
    REQUIRE(labels != nullptr);
    REQUIRE(labels->size() == 15);
    CHECK(labels->getReference(0).toString() == "Ch1");
    CHECK(labels->getReference(2).toString() == "Lead Vox");

    // channel 未分配(channel_id=0)→ 没有「本轨」配置可取,回退默认值但 label 表仍镜像全组。
    ConfigSnapshot unassigned = s;
    unassigned.channelId = 0;
    const auto u = obj(buildConfigPayload(unassigned));
    CHECK(static_cast<int>(u->getProperty("priority")) == 0);
    CHECK(u->getProperty("label").toString().isEmpty());
    CHECK(u->getProperty("channelLabels").getArray()->getReference(2).toString() == "Lead Vox");
}

TEST_CASE("T30 buildConfigPayload:广播区读不到时回退默认值 + 本机实测 source_channels(§4.3)")
{
    ConfigSnapshot mono;
    mono.sourceChannels = 1;
    mono.configSeq = 42;
    const auto m = obj(buildConfigPayload(mono));
    CHECK(m->getProperty("label").toString().isEmpty());
    CHECK(static_cast<int>(m->getProperty("priority")) == 0);
    CHECK(static_cast<bool>(m->getProperty("lead_lock")) == false);
    CHECK(static_cast<int>(m->getProperty("pair_id")) == 0);
    CHECK(static_cast<int>(m->getProperty("freeze")) == 0);
    CHECK(static_cast<int>(m->getProperty("source_channels")) == 1);
    CHECK(static_cast<bool>(m->getProperty("participate_in_auto_pan")) == true); // [J83] 默认参与
    CHECK(static_cast<int>(m->getProperty("config_seq")) == 42);
    const auto labels = m->getProperty("channelLabels").getArray();
    REQUIRE(labels != nullptr);
    CHECK(labels->size() == 15); // A-32:15 张卡 label 镜像
    for (int i = 0; i < labels->size(); ++i)
    {
        CHECK(labels->getReference(i).toString().isEmpty());
    }

    // [J83]:**三种检测态一律默认参与**。source_channels 来自轨道总线布局而非素材声道数
    // (单声道人声放在立体声轨上就报 2),按它推导会让降级路径把绝大多数人声轨报成「不参与」,
    // 与 Output 侧 participatesInAutoPan() 的实况相反。改回 `s.sourceChannels == 1` 时
    // stereo/未检测两档即红。
    for (const int detected : {0, 1, 2})
    {
        ConfigSnapshot cs;
        cs.sourceChannels = detected;
        const auto p = obj(buildConfigPayload(cs));
        INFO("source_channels = " << detected);
        CHECK(static_cast<bool>(p->getProperty("participate_in_auto_pan")) == true);
        // 检测值本身照旧原样上报(它继续服务 ST 角标 / 张开线 / viz stereoMask)。
        CHECK(static_cast<int>(p->getProperty("source_channels")) == detected);
    }
}

TEST_CASE("T30 buildGroupsPayload(§4.4)")
{
    const auto g = obj(buildGroupsPayload(0x83));
    CHECK(static_cast<int>(g->getProperty("groups_online")) == 0x83);
}

TEST_CASE("T30 buildErrorPayload:ch 条件存在 + detail/active(§4.5)")
{
    const auto withCh = obj(buildErrorPayload("channelConflict", 3, juce::var(1), true));
    CHECK(withCh->getProperty("code").toString() == "channelConflict");
    CHECK(static_cast<int>(withCh->getProperty("ch")) == 3);
    CHECK(static_cast<bool>(withCh->getProperty("active")) == true);

    const auto noCh = obj(buildErrorPayload("secondOutput", 0, juce::var(2), false));
    CHECK_FALSE(noCh->hasProperty("ch"));
    CHECK(static_cast<bool>(noCh->getProperty("active")) == false);
}

TEST_CASE("T30 buildPriorityResponse 形状(§3.4)")
{
    const auto ok = obj(buildPriorityResponse(true, ""));
    CHECK(static_cast<bool>(ok->getProperty("queued")) == true);
    CHECK_FALSE(ok->hasProperty("reason")); // queued:true 不带 reason

    const auto rej = obj(buildPriorityResponse(false, "ringFull"));
    CHECK(static_cast<bool>(rej->getProperty("queued")) == false);
    CHECK(rej->getProperty("reason").toString() == "ringFull");
}

TEST_CASE("T30 conflictResponse(§5.6)")
{
    const auto c = obj(conflictResponse());
    CHECK(static_cast<bool>(c->getProperty("conflict")) == true);
}

TEST_CASE("T30 buildInputSnapshot 首帧快照形状(§3.1)")
{
    ConfigSnapshot cfg;
    cfg.sourceChannels = 2;
    cfg.configSeq = 7;
    const auto s = obj(buildInputSnapshot(4, 1, connSnapshot(true, true, false, false, false, 8), cfg, 1.0f, "zh",
                                          false, true, "0.1.0", 1));
    CHECK(static_cast<int>(s->getProperty("channel_id")) == 4);
    CHECK(static_cast<int>(s->getProperty("group_id")) == 1);
    CHECK(s->getProperty("role").toString() == "input");
    CHECK_FALSE(s->hasProperty("claim")); // claim 不经快照回推(唯一通道 = scvb.state)
    CHECK(static_cast<bool>(obj(s->getProperty("conn"))->getProperty("outputOnline")) == true);
    CHECK(static_cast<int>(obj(s->getProperty("config"))->getProperty("config_seq")) == 7);
    const auto ui = obj(s->getProperty("ui"));
    CHECK(static_cast<float>(ui->getProperty("scale")) == 1.0f);
    CHECK(ui->getProperty("language").toString() == "zh");
    // [SL-258] §3.1:工程位进 ui 子树、全局判定位挂**顶层**(语义行:只读、不属工程 state)。
    // 两者**必须能各自取值** —— 首启判据是「工程 false 且 全局 false 才弹」,合成一个位就废了。
    CHECK(static_cast<bool>(ui->getProperty("guide_seen")) == false);
    CHECK(static_cast<bool>(s->getProperty("guide_seen_global")) == true);
    CHECK_FALSE(ui->hasProperty("guide_seen_global")); // 不重复挂进 ui(同一语义两个落点,§0.1 第 4 条)
    const auto version = obj(s->getProperty("version"));
    CHECK(version->getProperty("plugin").toString() == "0.1.0");
    CHECK(static_cast<int>(version->getProperty("abi")) == 1);
}
