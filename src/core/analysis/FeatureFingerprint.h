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
//   ① 无基线(tile 未被完整覆盖)→ 不比对、不计入分母、并打断滞回连续段;
//   ② 连续 3 **条**失配上报才把它们计入「失配集合」(滞回;一条匹配上报即断段);
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
    // tile 号只有 16 位(J46 打包上限),所以「已比对」「已失配」两个集合各自是一张定长位图:
    // 65536 位 = 8KB/张,15 轨两张共 240KB 封顶。**不用 unordered_set** —— 那是按元素分配的,
    // 一场 18 小时的连续试听能把它撑到几十 MB,而这两张表只是布尔集合,位图既有上界又更快。
    static constexpr std::uint32_t kTileBitmapWords = (kFpMaxTileIdx + 1u) / 64u; // 1024
    using TileBitmap = std::vector<std::uint64_t>; // 惰性分配:没用过的轨不占内存

    struct ChannelState
    {
        TileBitmap checked; // 已比对过的 tile(去重后的分母)
        TileBitmap mismatched; // 已定谳失配的 tile(分子)
        std::uint32_t checkedCount = 0;
        std::uint32_t mismatchedCount = 0;
        // 滞回②的连续段:计的是**连续几条失配上报**,不是「tile 号连不连号」。
        // 曾按 tile 号邻接判连续,结果在本功能的主场景上失灵 —— 用户改完 EQ 圈一段 2 秒
        // 循环区反复试听:循环回卷触发 startRun、tile 号回到起点,于是永远只在 T、T+1 之间
        // 交替,runLen 顶到 2 就被「不相邻」打回 1,提示永远出不来(全是失配却一条都不定谳)。
        // 改判「连续上报」后:任何一条匹配上报清零(暖机误报仍被挡住,它后面紧跟的就是匹配),
        // 无基线上报也清零(保守:覆盖有洞时不靠拼接得结论)。
        std::uint32_t runLen = 0;
        // 当前连续段里最近 kFpHysteresisTiles 条失配上报的 tile 号(环形覆写)。
        // 达到门槛的那一拍要把整段一并定谳,而这些 tile 号在循环试听下并不连号,存不下就补不回。
        std::uint32_t pending[kFpHysteresisTiles] = {};

        static bool test(const TileBitmap& b, std::uint32_t tile);
        // 置位/清位;返回「本次真的改变了」以维护计数。
        static bool set(TileBitmap& b, std::uint32_t tile);
        static bool clear(TileBitmap& b, std::uint32_t tile);
    };

    ChannelState* stateOf(std::uint32_t channel);
    const ChannelState* stateOf(std::uint32_t channel) const;

    std::vector<ChannelState> channels_;
};

} // namespace scvb::analysis
