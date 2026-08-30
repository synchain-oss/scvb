// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FrameStore —— 特征权威存储(04 §3.1)。按 channel 独立的分页量化存储 + coverage 记账。
// IPC 特征段只是运输(ipc-contract-v0 §3);持久化真身是 Output 侧 FrameStore。分页(4096 hop/页)
// 保证长时间线稀疏覆盖不炸内存。量化即入库:f32 → int16 dB(×100),0.01dB 分辨率。

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "analysis/CoverageMap.h"

namespace scvb::analysis
{

// 静音地板(04 §3.1):kw_ms<=0 → -120dB = -12000。
inline constexpr int16_t kSilenceDbq = -12000;

// 特征页:4096 hop = 40.96s @10ms,按需分配。5B/hop(kw 2 + peak 2 + vadP 1)。
struct FeatPage
{
    static constexpr uint32_t kHops = 4096;
    std::array<int16_t, kHops> kw_dBq{}; // 10*log10(kw_ms)*100
    std::array<int16_t, kHops> peak_dBq{}; // 20*log10(peak)*100
    std::array<uint8_t, kHops> vadP{}; // VAD 后验 0..255(分析时填,可重算)
};

// 量化/反量化(f32 ↔ int16 dB×100;-120dB 为地板)。
int16_t quantizeKwDbq(float kw_ms) noexcept;
int16_t quantizePeakDbq(float peak) noexcept;
float dequantizeKwMs(int16_t dbq) noexcept;
float dequantizePeak(int16_t dbq) noexcept;

// [SL-206/SL-232] VAD 后验量化:0..1 → 0..255。
//
// 口径与**读侧**对齐:`waveformOf` 判 `vadP(h) > 127` 为有声,所以取四舍五入 —— p=0.5 恰好
// 落 128(> 127)算有声,与 EnergyVad 的判决阈同侧。clamp 是**防御性**的、不是补漏:
// EnergyVad 出口已把后验夹到 [0,1];留着只为「量化这一步不依赖上游的值域承诺」。
// 放在这里(而不是调用方)是因为 vadP 归 FrameStore 所有,与上面两个量化器同族。
uint8_t quantizeVadPosterior(float p) noexcept;

// 单 channel 特征存储(04 §3.1 ChannelFrames)。
class ChannelFrames
{
public:
    void setSampleRate(double sr) noexcept { sampleRate_ = sr; }
    double sampleRate() const noexcept { return sampleRate_; }

    // 采集 OFF → 只读:任何写路径丢弃(release 静默,不记账)。
    void setReadOnly(bool ro) noexcept { readOnly_ = ro; }
    bool readOnly() const noexcept { return readOnly_; }

    // 拉取/记账门控(04 §1.1/§4.2):写入口检查 hop ∈ gate。默认全范围(不门控);
    // 布防期由 Output 换读「工作选区」。
    void setGate(HopRange r) noexcept { gate_ = r; }

    // 整轨作废:清页 + 清覆盖记账。用于 Output **改组**(J66)—— frameStore 按 channel 索引存、
    // 没有 group 维度,不清的话新组的 ch3 会继承旧组 ch3 的 CoverageMap,并把两组的特征并进
    // 同一张表。注意与 invalidate(打洞)不同:那是按区间清覆盖、页留着待覆写。
    void reset()
    {
        pages_.clear();
        coverage_.clear();
    }
    HopRange gate() const noexcept { return gate_; }

    const CoverageMap& coverage() const noexcept { return coverage_; }
    CoverageMap& coverage() noexcept { return coverage_; }

    // 写入口:readOnly 或 hop 越 gate → 静默丢弃(不写页、不记账)。
    void write(uint64_t hop, float kw_ms, float peak);

