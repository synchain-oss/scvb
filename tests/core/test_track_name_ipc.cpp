// SPDX-License-Identifier: GPL-3.0-or-later
// test_track_name_ipc —— [J150] ctrl 段轨道名区(Input → Output,IPC_CONTRACT §4)。
//
// 两层:
//   · CtrlPlane 的写 / 读口:往返、截断不切半个码点、对端不可信字节、seqlock(写方在写 / 奇数残值 /
//     并发撕裂)、从未写过;
//   · OutputSession::readOwnedTrackName 的三道门:已连接 / 归属(owner_heartbeat_ms == slot 心跳)/
//     名字非空 —— 以及 Input 侧提供归属判据的 InputSession::ownSlotHeartbeatMs。
// 用 SegmentBackendInProcess(进程内模拟段),不碰全局段。宿主那几跳(updateTrackProperties →
// Input timer 写 → Output timer 读 → label)在 tests/host/test_host_track_name.cpp。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "input/InputSession.h"
#include "ipc/CtrlPlane.h"
#include "ipc/SegmentBackendInProcess.h"
#include "output/OutputSession.h"

using scvb::u32;
using scvb::u64;
using scvb::input::InputClaimState;
using scvb::input::InputSession;
using scvb::output::OutputClaimState;
using scvb::output::OutputSession;

namespace
{
// 段里某一条的裸地址 —— 模拟「对端进程往里写了什么」(CtrlPlane 自己的写口只会写合法字节,
// 读方的自保判据只能靠直接改段内字节来喂)。
scvb::CtrlTrackName* rawEntry(scvb::CtrlPlane& plane, u32 channel)
{
    auto* base = static_cast<unsigned char*>(plane.broadcastBase()) - scvb::kCtrlBroadcastOffset;
    return reinterpret_cast<scvb::CtrlTrackName*>(base + scvb::kCtrlTrackNamesOffset +
                                                  (channel - 1) * sizeof(scvb::CtrlTrackName));
}
} // namespace

TEST_CASE("J150 轨道名区:落在广播区预算内、紧跟 CtrlBroadcast、每条 128 字节", "[ipc][j150]")
{
    // 与 tests/golden/ipc-layout.txt 那几行(struct CtrlTrackName / offset ctrl_track_names)同一组数;
    // golden 由 test_ipc_layout 逐行对拍,这里只钉「为什么不升 abi」的那条算术:不越出广播区预算。
    STATIC_REQUIRE(sizeof(scvb::CtrlTrackName) == 128);
    STATIC_REQUIRE(scvb::kCtrlTrackNamesOffset == scvb::kCtrlBroadcastOffset + sizeof(scvb::CtrlBroadcast));
    STATIC_REQUIRE(scvb::kCtrlTrackNamesOffset + scvb::kMaxChannels * sizeof(scvb::CtrlTrackName) <=
                   scvb::kCtrlGlobalInfoOffset);
}

TEST_CASE("J150 CtrlPlane 轨道名:写读往返、各条互不串、从未写过的条读不到", "[ipc][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane writer(backend, 1);
    scvb::CtrlPlane reader(backend, 1);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    REQUIRE(reader.open() == scvb::InitResult::kOk);

    const std::string vox = "\xE4\xB8\xBB\xE5\x94\xB1 Lead"; // 「主唱 Lead」(多字节)
    writer.writeTrackName(3, 1234, vox);

    u64 owner = 0;
    std::string name;
    REQUIRE(reader.readTrackName(3, owner, name));
    CHECK(name == vox);
    CHECK(owner == 1234);

    // 从未写过的条(seq==0):读不到,而不是读出「空名 + owner 0」。
    CHECK_FALSE(reader.readTrackName(4, owner, name));

    // 覆写同一条:不留上一次的尾巴(短名覆盖长名)。
    writer.writeTrackName(3, 1300, "Vx");
    REQUIRE(reader.readTrackName(3, owner, name));
    CHECK(name == "Vx");
    CHECK(owner == 1300);

    // 空名照样可写可读(= 宿主没给轨道名);是否「算有名字」是 OutputSession 那一层的事。
    writer.writeTrackName(3, 1400, "");
    REQUIRE(reader.readTrackName(3, owner, name));
    CHECK(name.empty());

    // 非法 channel / 未打开:写静默、读 false。
    writer.writeTrackName(0, 1, "x");
    writer.writeTrackName(16, 1, "x");
    CHECK_FALSE(reader.readTrackName(0, owner, name));
    CHECK_FALSE(reader.readTrackName(16, owner, name));
    scvb::CtrlPlane closed(backend, 1);
    CHECK_FALSE(closed.readTrackName(3, owner, name));
}

