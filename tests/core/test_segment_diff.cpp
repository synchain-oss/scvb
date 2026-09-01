// SPDX-License-Identifier: GPL-3.0-or-later
// [SL-255] 段表前后比对(§2.8 载荷的 diff 块)。
//
// 立卡背景:§1.18/§1.19 的语义行明写「完成后回发 scvb.segments(reason:"vad",**含 diff 摘要**)」,
// 而 native 那个 diff 块一直是**硬编码全 0 的桩**。只把流水线接上、不算 diff 的话,用户会从
// 「拖了没效果」变成「有效果但摘要说改了 0 段」—— 本卡的立卡理由恰恰就是「承诺没兑现」。
//
// 判同口径(与 UI 的渲染面对齐,理由见 SegmentDiff.h 头注):
//   · 配对按**时间重叠最大**,不按 (t0,t1) 全等 —— 重分段本来就挪边界;
//   · changed 只看 pan/volDb,且按**显示精度 1 位小数**判(UI 只渲染这两个值);
//   · added/removed = 两侧各自没配上的条数;
//   · **kept 不是「没改动的条数」** —— 契约 §2.8 逐字:「`diff.kept` = 本次**手动编辑/锁定段
//     已保留**计数」,词条原文「{k} 处手动编辑/锁定段已保留」。数的是新表里 locked/非 auto 的段。
//     (第一版我写成了「配对上且未改动」,读契约才发现是两回事 —— 这一条单独立用例守着。)

#include <catch2/catch_test_macros.hpp>

#include "output/SegmentDiff.h"

using scvb::output::diffTrackInto;
using scvb::output::SegmentDiff;
using scvb::state::Segment;

namespace
{
Segment seg(std::int64_t t0, std::int64_t t1, float pan, float vol)
{
    Segment s;
    s.t0 = t0;
    s.t1 = t1;
    s.pan = pan;
    s.volDb = vol;
    return s;
}
} // namespace

TEST_CASE("SegmentDiff:同一段值变了 → changed,带前后值与新表下标", "[output][diff][SL255]")
{
    const std::vector<Segment> before{seg(0, 100, -20.0f, 1.0f), seg(100, 200, 30.0f, -2.0f)};
    const std::vector<Segment> after{seg(0, 100, -20.0f, 1.0f), seg(100, 200, 45.0f, -2.0f)};

    SegmentDiff d;
    diffTrackInto(3, before, after, d);

    CHECK(d.added == 0);
    CHECK(d.removed == 0);
    CHECK(d.kept == 0); // 全是 auto 段 ⇒ 没有「手动编辑/锁定段」可保留
    REQUIRE(d.changed.size() == 1);
    CHECK(d.changed[0].ch == 3);
    CHECK(d.changed[0].segIdx == 1); // ★ **新表**下标(UI 显示 segIdx+1 =「第 2 段」)
    CHECK(d.changed[0].panFrom == 30.0f);
    CHECK(d.changed[0].panTo == 45.0f);
}

TEST_CASE("SegmentDiff:边界挪了但仍是同一段 → 不算增删", "[output][diff][SL255]")
{
    // 重分段最常见的形态:边界移动、值不变。若按 (t0,t1) 全等配对,这里会变成
    // 「删 1 加 1」—— 摘要就没信息量了,用户看到的会是「改了 0 段、增删各 1」。
    const std::vector<Segment> before{seg(0, 100, -20.0f, 1.0f)};
    const std::vector<Segment> after{seg(10, 130, -20.0f, 1.0f)};

    SegmentDiff d;
    diffTrackInto(1, before, after, d);

    CHECK(d.added == 0); // ★ 重叠配对认出是同一段
    CHECK(d.removed == 0);
    CHECK(d.kept == 0); // auto 段,见 kept 口径
    CHECK(d.changed.empty());
}

