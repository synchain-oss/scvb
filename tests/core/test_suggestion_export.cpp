// SPDX-License-Identifier: GPL-3.0-or-later
// test_suggestion_export —— T41 建议表行集 + CSV 序列化单测(07 T41 验收逐条)。
// 覆盖:列顺序与冻结定义逐字一致;15 轨 × 2 版本 × 每轨 40 段下行数 == 段总数;
// UTF-8 BOM + CRLF + 表头;RFC 4180 转义(轨名含逗号/引号/换行);origin/locked 与 T19 编码
// 逐字一致;stereo 轨 width 有值而 mono 轨该列为空(不是 0);scope 的版本/轨/时间窗过滤;
// 负零归一。无 JUCE(ADR-011)。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>
#include <vector>

#include "export/SuggestionExport.h"
#include "state/StateCodec.h"

namespace
{

using scvb::state::CrvsData;
using scvb::state::kNumTracks;
using scvb::state::kNumVersions;
using scvb::state::makeSegmentFlags;
using scvb::state::Segment;
using scvb::state::SegmentOrigin;
using scvb::suggest::buildRows;
using scvb::suggest::ExportInput;
using scvb::suggest::Row;
using scvb::suggest::Scope;
using scvb::suggest::toCsv;

constexpr double kSr = 48000.0;

Segment seg(std::int64_t t0, std::int64_t t1, float pan, float volDb, SegmentOrigin origin, bool locked)
{
    Segment s;
    s.t0 = t0;
    s.t1 = t1;
    s.pan = pan;
    s.volDb = volDb;
    s.flags = makeSegmentFlags(origin, locked);
    return s;
}

// 07 T41 验收的规模 fixture:15 轨 × 2 版本 × 每轨 40 段;偶数轨 stereo。
// `input.curves` 由 in() 现场绑定 —— fixture 是按值返回的,构造期存下 &curves 会在
// 返回(无 NRVO 时)后指向已析构的局部量。
struct Fixture
{
    CrvsData curves;
    ExportInput input;

    ExportInput& in()
    {
        input.curves = &curves;
        return input;
    }
};

Fixture makeFixture(int segsPerTrack = 40)
{
    Fixture f;
    for (std::size_t v = 0; v < kNumVersions; ++v)
    {
        f.curves.versions[v].meta.name = "V" + std::to_string(v + 1);
        for (std::size_t t = 0; t < kNumTracks; ++t)
        {
            auto& segments = f.curves.versions[v].tracks[t].segments;
            for (int i = 0; i < segsPerTrack; ++i)
            {
                const std::int64_t t0 = static_cast<std::int64_t>(i) * 48000;
                segments.push_back(seg(t0, t0 + 48000, static_cast<float>((static_cast<int>(t) * 7 + i) % 41) - 20.0f,
                                       static_cast<float>(i % 5) - 2.0f, SegmentOrigin::Auto, false));
            }
        }
    }
    f.input.sampleRate = kSr;
    for (std::size_t t = 0; t < kNumTracks; ++t)
    {
        f.input.tracks[t].label = "T" + std::to_string(t + 1);
        f.input.tracks[t].sourceChannels = (t % 2 == 1) ? 2 : 1; // 轨 2/4/…/14 为 stereo
        for (std::size_t v = 0; v < kNumVersions; ++v)
            f.input.widthPercent[v][t] = 80.0f + static_cast<float>(v) * 5.0f;
    }
    return f;
}

// CSV 切行(CRLF)。末行行尾也是 CRLF,故最后一个元素为空串。
std::vector<std::string> splitCrLf(const std::string& csv)
{
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t at = csv.find("\r\n", pos);
        if (at == std::string::npos)
        {
            lines.push_back(csv.substr(pos));
            break;
        }
        lines.push_back(csv.substr(pos, at - pos));
        pos = at + 2;
    }
    return lines;
}

// 单行按逗号切(仅用于无转义的行)。
std::vector<std::string> splitComma(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t at = line.find(',', pos);
        if (at == std::string::npos)
        {
            out.push_back(line.substr(pos));
            break;
        }
        out.push_back(line.substr(pos, at - pos));
        pos = at + 1;
    }
    return out;
}

} // namespace