TEST_CASE("J150 CtrlPlane 轨道名:超长名在码点边界截断,读回仍是合法 UTF-8", "[ipc][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == scvb::InitResult::kOk);

    // 98 个 ASCII + 「中」(3 字节)+ 「文」= 104 字节;槽上限 99 字节正好落在「中」的第 2 字节上。
    // 正确截断回退到「中」之前 ⇒ 98 字节;不回退 ⇒ 99 字节以 0xE4 结尾(半个码点)⇒ 读方判非法、读不到。
    const std::string ascii(98, 'a');
    const std::string longName = ascii + "\xE4\xB8\xAD\xE6\x96\x87";
    plane.writeTrackName(5, 77, longName);

    u64 owner = 0;
    std::string name;
    REQUIRE(plane.readTrackName(5, owner, name));
    CHECK(name == ascii);

    // 恰好 99 字节的纯 ASCII:不截(边界本身)。
    const std::string exact(99, 'b');
    plane.writeTrackName(5, 78, exact);
    REQUIRE(plane.readTrackName(5, owner, name));
    CHECK(name == exact);
}

TEST_CASE("J150 CtrlPlane 轨道名:对端写来的非法 UTF-8 / 缺 NUL 由读方自保", "[ipc][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == scvb::InitResult::kOk);

    plane.writeTrackName(2, 10, "ok"); // 先让 seq 成为「写过、稳定」的偶数
    scvb::CtrlTrackName* e = rawEntry(plane, 2);
    u64 owner = 0;
    std::string name;

    // 过长编码(0xC0 0xAF = '/' 的两字节写法):严格 UTF-8 必须拒。
    std::memset(e->utf8, 0, sizeof(e->utf8));
    e->utf8[0] = 'A';
    e->utf8[1] = static_cast<char>(0xC0);
    e->utf8[2] = static_cast<char>(0xAF);
    CHECK_FALSE(plane.readTrackName(2, owner, name));

    // 孤立续字节。
    std::memset(e->utf8, 0, sizeof(e->utf8));
    e->utf8[0] = static_cast<char>(0x80);
    CHECK_FALSE(plane.readTrackName(2, owner, name));

    // UTF-16 代理区(U+D800 = ED A0 80)。
    std::memset(e->utf8, 0, sizeof(e->utf8));
    e->utf8[0] = static_cast<char>(0xED);
    e->utf8[1] = static_cast<char>(0xA0);
    e->utf8[2] = static_cast<char>(0x80);
    CHECK_FALSE(plane.readTrackName(2, owner, name));

    // 整槽 100 字节都不是 NUL(对端漏写 NUL):读方把最后一字节当 NUL,读出 99 字节,不越界。
    std::memset(e->utf8, 'z', sizeof(e->utf8));
    REQUIRE(plane.readTrackName(2, owner, name));
    CHECK(name == std::string(99, 'z'));
}

TEST_CASE("J150 CtrlPlane 轨道名:写方在写(奇数 seq)读不到;奇数残值下一次写入即扶正", "[ipc][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane plane(backend, 1);
    REQUIRE(plane.open() == scvb::InitResult::kOk);

    plane.writeTrackName(7, 50, "Alto");
    scvb::CtrlTrackName* e = rawEntry(plane, 7);
    u64 owner = 0;
    std::string name;
    REQUIRE(plane.readTrackName(7, owner, name));

    // 写方进了临界区还没出来(seq 为奇数):载荷可能半新半旧,读方必须放弃。
    const u32 even = e->seq.load();
    REQUIRE((even & 1u) == 0u);
    e->seq.store(even + 1u);
    CHECK_FALSE(plane.readTrackName(7, owner, name));

    // 上一任写方死在临界区里(seq 停在奇数)。下一次写入必须把它扶正:写完是偶数、读得到新名字。
    // (若按 fetch_add 的写法,奇数 +1 会在**写载荷期间**呈偶数、写完反而停在奇数 —— 此后永远读不到。)
    plane.writeTrackName(7, 51, "Tenor");
    CHECK((e->seq.load() & 1u) == 0u);
    REQUIRE(plane.readTrackName(7, owner, name));
    CHECK(name == "Tenor");
    CHECK(owner == 51);
}

