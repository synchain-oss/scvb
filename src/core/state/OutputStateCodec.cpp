// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/OutputStateCodec.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace scvb::state
{

namespace
{
constexpr std::size_t kHeaderBytes = 24; // 6 个 u32
constexpr std::size_t kEnumBytes = 8; // 2 个 u32(loudness_mode + center_slot_policy)
// [SL-279] 第二级尾扩:applied.{loudness_mode,center_slot_policy}(abi 2→3)。
constexpr std::size_t kAppliedBytes = 8; // 2 个 u32
// [SL-411] 第三级尾扩:analysis.segmentation.{mode,sensitivity,min_segment_ms}(abi 3→4)。
// **这一档是 12 字节而不是 8**:三个字段里有一个是 f32(灵敏度是 0..100 的连续刻度,
// 压成 u32 只会平白丢精度),而 u32+f32+u32 一起构成**一个整档** —— 档内不许半截,
// 见下面的长度回退。
constexpr std::size_t kSegmentationBytes = 12;
// [SL-416] 第四级尾扩:analysis.vad.{threshold_db,hysteresis_db,hangover_ms,padding_pre_ms,
// padding_post_ms} + analysis.transition_ramp_ms(abi 4→5)。
// **这一档是 24 字节**:两个 dB 类字段是 f32(连续刻度,压成 u32 只会平白丢精度)、四个 ms 类字段是
// u32(`hangover_ms` / `padding_*_ms` / `transition_ramp_ms` 都是整数毫秒),u32+f32+f32+u32×4
// 一起构成**一个整档** —— 档内不许半截,见下面的长度回退。
constexpr std::size_t kVadBytes = 24;
// [SL-472] 第五级尾扩:channels[15] 七项(abi 5→6)。每轨一条**定长**记录 = 7×u32 + 96 字节 label 槽,
// 15 轨一整档 1860 字节 —— 档内不许半截,见下面的长度回退。定长的理由写在头注(变长记录会让「这一档
// 到哪里结束」依赖档内的 labelBytes,一个坏长度就把其后的未知尾部一起读歪)。
constexpr std::size_t kChannelRecordBytes = 7 * 4 + kOutputChannelLabelMaxBytes; // 124
constexpr std::size_t kChannelsBytes = kOutputChannelCount * kChannelRecordBytes; // 1860
// 已知尾部各档的累计长度(remaining 只接受 0 / 8 / 16 / 28 / 52 / 1912 及 1912+)。
constexpr std::size_t kTailThroughVad = kEnumBytes + kAppliedBytes + kSegmentationBytes + kVadBytes; // 52
constexpr std::size_t kTailThroughChannels = kTailThroughVad + kChannelsBytes; // 1912

static_assert(sizeof(float) == 4, "f32 尾字段依赖 IEEE-754 单精度(4 字节)");
static_assert(kChannelRecordBytes == 124 && kChannelsBytes == 1860 && kTailThroughChannels == 1912,
              "[SL-472] channels 档的 wire 长度写进了头注与 STATE_SCHEMA,改这里要连着两处一起改");

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

bool readU32(const std::uint8_t* p, std::size_t size, std::uint32_t& out)
{
    if (size < 4)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
          (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return true;
}

// f32 走**位模式**而不是 reinterpret_cast<float*>:既避开严格别名(UB),也保证写盘字节
// 与机器浮点寄存器宽度/对齐无关 —— 先 memcpy 出 IEEE-754 位模式,再按 u32 的小端规则落盘,
// 读侧反过来。于是本 codec 的字节序纪律只有一条(全 u32),f32 不再单开一套。
void putF32(std::vector<std::uint8_t>& out, float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(out, bits);
}

bool readF32(const std::uint8_t* p, std::size_t size, float& out)
{
    std::uint32_t bits = 0;
    if (!readU32(p, size, bits))
    {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

// ---- [J69/U24] 枚举字符串 ↔ 序号(STATE_SCHEMA 口径;未知一律回落默认档)----

const char* loudnessModeString(std::uint32_t ordinal)
{
    switch (ordinal)
    {
    case 1:
        return "rms";
    case 2:
        return "peak_dbfs";
    case 0:
    default:
        return "kw_integrated";
    }
}

std::uint32_t loudnessModeOrdinal(const std::string& s)
{
    if (s == "rms")
        return 1;
    if (s == "peak_dbfs")
        return 2;
    return 0; // "kw_integrated" 或未知 → 默认
}

const char* centerSlotPolicyString(std::uint32_t ordinal)
{
    switch (ordinal)
    {
    case 1:
        return "lead_exclusive";
    case 2:
        return "even_spread";
    case 0:
    default:
        return "priority_queue";
    }
}

std::uint32_t centerSlotPolicyOrdinal(const std::string& s)
{
    if (s == "lead_exclusive")
        return 1;
    if (s == "even_spread")
        return 2;
    return 0; // "priority_queue" 或未知 → 默认
}

// ---- [SL-411] segmentation.mode ↔ 序号(02-dsp-spec §362 只认这两档)----

const char* segModeString(std::uint32_t ordinal)
{
    return ordinal == 1 ? "vad_only" : "valley"; // 0 与越界都归 valley
}

std::uint32_t segModeOrdinal(const std::string& s)
{
    // 真源白名单与 `BridgeArgs.h::isSegmentationMode` 逐字一致 —— 那边不认的串这里也不许落盘,
    // 否则一份手改过的工程能把 UI 送进「本地有、native 不认」的第三档(mode 是**整包**下发的,
    // 一个坏串会让整个 setSegmentation badArg,三个字段谁都进不去)。
    return s == "vad_only" ? 1 : 0;
}

// ---- [SL-472] label 的 UTF-8 边界 ----

// 从 s[i] 起解一个**严格**合法的 UTF-8 码点:返回其字节数(1..4),非法返回 0。
// 严格 = 拒过长编码、拒 UTF-16 代理区(U+D800..U+DFFF)、拒 > U+10FFFF、拒截断的续字节。
std::size_t utf8SeqLen(const std::uint8_t* s, std::size_t n, std::size_t i)
{
    const std::uint8_t b0 = s[i];
    if (b0 < 0x80u)
    {
        return 1;
    }
    std::size_t len = 0;
    std::uint32_t cp = 0;
    std::uint32_t minCp = 0;
    if ((b0 & 0xE0u) == 0xC0u)
    {
        len = 2;
        cp = b0 & 0x1Fu;
        minCp = 0x80u;
    }
    else if ((b0 & 0xF0u) == 0xE0u)
    {
        len = 3;
        cp = b0 & 0x0Fu;
        minCp = 0x800u;
    }
    else if ((b0 & 0xF8u) == 0xF0u)
    {
        len = 4;
        cp = b0 & 0x07u;
        minCp = 0x10000u;
    }
    else
    {
        return 0; // 孤立续字节或 0xF8.. 前缀
    }
    if (i + len > n)
    {
        return 0;
    }
    for (std::size_t k = 1; k < len; ++k)
    {
        const std::uint8_t b = s[i + k];
        if ((b & 0xC0u) != 0x80u)
        {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
    {
        return 0;
    }
    return len;
}

// 编码侧截断:取 s 的最长前缀,使其 ≤ kOutputChannelLabelMaxChars 个码点且 ≤ kOutputChannelLabelMaxBytes
// 字节,且不切出半个码点。遇到非法字节(本进程内存里的 label 来自 juce::String,正常不会有)即在那里截止,
// 保证写下去的字节一定能被 decode 原样收回 —— 否则一次坏字节会让整条 label 在下次打开时回落成空。
std::size_t labelEncodeBytes(const std::string& s)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    std::uint32_t chars = 0;
    while (i < n && chars < kOutputChannelLabelMaxChars)
    {
        const std::size_t len = (p[i] == 0u) ? 0u : utf8SeqLen(p, n, i);
        if (len == 0 || i + len > kOutputChannelLabelMaxBytes)
        {
            break;
        }
        i += len;
        ++chars;
    }
    return i;
}

// 解码侧校验:bytes 是否为 ≤24 码点、无 NUL 的严格 UTF-8(长度 ≤96 由调用方先判)。
bool labelDecodeValid(const std::uint8_t* p, std::size_t n)
{
    std::size_t i = 0;
    std::uint32_t chars = 0;
    while (i < n)
    {
        if (p[i] == 0u)
        {
            return false;
        }
        const std::size_t len = utf8SeqLen(p, n, i);
        if (len == 0)
        {
            return false;
        }
        i += len;
        if (++chars > kOutputChannelLabelMaxChars)
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out)
{
    out.clear();
    const std::size_t langBytes = std::min<std::size_t>(s.uiLanguage.size(), kOutputLanguageMaxBytes);
    try
    {
        out.reserve(kHeaderBytes + langBytes + kTailThroughChannels + s.unknownTail.size());
    }
    catch (...)
    {
        return false;
    }
    putU32(out, s.groupId);
    putU32(out, s.captureEnabled);
    putU32(out, s.outputEnabled);
    putU32(out, s.versionActive);
    putU32(out, s.uiScale);
    putU32(out, static_cast<std::uint32_t>(langBytes));
    out.insert(out.end(), s.uiLanguage.begin(), s.uiLanguage.begin() + static_cast<std::ptrdiff_t>(langBytes));
    putU32(out, loudnessModeOrdinal(s.loudnessMode));
    putU32(out, centerSlotPolicyOrdinal(s.centerSlotPolicy));
    putU32(out, loudnessModeOrdinal(s.appliedLoudnessMode)); // [SL-279]
    putU32(out, centerSlotPolicyOrdinal(s.appliedCenterSlotPolicy)); // [SL-278/SL-279]
    // [SL-411] 恒写这一整档(12 字节):编码侧不做值域校验 —— encode 的入参是**本进程自己的**
    // runtime_(桥面已按 `kOutputSeg*` 常量夹过,值域真源见本文件头注;非有限值由桥面的
    // `std::isfinite` 守卫挡在 runtime_ 之外),值域校验是 decode 的职责(不可信字节在磁盘上,
    // 不在内存里)。两侧是**同一条规则的两端**,不是重复实现:encode 侧挡的是「本进程造出来的值」,
    // decode 侧挡的是「磁盘上来的值」—— 后者再兜一次才能保证 decode 出口只有「规格内」与「规格默认」。
    putU32(out, segModeOrdinal(s.segmentationMode));
    putF32(out, s.segmentationSensitivity);
    putU32(out, s.segmentationMinSegmentMs);
    // [SL-416] 同样**恒写这一整档**(24 字节),理由与上一档逐字相同:encode 的入参是本进程自己的
    // runtime_(桥面 `handleSetVad` / `handleSetTransitionRampMs` 已按 `kOutputVad*` /
    // `kOutputTransitionRampMs*` 常量在 **double 域**夹过并把非有限值挡在 runtime_ 之外),值域校验
    // 是 decode 的职责。`transitionRampMs` 在 runtime_ 里是 float(契约 §1.20 的 UI 也是连续刻度),
    // 落盘时按规格值域窄化成 u32 —— 桥面已令它落在 20..300 的整数值上,窄化无损。
    putF32(out, s.vadThresholdDb);
    putF32(out, s.vadHysteresisDb);
    putU32(out, s.vadHangoverMs);
    putU32(out, s.vadPaddingPreMs);
    putU32(out, s.vadPaddingPostMs);
    putU32(out, s.transitionRampMs);
    // [SL-472] channels[15] 恒写一整档(15 × 124 字节)。与前几档同一条分工:数值字段编码侧不校验
    // (入参是本进程的 runtime_,桥面已按同一值域夹过),值域校验是 decode 的职责。**唯一的例外是 label**:
    // 它必须被截到槽里放得下(≤96 字节、≤24 码点、不切半个码点)—— 那不是「校验」,是「能不能写下去」,
    // 与 uiLanguage 的超长截断同一类。截断后的字节一定能被 decode 原样收回(见 labelEncodeBytes)。
    for (const OutputChannelState& c : s.channels)
    {
        putU32(out, c.enabled ? 1u : 0u);
        putU32(out, c.participateAutoPan);
        putU32(out, c.priority);
        putU32(out, c.leadLock ? 1u : 0u);
        putU32(out, c.leadVolExempt ? 1u : 0u);
        putU32(out, c.pairId);
        const std::size_t labelBytes = labelEncodeBytes(c.label);
        putU32(out, static_cast<std::uint32_t>(labelBytes));
        out.insert(out.end(), c.label.begin(), c.label.begin() + static_cast<std::ptrdiff_t>(labelBytes));
        out.insert(out.end(), kOutputChannelLabelMaxBytes - labelBytes, std::uint8_t{0}); // 槽内补 0
    }
    out.insert(out.end(), s.unknownTail.begin(), s.unknownTail.end()); // 未知尾部原样回写
    return true;
}

bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out, OutputDecodeReport* report)
{
    if (data == nullptr || size < kHeaderBytes)
    {
        return false;
    }
    std::uint32_t groupId = 0;
    std::uint32_t captureEnabled = 0;
    std::uint32_t outputEnabled = 0;
    std::uint32_t versionActive = 0;
    std::uint32_t uiScale = 0;
    std::uint32_t langBytes = 0;
    if (!readU32(data, size, groupId) || !readU32(data + 4, size - 4, captureEnabled) ||
        !readU32(data + 8, size - 8, outputEnabled) || !readU32(data + 12, size - 12, versionActive) ||
        !readU32(data + 16, size - 16, uiScale) || !readU32(data + 20, size - 20, langBytes))
    {
        return false;
    }
    if (groupId < kOutputGroupIdMin || groupId > kOutputGroupIdMax)
    {
        return false;
    }
    if (captureEnabled > 1 || outputEnabled > 1)
    {
        return false;
    }
    if (versionActive < kOutputVersionMin || versionActive > kOutputVersionMax)
    {
        return false;
    }
    if (langBytes > kOutputLanguageMaxBytes)
    {
        return false; // langBytes 超上限 → 拒载(不可信字节,§7.3;尾字段/未知尾部由下方长度回退逻辑处理)
    }
    const std::size_t base = kHeaderBytes + langBytes;
    if (base > size)
    {
        return false;
    }
    const std::size_t remaining = size - base;
    const bool hasEnums = (remaining >= kEnumBytes);
    if (remaining != 0 && !hasEnums)
    {
        return false; // 0 < remaining < 8:枚举字段被截断 → 拒载(不可信字节)
    }
    // [SL-279] 第二级:applied 那两个 u32 要么齐、要么整段没有 —— 半截同样拒载。
    const bool hasApplied = (remaining >= kEnumBytes + kAppliedBytes);
    if (hasEnums && remaining > kEnumBytes && !hasApplied)
    {
        return false; // 8 < remaining < 16:applied 字段被截断 → 拒载(不可信字节)
    }
    // [SL-411] 第三级:segmentation 三字段(u32 + f32 + u32 = 12 字节)**整档**要么齐、要么全没有。
    // 半截(16 < remaining < 28)同样拒载 —— 一整档里的三个字段是同一个 commit 写下去的,
    // 「只有前两个」这种形态不可能是任何真实构建的产物。
    const bool hasSegmentation = (remaining >= kEnumBytes + kAppliedBytes + kSegmentationBytes);
    if (hasApplied && remaining > kEnumBytes + kAppliedBytes && !hasSegmentation)
    {
        return false; // 16 < remaining < 28:segmentation 字段被截断 → 拒载(不可信字节)
    }
    // [SL-416] 第四级:vad 五字段 + transition_ramp_ms(2×f32 + 4×u32 = 24 字节)**整档**要么齐、
    // 要么全没有。半截(28 < remaining < 52)同样拒载 —— 一整档里的六个字段是同一个 commit 写下去的,
    // 「只有前几个」这种形态不可能是任何真实构建的产物。
    const bool hasVad = (remaining >= kEnumBytes + kAppliedBytes + kSegmentationBytes + kVadBytes);
    if (hasSegmentation && remaining > kEnumBytes + kAppliedBytes + kSegmentationBytes && !hasVad)
    {
        return false; // 28 < remaining < 52:vad/ramp 字段被截断 → 拒载(不可信字节)
    }
    // [SL-472] 第五级:channels[15](15 × 124 = 1860 字节)**整档**要么齐、要么全没有。半截
    // (52 < remaining < 1912)同样拒载 —— 与前四档同一条「档内不许半截」。
    const bool hasChannels = (remaining >= kTailThroughChannels);
    if (hasVad && remaining > kTailThroughVad && !hasChannels)
    {
        return false; // 52 < remaining < 1912:channels 档被截断 → 拒载(不可信字节)
    }

    // 兼容:旧版(abi=1)payload 无末两个 u32 → 两字段回落默认,不计未知回落。
    std::uint32_t loudnessOrdinal = 0;
    std::uint32_t centerOrdinal = 0;
    if (hasEnums)
    {
        if (!readU32(data + base, remaining, loudnessOrdinal) ||
            !readU32(data + base + 4, remaining - 4, centerOrdinal))
        {
            return false;
        }
    }

    OutputState parsed;
    parsed.groupId = groupId;
    parsed.captureEnabled = captureEnabled;
    parsed.outputEnabled = outputEnabled;
    parsed.versionActive = versionActive;
    parsed.uiScale = uiScale;
    parsed.uiLanguage.assign(reinterpret_cast<const char*>(data + kHeaderBytes), langBytes);

    if (loudnessOrdinal > kOutputLoudnessModeMax)
    {
        if (report != nullptr)
        {
            ++report->loudnessModeFallbacks;
        }
        loudnessOrdinal = 0;
    }
    if (centerOrdinal > kOutputCenterSlotPolicyMax)
    {
        if (report != nullptr)
        {
            ++report->centerSlotPolicyFallbacks;
        }
        centerOrdinal = 0;
    }
    parsed.loudnessMode = loudnessModeString(loudnessOrdinal);
    parsed.centerSlotPolicy = centerSlotPolicyString(centerOrdinal);

    // [SL-279] applied.*:**缺席时取当前值,不取默认值**。旧工程(abi=2)没有这两个字段,
    // 语义是「它存着的那档就是上次分析用的那档」—— 取默认会让存了非默认档的工程一打开就
    // 误报「需重新分析」。越界值单独计数(见 OutputDecodeReport 那两个新字段的注释)。
    std::uint32_t appliedLoudnessOrdinal = loudnessOrdinal;
    std::uint32_t appliedCenterOrdinal = centerOrdinal;
    if (hasApplied)
    {
        if (!readU32(data + base + kEnumBytes, remaining - kEnumBytes, appliedLoudnessOrdinal) ||
            !readU32(data + base + kEnumBytes + 4, remaining - kEnumBytes - 4, appliedCenterOrdinal))
        {
            return false;
        }
        if (appliedLoudnessOrdinal > kOutputLoudnessModeMax)
        {
            if (report != nullptr)
            {
                ++report->appliedLoudnessModeFallbacks;
            }
            appliedLoudnessOrdinal = 0;
        }
        if (appliedCenterOrdinal > kOutputCenterSlotPolicyMax)
        {
            if (report != nullptr)
            {
                ++report->appliedCenterSlotPolicyFallbacks;
            }
            appliedCenterOrdinal = 0;
        }
    }
    parsed.appliedLoudnessMode = loudnessModeString(appliedLoudnessOrdinal);
    parsed.appliedCenterSlotPolicy = centerSlotPolicyString(appliedCenterOrdinal);

    // [SL-411] segmentation 三项:**缺席时回落规格默认且不计回落**(abi≤3 的旧工程「当年没存过」,
    // 不是「存的值不可信」—— 那个区别正是这三个计数器存在的意义)。在席时逐个值域校验,
    // 越界 → 回落该字段默认 + 计一次回落(理由写在头注那段「唯一一处值越界 → 回落默认」)。
    std::uint32_t segMode = kOutputSegModeDefault;
    float segSensitivity = kOutputSegSensitivityDefault;
    std::uint32_t segMinMs = kOutputSegMinSegmentMsDefault;
    if (hasSegmentation)
    {
        const std::size_t segOff = kEnumBytes + kAppliedBytes;
        const std::size_t segAvail = remaining - segOff;
        if (!readU32(data + base + segOff, segAvail, segMode) ||
            !readF32(data + base + segOff + 4, segAvail - 4, segSensitivity) ||
            !readU32(data + base + segOff + 8, segAvail - 8, segMinMs))
        {
            return false;
        }
        if (segMode > kOutputSegModeMax)
        {
            if (report != nullptr)
            {
                ++report->segmentationModeFallbacks;
            }
            segMode = kOutputSegModeDefault;
        }
        // NaN 与 ±Inf 都走这一支:`x < lo || x > hi` 对 NaN **恒假**,只写范围比较会把它放进去,
        // 而 NaN 一旦进了 runtime_ → PipelineConfig 的灵敏度,下游所有比较都是假 —— 那种坏法是静默的。
        // ⚠ `!(x >= lo && x <= hi)` 是这里**唯一**正确的写法,而它成立有个前提:编译器没把 NaN 语义
        // 优化掉。已核:全仓 `CMakeLists.txt` / `cmake/` / 源码里没有任何 `/fp:fast` 或 `-ffast-math`
        // (MSVC 默认 `/fp:precise`),所以这个判定不会被当成恒假删掉。**换一次浮点编译选项就会让它
        // 静默失效** —— 加那类开关的人要连着这一行一起想([SL-411 R4] 复审指出,记在这里免得下一个人
        // 以为这是「随便写写都对」)。
        if (!(segSensitivity >= kOutputSegSensitivityMin && segSensitivity <= kOutputSegSensitivityMax))
        {
            if (report != nullptr)
            {
                ++report->segmentationSensitivityFallbacks;
            }
            segSensitivity = kOutputSegSensitivityDefault;
        }
        if (segMinMs < kOutputSegMinSegmentMsMin || segMinMs > kOutputSegMinSegmentMsMax)
        {
            if (report != nullptr)
            {
                ++report->segmentationMinSegmentMsFallbacks;
            }
            segMinMs = kOutputSegMinSegmentMsDefault;
        }
    }
    parsed.segmentationMode = segModeString(segMode);
    parsed.segmentationSensitivity = segSensitivity;
    parsed.segmentationMinSegmentMs = segMinMs;

    // [SL-416] vad 五字段 + transition_ramp_ms:**缺席时回落规格默认且不计回落**(与上一档同一条
    // 取舍 —— abi≤4 的旧工程「当年没存过」,不是「存的值不可信」)。在席时逐个做值域校验,
    // 越界 → 回落该字段默认 + 计一次回落。浮点字段用 `!(x >= lo && x <= hi)`(NaN/±Inf 同支,
    // 理由与上文 segmentSensitivity 那一段逐字相同 —— 同一个 `/fp:precise` 前提)。
    float vadThresholdDb = kOutputVadThresholdDbDefault;
    float vadHysteresisDb = kOutputVadHysteresisDbDefault;
    std::uint32_t vadHangoverMs = kOutputVadHangoverMsDefault;
    std::uint32_t vadPaddingPreMs = kOutputVadPaddingPreMsDefault;
    std::uint32_t vadPaddingPostMs = kOutputVadPaddingPostMsDefault;
    std::uint32_t transitionRampMs = kOutputTransitionRampMsDefault;
    if (hasVad)
    {
        const std::size_t vadOff = kEnumBytes + kAppliedBytes + kSegmentationBytes;
        const std::size_t vadAvail = remaining - vadOff;
        if (!readF32(data + base + vadOff, vadAvail, vadThresholdDb) ||
            !readF32(data + base + vadOff + 4, vadAvail - 4, vadHysteresisDb) ||
            !readU32(data + base + vadOff + 8, vadAvail - 8, vadHangoverMs) ||
            !readU32(data + base + vadOff + 12, vadAvail - 12, vadPaddingPreMs) ||
            !readU32(data + base + vadOff + 16, vadAvail - 16, vadPaddingPostMs) ||
            !readU32(data + base + vadOff + 20, vadAvail - 20, transitionRampMs))
        {
            return false;
        }
        if (!(vadThresholdDb >= kOutputVadThresholdDbMin && vadThresholdDb <= kOutputVadThresholdDbMax))
        {
            if (report != nullptr)
            {
                ++report->vadThresholdDbFallbacks;
            }
            vadThresholdDb = kOutputVadThresholdDbDefault;
        }
        if (!(vadHysteresisDb >= kOutputVadHysteresisDbMin && vadHysteresisDb <= kOutputVadHysteresisDbMax))
        {
            if (report != nullptr)
            {
                ++report->vadHysteresisDbFallbacks;
            }
            vadHysteresisDb = kOutputVadHysteresisDbDefault;
        }
        if (vadHangoverMs < kOutputVadHangoverMsMin || vadHangoverMs > kOutputVadHangoverMsMax)
        {
            if (report != nullptr)
            {
                ++report->vadHangoverMsFallbacks;
            }
            vadHangoverMs = kOutputVadHangoverMsDefault;
        }
        if (vadPaddingPreMs < kOutputVadPaddingPreMsMin || vadPaddingPreMs > kOutputVadPaddingPreMsMax)
        {
            if (report != nullptr)
            {
                ++report->vadPaddingPreMsFallbacks;
            }
            vadPaddingPreMs = kOutputVadPaddingPreMsDefault;
        }
        if (vadPaddingPostMs < kOutputVadPaddingPostMsMin || vadPaddingPostMs > kOutputVadPaddingPostMsMax)
        {
            if (report != nullptr)
            {
                ++report->vadPaddingPostMsFallbacks;
            }
            vadPaddingPostMs = kOutputVadPaddingPostMsDefault;
        }
        if (transitionRampMs < kOutputTransitionRampMsMin || transitionRampMs > kOutputTransitionRampMsMax)
        {
            if (report != nullptr)
            {
                ++report->transitionRampMsFallbacks;
            }
            transitionRampMs = kOutputTransitionRampMsDefault;
        }
    }
    parsed.vadThresholdDb = vadThresholdDb;
    parsed.vadHysteresisDb = vadHysteresisDb;
    parsed.vadHangoverMs = vadHangoverMs;
    parsed.vadPaddingPreMs = vadPaddingPreMs;
    parsed.vadPaddingPostMs = vadPaddingPostMs;
    parsed.transitionRampMs = transitionRampMs;

    // [SL-472] channels[15] 七项:**缺席时取构造默认且不计回落**(abi≤5 的旧工程「当年没存过」);
    // 在席时逐轨逐项校验,非法 → 该轨该项回落构造默认 + 该字段计一次(与前几档同一族,理由见头注)。
    // `parsed.channels` 由 OutputState 的默认构造给出构造默认,缺席分支因此什么都不用做。
    if (hasChannels)
    {
        const std::uint8_t* rec = data + base + kTailThroughVad;
        for (std::size_t t = 0; t < kOutputChannelCount; ++t, rec += kChannelRecordBytes)
        {
            std::uint32_t enabled = 0;
            std::uint32_t participate = 0;
            std::uint32_t priority = 0;
            std::uint32_t leadLock = 0;
            std::uint32_t leadVolExempt = 0;
            std::uint32_t pairId = 0;
            std::uint32_t labelBytes = 0;
            // 整档长度已在上面判过,这里的 readU32 不会越界;仍按「每次读都给剩余长度」的口径调用,
            // 与本文件其余读点一致(别在这一档破例写成裸指针解引用)。
            if (!readU32(rec, kChannelRecordBytes, enabled) ||
                !readU32(rec + 4, kChannelRecordBytes - 4, participate) ||
                !readU32(rec + 8, kChannelRecordBytes - 8, priority) ||
                !readU32(rec + 12, kChannelRecordBytes - 12, leadLock) ||
                !readU32(rec + 16, kChannelRecordBytes - 16, leadVolExempt) ||
                !readU32(rec + 20, kChannelRecordBytes - 20, pairId) ||
                !readU32(rec + 24, kChannelRecordBytes - 24, labelBytes))
            {
                return false;
            }
            OutputChannelState& c = parsed.channels[t];
            const auto count = [report](std::uint32_t OutputDecodeReport::*field) {
                if (report != nullptr)
                {
                    ++(report->*field);
                }
            };
            if (enabled <= 1u)
            {
                c.enabled = enabled != 0u;
            }
            else
            {
                count(&OutputDecodeReport::channelEnabledFallbacks);
            }
            if (participate <= kOutputParticipateUnset)
            {
                c.participateAutoPan = participate;
            }
            else
            {
                count(&OutputDecodeReport::channelParticipateFallbacks);
            }
            if (priority <= kOutputChannelPriorityMax)
            {
                c.priority = priority;
            }
            else
            {
                count(&OutputDecodeReport::channelPriorityFallbacks);
            }
            if (leadLock <= 1u)
            {
                c.leadLock = leadLock != 0u;
            }
            else
            {
                count(&OutputDecodeReport::channelLeadLockFallbacks);
            }
            if (leadVolExempt <= 1u)
            {
                c.leadVolExempt = leadVolExempt != 0u;
            }
            else
            {
                count(&OutputDecodeReport::channelLeadVolExemptFallbacks);
            }
            if (pairId <= kOutputChannelPairIdMax)
            {
                c.pairId = pairId;
            }
            else
            {
                count(&OutputDecodeReport::channelPairIdFallbacks);
            }
            // labelBytes 先判上限再用于索引(§7.3);槽是定长的,所以坏长度只坏这一格,不会把后面读歪。
            if (labelBytes <= kOutputChannelLabelMaxBytes && labelDecodeValid(rec + 28, labelBytes))
            {
                c.label.assign(reinterpret_cast<const char*>(rec + 28), labelBytes);
            }
            else
            {
                count(&OutputDecodeReport::channelLabelFallbacks);
            }
        }
    }

    if (hasChannels && remaining > kTailThroughChannels)
    {
        // 未知尾部(未来小版本追加字段)保留,编码时原样回写,防静默丢字段。
        parsed.unknownTail.assign(data + base + kTailThroughChannels, data + size);
    }

    out = std::move(parsed);
    return true;
}

bool encodeUiConfig(std::uint32_t masterChartMode, std::vector<std::uint8_t>& out)
{
    out.clear();
    try
    {
        out.reserve(kUiConfigBytes);
    }
    catch (...)
    {
        return false;
    }
    putU32(out, masterChartMode); // [J75] T43 恒写 4 字节(0=distribution | 1=trajectory)
    return true;
}

bool decodeUiConfig(const std::uint8_t* data, std::size_t size, std::uint32_t& out)
{
    out = kMasterChartModeDistribution; // 缺失/非法长度/未知值一律回落默认 distribution
    if (data == nullptr || size != kUiConfigBytes)
    {
        return false; // 长度非法 → 拒载该 chunk(§7.3);调用方回落默认
    }
    std::uint32_t tag = 0;
    if (!readU32(data, size, tag))
    {
        return false;
    }
    out = (tag == kMasterChartModeTrajectory) ? kMasterChartModeTrajectory : kMasterChartModeDistribution;
    return true;
}

} // namespace scvb::state
