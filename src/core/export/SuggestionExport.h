// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SuggestionExport —— 建议表行集构造 + CSV 序列化(07 T41;11 §4.2.3 通路 B 的 B1/B2)。
// 纯 C++17、只依赖 scvb_core 的 state 层,**不链接 JUCE、不碰文件系统**(ADR-001/011):
// 本层只把 state CRVS 的段真身翻译成「一段一行」的行集并序列化成字节,
// 落盘与保存对话框归 Output 的 JUCE 插件层(桥函数 exportSuggestions,契约变更文档
// docs/contract-changes/20260825-export-suggestions.md)。于是本层可离线单测。
//
// 【它是建议,不是执行】行集里的 pan/vol/width 是 SCVB 算出来的**建议值**;
// SCVB 不会替用户在 DAW 里应用它们。这句话必须在 UI 与文档里写清(07 T41 强调栏)。
//
// 【列定义冻结】(07 T41 / 11 §4.2.3 B2 逐字,顺序不可改、不可增删):
//   track_index, track_label, source_channels, version, version_name,
//   segment_index, t0_sec, t1_sec, pan, vol_db, width, origin, locked
// 其中 [J57] 新增的 source_channels 与 width **不可省** —— 不带 width 的建议对 stereo 轨
// 不可执行(一个 pan 点描述不了一条立体声轨的张开度)。
//
// 【文件形制】UTF-8 **带 BOM**(否则 Excel 打开中文轨名乱码,这是本功能最常见的投诉面)、
// 换行 **CRLF**、含表头行、字段按 RFC 4180 转义(含 , " CR LF 的字段整体加双引号,
// 内部双引号翻倍)。
//
// 【数值口径】与 UI 显示值同档,且两侧**同一份行集**出表格与出 CSV(不各算一遍):
//   t0_sec / t1_sec  3 位小数(与 05 §2.3a 的 mm:ss.mmm 同精度)
//   pan / vol_db     1 位小数(05 §2.3a:pan step 1 / shift 0.1;vol step 0.1)
//   width            1 位小数(0..100 %);**mono 轨该列为空**——不是 0,
//                    0 是「收成 mono」的有效值([J57]),语义冲突。
//   origin / locked  与 T19 state 编码逐字一致:auto|user_edited|user_created;true|false

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "state/StateCodec.h"

namespace scvb::suggest
{

// 冻结表头(逐字 = 上面的列定义;JS 侧 web/output/tab-suggestions.js 的 CSV_HEADER 与本串
// 由 web-preview/tests/smoke-t41-suggestions.mjs 读源码对拍,漂了当场红)。
inline constexpr char kCsvHeader[] =
    "track_index,track_label,source_channels,version,version_name,segment_index,t0_sec,t1_sec,pan,vol_db,width,origin,"
    "locked";
inline constexpr int kNumColumns = 13;
inline constexpr char kCsvNewline[] = "\r\n";
inline constexpr char kUtf8Bom[] = "\xEF\xBB\xBF";

// 小数位数(见文件头【数值口径】)。
inline constexpr int kDecimalsSec = 3;
inline constexpr int kDecimalsPan = 1;
inline constexpr int kDecimalsVol = 1;
inline constexpr int kDecimalsWidth = 1;

// 每轨的只读元数据。sourceChannels 是**检测值**(契约 §1.15:只读,不可写):1=mono,2=stereo。
struct TrackMeta
{
    std::string label; // channels[ch].label;空串 = 用户未命名
    int sourceChannels = 1; // 1 | 2;非法值按 1 处理
};

// 行集输入。widthPercent[v-1][t-1] = 参数 v{v}_t{tt}_width 的当前值(0..100 %)。
// mono 轨该参数是 v1 no-op 占位(params v2.0),故 mono 轨的 width 列一律留空(见 buildRows)。
struct ExportInput
{
    const state::CrvsData* curves = nullptr;
    std::array<TrackMeta, state::kNumTracks> tracks{};
    std::array<std::array<float, state::kNumTracks>, state::kNumVersions> widthPercent{};
    double sampleRate = 48000.0; // 样本→秒换算;≤0 视为非法,buildRows 返回空行集
};

// 导出范围。默认 = 当前激活版本 + 全部 15 轨 + 全时间线。
struct Scope
{
    bool allVersions = false; // false = 只导 activeVersion;true = 版本 1..kNumVersions 顺序拼接
    int activeVersion = 1; // 1..kNumVersions;allVersions=true 时忽略
    std::uint16_t tracksMask = 0x7FFFu; // bit0=轨1 … bit14=轨15;bit15 保留 0(契约 §9.2)
    // 时间窗:两者都 ≥0 且 end>start 时生效,只保留**与窗口有重叠**的段;
    // 段的 t0/t1 值**不裁剪**(建议值属于整段,截半段会给出一个没人建议过的区间)。
    double startSec = -1.0;
    double endSec = -1.0;
};

// 一行 = 一个段。字段名与列名一一对应。
struct Row
{
    int trackIndex = 0; // 1..15
    std::string trackLabel;
    int sourceChannels = 1;
    int version = 1; // 1..2
    std::string versionName;
    int segmentIndex = 0; // 0 基,与 §2.8 段表下标同源
    double t0Sec = 0.0;
    double t1Sec = 0.0;
    double pan = 0.0;
    double volDb = 0.0;
    bool hasWidth = false; // false ⇒ width 列留空(mono 轨)
    double width = 0.0;
    state::SegmentOrigin origin = state::SegmentOrigin::Auto;
    bool locked = false;
};

// origin 的 CSV 字面值(与 T19 state 编码逐字一致)。
const char* originName(state::SegmentOrigin origin) noexcept;

// 行集构造:版本外层、轨中层、段内层,三层都按升序 —— 行序即「版本 → 轨 → 段」的字典序。
// curves 为 nullptr 或 sampleRate ≤ 0 时返回空行集(不抛)。
std::vector<Row> buildRows(const ExportInput& input, const Scope& scope);

// RFC 4180 字段转义:含 , " CR LF 的字段整体加双引号并把内部 " 翻倍;否则原样。
std::string csvField(const std::string& raw);

// 定点格式化:decimals 位小数,-0.0 归一成 0.0(否则同一个零在不同轨上写出两种字面值)。
std::string fmtFixed(double v, int decimals);

// 序列化:BOM + 表头 + 每行,行尾一律 CRLF(**含最后一行**)。
std::string toCsv(const std::vector<Row>& rows);

} // namespace scvb::suggest