TEST_CASE("SUGGEST-COL-1 表头逐字 = 冻结列定义,13 列", "[suggest]")
{
    // 07 T41 / 11 §4.2.3 B2 的列定义逐字。这一条断言就是「列顺序冻结」本身。
    REQUIRE(std::string(scvb::suggest::kCsvHeader) ==
            "track_index,track_label,source_channels,version,version_name,segment_index,"
            "t0_sec,t1_sec,pan,vol_db,width,origin,locked");
    REQUIRE(splitComma(scvb::suggest::kCsvHeader).size() == 13);
    REQUIRE(scvb::suggest::kNumColumns == 13);
}

TEST_CASE("SUGGEST-ROWS-1 15 轨 × 2 版本 × 40 段:行数 == 段总数", "[suggest]")
{
    auto f = makeFixture(40);

    Scope all;
    all.allVersions = true;
    const auto rows = buildRows(f.in(), all);
    REQUIRE(rows.size() == static_cast<std::size_t>(15 * 2 * 40));

    const auto csv = toCsv(rows);
    const auto lines = splitCrLf(csv);
    // BOM+表头 1 行 + 1200 数据行 + 末尾 CRLF 产生的空串
    REQUIRE(lines.size() == 1 + 1200 + 1);
    REQUIRE(lines.back().empty());

    // 只导当前激活版本 = 一半
    Scope one;
    one.allVersions = false;
    one.activeVersion = 2;
    const auto rows2 = buildRows(f.in(), one);
    REQUIRE(rows2.size() == static_cast<std::size_t>(15 * 40));
    for (const Row& r : rows2)
        REQUIRE(r.version == 2);
}

TEST_CASE("SUGGEST-FMT-1 文件形制:UTF-8 BOM + CRLF + 表头行", "[suggest]")
{
    auto f = makeFixture(2);
    Scope s;
    const auto csv = toCsv(buildRows(f.in(), s));

    // BOM —— 没有它,Excel 打开中文轨名就是乱码(本功能最常见的投诉面)
    REQUIRE(csv.size() > 3);
    REQUIRE(static_cast<unsigned char>(csv[0]) == 0xEF);
    REQUIRE(static_cast<unsigned char>(csv[1]) == 0xBB);
    REQUIRE(static_cast<unsigned char>(csv[2]) == 0xBF);
    REQUIRE(csv.compare(3, std::string(scvb::suggest::kCsvHeader).size(), scvb::suggest::kCsvHeader) == 0);

    // 每个 \n 前面都必须是 \r(没有裸 LF)
    for (std::size_t i = 0; i < csv.size(); ++i)
    {
        if (csv[i] == '\n')
            REQUIRE((i > 0 && csv[i - 1] == '\r'));
    }
    REQUIRE(csv.size() >= 2);
    REQUIRE(csv.compare(csv.size() - 2, 2, "\r\n") == 0); // 末行也有行尾
}

