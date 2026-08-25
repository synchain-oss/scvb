// SPDX-License-Identifier: GPL-3.0-or-later
// test_ipc_viz —— viz 段([T44/J75])的 L1 双进程契约测试。
//
// **独立文件、不碰 test_ipc_contract.cpp 本体** —— 那个文件正被另一路(IPC-16 flake 修复)
// 系统性改动,合并冲突面必须归零。进程 spawn / CSV 回收 helper 走新头 tests/support/peer_spawn.h。
//
// 对端 = tests/tools/scvb_ipc_peer 的 viz-writer / viz-publisher / viz-reader 三个角色。
// 真跨进程:真共享内存、真进程退出(段随最后一个句柄消失)、真 TerminateProcess,
// 绝不用 InProcess 后端模拟(J19 口径)。
//
// 覆盖:
//   VIZ-1  只读 attach + 几何自检 + 帧头逐项一致 + 车道/位图/断线逐点校验
//   VIZ-2  段不存在时只读方拿 kFailed(**绝不建段**)
//   VIZ-3  写方进程退出 → 段消失 → 读方回空态;写方再上线 → 读方重连拿到新数据
//   VIZ-4  真 VizPublisher(而非手搓快照)发布 → 读侧看到降采样数据与断线口径

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ipc/SegmentBackendWin32.h"
#include "ipc/VizPlane.h"
#include "support/peer_spawn.h"

using scvb::ipctest::peer::csvLL;
using scvb::ipctest::peer::deleteFile;
using scvb::ipctest::peer::parseCsv;
using scvb::ipctest::peer::PeerGuard;
using scvb::ipctest::peer::readFile;
using scvb::ipctest::peer::spawnPeer;
using scvb::ipctest::peer::tempCsvPath;
using scvb::ipctest::peer::waitPeer;

namespace
{
const std::wstring kPeer = L"scvb_ipc_peer.exe";

// 轮询到条件成立或超时;返回是否成立(不自旋死等,超时即判失败并给出可读信息)。
template<typename Fn>
bool waitUntil(Fn&& fn, DWORD timeoutMs)
{
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;)
    {
        if (fn())
        {
            return true;
        }
        if (::GetTickCount64() >= deadline)
        {
            return false;
        }
        ::Sleep(20);
    }
}
} // namespace