TEST_CASE("SegmentDiff:一分为二 → 一条配上、另一条算新增", "[output][diff][SL255]")
{
    // 每条旧段**只配一次**:否则一条旧段被两条新段共用,removed 会少数、added 会多数。
    const std::vector<Segment> before{seg(0, 200, -20.0f, 1.0f)};
    const std::vector<Segment> after{seg(0, 90, -20.0f, 1.0f), seg(110, 200, -20.0f, 1.0f)};

    SegmentDiff d;
    diffTrackInto(1, before, after, d);

    CHECK(d.kept == 0);
    CHECK(d.added == 1); // ★ 第二条没有旧段可配
    CHECK(d.removed == 0);
}

TEST_CASE("SegmentDiff:整段消失 / 凭空出现", "[output][diff][SL255]")
{
    SegmentDiff gone;
    diffTrackInto(1, {seg(0, 100, 0.0f, 0.0f)}, {}, gone);
    CHECK(gone.removed == 1);
    CHECK(gone.added == 0);

    SegmentDiff born;
    diffTrackInto(1, {}, {seg(0, 100, 0.0f, 0.0f)}, born);
    CHECK(born.added == 1);
    CHECK(born.removed == 0);

    SegmentDiff both; // 完全不重叠 = 一删一增,不是 changed
    diffTrackInto(1, {seg(0, 100, 0.0f, 0.0f)}, {seg(500, 600, 0.0f, 0.0f)}, both);
    CHECK(both.added == 1);
    CHECK(both.removed == 1);
    CHECK(both.changed.empty());
}

TEST_CASE("SegmentDiff:亚显示精度的抖动不算改动", "[output][diff][SL255]")
{
    // UI 用 1 位小数渲染。-20.00 与 -20.04 在屏幕上是同一个数,判成 changed 会让摘要条
    // 报出用户根本看不见的「改动」—— 浮点流水线每次都会有这种量级的差。
    const std::vector<Segment> before{seg(0, 100, -20.00f, 1.00f)};
    const std::vector<Segment> after{seg(0, 100, -20.04f, 1.04f)};

    SegmentDiff d;
    diffTrackInto(1, before, after, d);
    CHECK(d.kept == 0);
    CHECK(d.changed.empty());

    // 但跨过显示精度的那一档要认出来(-20.0 → -19.9)。
    SegmentDiff d2;
    diffTrackInto(1, before, {seg(0, 100, -19.94f, 1.00f)}, d2);
    CHECK(d2.changed.size() == 1);
}

TEST_CASE("SegmentDiff:kept 数的是「保留下来的手动/锁定段」,不是「没改动的段」", "[output][diff][SL255]")
{
    // 契约 §2.8 字段纪律 + 词条 `wave.diffKept`「{k} 处手动编辑/锁定段已保留」。
    // ★ 这一条是本文件里唯一守着「kept 口径」的用例:把它改回「配对上且未改动」的写法,
    //   下面三条断言会立刻红 —— 而那正是我第一版写错的形态。
    auto userSeg = [](std::int64_t t0, std::int64_t t1) {
        Segment s = seg(t0, t1, 5.0f, 0.0f);
        s.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::UserEdited, false);
        return s;
    };
    auto lockedSeg = [](std::int64_t t0, std::int64_t t1) {
        Segment s = seg(t0, t1, 7.0f, 0.0f);
        s.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::Auto, true);
        return s;
    };

    // 三段:一条 auto(值变了)、一条 user_edited、一条 locked。
    const std::vector<Segment> before{seg(0, 100, -20.0f, 1.0f), userSeg(100, 200), lockedSeg(200, 300)};
    const std::vector<Segment> after{seg(0, 100, -5.0f, 1.0f), userSeg(100, 200), lockedSeg(200, 300)};

    SegmentDiff d;
    diffTrackInto(2, before, after, d);

    CHECK(d.kept == 2); // ★ user_edited + locked 各一 —— 与「改没改」无关
    CHECK(d.changed.size() == 1); // 只有那条 auto 段值变了
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
}

TEST_CASE("SegmentDiff:空对空 → 全零(恒等变换不该报出改动)", "[output][diff][SL255]")
{
    SegmentDiff d;
    diffTrackInto(1, {}, {}, d);
    CHECK(d.kept == 0);
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
    CHECK(d.changed.empty());
}