TEST_CASE("SUGGEST-ESC-1 RFC 4180:轨名含逗号/引号/换行", "[suggest]")
{
    using scvb::suggest::csvField;
    REQUIRE(csvField("Lead") == "Lead");
    REQUIRE(csvField("Lead, Dbl") == "\"Lead, Dbl\"");
    REQUIRE(csvField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    REQUIRE(csvField("a\r\nb") == "\"a\r\nb\"");
    REQUIRE(csvField("") == "");

    // 端到端:带逗号的轨名进 CSV 后,该行仍是 13 个 RFC 4180 字段
    auto f = makeFixture(1);
    f.input.tracks[0].label = "主唱, 双轨 \"A\"";
    Scope s;
    const auto csv = toCsv(buildRows(f.in(), s));
    const auto lines = splitCrLf(csv);
    REQUIRE(lines.size() >= 2);
    REQUIRE(lines[1].find("\"主唱, 双轨 \"\"A\"\"\"") != std::string::npos);
}

TEST_CASE("SUGGEST-ENUM-1 origin / locked 与 T19 state 编码逐字一致", "[suggest]")
{
    CrvsData curves;
    curves.versions[0].meta.name = "V1";
    auto& segs = curves.versions[0].tracks[0].segments;
    segs.push_back(seg(0, 48000, 0.0f, 0.0f, SegmentOrigin::Auto, false));
    segs.push_back(seg(48000, 96000, 0.0f, 0.0f, SegmentOrigin::UserEdited, true));
    segs.push_back(seg(96000, 144000, 0.0f, 0.0f, SegmentOrigin::UserCreated, true));
    segs.push_back(seg(144000, 192000, 0.0f, 0.0f, SegmentOrigin::Auto, true)); // set_locked 不改 origin

    ExportInput in;
    in.curves = &curves;
    in.sampleRate = kSr;
    in.tracks[0].label = "T1";

    Scope s;
    s.tracksMask = 0x0001;
    const auto lines = splitCrLf(toCsv(buildRows(in, s)));
    REQUIRE(lines.size() == 1 + 4 + 1);

    const std::array<const char*, 4> origins{"auto", "user_edited", "user_created", "auto"};
    const std::array<const char*, 4> locked{"false", "true", "true", "true"};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto cols = splitComma(lines[i + 1]);
        REQUIRE(cols.size() == 13);
        REQUIRE(cols[11] == origins[i]);
        REQUIRE(cols[12] == locked[i]);
    }
}

TEST_CASE("SUGGEST-WIDTH-1 width:stereo 有值,mono 留空(不是 0)", "[suggest]")
{
    CrvsData curves;
    curves.versions[0].meta.name = "V1";
    // 轨 1 = mono,轨 2 = stereo(width 设成 0 —— 「收成 mono」的有效建议)
    curves.versions[0].tracks[0].segments.push_back(seg(0, 48000, 10.0f, -1.0f, SegmentOrigin::Auto, false));
    curves.versions[0].tracks[1].segments.push_back(seg(0, 48000, -10.0f, 1.0f, SegmentOrigin::Auto, false));

    ExportInput in;
    in.curves = &curves;
    in.sampleRate = kSr;
    in.tracks[0].sourceChannels = 1;
    in.tracks[1].sourceChannels = 2;
    in.widthPercent[0][0] = 100.0f; // mono 轨的参数值存在也不许写进列(v1 no-op 占位)
    in.widthPercent[0][1] = 0.0f;

    Scope s;
    s.tracksMask = 0x0003;
    const auto rows = buildRows(in, s);
    REQUIRE(rows.size() == 2);
    REQUIRE_FALSE(rows[0].hasWidth);
    REQUIRE(rows[1].hasWidth);

    const auto lines = splitCrLf(toCsv(rows));
    const auto mono = splitComma(lines[1]);
    const auto stereo = splitComma(lines[2]);
    REQUIRE(mono[2] == "1");
    REQUIRE(mono[10].empty()); // ← 空,不是 "0.0"
    REQUIRE(stereo[2] == "2");
    REQUIRE(stereo[10] == "0.0"); // ← stereo 的 0 是有效值,必须写出来
}

TEST_CASE("SUGGEST-WIDTH-2 调用方没装 width 的那一格 → 留空,不落 0", "[suggest]")
{
    // 这是本层最危险的默认值:零值初始化会让「native 忘了装」静默产出 0.0 = 「收成 mono」,
    // 而那是一个语义与真相相反的建议。故 ExportInput 的 width 表默认是 kWidthUnknown 哨兵。
    ExportInput fresh;
    REQUIRE(fresh.widthPercent[0][0] == scvb::suggest::kWidthUnknown);
    REQUIRE(scvb::suggest::kWidthUnknown < 0.0f); // 必须在 0..100 之外

    CrvsData curves;
    curves.versions[0].meta.name = "V1";
    curves.versions[0].tracks[0].segments.push_back(seg(0, 48000, 5.0f, 0.0f, SegmentOrigin::Auto, false));

    ExportInput in; // ← width 表整张不装
    in.curves = &curves;
    in.sampleRate = kSr;
    in.tracks[0].sourceChannels = 2; // stereo,但 width 未知

    Scope s;
    s.tracksMask = 0x0001;
    const auto rows = buildRows(in, s);
    REQUIRE(rows.size() == 1);
    REQUIRE_FALSE(rows[0].hasWidth);
    REQUIRE(splitComma(splitCrLf(toCsv(rows))[1])[10].empty());
}