TEST_CASE("VIZ-1 viz 段跨进程只读 attach 与一致性读", "[ipc][viz]")
{
    PeerGuard pw;
    PeerGuard pr;
    int err = 0;
    pw.pi = spawnPeer(kPeer, {"--role=viz-writer", "--group=1", "--sr=48000", "--linger-ms=2000"}, &err);
    REQUIRE(err == 0);
    const std::string csv = tempCsvPath();
    pr.pi = spawnPeer(kPeer, {"--role=viz-reader", "--group=1", "--out=" + csv}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(pw.pi, 20000) == 0);
    REQUIRE(waitPeer(pr.pi, 20000) == 0);
    const auto m = parseCsv(readFile(csv));
    deleteFile(csv);

    // 只读 attach 成功 + 几何自检通过(段内 column_count/track_count/pan_scale 与编译期常量一致)。
    REQUIRE(csvLL(m, "attach") == static_cast<long long>(scvb::InitResult::kOk));
    REQUIRE(csvLL(m, "read_ok") == 1);
    REQUIRE(csvLL(m, "read_only") == 1);
    REQUIRE(csvLL(m, "geometry_ok") == 1);
    REQUIRE(csvLL(m, "columns") == static_cast<long long>(scvb::kVizColumns));

    // 帧头标量逐项一致。
    REQUIRE(csvLL(m, "window_span") == 48000LL * 120);
    REQUIRE(csvLL(m, "playhead") == 48000);
    REQUIRE(csvLL(m, "loop_start") == 96000);
    REQUIRE(csvLL(m, "loop_end") == 192000);
    REQUIRE(csvLL(m, "playhead_flags") ==
            static_cast<long long>(scvb::kVizPlaying | scvb::kVizLooping | scvb::kVizLoopValid));
    REQUIRE(csvLL(m, "playhead_epoch") == 7);
    REQUIRE(csvLL(m, "version_active") == 2);
    REQUIRE(csvLL(m, "online_mask") == 0x7FFF);
    REQUIRE(csvLL(m, "covered_mask") == 0x0003);
    REQUIRE(csvLL(m, "stereo_mask") == 0x0002);
    REQUIRE(csvLL(m, "lead_mask") == 0x0004); // 轨3 主唱锁定(分布图柱顶绿帽)
    REQUIRE(csvLL(m, "lane_revision") == 42);

    // 轨色索引 = 轨号(v1 恒等映射)。
    REQUIRE(csvLL(m, "color1") == 1);
    REQUIRE(csvLL(m, "color15") == 15);

    // 每轨当前值三件套(分布图数据面)。
    REQUIRE(csvLL(m, "now_pan1") == scvb::vizPackPan(-12.5));
    REQUIRE(csvLL(m, "now_vol1") == scvb::vizPackFixed(-6.25));
    REQUIRE(csvLL(m, "now_width1") == scvb::vizPackFixed(80.0));
    REQUIRE(csvLL(m, "now_pan3") == static_cast<long long>(scvb::kVizPanNone)); // 无数据轨:哨兵

    // 轨名(UTF-8,含多字节 —— 截断必须落在字符边界)。
    REQUIRE(scvb::ipctest::peer::csvStr(m, "label1") == "Lead");
    REQUIRE(scvb::ipctest::peer::csvStr(m, "label2") == "\xE4\xB8\xBB\xE5\x94\xB1"); // 主唱

    // 降采样车道:轨1 沿列线性上升(-100 → +100,定点 ×100)。
    REQUIRE(csvLL(m, "pan_t1_first") == -10000);
    REQUIRE(csvLL(m, "pan_t1_last") == 10000);
    // 轨2 前半有值、后半是哨兵(断线场景)。
    REQUIRE(csvLL(m, "pan_t2_mid") == 2500);
    REQUIRE(csvLL(m, "pan_t2_tail") == static_cast<long long>(scvb::kVizPanNone));
    // 轨3 无数据:整条哨兵。
    REQUIRE(csvLL(m, "pan_t3_any") == static_cast<long long>(scvb::kVizPanNone));

    // 覆盖位图与断线口径一致(位序 = LSB 起、每字 32 格;web 侧 word=i>>>5 / bit=i&31 同源)。
    REQUIRE(csvLL(m, "cov_t1_0") == 1);
    REQUIRE(csvLL(m, "cov_t1_last") == 1);
    REQUIRE(csvLL(m, "cov_t2_0") == 1);
    REQUIRE(csvLL(m, "cov_t2_last") == 0);
    REQUIRE(csvLL(m, "cov_t3_0") == 0);
    // 字边界逐位钉死(位序写反时图照画,只是断线整体错开 32 的倍数,肉眼查不出来)。
    REQUIRE(csvLL(m, "cov_bit31") == 1);
    REQUIRE(csvLL(m, "cov_bit32") == 1);
    REQUIRE(csvLL(m, "cov_word0") == static_cast<long long>(0xFFFFFFFFu));
}

TEST_CASE("VIZ-2 viz 段不存在时只读方拿到 kFailed(空态,绝不建段)", "[ipc][viz]")
{
    // 用一个没有任何写方的组(g7):attach 必须失败而非创建段。
    scvb::SegmentBackendWin32 backend;
    scvb::VizPlane reader(backend, 7);
    REQUIRE(reader.attachReadOnly() == scvb::InitResult::kFailed);
    REQUIRE_FALSE(reader.isOpen());

    // 再次确认没有被误建:写方此刻 open 应当是「创建者」路径(generation == 1)。
    scvb::VizPlane writer(backend, 7);
    REQUIRE(writer.open() == scvb::InitResult::kOk);
    REQUIRE(writer.generation() == 1);
}

