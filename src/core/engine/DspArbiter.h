// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "analysis/PanCurve.h"
#include "dsp/ParamSmoother.h"
#include "engine/CurveEvaluator.h"

// DspArbiter:DSP 取值仲裁 + 统一平滑层(03 §2.3 / §2.4)。
// 双源:分析曲线(CurveEvaluator,真身)vs host 参数(raw atomic,打印头)。
// 纯 C++17:只读 const std::atomic<float>* 与 std::shared_ptr<const CurveEvaluator>,绝不写入曲线/参数真身(ADR-005)。
// 平滑:pan 在参数值域、vol 在 dB 域、每轨 width 在其值域;常规 10ms,权威/版本/lead_select/freeze
// 切换 30ms,切换只换目标不重置当前值 → 零跳变。
// 线程契约:来源经「不可变快照 + std::atomic<const Snapshot*>」发布 —— 消息线程构建完整快照后
// release-store;音频线程每 block acquire-load 一次、整 block 用同一份快照(无撕裂)。旧快照由发布方
// 保活(进程寿命),绝不在音频线程可能仍读时释放。
namespace scvb::engine
{

struct DspArbiterConfig
{
    double normalRampSec = 0.010; // §2.4 常规跟随 10ms
    double switchRampSec = 0.030; // §2.4 权威/版本/lead_select/freeze 切换 30ms
};

class DspArbiter
{
public:
    static constexpr int kNumTracks = 15; // [J59]

    static_assert(std::atomic<float>::is_always_lock_free, "DspArbiter requires lock-free std::atomic<float>");

    // 每轨的「活动版本」取值来源。raw* 裸指针由 Output 侧持有(APVTS 生命周期);curve 由
    // std::shared_ptr<const CurveEvaluator> 持有 —— 曲线不可变契约(PR #43 终审):setCurve 注入后
    // 曲线对象必须不可变、生命周期 ≥ 音频线程寿命;快照持 shared_ptr 保活旧曲线,音频线程只经
    // const 解引用采样,零分配、零锁、绝不 write 曲线真身。
    // 值域契约:raw* 全部来自 APVTS getRawParameterValue(),JUCE 8 返回的是**去归一化**的实际单位
    // (ParameterAdapter::unnormalisedValue = convertFrom0to1(getValue())),不是 0..1 归一化值 ——
    // pan ∈ [-100,100]、vol ∈ [-24,12]、width ∈ [0,100]、freeze ∈ [0,3]、lead_select ∈ [0,15]。
    // 切勿在此再 convertFrom0to1(会双重转换),契约由 tests/core/test_authority_params.cpp AUTH-PARAMS-5 锁定。
    struct TrackSources
    {
        const std::atomic<float>* rawPan = nullptr; // host 参数 pan(-100..100,去归一化)
        const std::atomic<float>* rawVol = nullptr; // host 参数 vol(-24..12,去归一化)
        const std::atomic<float>* rawTrkW = nullptr; // 每轨 width(0..100,[J57],去归一化)
        const std::atomic<float>* rawFrz = nullptr; // 每轨 freeze(0..3,[J65],int 存 float,去归一化)
        std::shared_ptr<const CurveEvaluator> curve; // 活动版本曲线真身(null → 恒 0)
    };

    // 不可变来源快照:消息线程完整构建后经 publish 一次性发布;发布后不得再改任何字段。
    struct Snapshot
    {
        std::array<TrackSources, kNumTracks> sources{};
        const std::atomic<float>* rawLeadSelect = nullptr; // lead_select(0..15,去归一化)
        // pan 角度域增益曲线 G 的 LUT(02 §7 / §8.1 步骤 5)。**全曲一份,不是 per-track** ——
        // pan_curve 存在 `CrvsData.versions[]` 上,per-version 而非 per-track。活动版本的那张
        // 在 rebindSources 里塞进来;null → G≡0(等价于点列表为空,PanCurveLut 默认全 0)。
        // 与 TrackSources::curve 同一套不可变契约:LUT 对象发布后绝不原地重建,改曲线 = 新建
        // 一张再发新快照,旧张由快照池保活 —— 音频线程换表那一瞬间读到的必是某张完整的表。
        std::shared_ptr<const scvb::PanCurveLut> panCurveLut;
    };

    struct TrackValues
    {
        float pan = 0.0f; // 参数值域 [-100,100]
        float volDb = 0.0f; // dB 域 [-24,12]
        float width = 100.0f; // 每轨 width [0,100]
    };

    void prepare(double sampleRate, const DspArbiterConfig& cfg = {});

    // 消息线程(或音频停摆期):release-store 新快照。调用前快照须已完整构造;旧快照由发布方保活
    // (进程寿命)。音频线程在 processBlock 开头 acquire-load 一次。
    void publish(const Snapshot* snapshot);

    // 音频线程:每 block 算 raw 目标、检测切换并 arm 平滑。返回 raw(未平滑)目标。
    std::array<TrackValues, kNumTracks> processBlock(bool engineAuthority, double tSec);

    // 音频线程:每样本前进一格平滑器。
    std::array<TrackValues, kNumTracks> nextSample();

    // 音频线程:本 block 生效的 G 查表。processBlock 已从**同一份** acquire-load 的快照里锁定它,
    // 与本块的 TrackValues 同源同块;整块逐样本复用,期间不再触碰原子。
    // 返回 null → G≡0(未接线 / 未发布过快照)。裸指针的存活由快照池的 shared_ptr 兜底(进程寿命)。
    const scvb::PanCurveLut* panCurveLut() const noexcept { return m_panCurveLut; }

    const std::array<TrackValues, kNumTracks>& lastTargets() const { return m_targets; }
    bool lastBlockWasSwitch() const { return m_lastAnySwitch; }

    const scvb::dsp::LinearSmoother& panSmoother(int track) const
    {
        return m_panSmoother[static_cast<std::size_t>(track)];
    }
    const scvb::dsp::LinearSmoother& volSmoother(int track) const
    {
        return m_volSmoother[static_cast<std::size_t>(track)];
    }
    const scvb::dsp::LinearSmoother& widthSmoother(int track) const
    {
        return m_widthSmoother[static_cast<std::size_t>(track)];
    }

private:
    static const std::array<TrackSources, kNumTracks>& emptySources();

    float readRaw(const std::atomic<float>* p) const;
    float readRawDefault(const std::atomic<float>* p, float def) const;
    int readFrz(const TrackSources& src) const;
    int readLeadSelect(const std::atomic<float>* rawLead) const;
    void armSmoother(scvb::dsp::LinearSmoother& s, float target, bool isSwitch);

    DspArbiterConfig m_cfg;
    double m_sampleRate = 0.0;
    std::atomic<const Snapshot*> m_snapshot{nullptr}; // 消息线程写 / 音频线程读

    // 以下全部为音频线程独占状态(仅 processBlock/nextSample 访问,不跨线程):
    const Snapshot* m_prevSnapshot = nullptr; // 上块快照指针(用于检测版本切换)
    const scvb::PanCurveLut* m_panCurveLut = nullptr; // 本块生效的 G 查表(processBlock 锁定)
    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_panSmoother{};
    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_volSmoother{};
    std::array<scvb::dsp::LinearSmoother, kNumTracks> m_widthSmoother{};
    std::array<TrackValues, kNumTracks> m_targets{};
    bool m_initialized = false;
    bool m_prevEngineAuthority = false;
    int m_prevLeadSelect = 0;
    std::array<int, kNumTracks> m_prevFrz{};
    bool m_lastAnySwitch = false;
};

} // namespace scvb::engine