TEST_CASE("SUGGEST-SCOPE-1 scope:轨掩码 / 版本 / 时间窗", "[suggest]")
{
    auto f = makeFixture(10); // 每段 1 秒,共 10 秒

    Scope mask;
    mask.tracksMask = 0x0005; // 轨 1 与轨 3
    const auto rows = buildRows(f.in(), mask);
    REQUIRE(rows.size() == 20);
    for (const Row& r : rows)
        REQUIRE((r.trackIndex == 1 || r.trackIndex == 3));

    // bit15 保留 0:置位也不该多出第 16 轨(轨数只有 15)
    Scope bit15;
    bit15.tracksMask = 0xFFFF;
    REQUIRE(buildRows(f.in(), bit15).size() == 15 * 10);

    // 时间窗:与 [2.5, 4.5) 有重叠的段 = 第 2/3/4 段(0 基:2,3,4)
    Scope win;
    win.tracksMask = 0x0001;
    win.startSec = 2.5;
    win.endSec = 4.5;
    const auto wrows = buildRows(f.in(), win);
    REQUIRE(wrows.size() == 3);
    REQUIRE(wrows[0].segmentIndex == 2);
    REQUIRE(wrows[2].segmentIndex == 4);
    // 段值不裁剪:t0/t1 仍是整段边界
    REQUIRE(wrows[0].t0Sec == 2.0);
    REQUIRE(wrows[2].t1Sec == 5.0);
}

TEST_CASE("SUGGEST-EDGE-1 退化输入:空 curves / 非法采样率 / 非法版本 → 空行集,只出表头", "[suggest]")
{
    ExportInput empty;
    Scope s;
    REQUIRE(buildRows(empty, s).empty());

    auto f = makeFixture(1);
    f.input.sampleRate = 0.0;
    REQUIRE(buildRows(f.in(), s).empty());

    auto g = makeFixture(1);
    Scope bad;
    bad.activeVersion = 3; // 只有 2 个版本
    REQUIRE(buildRows(g.in(), bad).empty());

    const auto csv = toCsv({});
    REQUIRE(splitCrLf(csv).size() == 2); // 表头 + 末尾空串
}

TEST_CASE("SUGGEST-NUM-1 数值格式:小数位固定,负零归一", "[suggest]")
{
    using scvb::suggest::fmtFixed;
    REQUIRE(fmtFixed(1.0 / 3.0, 3) == "0.333");
    REQUIRE(fmtFixed(-0.0, 1) == "0.0");
    REQUIRE(fmtFixed(-0.04, 1) == "0.0"); // 四舍五入到 -0.0 也归一
    REQUIRE(fmtFixed(-12.34, 1) == "-12.3");
    REQUIRE(fmtFixed(2.0, 3) == "2.000");

    CrvsData curves;
    curves.versions[0].meta.name = "V1";
    curves.versions[0].tracks[0].segments.push_back(seg(1234, 5678, -0.001f, 1.5f, SegmentOrigin::Auto, false));
    ExportInput in;
    in.curves = &curves;
    in.sampleRate = kSr;
    Scope s;
    s.tracksMask = 0x0001;
    const auto cols = splitComma(splitCrLf(toCsv(buildRows(in, s)))[1]);
    REQUIRE(cols[6] == "0.026"); // 1234 / 48000
    REQUIRE(cols[7] == "0.118"); // 5678 / 48000
    REQUIRE(cols[8] == "0.0"); // -0.001 → -0.0 → 归一
    REQUIRE(cols[9] == "1.5");
}