    // [SL-240] 写入时是否**作废这一 hop 的旧 VAD 判决**(见 write() 里那段注释)。
    // 布防重采集期由 Output 置 true:那是用户明说「这段素材要换」,而 §1.23 规定布防
    // **保留既有覆盖**(门控只挡写入),所以 write() 里那条「没覆盖才清」看不见它。
    // 常态 false —— 采集开着又放一遍同一段音频不是换素材,判决没有作废的理由。
    // ⚠ 「换了素材却既没打洞也没布防」那一档:采集 **OFF** 时由指纹守望提示(⚠),
    // 采集 **ON** 时 `fp_report` 被抑制、⚠ 不会亮,旧判决会留到下次重新分析 ——
    // 那是明知的取舍,完整说明见 `write()` 里那段注释,别只读这半句。
    void setVadInvalidateOnWrite(bool on) noexcept { vadInvalidateOnWrite_ = on; }
    bool vadInvalidateOnWrite() const noexcept { return vadInvalidateOnWrite_; }

    // 打洞:coverage_.punch(r)(重采集/清除用;页数据留待后续覆盖)。
    void invalidate(HopRange r) { coverage_.punch(r); }

    // [SL-226] 持久化回灌入口(工程加载:FEAT → FrameStore)。与 write() 的三点不同,每一点都是
    // 必需的,别把它合回 write():
    //   ① 写的是**已量化**的 int16,不再过一遍 f32→dB 量化 —— 落盘存的就是量化值,再量化一次
    //      等于把 0.01dB 的台阶又叠一层,往返对拍会漂;
    //   ② **绕开 readOnly_/gate_**。加载时采集通常是 OFF(readOnly_=true),布防门也还没设,
    //      走 write() 会被这两道门静默吃掉 —— 回灌一个字都进不去,症状与没接持久化完全一样;
    //   ③ **不逐 hop 记账**。覆盖区间在 FEAT 里是显式存着的,由调用方经 addCoverage() 整段并入,
    //      比一个 hop 一次 add 既快也精确(空洞不会被相邻合并抹平)。
    void restoreHop(uint64_t hop, int16_t kwDbq, int16_t peakDbq, uint8_t vad);

    // [SL-226] 整段并入覆盖记账(回灌配套;不变量仍由 CoverageMap 维持)。
    void addCoverage(HopRange r) { coverage_.add(r); }

    // [SL-226] 批量导出一段([begin,end) 追加到三个 out)。逐 hop 调 kwDbq/peakDbq/vadP 是**每个
    // hop 三次 std::map 查找**,满配 20min×15 轨 ≈ 540 万次 —— 而这段跑在 getStateInformation
    // 的 lifecycleMutex_ 临界区里,25Hz tick 正在抢同一把锁。按页取则每 4096 个 hop 才查一次。
    void appendRange(HopRange r, std::vector<int16_t>& kw, std::vector<int16_t>& peak, std::vector<uint8_t>& vad) const;

    bool hasHop(uint64_t hop) const { return coverage_.coversFully(HopRange{hop, hop + 1}); }

    int16_t kwDbq(uint64_t hop) const;
    int16_t peakDbq(uint64_t hop) const;
    uint8_t vadP(uint64_t hop) const;

    // ⚠ [SL-232 起] **生产写侧已统一走 `setVadPosteriorRange`**,本函数生产代码零调用方,
    // 保留**只为给用例塞哨兵值**。别拿它把批量版绕回逐 hop —— 那正是本卡要修掉的形态
    // (每 hop 两次 map 查找、迭代数等于跨度)。
    // ⚠ 也**别拿它写恢复路径**:它与 `setVadPosteriorRange` 一样绕开 `readOnly_`/`gate_`,
    // 且**不记 coverage**;没有覆盖记账,`waveformOf` 那边照样按「未覆盖」回 0 —— 写进去等于白写。
    // 回灌走 `restoreHop()` + `addCoverage()`(那对函数就是为这件事准备的,见其注释)。
    void setVadP(uint64_t hop, uint8_t v);

