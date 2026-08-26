// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FeatureFingerprint —— 上游改动的过期检测(04 §4.5 fingerprint watchdog,[J46])。
//
// 用途:用户在 Input 前面插了 EQ/压缩并改了参数,已采集的特征就不再代表当前素材,但除非
// 重新采集,Output 侧看不出任何异常。本模块给出「素材变了」的判据,只提示、不自动失效、
// 不阻断任何操作(04 §4.5 UI 条)。
//
// 基线零成本:不新增任何持久化字段(params-v0 无 fingerprint 字段),基线随时由 Output 侧
// FrameStore 已存的 kw 值重算 —— 见 baselineTileFingerprint。
//
// tile = 1 秒 = 100 hop(kFeatHopMs=10)。tile 指纹 = FNV-1a 64(该秒 kw 值量化到 0.5dB 的
// 字节序列)。上报载荷经 packFpReport 打成单个 u64(tile_idx 高 16 位 | hash 截断低 48 位)。
//
// 【两端逐位一致的前提】量化的唯一入口是 TileFingerprint::pushKwDbq —— 两端都喂 int16 dBq
// (FrameStore 的量化单位,0.01dB/LSB)。Input 侧的 f32 kw_ms 先经 analysis::quantizeKwDbq
// 落到同一单位,Output 侧直接读 FrameStore 的 kwDbq。同一段音频 ⇒ 同一 dBq ⇒ 同一字节序列。
// 0.5dB 的二次量化是余量:即使两端浮点路径有末位差,也落在同一桶里。

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scvb::analysis
{

class ChannelFrames;

// 一个 tile = 1 秒 = 100 hop @ kFeatHopMs=10(04 §4.5「每 1 秒(100 hop)一个 tile」)。
inline constexpr std::uint32_t kFpTileHops = 100;

// 0.5dB 量化:FrameStore 的 dBq 单位是 0.01dB,÷50 即 0.5dB 一桶。
inline constexpr std::int16_t kFpQuantDivisor = 50;

// tile_idx 上限(J46 打包只有 16 位;tile=1s ⇒ ≈18.2 小时时间线)。超上限的 tile 不上报。
inline constexpr std::uint32_t kFpMaxTileIdx = 0xFFFFu;

// 滞回(04 §4.5「连续 3 秒不匹配才计数」):防暖机/首块滤波差造成的单点误报。
inline constexpr std::uint32_t kFpHysteresisTiles = 3;

// 判 stale 的比例门槛(04 §4.5「>10% tile 不匹配」)。用整数比较避免浮点:
// mismatched * 10 > checked。
inline constexpr std::uint32_t kFpStalePercent = 10;

// FNV-1a 64 常量。
inline constexpr std::uint64_t kFnv1a64Offset = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

// 0.5dB 量化(两端共用的唯一入口)。截断除法在两端是同一份代码,负值行为一致即可。
inline std::int16_t fpQuantizeDbq(std::int16_t kwDbq) noexcept
{
    return static_cast<std::int16_t>(kwDbq / kFpQuantDivisor);
}

// 单个 tile 的流式 FNV-1a 累加器([A] 上零分配、无分支循环)。
class TileFingerprint
{
public:
    void reset() noexcept { h_ = kFnv1a64Offset; }

    // 喂一个 hop 的 kw(单位 0.01dB)。内部先做 0.5dB 量化,再按小端 2 字节入 FNV-1a。
    void pushKwDbq(std::int16_t kwDbq) noexcept
    {
        const std::uint16_t q = static_cast<std::uint16_t>(fpQuantizeDbq(kwDbq));
        h_ = (h_ ^ static_cast<std::uint8_t>(q & 0xFFu)) * kFnv1a64Prime;
        h_ = (h_ ^ static_cast<std::uint8_t>((q >> 8) & 0xFFu)) * kFnv1a64Prime;
    }

    std::uint64_t value() const noexcept { return h_; }

private:
    std::uint64_t h_ = kFnv1a64Offset;
};

// 从 Output 侧已存特征重算某 tile 的基线指纹(04 §4.5「基线可随时由 state 内 kw 重算」)。
// 返回 false = 该 tile 的 100 个 hop 未被 coverage 完整覆盖 ⇒ 无基线,不参与比对
// (刚开工程、没采过的区间不该冒出「数据过期」)。
bool baselineTileFingerprint(const ChannelFrames& frames, std::uint32_t tileIdx, std::uint64_t& out);

// ---------------------------------------------------------------------------
// FingerprintWatch —— Output [M] 侧的比对与判定(消费 kFpReport)。
//
// 判据(04 §4.5):
//   ① 无基线(tile 未被完整覆盖)→ 不比对、不计入分母;
//   ② 连续 3 个相邻 tile 不匹配才把它们计入「失配集合」(滞回);
//   ③ 失配 tile 数 > 已比对 tile 数的 10% → 该轨 stale。
//
// 【v1 口径收窄(实现边界,写进 PR)】设计写的是「某 coverage 区间 >10% tile 不匹配 →
// 该(轨×区间)标 stale」。桥面 §2.8 的 stale 是**轨级**字段,且我们只可能比对到用户实际
// 播放过的 tile。若分母取整条 coverage 的 tile 数,用户播 5 秒改过 EQ 的段落在 5 分钟的
// coverage 里只占 1.7%,永远不会提示 —— 那等于这条功能不存在。故 v1 分母 = **本轨已比对过
// 的 tile 数**(去重),粒度落到轨级。滞回②已经挡掉了暖机误报,②③叠加后最少要连续 3 秒
// 失配才可能触发。
// ---------------------------------------------------------------------------
class FingerprintWatch
{
public:
    explicit FingerprintWatch(std::uint32_t maxChannels = 15) : channels_(maxChannels) {}

    // [M] 收到一条 kFpReport。frames = 该轨的已存特征(基线来源)。
    // channel ∈ [1, maxChannels];越界静默忽略。
    void onReport(std::uint32_t channel, std::uint64_t packedValue, const ChannelFrames& frames);

    // 该轨是否「上游音频与已采集特征不一致」(桥面 §2.8 channels[].stale)。
    bool stale(std::uint32_t channel) const;

    // 诊断/测试:已比对 tile 数 / 已定谳失配 tile 数。
    std::uint32_t tilesChecked(std::uint32_t channel) const;
    std::uint32_t tilesMismatched(std::uint32_t channel) const;

    // 重新采集/改组/整轨作废后清账(与 FrameStore::reset / ChannelFrames::reset 同点调用)。
    void resetChannel(std::uint32_t channel);
    void reset();

private:
    struct ChannelState
    {
        std::unordered_set<std::uint32_t> checked; // 已比对过的 tile(去重后的分母)
        std::unordered_set<std::uint32_t> mismatched; // 已定谳失配的 tile(分子)
        std::uint32_t runLen = 0; // 当前连续失配 tile 的长度(滞回②)
        std::uint32_t lastMismatchTile = 0; // 连续性判定用(相邻 tile 才算「连续」)
        bool hasRun = false;
    };

    ChannelState* stateOf(std::uint32_t channel);
    const ChannelState* stateOf(std::uint32_t channel) const;

    std::vector<ChannelState> channels_;
};

} // namespace scvb::analysis