// -----------------------------------------------------------------------------
// 跨语言格式化黄金表 —— 本块由 web-preview/tests/smoke-t41-suggestions.mjs **读源码**
// 解析出来,拿 JS 的 fmtFixed 跑一遍对同一批期望串。本卡的卖点是「两侧同一口径」,
// 而两侧的底座不同(C++ `printf %.*f` 就近偶数 / JS `toFixed` 平局取大),光对拍表头
// 与小数位常量还盖不住数值这一层,所以立一张两边都认的表。
//
// 表里**刻意不放二进制精确平局**(如 pan=12.25):那种值上两侧确实会分叉,而它在本卡
// 不可达 —— 秒值是 样本数/采样率、pan/vol/width 是 0.1 步进的 f32,落不到精确的
// 十进制半点上。真要放宽这条前提(比如将来允许任意精度手输),得先在两侧统一舍入方向。
// 格式:{ 值, 小数位, 期望串 } —— 每行一条,冒烟按这个形状正则解析。
struct FmtGolden
{
    double v;
    int d;
    const char* s;
};

// clang-format off
static const FmtGolden kFmtGolden[] = {
    { 0.0, 1, "0.0" },
    { -0.0, 1, "0.0" },
    { -0.04, 1, "0.0" },
    { 2.0, 3, "2.000" },
    { 1.5, 1, "1.5" },
    { -12.34, 1, "-12.3" },
    { 100.0, 1, "100.0" },
    { 0.0257083333333333, 3, "0.026" },
    { 0.1182916666666667, 3, "0.118" },
    { -62.7, 1, "-62.7" },
    { 99.96, 1, "100.0" },
    { -0.049, 1, "0.0" },
    { 3.14159265358979, 3, "3.142" },
};
// clang-format on

TEST_CASE("SUGGEST-NUM-2 跨语言格式化黄金表(与 JS 侧同一批期望串)", "[suggest]")
{
    for (const FmtGolden& g : kFmtGolden)
    {
        INFO("v=" << g.v << " d=" << g.d);
        REQUIRE(scvb::suggest::fmtFixed(g.v, g.d) == std::string(g.s));
    }
}

TEST_CASE("SUGGEST-ESC-2 version_name 与 track_label 同样按 RFC 4180 转义", "[suggest]")
{
    // 版本名也是用户可改文本([J05] `setVersionName`),「V1, 备份」这种名字很常见 ——
    // 它与轨名共用 csvField,但只有轨名有端到端用例的话,哪天有人给版本名走了别的路就没人拦。
    CrvsData curves;
    curves.versions[0].meta.name = "V1, \"备份\"";
    curves.versions[0].tracks[0].segments.push_back(seg(0, 48000, 0.0f, 0.0f, SegmentOrigin::Auto, false));

    ExportInput in;
    in.curves = &curves;
    in.sampleRate = kSr;
    in.tracks[0].label = "Lead";

    Scope s;
    s.tracksMask = 0x0001;
    const auto line = splitCrLf(toCsv(buildRows(in, s)))[1];
    REQUIRE(line.find("\"V1, \"\"备份\"\"\"") != std::string::npos);
}

TEST_CASE("SUGGEST-EDGE-2 段里带 NaN/inf → 整条链路不产出非数字字面量", "[suggest]")
{
    // 上游真出了 NaN(除零 / 未初始化 buffer),CSV 里写出 "nan" 会把下游表格软件与
    // 脚本一起毒化;fmtFixed 层已经兜了,这里验它在完整 buildRows→toCsv 链路上仍然成立。
    CrvsData curves;
    curves.versions[0].meta.name = "V1";
    curves.versions[0].tracks[0].segments.push_back(seg(0, 48000, std::numeric_limits<float>::quiet_NaN(),
                                                        std::numeric_limits<float>::infinity(), SegmentOrigin::Auto,
                                                        false));
    ExportInput in;
    in.curves = &curves;
    in.sampleRate = kSr;
    in.tracks[0].sourceChannels = 2;
    in.widthPercent[0][0] = -std::numeric_limits<float>::infinity(); // 负 inf 落在哨兵一侧 ⇒ 留空

    Scope s;
    s.tracksMask = 0x0001;
    const auto csv = toCsv(buildRows(in, s));
    REQUIRE(csv.find("nan") == std::string::npos);
    REQUIRE(csv.find("inf") == std::string::npos);
    const auto cols = splitComma(splitCrLf(csv)[1]);
    REQUIRE(cols.size() == 13);
    REQUIRE(cols[8] == "0.0");
    REQUIRE(cols[9] == "0.0");
    REQUIRE(cols[10].empty());
}
