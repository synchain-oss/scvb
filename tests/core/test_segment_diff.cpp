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

#include <limits>
#include <numeric>
#include <random>

#include "output/SegmentDiff.h"

using scvb::output::diffTrackInto;
using scvb::output::SegmentDiff;
using scvb::state::Segment;

namespace
{
// 既有各例都不测**范围维**:给一个覆盖全时间轴的范围 ⇒ kept 判据与加范围过滤之前逐字等价。
// 范围维单独由下面「kept 只数范围内的手动/锁定段」一例守着。
void diffAll(int ch, const std::vector<Segment>& a, const std::vector<Segment>& b, SegmentDiff& out)
{
    diffTrackInto(ch, a, b, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), out);
}

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
    diffAll(3, before, after, d);

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
    diffAll(1, before, after, d);

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
    diffAll(1, before, after, d);

    CHECK(d.kept == 0);
    CHECK(d.added == 1); // ★ 第二条没有旧段可配
    CHECK(d.removed == 0);
}

TEST_CASE("SegmentDiff:整段消失 / 凭空出现", "[output][diff][SL255]")
{
    SegmentDiff gone;
    diffAll(1, {seg(0, 100, 0.0f, 0.0f)}, {}, gone);
    CHECK(gone.removed == 1);
    CHECK(gone.added == 0);

    SegmentDiff born;
    diffAll(1, {}, {seg(0, 100, 0.0f, 0.0f)}, born);
    CHECK(born.added == 1);
    CHECK(born.removed == 0);

    SegmentDiff both; // 完全不重叠 = 一删一增,不是 changed
    diffAll(1, {seg(0, 100, 0.0f, 0.0f)}, {seg(500, 600, 0.0f, 0.0f)}, both);
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
    diffAll(1, before, after, d);
    CHECK(d.kept == 0);
    CHECK(d.changed.empty());

    // 但跨过显示精度的那一档要认出来(-20.0 → -19.9)。
    SegmentDiff d2;
    diffAll(1, before, {seg(0, 100, -19.94f, 1.00f)}, d2);
    CHECK(d2.changed.size() == 1);
}

// ---------------------------------------------------------------------------
// [SL-274] 幅度闸门 `changedAtDisplayPrecision`:量化比较之外再问一次「差了多少」。
//
// 三条断言各自钉住一处**删掉就红**的实现细节,别合并:
//   (1) 量化边界贴边者不进表  → 删掉幅度闸门(退回纯 `!sameAtDisplayPrecision`)⇒ 红;
//   (2) 真差一整档者必进表    → 把闸门从半步长调回 `>= kDisplayStep` ⇒ 红;
//   (3) 非有限值仍判「改动」  → 删掉函数里那行 `isfinite` 早退 ⇒ 红。
// ---------------------------------------------------------------------------
TEST_CASE("SegmentDiff:幅度闸门只滤掉贴量化边界的假改动,不滤真差一档的", "[output][diff][SL274]")
{
    using scvb::output::changedAtDisplayPrecision;

    // (1) 贴边:4.249 与 4.251 量化成 42 / 43(屏幕上 4.2 → 4.3),但只差 0.002。
    //     屏幕上 `4.2 → 4.3` 数字确实变了,滤掉它是因为**变化量小到不值一提** ——
    //     这正是用户 v5.6.5 实测「摘要弹出全轨全段」里那批条目的来源。
    CHECK_FALSE(changedAtDisplayPrecision(4.249f, 4.251f));
    CHECK_FALSE(changedAtDisplayPrecision(-4.251f, -4.249f));
    // 同一件事走到 diff 出口:贴边对不得进 changed[]。
    SegmentDiff straddle;
    diffAll(1, {seg(0, 100, 4.249f, 0.0f)}, {seg(0, 100, 4.251f, 0.0f)}, straddle);
    CHECK(straddle.changed.empty());

    // (2) 真差一整档 —— **且专挑 float 下相减不足 0.1 的那些对**。
    //     `-100.0f → -99.9f` 实测差 0.099998474;闸门写 `>= kDisplayStep` 时这一批
    //     全被吞掉(pan 值域 2000 对相邻档里有 1188 对是这样),用户屏幕上读得到的
    //     改动却报不出来。逐对断言,不用循环 —— 红的时候要一眼看出是哪一对。
    CHECK(changedAtDisplayPrecision(-100.0f, -99.9f));
    CHECK(changedAtDisplayPrecision(-20.0f, -19.9f));
    CHECK(changedAtDisplayPrecision(5.9f, 6.0f));
    CHECK(changedAtDisplayPrecision(0.3f, 0.4f));
    // 走到 diff 出口同样要进表。
    SegmentDiff oneStep;
    diffAll(1, {seg(0, 100, -100.0f, 0.0f)}, {seg(0, 100, -99.9f, 0.0f)}, oneStep);
    CHECK(oneStep.changed.size() == 1);

    // (3) 非有限值:`sameAtDisplayPrecision` 对它们返回 false(判「不同」),控制流
    //     **穿过**早退落到幅度比较 —— 靠函数自己那行 isfinite 保守报改动。删掉它,
    //     NaN 走进 fabs、比较恒假 ⇒ 悄悄判成「没改」,这三条随即红。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(changedAtDisplayPrecision(nan, nan));
    CHECK(changedAtDisplayPrecision(0.0f, nan));
    CHECK(changedAtDisplayPrecision(inf, 1.0f));
}