TEST_CASE("VIZ-3 写方进程退出 → 读方空态;再上线 → 读方重连", "[ipc][viz]")
{
    constexpr scvb::u32 kGroup = 3;
    scvb::SegmentBackendWin32 backend;

    // 起点:段不存在。
    {
        scvb::VizPlane probe(backend, kGroup);
        REQUIRE(probe.attachReadOnly() == scvb::InitResult::kFailed);
    }

    // ---- 写方上线(持段 linger-ms)----
    PeerGuard w1;
    int err = 0;
    w1.pi = spawnPeer(kPeer, {"--role=viz-writer", "--group=3", "--sr=48000", "--linger-ms=4000"}, &err);
    REQUIRE(err == 0);

    scvb::VizPlane reader(backend, kGroup);
    REQUIRE(waitUntil([&] { return reader.attachReadOnly() == scvb::InitResult::kOk; }, 10000));
    auto snap = std::make_unique<scvb::VizSnapshot>();
    REQUIRE(waitUntil([&] { return reader.read(*snap) && snap->laneRevision == 42; }, 10000));
    REQUIRE(snap->windowSpanSamples == 48000ull * 120);

    // ---- 写方退出:段随最后一个句柄消失 ----
    reader.release(); // 读方先松手,否则我们自己的句柄会把段吊住
    REQUIRE(waitPeer(w1.pi, 20000) == 0);

    // 空态:重新 attach 必须失败(而不是读到一份僵尸数据)。
    REQUIRE(waitUntil([&] { return reader.attachReadOnly() == scvb::InitResult::kFailed; }, 10000));
    REQUIRE_FALSE(reader.isOpen());

    // ---- 写方再上线:读方重连,且拿到的是**新一代**段 ----
    PeerGuard w2;
    w2.pi = spawnPeer(kPeer, {"--role=viz-writer", "--group=3", "--sr=44100", "--linger-ms=3000"}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitUntil([&] { return reader.attachReadOnly() == scvb::InitResult::kOk; }, 10000));
    REQUIRE(waitUntil([&] { return reader.read(*snap) && snap->sampleRate == 44100; }, 10000));
    REQUIRE(snap->windowSpanSamples == 44100ull * 120); // 新写方的几何,不是上一代的残留
    reader.release();
    REQUIRE(waitPeer(w2.pi, 20000) == 0);
}

TEST_CASE("VIZ-4 真 VizPublisher 发布 → 读侧看到降采样数据与断线", "[ipc][viz]")
{
    PeerGuard pw;
    PeerGuard pr;
    int err = 0;
    // viz-publisher 走真 VizPublisher(CRVS 分段 + CurveEvaluator 求值),不是手搓快照。
    pw.pi = spawnPeer(kPeer, {"--role=viz-publisher", "--group=2", "--sr=48000", "--linger-ms=2500"}, &err);
    REQUIRE(err == 0);
    const std::string csv = tempCsvPath();
    pr.pi = spawnPeer(kPeer, {"--role=viz-reader", "--group=2", "--out=" + csv}, &err);
    REQUIRE(err == 0);
    REQUIRE(waitPeer(pw.pi, 20000) == 0);
    REQUIRE(waitPeer(pr.pi, 20000) == 0);
    const auto m = parseCsv(readFile(csv));
    deleteFile(csv);

    REQUIRE(csvLL(m, "attach") == static_cast<long long>(scvb::InitResult::kOk));
    REQUIRE(csvLL(m, "read_ok") == 1);
    REQUIRE(csvLL(m, "geometry_ok") == 1);

    // 发布器造的场景(见 peer 的 runVizPublisher):
    //   轨1 [0,30s) pan=-50;轨2 [60,90s) pan=+40;其余轨无分段。窗口跨度量化到 90s。
    REQUIRE(csvLL(m, "window_span") == 48000LL * 90);
    REQUIRE(csvLL(m, "covered_mask") == 0x0003);
    REQUIRE(csvLL(m, "pan_t1_first") == scvb::vizPackPan(-50.0));
    REQUIRE(csvLL(m, "cov_t1_0") == 1);
    // 轨2 前 60s 无覆盖 → 断线(位图 0);末列在 [60,90) 内 → 有覆盖。
    REQUIRE(csvLL(m, "cov_t2_0") == 0);
    REQUIRE(csvLL(m, "cov_t2_last") == 1);
    REQUIRE(csvLL(m, "pan_t2_tail") == scvb::vizPackPan(40.0));
    // 无分段轨:整条哨兵 + 零覆盖。
    REQUIRE(csvLL(m, "pan_t3_any") == static_cast<long long>(scvb::kVizPanNone));
    REQUIRE(csvLL(m, "cov_t3_0") == 0);
    // 发布器至少发过一帧车道。
    REQUIRE(csvLL(m, "lane_revision") >= 1);
}