    // [SL-232] 批量写 VAD 后验(`appendRange` 的写侧镜像,同一个理由)。
    // `posterior[i]` 对应 hop `r.begin + i`,`count` = 可读元素个数;量化在内部做。
    //
    // 为什么带 `count`:同族的 `appendRange` 收的是 `vector&`(长度自证),这里若只收裸指针,
    // 「传宽了」就是一次**静默越界读**。带上长度后 r 会先被夹到 `[r.begin, r.begin+count)`,
    // 传窄了退化成少写几个 hop(no-op 那一段),不再读越界。
    //
    // 为什么非要它:逐 hop 的写法是**每个 hop 两次 std::map 查找**(`hasHop` 的
    // `coversFully` 走一趟 lower_bound,`setVadP` 的 `pageFor` 再 find 一次),而外层
    // 循环次数恒等于 `lastHop - firstHop` —— 与**实际有多少数据**无关。满选 1h × 15 轨
    // ≈ 1080 万次查找,还全程持 lifecycleMutex_ 跑在消息线程上,与 waveformOf 注释里
    // 记的那次 P0-A 冻死同一族(「迭代数与实际数据量无关」)。
    // 这里改成:先经 coverage_.intersect(r) 只取**真被覆盖**的子区间,再按页推进,
    // **每 4096 个 hop 才查一次索引** —— 迭代数与实际数据量同阶。
    // ⚠ 子区间数**没有硬上限**:`CoverageMap` 只保证有序 + 相邻合并,不保证条数。
    // 「几十」是现场经验量级(= 连续覆盖段数),**不是不变量**,别据此做更强假设。
    // 复杂度结论不受影响:O(重叠区间数 + 实际覆盖 hop 数),与**跨度**无关。
    //
    // 语义与逐 hop 版逐字相同:**未覆盖的 hop 不写**。后验在那里恒 0,写进去只会把 20KB 的
    // FeatPage 白建出来(`setVadP` 走 `pageFor(create=true)`,既不看 readOnly_ 也不看 gate_),
    // 正好抵消 FrameStore 的分页稀疏性;而读侧 waveformOf 本来就只遍历覆盖区。
    void setVadPosteriorRange(HopRange r, const float* posterior, std::size_t count);
    float kwMs(uint64_t hop) const; // 反量化(供分析流水线)
    float peak(uint64_t hop) const;

    uint64_t coveredHops(HopRange r) const { return coverage_.coveredHops(r); }
    bool coversFully(HopRange r) const { return coverage_.coversFully(r); }

    std::size_t pageCount() const noexcept { return pages_.size(); }
    std::size_t allocatedBytes() const noexcept { return pages_.size() * sizeof(FeatPage); }

private:
    FeatPage* pageFor(uint64_t hop, bool create);
    const FeatPage* pageForRead(uint64_t hop) const;

    double sampleRate_ = 48000.0;
    std::map<uint64_t, std::unique_ptr<FeatPage>> pages_;
    CoverageMap coverage_;
    bool readOnly_ = true; // 采集 OFF/未布防默认只读:Output 布防路径必须先 setReadOnly(false) 才能记账
    bool vadInvalidateOnWrite_ = false; // [SL-240] 见 setVadInvalidateOnWrite()
    HopRange gate_{0, std::numeric_limits<uint64_t>::max()};
};

// 全轨道 FrameStore 容器(04 §3.3 frameStore[ch];ch 为 1..maxChannels)。
class FrameStore
{
public:
    explicit FrameStore(uint32_t maxChannels = 15) : channels_(maxChannels) {}

    ChannelFrames& channel(uint32_t ch);
    const ChannelFrames& channel(uint32_t ch) const;
    uint32_t maxChannels() const noexcept { return static_cast<uint32_t>(channels_.size()); }

    // 全轨作废(改组路径;逐轨 ChannelFrames::reset)。
    void reset();

    std::size_t totalPageCount() const noexcept;
    std::size_t totalAllocatedBytes() const noexcept;

private:
    std::vector<ChannelFrames> channels_;
    ChannelFrames invalidChannel_; // 越界 channel 的只读哨兵:写被丢弃,不污染任何真实轨
};

} // namespace scvb::analysis