// ---------------------------------------------------------------------------
// [SL-274] 取整方向必须与 UI 的 `fmtSigned` 同向 —— **负的半格**是唯一会分歧的那一角。
//
// `tab-wave.js::fmtSigned` 用 `Math.round`(一律朝 +∞:`-42.5 → -42`),而 `std::lround`
// 是远离零(`-42.5 → -43`)。`-4.25f` 在 float 里可精确表示,所以这不是纯理论角。
// 本用例两条各钉一个方向,把 `q` 换回 `std::lround` ⇒ 两条一起红:
//   · `-4.20f → -4.25f`:屏幕上两边都写 `-4.2`(没变),lround 会判「不同」⇒ 摘要冒出
//     一条 `pan -4.2→-4.2` 的**空条目**,正是本卡要消灭的那类;
//   · `-4.25f → -4.30f`:屏幕上 `-4.2 → -4.3`(变了),lround 两边都得 −43 判「相同」
//     ⇒ 用户读得到的改动被**整条吞掉**。
// 两条都走 `diffAll` 出口而不只测纯函数:分歧要在**摘要里**看得见才算钉住。
// ---------------------------------------------------------------------------
TEST_CASE("SegmentDiff:显示精度量化与 UI 的 Math.round 同向(负半格)", "[output][diff][SL274]")
{
    using scvb::output::changedAtDisplayPrecision;
    using scvb::output::sameAtDisplayPrecision;

    // 前置:`-4.25f` 在 float 里**精确**,否则「恰好落在半格」这个角根本不存在,
    // 下面两条会退化成随便两个数的比较。
    REQUIRE(static_cast<double>(-4.25f) == -4.25);

    // ① 屏幕上没变(−4.2 → −4.2)⇒ 不得进表。lround 会把 −4.25 量化成 −43 ⇒ 红。
    CHECK(sameAtDisplayPrecision(-4.20f, -4.25f));
    CHECK_FALSE(changedAtDisplayPrecision(-4.20f, -4.25f));
    SegmentDiff noVisibleChange;
    diffAll(1, {seg(0, 100, -4.20f, 0.0f)}, {seg(0, 100, -4.25f, 0.0f)}, noVisibleChange);
    CHECK(noVisibleChange.changed.empty());

    // ② 屏幕上变了(−4.2 → −4.3)⇒ 必须进表。lround 两边同为 −43 ⇒ 判「相同」⇒ 红。
    CHECK_FALSE(sameAtDisplayPrecision(-4.25f, -4.30f));
    CHECK(changedAtDisplayPrecision(-4.25f, -4.30f));
    SegmentDiff visibleChange;
    diffAll(1, {seg(0, 100, -4.25f, 0.0f)}, {seg(0, 100, -4.30f, 0.0f)}, visibleChange);
    CHECK(visibleChange.changed.size() == 1);
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
    diffAll(2, before, after, d);

    CHECK(d.kept == 2); // ★ user_edited + locked 各一 —— 与「改没改」无关
    CHECK(d.changed.size() == 1); // 只有那条 auto 段值变了
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
}