TEST_CASE("J150 CtrlPlane 轨道名:并发写读,读到的永远是某一次完整写入(不撕裂)", "[ipc][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;
    scvb::CtrlPlane writer(backend, 1);
    scvb::CtrlPlane reader(backend, 1);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    REQUIRE(reader.open() == scvb::InitResult::kOk);

    // 两个等长名字交替写:每次写入的 owner 与名字一一对应(A ⇔ 偶 owner,B ⇔ 奇 owner)。
    // 读方拿到的任何一份都必须「名字整条同一个字母 且 与 owner 的奇偶对得上」—— 撕裂读会露出
    // 半 A 半 B,或 A 配奇 owner。
    const std::string a(99, 'A');
    const std::string b(99, 'B');
    writer.writeTrackName(9, 0, a);

    std::atomic<bool> stop{false};
    std::thread w([&] {
        u64 n = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            ++n;
            writer.writeTrackName(9, n, (n % 2 == 0) ? a : b);
        }
    });

    int good = 0;
    int bad = 0;
    for (int i = 0; i < 200000; ++i)
    {
        u64 owner = 0;
        std::string name;
        if (!reader.readTrackName(9, owner, name))
        {
            continue; // 撕裂 / 写方在写:本拍放弃,是合法结果
        }
        const bool allA = name == a;
        const bool allB = name == b;
        if ((allA && owner % 2 == 0) || (allB && owner % 2 == 1))
        {
            ++good;
        }
        else
        {
            ++bad;
        }
    }
    stop.store(true);
    w.join();

    // 断言放在主线程(SL-453:工作线程里不做 Catch2 断言)。
    CHECK(bad == 0);
    CHECK(good > 0); // 真的读到过东西,不是一路放弃后空过
}

TEST_CASE("J150 OutputSession::readOwnedTrackName:已连接 + 归属 + 非空,三道门缺一不可", "[output][session][j150]")
{
    scvb::SegmentBackendInProcess::resetAll();
    scvb::SegmentBackendInProcess backend;

    InputSession in(backend, 1001);
    in.setChannelId(3);
    REQUIRE(in.prepare(48000, 512, 1, 1000) == InputClaimState::kActive);
    in.heartbeat(1100);
    // Input 侧的归属判据来源:本实例实际持有那一条 slot 此刻的心跳。
    REQUIRE(in.ownSlotHeartbeatMs() == 1100);

    OutputSession out(backend, 2001);
    REQUIRE(out.prepare(48000, 512, 1200) == OutputClaimState::kActive);

    // 扮演 Input 的 ctrl 段写方(真插件里是 InputProcessor 的 ctrl_)。
    scvb::CtrlPlane inputCtrl(backend, 1);
    REQUIRE(inputCtrl.open() == scvb::InitResult::kOk);
    inputCtrl.writeTrackName(3, in.ownSlotHeartbeatMs(), "Lead Vox");

    std::string name;
    // 三道门全过。
    REQUIRE(out.readOwnedTrackName(3, 1300, name));
    CHECK(name == "Lead Vox");

    // 归属:条目记的心跳 ≠ slot 此刻的心跳 ⇒ 不采信(离开的 Input 留下的旧条目就是这个形状)。
    inputCtrl.writeTrackName(3, 1099, "Old Owner");
    CHECK_FALSE(out.readOwnedTrackName(3, 1300, name));
    // slot 心跳前进了而条目没重写 ⇒ 同样失配;重写后恢复。
    inputCtrl.writeTrackName(3, 1100, "Lead Vox");
    REQUIRE(out.readOwnedTrackName(3, 1300, name));
    in.heartbeat(1350);
    CHECK_FALSE(out.readOwnedTrackName(3, 1400, name));
    inputCtrl.writeTrackName(3, in.ownSlotHeartbeatMs(), "Lead Vox");
    CHECK(out.readOwnedTrackName(3, 1400, name));

    // 已连接:心跳陈旧(> kStaleDisplayMs)的轨一律不采信 —— 即便条目归属对得上。
    CHECK_FALSE(out.readOwnedTrackName(3, 1350 + scvb::kStaleDisplayMs + 500, name));

    // 非空:空串 = 宿主没给轨道名,不算有名字。
    inputCtrl.writeTrackName(3, in.ownSlotHeartbeatMs(), "");
    CHECK_FALSE(out.readOwnedTrackName(3, 1400, name));

    // 没有 Input 的轨 / 非法 channel。
    CHECK_FALSE(out.readOwnedTrackName(4, 1400, name));
    CHECK_FALSE(out.readOwnedTrackName(0, 1400, name));
    CHECK_FALSE(out.readOwnedTrackName(16, 1400, name));

    // Input 释放 slot 之后:它的 ownSlotHeartbeatMs 归 0(= 不该再写),Output 也不再采信那一条。
    inputCtrl.writeTrackName(3, in.ownSlotHeartbeatMs(), "Lead Vox");
    REQUIRE(out.readOwnedTrackName(3, 1400, name));
    in.release(1450);
    CHECK(in.ownSlotHeartbeatMs() == 0);
    CHECK_FALSE(out.readOwnedTrackName(3, 1500, name));
}
