// SPDX-License-Identifier: GPL-3.0-or-later
// test_input_state —— Input state 编解码 + StateCodec 容器往返单测([J66] group_id / [J01] channel_id)。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "state/InputStateCodec.h"
#include "state/StateCodec.h"

using scvb::state::InputState;
using scvb::state::kFourccCfgs;

TEST_CASE("J66① group_id 默认 1 且 save/load 往返保持", "[input][state]")
{
    InputState s;
    REQUIRE(s.groupId == 1); // [J66] 默认 1(UI 显示 A)
    REQUIRE(s.channelId == 0); // [J01] 默认 0 = 未分配

    s.channelId = 5;
    s.groupId = 1;
    s.uiScale = 120;
    s.uiLanguage = "zh";
    std::vector<std::uint8_t> blob;
    REQUIRE(scvb::state::encodeInputState(s, blob));

    InputState s2;
    REQUIRE(scvb::state::decodeInputState(blob.data(), blob.size(), s2));
    REQUIRE(s2.groupId == 1);
    REQUIRE(s2.channelId == 5);
    REQUIRE(s2.uiScale == 120);
    REQUIRE(s2.uiLanguage == "zh");
}

TEST_CASE("Input state 经 StateCodec 容器往返(CFGS chunk)", "[input][state]")
{
    InputState s;
    s.channelId = 7;
    s.groupId = 3;
    s.uiScale = 90;
    s.uiLanguage = "en";
    std::vector<std::uint8_t> payload;
    REQUIRE(scvb::state::encodeInputState(s, payload));

    scvb::state::StateChunks chunks;
    chunks.abi = scvb::state::kCurrentAbi;
    chunks.set(kFourccCfgs, payload);
    std::vector<std::uint8_t> blob;
    REQUIRE(scvb::state::encodeContainer(chunks, blob));

    scvb::state::StateChunks out;
    REQUIRE(scvb::state::decodeContainer(blob.data(), blob.size(), out) == scvb::state::DecodeStatus::Ok);
    const scvb::state::Chunk* cfg = out.find(kFourccCfgs);
    REQUIRE(cfg != nullptr);

    InputState s2;
    REQUIRE(scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), s2));
    REQUIRE(s2.channelId == 7);
    REQUIRE(s2.groupId == 3);
    REQUIRE(s2.uiScale == 90);
    REQUIRE(s2.uiLanguage == "en");
}

TEST_CASE("Input state 范围校验(不可信字节拒载)", "[input][state]")
{
    InputState out;

    // channel_id > 15 → 拒载。
    {
        InputState s;
        s.channelId = 16;
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeInputState(s, blob));
        REQUIRE_FALSE(scvb::state::decodeInputState(blob.data(), blob.size(), out));
    }
    // group_id 0 → 拒载。
    {
        InputState s;
        s.groupId = 0;
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeInputState(s, blob));
        REQUIRE_FALSE(scvb::state::decodeInputState(blob.data(), blob.size(), out));
    }
    // group_id 9 → 拒载。
    {
        InputState s;
        s.groupId = 9;
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeInputState(s, blob));
        REQUIRE_FALSE(scvb::state::decodeInputState(blob.data(), blob.size(), out));
    }
    // 截断字节 → 拒载。
    {
        InputState s;
        s.channelId = 3;
        s.groupId = 1;
        std::vector<std::uint8_t> blob;
        REQUIRE(scvb::state::encodeInputState(s, blob));
        blob.pop_back();
        REQUIRE_FALSE(scvb::state::decodeInputState(blob.data(), blob.size(), out));
    }
    // 空数据 → 拒载。
    REQUIRE_FALSE(scvb::state::decodeInputState(nullptr, 0, out));
}

TEST_CASE("state abi 决策门:高版本拒载,当前/低版本接受(PR#51 重要#2)", "[input][state]")
{
    using scvb::state::decideInputStateAbi;
    using scvb::state::InputStateAbiDecision;
    using scvb::state::kCurrentAbi;

    REQUIRE(decideInputStateAbi(kCurrentAbi) == InputStateAbiDecision::Accept);
    REQUIRE(decideInputStateAbi(0) == InputStateAbiDecision::Accept); // v1 无历史版本,低版本按当前布局直解
    REQUIRE(decideInputStateAbi(kCurrentAbi + 1) == InputStateAbiDecision::RejectNewer);
    REQUIRE(decideInputStateAbi(100) == InputStateAbiDecision::RejectNewer);
}