TEST_CASE("SegmentDiff:空对空 → 全零(恒等变换不该报出改动)", "[output][diff][SL255]")
{
    SegmentDiff d;
    diffAll(1, {}, {}, d);
    CHECK(d.kept == 0);
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
    CHECK(d.changed.empty());
}

// ---------------------------------------------------------------------------
// [SL-255 复审] 以下五例守的是本轮复审改出来的四条口径。
// ---------------------------------------------------------------------------

TEST_CASE("SegmentDiff:kept 只数**范围内**的手动/锁定段(与 previewAnalysis 同尺)", "[output][diff][SL255]")
{
    // 立此例的理由:`previewAnalysis` 的 `manualKept` 自 [SL-193] 起就是「范围相交才计数」,
    // 而 diff 这侧若不过滤,Tab3 同一张卡上 A-07 预览行与 A-02 摘要行的 {k} 会给两个数 ——
    // 正是 SL-193 修掉的那族对不齐,从另一侧再造一遍。
    auto user = [](std::int64_t t0, std::int64_t t1) {
        Segment s = seg(t0, t1, 0.0f, 0.0f);
        s.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::UserEdited, false);
        return s;
    };
    auto locked = [](std::int64_t t0, std::int64_t t1) {
        Segment s = seg(t0, t1, 0.0f, 0.0f);
        s.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::Auto, true);
        return s;
    };
    // 范围 = [1000, 2000)。三条手动/锁定段:一条在内、一条在外、一条只与右边界相邻(半开 ⇒ 在外)。
    const std::vector<Segment> table{user(1200, 1400), locked(1500, 1600), user(100, 900), user(2000, 2100)};

    SegmentDiff d;
    diffTrackInto(5, table, table, 1000, 2000, d);
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
    CHECK(d.changed.empty());
    // ★ 删掉 diffTrackInto 里那句范围过滤(`if (!(sg.t1 > r0 && sg.t0 < r1)) continue;`)
    //   这一条立刻变成 4 == 2 而红。
    CHECK(d.kept == 2);

    // 同一张表、范围放到全时间轴 ⇒ 四条全数(证明差别真的来自范围维,不是别的)。
    SegmentDiff all;
    diffAll(5, table, table, all);
    CHECK(all.kept == 4);
}

TEST_CASE("SegmentDiff:diff 全空 ≠ 段表没变(segmentsIdentical 才是判据)", "[output][diff][SL255]")
{
    // 这一条守的是 `finishAnalysis` 里「段表没变才不压撤销步」的**判据选型**:
    // 边界挪了、pan/volDb 一个没变 —— diff 四项全空,段表却真的变了。
    // 若拿「diff 是否全空」当作「改没改」,这一轮就漏压撤销步,用户撤不回来。
    const std::vector<Segment> before{seg(0, 100, -20.0f, 1.0f)};
    const std::vector<Segment> after{seg(0, 140, -20.0f, 1.0f)};

    SegmentDiff d;
    diffAll(1, before, after, d);
    CHECK(d.added == 0);
    CHECK(d.removed == 0);
    CHECK(d.changed.empty());
    CHECK(d.kept == 0); // 四项全空

    CHECK_FALSE(scvb::output::segmentsIdentical(before, after)); // ★ 但段表确实变了
    CHECK(scvb::output::segmentsIdentical(before, before));

    // flags 单独变(比如某段被锁上)也算变 —— 五个字段全比。
    std::vector<Segment> lockedOne = before;
    lockedOne[0].flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::Auto, true);
    CHECK_FALSE(scvb::output::segmentsIdentical(before, lockedOne));
}

TEST_CASE("SegmentDiff:changed 封顶,而 added/removed/kept 仍是精确总数", "[output][diff][SL255]")
{
    // 全量重分段时跨过 1 位小数的段可能数以千计,UI 那侧是逐条拼 <li> 的无封顶列表。
    const std::size_t n = scvb::output::kMaxChangedItems + 37;
    std::vector<Segment> before;
    std::vector<Segment> after;
    before.reserve(n);
    after.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto t0 = static_cast<std::int64_t>(i) * 100;
        before.push_back(seg(t0, t0 + 90, 0.0f, 0.0f));
        after.push_back(seg(t0, t0 + 90, 5.0f, 0.0f)); // 每一段的 pan 都跨过显示精度
    }
    // 再额外加一条新表独有的段(不与任何旧段重叠)⇒ added 必须照实数,不受封顶影响。
    after.push_back(
        seg(static_cast<std::int64_t>(n) * 100 + 500, static_cast<std::int64_t>(n) * 100 + 600, 0.0f, 0.0f));

    SegmentDiff d;
    diffAll(2, before, after, d);
    CHECK(d.changed.size() == scvb::output::kMaxChangedItems); // ★ 明细截断
    CHECK(d.added == 1); // ★ 但总数不撒谎
    CHECK(d.removed == 0);
}

TEST_CASE("SegmentDiff:非有限值不判同(lround(NaN) 是实现定义行为)", "[output][diff][SL255]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(scvb::output::sameAtDisplayPrecision(nan, nan));
    CHECK_FALSE(scvb::output::sameAtDisplayPrecision(nan, 0.0f));
    CHECK_FALSE(scvb::output::sameAtDisplayPrecision(std::numeric_limits<float>::infinity(), 1.0f));
    CHECK(scvb::output::sameAtDisplayPrecision(1.23f, 1.24f)); // 对照:仍按 1 位小数判同
}

TEST_CASE("SegmentDiff:线性配对与输入顺序无关(乱序输入结果一致)", "[output][diff][SL255]")
{
    // 配对从「逐对枚举」改成「按 t0 有序 + 单调游标」的线性扫([SL-255] 复审⑦:
    // 本函数对 15 轨全程持 lifecycleMutex_ 跑在消息线程上)。这一条钉住:
    // 改法只换复杂度,不换答案 —— 且不对调用方的入参顺序提要求。
    std::vector<Segment> before;
    std::vector<Segment> after;
    for (int i = 0; i < 120; ++i)
    {
        const auto t0 = static_cast<std::int64_t>(i) * 1000;
        before.push_back(seg(t0, t0 + 800, 0.0f, 0.0f));
        // 新表:边界整体右移 100,且每三段改一次 pan ⇒ 全部配得上,changed 恰 40 条。
        after.push_back(seg(t0 + 100, t0 + 900, (i % 3 == 0) ? 20.0f : 0.0f, 0.0f));
    }

    SegmentDiff sorted;
    diffAll(7, before, after, sorted);
    CHECK(sorted.added == 0);
    CHECK(sorted.removed == 0);
    CHECK(sorted.changed.size() == 40);

    std::mt19937 rng(12345);
    std::vector<Segment> beforeShuffled = before;
    std::shuffle(beforeShuffled.begin(), beforeShuffled.end(), rng);
    SegmentDiff shuffled;
    diffAll(7, beforeShuffled, after, shuffled);
    CHECK(shuffled.added == sorted.added);
    CHECK(shuffled.removed == sorted.removed);
    CHECK(shuffled.changed.size() == sorted.changed.size());
}
