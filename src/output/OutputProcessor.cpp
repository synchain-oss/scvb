// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BridgeBase.h" // Min/MaxUiScale(缩放档位边界的单一真源,§1.28)
#include "OutputUiState.h"
#include "SegmentEditService.h"
#include "UiDefaultsStore.h"
#include "ipc/RegistryProbe.h"
#include "output/MixMath.h"
#include "state/StateMigration.h"

namespace
{
constexpr int kGroupIdMax = 8; // [J66] 1..8
constexpr int kVersionMax = 2; // [J59] 1..2
constexpr int kTimelineInvalidTicks = 12; // 25Hz × 0.5s(§4.2 连续无效判定)
} // namespace

ScvbOutputAudioProcessor::ScvbOutputAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      session_(backend_, static_cast<scvb::u32>(::GetCurrentProcessId())),
      vizPublisher_(backend_, 1u), // [T44] 组号随 setGroupId/prepareToPlay 校正
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    setLatencySamples(0); // ADR-002:Output 不报额外 latency(对齐靠时间线,不靠 PDC)

    // 缩放的系统级全局默认(§1.29「保持」落盘的那一档);工程 state 若带 CFGS 会在
    // setStateInformation 里覆盖它 —— 工程 > 全局默认。0 = 从未「保持」过,沿用 100。
    // 构造期读一次本地小文件(几百字节 XML)是刻意的:此值必须在宿主取首个编辑器尺寸
    // 之前就位,而构造发生在宿主的加载线程、不在音频线程,毛刺风险已评估为可接受。
    if (const int defaultScale = scvb::uidefaults::uiScalePercent(); defaultScale > 0)
    {
        uiScale_ = defaultScale;
    }
    // 语言的系统级全局默认(§1.30 setLang 落盘的那一个);同样是「工程 > 全局默认」——
    // 带 CFGS 的工程会在 setStateInformation 里覆盖回去。新加载的实例靠这一行拿回用户选过的
    // 语言,否则「选过」的全局位只会把语言卡挡掉、界面照样是英文(v5 实测 P1-6)。
    if (const juce::String defaultLang = scvb::uidefaults::langGlobal(); defaultLang.isNotEmpty())
    {
        uiLanguage_ = defaultLang;
    }

    handles_ = scvb::params::collectParamHandles(apvts);
    printer_.setShot(&playheadShot_);
    printer_.installHostEchoShield(apvts);

    // CRVS 段真身默认版本名([J05]:空值回落 V{n};加载 CRVS 时会被覆盖)。
    crvsData_.versions[0].meta.name = "V1";
    crvsData_.versions[1].meta.name = "V2";
}

ScvbOutputAudioProcessor::~ScvbOutputAudioProcessor()
{
    stopTimer();
    // 顺序不可倒:先 cancelAnalysis() 把工作线程 signal + join 掉 —— join 返回即保证 run() 已经
    // 结束,此后不会再有人调 triggerAsyncUpdate();再 cancelPendingUpdate() 撤掉可能已经入队的
    // 那一次派发。两步做完,消息队列里不可能再有指向本对象的回调。
    //
    // 反例(修复前):run() 末尾裸 callAsync 捕获 &owner_,而析构不取消分析 —— 用户在分析跑着时
    // 把插件从轨上删掉,消息一旦入队就一定会被派发,于是 finishAnalysis 打在已析构对象上
    // (里面还要取 lifecycleMutex_、写 crvsData_)= 宿主崩溃。
    cancelAnalysis();
    // cancelAnalysis() 自 v5.3 起**只 signal 不 join**(消息线程不能被 join 堵住,见
    // retiredJobs_ 头注)。析构是唯一必须真的等到线程停的地方 —— 上面那段「join 返回即保证
    // run() 已结束」的推理**全靠这一步**,少了它就退回 R5 那个 use-after-free。
    // 这里阻塞是对的:拆机时刻,而且 run() 看到 threadShouldExit 会很快返回。
    joinRetiredJobs();
    cancelPendingUpdate();

    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
}

juce::AudioProcessorValueTreeState::ParameterLayout ScvbOutputAudioProcessor::createParameterLayout()
{
    // T15:123 参数冻结布局(params-v0 v2.2 §一 / J59 / J65)。
    return scvb::params::makeOutputLayout();
}

void ScvbOutputAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    sampleRate_.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_relaxed); // 原子写(PR#55 第9轮)
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    preparedMaxBlock_ = samplesPerBlock > 0 ? samplesPerBlock : 512;
    accumL_.assign(static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    accumR_.assign(static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    for (auto& tb : trackBuf_)
    {
        tb.assign(static_cast<std::size_t>(2) * static_cast<std::size_t>(preparedMaxBlock_), 0.0f);
    }

    handles_ = scvb::params::collectParamHandles(apvts);

    const auto now = scvb::steadyNowMs();
    session_.prepare(static_cast<scvb::u32>(sr), static_cast<scvb::u32>(preparedMaxBlock_), now);
    authority_.prepare(sr, handles_);
    busXfade_.prepare(sr);
    busXfade_.resetToPassthrough();

    // 平滑器:ms_balance g_M/g_S 与全局 width 10ms;per-channel 注入 fade 80ms(§5.2 过渡语义)。
    gMSmoother_.reset(sr, 0.010);
    gSSmoother_.reset(sr, 0.010);
    globalWidthSmoother_.reset(sr, 0.010);
    for (auto& f : channelFade_)
    {
        f.reset(sr, 0.080);
    }

    // 打印器接线(C8 setShot 已在构造完成;此处重绑车道 + 启打印 Timer,见 startPrinting)。
    rebindVersion();
    printer_.startPrinting();

    lastT0Out_ = std::numeric_limits<int64_t>::lowest();
    expectedNextOut_ = std::numeric_limits<int64_t>::lowest();
    prepared_ = true;

    // [T44/J75] viz 段**不在这里建**。建/释放一律交给 [M] 每拍按 claim 态裁决(syncVizSegment)——
    // 理由见 R1:同组第二个 Output 是 kObserver([J66]「同组内只读观察」),它若也建段并 4Hz 写,
    // 两个写方会同时推同一个 seqlock,读方拿到的帧可以是两次发布的拼接(seq 偶数、内容却撕裂),
    // 而且**看起来完全正常**。此处只把段指向当前组。
    vizPublisher_.setGroup(static_cast<scvb::u32>(groupId_));

    startTimerHz(25);
}

void ScvbOutputAudioProcessor::releaseResources()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    session_.release(scvb::steadyNowMs());
    vizPublisher_.release(); // [T44] viz 段与主链路同生命周期
    printer_.endAllGestures();
    prepared_ = false;
    sampleRate_.store(
        0.0, std::memory_order_relaxed); // 复位:isPrepared()/sr 守卫在 release 后回到「未 prepare」(PR#55 第10轮缺陷1)
}

void ScvbOutputAudioProcessor::rebindVersion()
{
    authority_.setVersionActive(versionActive_);
    rebuildAllCurves(); // 由 CRVS 段真身重建曲线 + 打印器重取活动版本曲线(T29)
    printer_.bindVersion(versionActive_, handles_);
}

bool ScvbOutputAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // ADR-003:Output 总线固定 stereo(读 15 环求和后替换总线)。
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() &&
           layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

float ScvbOutputAudioProcessor::readGlobalWidth() const noexcept
{
    return handles_.rawWidth != nullptr ? handles_.rawWidth->load(std::memory_order_relaxed) : 100.0f;
}

float ScvbOutputAudioProcessor::readMsBalance() const noexcept
{
    return handles_.rawMsBalance != nullptr ? handles_.rawMsBalance->load(std::memory_order_relaxed) : 0.0f;
}

void ScvbOutputAudioProcessor::publishPlayhead(const juce::AudioPlayHead::PositionInfo& pos, bool haveTime,
                                               bool playing)
{
    scvb::engine::PlayheadPod pod;
    pod.sampleRate = sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    pod.epoch = podEpoch_;
    pod.timeSamples = haveTime ? *pos.getTimeInSamples() : -1;
    if (playing)
    {
        pod.flags |= scvb::engine::kPlayheadIsPlaying;
    }
    if (pos.getIsLooping())
    {
        pod.flags |= scvb::engine::kPlayheadIsLooping;
    }
    if (const auto ppq = pos.getPpqPosition(); ppq.hasValue())
    {
        pod.ppq = *ppq;
        pod.flags |= scvb::engine::kPlayheadMusicValid;
    }
    if (const auto bpm = pos.getBpm(); bpm.hasValue())
    {
        pod.bpm = *bpm;
        pod.flags |= scvb::engine::kPlayheadTempoValid;
    }
    if (const auto loop = pos.getLoopPoints(); loop.hasValue())
    {
        pod.loopStartPpq = loop->ppqStart;
        pod.loopEndPpq = loop->ppqEnd;
        pod.flags |= scvb::engine::kPlayheadCycleValid;
    }
    playheadShot_.publish(pod);
}

void ScvbOutputAudioProcessor::publishSilentMeters() noexcept
{
    // 直通/观察/无注入轨:没有「本轨电平」这回事 —— 发全零,液柱落回地板。
    // 不发的话 seqlock 会停在上一块的值,电平表冻在最后一次有声的高度。
    meterShot_.publish(scvb::output::MeterPod{});
}

void ScvbOutputAudioProcessor::publishMeters(const std::array<bool, 15>& hasData, const std::array<scvb::u32, 15>& nch,
                                             const std::array<float, 15>& trackGain, const float* busL,
                                             const float* busR, int n) noexcept
{
    // [A] 零分配零锁:pod 是栈上 POD,只做乘加/取绝对值/一次 sqrt,不碰 log10(留给 [M])。
    scvb::output::MeterPod pod;

    for (int ch = 0; ch < 15; ++ch)
    {
        const std::size_t idx = static_cast<std::size_t>(ch);
        if (!hasData[idx])
        {
            continue; // 该轨本块无可读数据 → 保持 0(地板)
        }
        const float* buf = trackBuf_[idx].data();
        const int stride = nch[idx] == 2 ? 2 : 1;
        const int count = n * stride;

        float peak = 0.0f;
        float sumSq = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const float s = buf[i];
            const float a = std::fabs(s);
            if (a > peak)
            {
                peak = a;
            }
            sumSq += s * s;
        }

        const float g = trackGain[idx];
        pod.trackPeak[idx] = peak * g;
        pod.trackRms[idx] = std::sqrt(sumSq / static_cast<float>(count)) * g;
    }

    if (busL != nullptr && busR != nullptr)
    {
        const float* bus[2] = {busL, busR};
        for (int c = 0; c < 2; ++c)
        {
            float peak = 0.0f;
            float sumSq = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float s = bus[c][i];
                const float a = std::fabs(s);
                if (a > peak)
                {
                    peak = a;
                }
                sumSq += s * s;
            }
            pod.busPeak[c] = peak;
            pod.busRms[c] = std::sqrt(sumSq / static_cast<float>(n));
        }
    }

    meterShot_.publish(pod);
}

double ScvbOutputAudioProcessor::featHopSeconds()
{
    return static_cast<double>(scvb::output::OutputSession::featHopMs()) / 1000.0;
}

ScvbOutputAudioProcessor::CoverageInfo ScvbOutputAudioProcessor::coverageOf(int channel, double startS, double endS)
{
    CoverageInfo info;
    if (channel < 1 || channel > 15 || !(endS > startS))
    {
        return info;
    }

    const juce::ScopedLock lock(lifecycleMutex_);

    // 秒 → hop:hop 时长是 feat 段的几何常量(kFeatHopMs=10ms),不是采样率派生量。
    const double hopS = featHopSeconds();
    const auto toHop = [hopS](double s) {
        const double h = s / hopS;
        return h <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(h);
    };
    const scvb::analysis::HopRange range{toHop(startS), toHop(endS)};
    if (range.end <= range.begin)
    {
        return info;
    }

    const auto& frames = session_.frameStore().channel(static_cast<scvb::u32>(channel));
    const std::uint64_t covered = frames.coveredHops(range);
    const std::uint64_t total = range.end - range.begin;

    info.pct = static_cast<float>(100.0 * static_cast<double>(covered) / static_cast<double>(total));
    info.coveredS = static_cast<double>(covered) * hopS;
    info.ranges = frames.coverage().intersect(range);
    return info;
}

ScvbOutputAudioProcessor::WaveformTile ScvbOutputAudioProcessor::waveformOf(int channel, double startS, double endS,
                                                                            int cols)
{
    WaveformTile tile;
    if (cols < 1)
    {
        return tile;
    }
    // 先按「整列未覆盖」铺满哨兵:任何提前返回都得是一张形状合法的瓦片,
    // 否则 §1.27 的回包过不了 JS 侧 isTileShape,泳道会整块 clearRect 成纯黑。
    tile.minDb.assign(static_cast<std::size_t>(cols), -160.0);
    tile.maxDb.assign(static_cast<std::size_t>(cols), -160.0);
    tile.vad.assign(static_cast<std::size_t>(cols), 0);
    tile.covered.assign(static_cast<std::size_t>(cols), 0);
    if (channel < 1 || channel > 15 || !(endS > startS))
    {
        return tile;
    }

    const juce::ScopedLock lock(lifecycleMutex_);

    const double hopS = featHopSeconds();
    const auto& frames = session_.frameStore().channel(static_cast<scvb::u32>(channel));
    const double colS = (endS - startS) / static_cast<double>(cols);

    for (int i = 0; i < cols; ++i)
    {
        const double c0 = startS + colS * static_cast<double>(i);
        const double c1 = c0 + colS;
        const auto toHop = [hopS](double s) {
            const double h = s / hopS;
            return h <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(h);
        };
        std::uint64_t h0 = toHop(c0);
        // 列窄于一个 hop(放大到 10ms 以下)时 h1==h0,整列会判成空 —— 至少取一个 hop,
        // 否则越放大波形越消失。
        std::uint64_t h1 = std::max(toHop(c1), h0 + 1);

        double mx = -160.0;
        double mn = 160.0;
        bool any = false;
        bool voiced = false;

        // **先与覆盖区求交,再对残余限步长** —— 这两道是 P0-A 卡死的止血点。
        //
        // 原写法是 `for (h = h0; h < h1; ++h) if (!hasHop(h)) continue;`:内外两层加起来的
        // 迭代总数恒等于 **(endS−startS)/hop,与 cols 无关**。真机转储里前端把一个被污染的
        // 工程时长(2^40 采样 ÷ 48k = 22,906,492 秒)当 endS 发进来,单次调用要空转
        // **22.9 亿次**、每次还调一趟 coversFully 做 lower_bound,而全程持 lifecycleMutex_
        // 且跑在 WebView2 的 web-message 回调里 —— 宿主消息泵被占住,UI 整体冻死。
        // 那次现场里真正有数据的只有 2475 个 hop,**99.99989% 的迭代是纯空转**。
        //
        // 求交之后代价与「实际有多少数据」同阶;再按 kMaxHopsPerCol 限步长,单列代价封顶。
        // 概览块(512 列跨全曲)本来就是缩略图,抽样对观感无损。
        const auto covered = frames.coverage().intersect(scvb::analysis::HopRange{h0, h1});
        std::uint64_t coveredHops = 0;
        for (const auto& cr : covered)
        {
            coveredHops += cr.end - cr.begin;
        }
        const std::uint64_t stride =
            coveredHops > kMaxHopsPerCol ? (coveredHops + kMaxHopsPerCol - 1) / kMaxHopsPerCol : 1;
        for (const auto& cr : covered)
        {
            // 求交的产物按定义就是「有数据的 hop」,不必再 hasHop 一次(那正是原实现里
            // 每次空转都要付的那笔 lower_bound)。
            for (std::uint64_t h = cr.begin; h < cr.end; h += stride)
            {
                any = true;
                mx = std::max(mx, static_cast<double>(frames.peakDbq(h)) / 100.0);
                mn = std::min(mn, static_cast<double>(frames.kwDbq(h)) / 100.0);
                if (frames.vadP(h) > 127)
                {
                    voiced = true;
                }
            }
        }
        if (!any)
        {
            continue; // 保持 covered=0 + 哨兵:泳道画斜纹
        }
        const auto k = static_cast<std::size_t>(i);
        tile.covered[k] = 1;
        tile.maxDb[k] = mx;
        // 包络下沿不得高过上沿(全静音列两者都会压在地板上)。
        tile.minDb[k] = std::min(mn, mx);
        tile.vad[k] = voiced ? 1 : 0;
    }
    return tile;
}

double ScvbOutputAudioProcessor::capturedExtentSeconds()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    const double hopS = featHopSeconds();
    std::uint64_t lastHop = 0;
    for (int t = 0; t < 15; ++t)
    {
        const auto& frames = session_.frameStore().channel(static_cast<scvb::u32>(t + 1));
        for (const auto& r : frames.coverage().ranges())
        {
            lastHop = std::max(lastHop, r.end);
        }
    }
    return static_cast<double>(lastHop) * hopS;
}

double ScvbOutputAudioProcessor::clearCoverage(std::uint16_t tracksMask, double startS, double endS)
{
    if (tracksMask == 0 || !(endS > startS))
    {
        return 0.0;
    }

    const juce::ScopedLock lock(lifecycleMutex_);

    const double hopS = featHopSeconds();
    const auto toHop = [hopS](double s) {
        const double h = s / hopS;
        return h <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(h);
    };
    const scvb::analysis::HopRange range{toHop(startS), toHop(endS)};
    if (range.end <= range.begin)
    {
        return 0.0;
    }

    double clearedS = 0.0;
    for (int t = 0; t < 15; ++t)
    {
        if ((tracksMask & (1u << t)) == 0)
        {
            continue;
        }
        auto& frames = session_.frameStore().channel(static_cast<scvb::u32>(t + 1));
        // 先量出实际会被清掉的量,再打洞 —— 打完就问不出来了。
        clearedS += static_cast<double>(frames.coveredHops(range)) * hopS;
        frames.invalidate(range);
        // 打洞即作废基线(04 §4.5):被清掉的那些 tile 从此没有基线可比,而**已定谳的失配**
        // 也不该活过一次「用户主动清除并准备重采」的动作 —— 否则重采完了 ⚠ 还挂着,
        // 用户只能靠重开工程把它甩掉。
        session_.resetChannelStale(static_cast<scvb::u32>(t + 1));
    }
    return clearedS;
}

scvb::output::MeterPod ScvbOutputAudioProcessor::meterSnapshot() const
{
    scvb::output::MeterPod pod{};
    // 撕裂读返回全零 pod(= 静音一帧),下一 25Hz tick 重读自愈 —— 与 playheadSnapshot 同口径。
    meterShot_.read(pod);
    return pod;
}

void ScvbOutputAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;

    // 越界夹取(research/01 §2.3,Bridge #169 教训)。
    const int n = juce::jmin(buffer.getNumSamples(), preparedMaxBlock_);
    if (n <= 0)
    {
        return;
    }

    session_.bumpBlockCounter(); // [J52] 存活计数 → [M] 停摆看门狗(§4.2)

    // 时间线定位(§5.2 步骤 2)。
    bool playing = false;
    bool haveT0 = false;
    int64_t t0 = 0;
    juce::AudioPlayHead::PositionInfo pos = juce::AudioPlayHead::PositionInfo{};
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const juce::Optional<juce::AudioPlayHead::PositionInfo> p = ph->getPosition();
        if (p.hasValue())
        {
            pos = *p;
            playing = pos.getIsPlaying();
            if (const juce::Optional<int64_t> ts = pos.getTimeInSamples(); ts.hasValue())
            {
                t0 = *ts;
                haveT0 = true;
            }
        }
    }

    // pod epoch(§5.2 步骤 2;停走带静止 t0 不变不算跳变)。
    if (haveT0 && t0 >= 0)
    {
        if (t0 != expectedNextOut_ && (playing || t0 != lastT0Out_))
        {
            ++podEpoch_;
        }
        lastT0Out_ = t0;
        expectedNextOut_ = t0 + n;
    }
    publishPlayhead(pos, haveT0, playing);

    const bool noTimeline = !haveT0;
    const bool negT0 = haveT0 && t0 < 0;
    const bool observer = session_.state() != scvb::output::OutputClaimState::kActive;

    if (observer || noTimeline || negT0)
    {
        // 只读/无时间线/负 t0:总线直通(不混音不读环)。
        if (noTimeline)
        {
            timelineInvalidBlocks_.fetch_add(1, std::memory_order_relaxed); // 仅「无时间线」计入(§4.2)
            timelineValid_.store(0, std::memory_order_relaxed);
        }
        else
        {
            timelineInvalidBlocks_.store(0, std::memory_order_relaxed);
            timelineValid_.store(1, std::memory_order_relaxed); // 负 t0 是有效时间线([J51])
        }
        const float* const* in = buffer.getArrayOfReadPointers();
        float* const* out = buffer.getArrayOfWritePointers();
        const float* lastMix[2] = {accumL_.data(), accumR_.data()};
        busXfade_.render(out, in, lastMix, n, /*targetMix=*/false);
        publishSilentMeters();
        return;
    }

    timelineInvalidBlocks_.store(0, std::memory_order_relaxed);
    timelineValid_.store(1, std::memory_order_relaxed);

    // 与轨启用位求交:关掉的轨整轨不进混音(§1.15;DAW_COMPATIBILITY §「设备停用」把面板轨道
    // 开关定为 A/B 的推荐手段 —— 那就必须真的听得出来)。
    const scvb::u32 inject = session_.injectMask() & enabledMask_.load(std::memory_order_acquire);
    if (inject == 0)
    {
        // 无注入轨(全部离线/注入延迟中/全被关掉):总线直通,经同一 busXfade 状态机(无硬切)。
        const float* const* in = buffer.getArrayOfReadPointers();
        float* const* out = buffer.getArrayOfWritePointers();
        const float* lastMix[2] = {accumL_.data(), accumR_.data()};
        busXfade_.render(out, in, lastMix, n, /*targetMix=*/false);
        publishSilentMeters();
        return;
    }

    // 仲裁 + arm 平滑(整 block 用同一份快照;engineAuthority = output_enabled)。
    const double tSec = static_cast<double>(t0) / sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    const auto blockTargets = authority_.processBlock(session_.outputEnabled(), tSec);

    // 电平表用的每轨线性增益:取本块起点的仲裁目标(逐样本平滑的差异在电平表上不可见)。
    std::array<float, 15> meterGain{};
    for (int ch = 0; ch < 15; ++ch)
    {
        meterGain[static_cast<std::size_t>(ch)] =
            scvb::output::dbToLinear(blockTargets[static_cast<std::size_t>(ch)].volDb);
    }

    // 读注入 channel 的环(covered/换代/套圈判定在 ShmRingMixSource::read)。
    std::array<bool, 15> hasData{};
    std::array<scvb::u32, 15> nch{};
    for (int ch = 1; ch <= 15; ++ch)
    {
        if ((inject & (1u << (ch - 1))) == 0)
        {
            continue;
        }
        auto& src = session_.mixSource(static_cast<scvb::u32>(ch));
        if (!src.bound())
        {
            continue;
        }
        if (src.read(t0, trackBuf_[static_cast<std::size_t>(ch - 1)].data(), n))
        {
            hasData[static_cast<std::size_t>(ch - 1)] = true;
            nch[static_cast<std::size_t>(ch - 1)] = src.channels();
        }
    }

    // arm 平滑器:per-channel 注入 fade(上线/下线 80ms)+ 全局 width + ms_balance g_M/g_S(10ms)。
    for (int ch = 0; ch < 15; ++ch)
    {
        channelFade_[static_cast<std::size_t>(ch)].setTargetValue((inject & (1u << ch)) != 0 ? 1.0f : 0.0f);
    }
    globalWidthSmoother_.setTargetValue(readGlobalWidth());
    const float ms = readMsBalance();
    const float tms = ms / 100.0f;
    gMSmoother_.setTargetValue(1.0f - std::max(tms, 0.0f));
    gSSmoother_.setTargetValue(1.0f - std::max(-tms, 0.0f));

    // 逐样本:取值(平滑)→ gain/pan(mono equal-power / stereo dual-pan+width)→ 求和 → ms_balance。
    for (int i = 0; i < n; ++i)
    {
        const auto tv = authority_.nextSample();
        const float gw = globalWidthSmoother_.getNextValue();
        const float gM = gMSmoother_.getNextValue();
        const float gS = gSSmoother_.getNextValue();

        float l = 0.0f;
        float r = 0.0f;
        for (int ch = 0; ch < 15; ++ch)
        {
            const float fade = channelFade_[static_cast<std::size_t>(ch)].getNextValue();
            if (!hasData[static_cast<std::size_t>(ch)])
            {
                continue;
            }
            const auto& t = tv[static_cast<std::size_t>(ch)];
            if (nch[static_cast<std::size_t>(ch)] == 1)
            {
                scvb::output::mixMonoSample(trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)], t.pan,
                                            t.volDb, gw, fade, l, r);
            }
            else
            {
                scvb::output::mixStereoSample(
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i)],
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i + 1)], t.pan, t.volDb,
                    t.width, gw, fade, l, r);
            }
        }
        scvb::output::applyMsGains(gM, gS, l, r); // 总线级 M/S 音量比(求和之后、替换之前,[J58])
        accumL_[static_cast<std::size_t>(i)] = l;
        accumR_[static_cast<std::size_t>(i)] = r;
    }

    // 替换总线(ADR-002):稳态=完全替换;进出瞬间经 busXfade 等功率交叉(§5.2 过渡语义)。
    const float* const* in = buffer.getArrayOfReadPointers();
    float* const* out = buffer.getArrayOfWritePointers();
    const float* mix[2] = {accumL_.data(), accumR_.data()};
    busXfade_.render(out, in, mix, n, /*targetMix=*/true);

    // 电平发布(§2.5):轨道取 post-gain/pre-pan,总线取求和后的 accum。
    publishMeters(hasData, nch, meterGain, accumL_.data(), accumR_.data(), n);
}

void ScvbOutputAudioProcessor::renderBypassedUnity(juce::AudioBuffer<float>& buffer, int n, int64_t t0)
{
    // §5.4:按时间线读 15 环做 unity 求和(mono 居中复制到 L/R、stereo L→L/R→R;不 gain/pan/width)。
    // PR#53 I1:无注入轨或无可读源 → 直通(不改 buffer),绝不把总线替换成静音。
    // PR#53 复审:早退发生在清零之前 —— 无数据时不清 accum,保留 lastMix 供后续「混音→直通」等功率交叉。
    const scvb::u32 inject = session_.injectMask() & enabledMask_.load(std::memory_order_acquire);
    if (inject == 0)
    {
        bypassHasData_.fill(false); // 早退也要清:否则收尾的 publishMeters 会拿上一块的 hasData
        return; // 总线上只挂 Output / 尚无 Input / 全被关掉:直通
    }

    // 第一遍:读注入源到 trackBuf_,只判定「是否有可读数据」,不触碰 accum。
    // hasData/srcCh 落成员:processBlockBypassed 收尾时要拿它们上报电平(I5)。
    std::array<bool, 15>& hasData = bypassHasData_;
    std::array<scvb::u32, 15>& srcCh = bypassChannels_;
    hasData.fill(false);
    srcCh.fill(0);
    bool anyData = false;
    for (int ch = 1; ch <= 15; ++ch)
    {
        if ((inject & (1u << (ch - 1))) == 0)
        {
            continue;
        }
        auto& src = session_.mixSource(static_cast<scvb::u32>(ch));
        if (!src.bound())
        {
            continue;
        }
        if (!src.read(t0, trackBuf_[static_cast<std::size_t>(ch - 1)].data(), n))
        {
            continue; // 缺口 → 该轨该块静音(失准计数已在 read 内)
        }
        hasData[static_cast<std::size_t>(ch - 1)] = true;
        srcCh[static_cast<std::size_t>(ch - 1)] = src.channels();
        anyData = true;
    }

    if (!anyData)
    {
        return; // 有注入轨但均无可读数据(缺口):直通,不清 accum(保留 lastMix)
    }

    // 确认有数据才清零并求和。
    std::fill(accumL_.begin(), accumL_.begin() + n, 0.0f);
    std::fill(accumR_.begin(), accumR_.begin() + n, 0.0f);
    for (int ch = 0; ch < 15; ++ch)
    {
        if (!hasData[static_cast<std::size_t>(ch)])
        {
            continue;
        }
        const scvb::u32 chCount = srcCh[static_cast<std::size_t>(ch)];
        for (int i = 0; i < n; ++i)
        {
            if (chCount == 1)
            {
                const float s = trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)];
                accumL_[static_cast<std::size_t>(i)] += s;
                accumR_[static_cast<std::size_t>(i)] += s;
            }
            else
            {
                accumL_[static_cast<std::size_t>(i)] +=
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i)];
                accumR_[static_cast<std::size_t>(i)] +=
                    trackBuf_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(2 * i + 1)];
            }
        }
    }

    auto* outL = buffer.getWritePointer(0);
    auto* outR = buffer.getWritePointer(1);
    for (int i = 0; i < n; ++i)
    {
        outL[i] = accumL_[static_cast<std::size_t>(i)];
        outR[i] = accumR_[static_cast<std::size_t>(i)];
    }
}

void ScvbOutputAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;
    const int n = juce::jmin(buffer.getNumSamples(), preparedMaxBlock_);
    if (n <= 0)
    {
        return;
    }
    session_.bumpBlockCounter(); // [J52] 存活计数 → [M] 停摆看门狗(§4.2);bypass 期间同样推进

    int64_t t0 = 0;
    bool haveT0 = false;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        const juce::Optional<juce::AudioPlayHead::PositionInfo> p = ph->getPosition();
        if (p.hasValue())
        {
            if (const juce::Optional<int64_t> ts = p->getTimeInSamples(); ts.hasValue() && *ts >= 0)
            {
                t0 = *ts;
                haveT0 = true;
            }
        }
    }
    if (!haveT0)
    {
        publishSilentMeters(); // 与 processBlock 三条早退同口径:不发就冻在最后一次有声的高度
        return; // 无时间线:直通(不替换)
    }
    renderBypassedUnity(buffer, n, t0);
    // 宿主 bypass 期间同样要更新电平表 —— 否则液柱冻在 bypass 前那一刻。
    // renderBypassedUnity 是 unity 求和(不 gain/pan/width),故每轨增益按 1.0 报。
    std::array<float, 15> unityGain{};
    unityGain.fill(1.0f);
    publishMeters(bypassHasData_, bypassChannels_, unityGain, accumL_.data(), accumR_.data(), n);
}

void ScvbOutputAudioProcessor::timerCallback()
{
    const auto now = scvb::steadyNowMs();
    const juce::ScopedLock lock(lifecycleMutex_);

    // 4Hz 心跳(250ms 折半在 25Hz 定时器内)。
    if (now - lastHeartbeatMs_ >= scvb::kHeartbeatIntervalMs)
    {
        lastHeartbeatMs_ = now;
        session_.heartbeat(now);
    }

    // 停流判定要知道走带在不在跑:走带停住时所有 Input 的写头本来就该冻着,那不是故障。
    session_.setTransportPlaying((playheadSnapshot().flags & scvb::engine::kPlayheadIsPlaying) != 0);
    session_.tick(now);
    reapRetiredJobs(); // 退休分析作业的非阻塞回收(见 retiredJobs_ 头注)

    // 时间线健康前置(§4.2 [J51]):连续无时间线 ≥0.5s → 清 mask(Inputs 走 J12 直通)。
    if (timelineValid_.load(std::memory_order_relaxed) == 0)
    {
        if (++timelineInvalidTicks_ >= kTimelineInvalidTicks)
        {
            session_.forceClearMask();
            timelineInvalidTicks_ = kTimelineInvalidTicks; // 钳住,避免重复清
            // [J51] 诊断:上报连续无时间线期间累计的无效块数(timelineInvalidBlocks_ 接线落点)。
            DBG("SCVB Output: timeline invalid ≥0.5s, clearing inject mask ("
                << timelineInvalidBlocks_.load(std::memory_order_relaxed) << " invalid blocks)");
        }
    }
    else
    {
        timelineInvalidTicks_ = 0;
    }

    // 打印器模式(03 §2.2 三态)。此前这里写死 Armed —— 当时的行注「T24 无分析曲线,
    // Armed 与 Print 等效」在**有**分析曲线之后就不成立了:AutomationPrinter 只在 **Print**
    // 态写宿主参数(非 Print 一律 endAllGestures + return),而且还要求 m_hasRange。
    // 两个前提在生产代码里**一个都没有人满足过**:setAnalyzedRange 与 stepAuthority 的
    // 调用方数都是 0。于是分析出多好的曲线,自动化面都恒零写入 —— 这正是 v5 实测 P0-1
    // 的另一半「自动化零写入」。
    //
    // 这里按 §2.2 的三个条件直接求值(而不是引 10 行事件状态机):
    //   FOLLOW —— 输出开关 OFF,宿主参数是权威;
    //   ARMED  —— 引擎权威,但 transport 停 / 播放头在已分析区间外 → 零 gesture 零写入;
    //   PRINT  —— 引擎权威 ∧ 正在播 ∧ 播放头在区间内 → 打印。
    // 离开 PRINT 的所有路径由 setMode 内部统一 endAllGestures(§3.3),故三态直接切是安全的。
    {
        // 已分析区间 = 活动版本里所有段的时间跨度并集的外包络(§4.1「未设置区间 → 零打印」)。
        // 段表变了才重算:crvsRevision 是 CRVS 的唯一修订号。
        const std::uint32_t rev = curvesRevision_.load(std::memory_order_acquire);
        if (rev != lastPrintRangeRevision_)
        {
            lastPrintRangeRevision_ = rev;
            const double sr = sampleRate_.load(std::memory_order_relaxed);
            double lo = 0.0;
            double hi = 0.0;
            bool any = false;
            if (sr > 0.0)
            {
                const auto& version = crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)];
                for (const auto& track : version.tracks)
                {
                    for (const auto& sg : track.segments)
                    {
                        const double t0 = static_cast<double>(sg.t0) / sr;
                        const double t1 = static_cast<double>(sg.t1) / sr;
                        lo = any ? std::min(lo, t0) : t0;
                        hi = any ? std::max(hi, t1) : t1;
                        any = true;
                    }
                }
            }
            printRangeValid_ = any && hi > lo;
            printRangeStartS_ = lo;
            printRangeEndS_ = hi;
            if (printRangeValid_)
            {
                printer_.setAnalyzedRange(lo, hi);
            }
        }

        const auto shot = playheadSnapshot();
        const bool playing = (shot.flags & scvb::engine::kPlayheadIsPlaying) != 0;
        const double tSec = shot.sampleRate > 0.0 ? static_cast<double>(shot.timeSamples) / shot.sampleRate : -1.0;
        const bool inRange = printRangeValid_ && tSec >= printRangeStartS_ && tSec <= printRangeEndS_;

        scvb::engine::AuthorityMode mode = scvb::engine::AuthorityMode::Follow;
        if (outputEnabled_)
        {
            mode = (playing && inRange) ? scvb::engine::AuthorityMode::Print : scvb::engine::AuthorityMode::Armed;
        }
        printer_.setMode(mode);
    }

    // 轨启用位(§1.15):推给打印器的车道闸(enabled=false 整轨不 begin、不写,03 §3.2),
    // 并落成 [A] 每块读的位图(混音时整轨不注入)。此前两处都没接 —— setTrackEnabled 实现完整
    // 且有单测,却**没有任何生产调用点**,于是开关一拧,音频与自动化都毫无反应(v4 实测 P1-5)。
    {
        std::uint32_t mask = 0;
        for (int t = 0; t < 15; ++t)
        {
            const bool on = runtime_.channels[static_cast<std::size_t>(t)].enabled;
            printer_.setTrackEnabled(t, on);
            if (on)
            {
                mask |= (1u << t);
            }
        }
        enabledMask_.store(mask, std::memory_order_release);
    }

    // 布防时间维(§1 setCaptureEnabled:ON = 对 {enabled 轨} × {global.range} 布防)。
    // follow 档 = 不限范围(全域);manual/daw_loop 档按 range 折成 hop 门,范围外不记账。
    {
        scvb::analysis::HopRange gate{0, std::numeric_limits<std::uint64_t>::max()};
        if (runtime_.rangeMode != 0 && runtime_.rangeEndS > runtime_.rangeStartS)
        {
            const double hopS = featHopSeconds();
            const auto toHop = [hopS](double sec) {
                const double h = sec / hopS;
                return h <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(h);
            };
            gate = scvb::analysis::HopRange{toHop(runtime_.rangeStartS), toHop(runtime_.rangeEndS)};
        }
        session_.setFeatureGate(gate);
    }

    // Input 远程改的优先级先落 state(§3.4),再把整个配置镜像推给广播区(§4.3)。
    // 顺序不能倒:倒过来这一拍的远程改动要等下一拍才广播出去,Input 的乐观值会先回滚再跳回。
    // 两个都要跑(不能靠 || 短路):优先级与检测值各自独立地弄脏配置。
    const bool prioChanged = applyRemotePriorities();
    const bool srcChanged = refreshSourceChannels();
    if (prioChanged || srcChanged)
    {
        ++runtime_.configSeq;
    }
    publishConfigBroadcast();

    // [T44/J75] viz 段:先按 claim 态裁决建/释放(唯一写方 = kActive 的那个 Output),再发布。
    syncVizSegment();
    publishVizFrame(now);
}

void ScvbOutputAudioProcessor::syncVizSegment()
{
    // viz 段的唯一写方 = **本组 claim 到 OutputSlot 的那一个** Output([J66] 同组内只读观察)。
    // claim 态会在运行中翻转(接管 / 让位 / 改组 / 段不可用),所以这条判定必须**每拍**做,
    // 不能只在 prepareToPlay 做一次 —— 否则「启动时是 observer、后来接管成功」的实例永远不发布,
    // 而「启动时 active、后来让位」的实例会与新主实例一起写同一个 seqlock。
    const bool shouldWrite = prepared_ && session_.state() == scvb::output::OutputClaimState::kActive;
    if (shouldWrite && !vizPublisher_.isOpen())
    {
        vizPublisher_.open(); // 失败(权限/内存)不影响主链路 —— Monitor 看到空态即可,下拍再试
    }
    else if (!shouldWrite && vizPublisher_.isOpen())
    {
        vizPublisher_.release(); // 让位 / 改组 / 未 prepare:立刻松手,不留第二个写方
    }
}

void ScvbOutputAudioProcessor::publishVizFrame(std::uint64_t nowMs)
{
    // 调用方已持 lifecycleMutex_(timerCallback):可直接读 crvsData_,免去 crvsSnapshot() 的深拷贝。
    // 双保险:syncVizSegment 已按 claim 态裁决过,这里再判一次。
    if (!vizPublisher_.isOpen())
    {
        return;
    }
    // 4Hz 闸门**前置**:下面要采 15 个轨名(每个一次 toStdString 堆分配)+ 曲线指针,
    // 而 tick() 只在 250ms 边界真的用得上 —— 25Hz 全采是白烧消息线程。
    if (!vizPublisher_.due(nowMs))
    {
        return;
    }

    scvb::output::VizPublishInput in;
    in.crvs = &crvsData_;
    in.curves = authority_.activeCurves();
    in.versionActive = static_cast<scvb::u32>(versionActive_);
    // 车道重算的判据取**求值曲线**修订号:段编辑不换整表、不动 crvsRevision_,但车道确实变了。
    in.crvsRevision = curvesRevision_.load(std::memory_order_acquire);
    in.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    in.playhead = playheadSnapshot();

    const int v = juce::jlimit(1, kVersionMax, versionActive_);
    // [N1] metaRevision **只哈希轨名**。width 走帧头段、每帧都刷,与 writeLanes 无关 ——
    // 把它掺进来会让「width 被自动化」变成 needLanes 恒真,每秒 15360 次曲线求值。
    // FNV-1a 64 位:碰撞概率可忽略,不再靠时间兜底兜住碰撞(见 kLaneRefreshMaxMs)。
    std::uint64_t meta = 1469598103934665603ull ^ static_cast<std::uint64_t>(v);
    const auto fnv = [&meta](unsigned char byte) { meta = (meta ^ byte) * 1099511628211ull; };
    for (int ch = 0; ch < 15; ++ch)
    {
        const auto& c = runtime_.channels[static_cast<std::size_t>(ch)];
        if (c.enabled)
        {
            in.enabledMask |= (1u << ch);
        }
        if (c.sourceChannels == 2)
        {
            in.stereoMask |= (1u << ch);
        }
        if (c.leadLock)
        {
            in.leadMask |= (1u << ch);
        }
        auto& label = in.label[static_cast<std::size_t>(ch)];
        label = c.label.toStdString();
        // 每轨 width:活动版本的参数 raw atomic(engineering 0..100)。句柄未就绪 → NaN → 段内哨兵。
        const auto* raw = handles_.rawTrkW[v - 1][ch];
        in.widthPct[static_cast<std::size_t>(ch)] =
            raw != nullptr ? raw->load(std::memory_order_relaxed) : std::numeric_limits<float>::quiet_NaN();
        for (const char byte : label)
        {
            fnv(static_cast<unsigned char>(byte));
        }
        fnv(0); // 分隔符:防「AB|C」与「A|BC」哈希相同
    }
    // 64 位哈希折成 32 位存进 metaRevision(只用于相等比较,折叠不降低碰撞抗性到可担忧的程度)。
    in.metaRevision = static_cast<scvb::u32>(meta ^ (meta >> 32));
    vizPublisher_.tick(nowMs, in);
}

bool ScvbOutputAudioProcessor::refreshSourceChannels()
{
    // §1.15 的 source_channels 是**检测值**,真源在 Input:它在 prepareToPlay 把 1|2 写进音频环
    // 段头([J57] AudioRingHeader.channels),Output 绑定时已把它快照进 ShmRingMixSource。
    // 此前没有任何生产代码把它搬进 runtime state,于是这一格恒为 0「未检测」,而 J60 的默认
    // 推导写的是 `sourceChannels == 1`(mono 才参与自动 pan)—— 0 落进 else 分支,**全 15 轨
    // 一律 participate=false**。指派层把不参与的轨按「保持现值」处理(AutoAssign.cpp:241),
    // 现值即参数面的 0,于是分析跑完每轨 pan 都是 0:段照出、轨照数、声像分布图与泳道全居中
    // (v5 实测 P0-1)。这里把检测值接上,并把未检测的回落改成 mono 侧(见下面三处推导)。
    bool changed = false;
    for (int t = 0; t < 15; ++t)
    {
        const auto detected = session_.mixSource(static_cast<scvb::u32>(t + 1)).channels();
        if (detected != 1u && detected != 2u)
        {
            continue; // 未绑定 / 段头非法:保留上一次的检测值,不把已知信息退回「未检测」
        }
        auto& channel = runtime_.channels[static_cast<std::size_t>(t)];
        const int next = static_cast<int>(detected);
        if (channel.sourceChannels != next)
        {
            channel.sourceChannels = next;
            changed = true;
        }
    }
    return changed;
}

bool ScvbOutputAudioProcessor::applyRemotePriorities()
{
    bool changed = false;
    for (int ch = 1; ch <= 15; ++ch)
    {
        scvb::u32 value = 0;
        if (!session_.takeRemotePriority(static_cast<scvb::u32>(ch), value))
        {
            continue;
        }
        auto& channel = runtime_.channels[static_cast<std::size_t>(ch - 1)];
        const int next = juce::jlimit(0, 10, static_cast<int>(value));
        if (channel.priority != next)
        {
            channel.priority = next;
            changed = true;
        }
    }
    return changed;
}

void ScvbOutputAudioProcessor::publishConfigBroadcast()
{
    // 只读观察实例不得写广播区:本组真源是那个 kActive 的 Output,两个实例抢写会让 Input
    // 在两份配置之间抖动。
    if (session_.state() != scvb::output::OutputClaimState::kActive)
    {
        return;
    }
    // 换组后必写一次:ctrl 段换了一张,新组广播区是全零,旧组的 config_seq 不能用来短路。
    if (runtime_.configSeq == lastBroadcastConfigSeq_ && groupId_ == lastBroadcastGroup_)
    {
        return; // 配置未变且同组:不写(广播区是低频面,不做每拍空转)
    }
    lastBroadcastConfigSeq_ = runtime_.configSeq;
    lastBroadcastGroup_ = groupId_;

    scvb::CtrlBroadcastSnapshot s;
    // +1:广播区的 0 保留给「本组没有 Output 在广播」。不偏移的话 Output 首次上线时 seq 恰为 0,
    // 与「无广播」撞值,Input 的 config_seq 变化门会把首帧真配置吞掉。
    s.config_seq = runtime_.configSeq + 1u;
    s.lead_select = handles_.rawLeadSelect != nullptr
                        ? static_cast<scvb::u32>(juce::jlimit(
                              0, 15, juce::roundToInt(handles_.rawLeadSelect->load(std::memory_order_relaxed))))
                        : 0u;

    for (int t = 0; t < 15; ++t)
    {
        const auto& c = runtime_.channels[static_cast<std::size_t>(t)];
        auto& dst = s.channels[t];
        dst.priority = static_cast<scvb::u32>(juce::jlimit(0, 10, c.priority));
        dst.pair_id = static_cast<scvb::u32>(juce::jlimit(0, 7, c.pairId));
        dst.source_channels = static_cast<scvb::u32>(c.sourceChannels);

        // J60:未显式设置时按 mono=true / stereo=false 推导(与 buildStateSubtree 同口径)。
        const bool participate = c.participatesInAutoPan();
        scvb::u32 flags = 0;
        if (c.enabled)
            flags |= scvb::kCfgFlagEnabled;
        if (c.leadLock)
            flags |= scvb::kCfgFlagLeadLock;
        if (c.leadVolExempt)
            flags |= scvb::kCfgFlagLeadVolExempt;
        if (participate)
            flags |= scvb::kCfgFlagParticipateAutoPan;
        dst.flags = flags;

        // freeze 是当前激活版本的自动化参数(J65 同一参数承载 pan/vol 两位)。
        const auto* frz = handles_.rawFrz[static_cast<std::size_t>(versionActive_ - 1)][static_cast<std::size_t>(t)];
        dst.freeze =
            frz != nullptr
                ? static_cast<scvb::u32>(juce::jlimit(0, 3, juce::roundToInt(frz->load(std::memory_order_relaxed))))
                : 0u;

        // label:UTF-8 按字节截断到最后一个完整序列边界,绝不切出半个码点(Input 侧直接当
        // C 字符串显示,半个码点会渲染成乱码方块)。
        const juce::String label = c.label;
        const char* utf8 = label.toRawUTF8();
        std::size_t len = std::strlen(utf8);
        if (len >= scvb::kCtrlLabelBytes)
        {
            len = scvb::kCtrlLabelBytes - 1;
            // 回退到序列头:UTF-8 后续字节形如 10xxxxxx。
            while (len > 0 && (static_cast<unsigned char>(utf8[len]) & 0xC0u) == 0x80u)
            {
                --len;
            }
        }
        std::memcpy(s.labels[t], utf8, len);
        s.labels[t][len] = '\0';
    }

    session_.publishConfigBroadcast(s);
}

void ScvbOutputAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String ScvbOutputAudioProcessor::getProgramName(int /*index*/)
{
    return {};
}

void ScvbOutputAudioProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void ScvbOutputAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    // PR#53 R1:拒载更高 abi 后原样回写宿主原始字节(preservedOriginal 语义,Input PR#51 重要#2 同款),
    // 绝不把高版本 blob 覆盖成当前版数据。
    if (stateAbiMismatch_ && !preservedStateBlob_.empty())
    {
        destData.append(preservedStateBlob_.data(), preservedStateBlob_.size());
        return;
    }

    // 从上次成功加载的容器重建:原位替换 PRMS/CFGS,其余 chunk(FEAT/CRVS/未知 fourcc)原样回写(T19)。
    scvb::state::StateChunks chunks = loadedChunks_;
    chunks.abi = scvb::state::kCurrentAbi;

    // PRMS:123 参数(ValueTree XML 二进制,host 自动化面)+ ui 首启已读位。
    // 两位挂在 PRMS 的根节点属性上而不是 CFGS 尾部 —— CFGS 是定长枚举式解码,追加字段会让
    // 旧构建整块拒载并静默把 group/开关/版本打回默认;ValueTree 两个方向都容忍字段增删。
    // 见 OutputUiState.h 头注(STATE_SCHEMA §三 的 ui 组本就登记在 PRMS 名下)。
    auto state = apvts.copyState();
    scvb::output::writeUiFlags(state, {runtime_.guideSeen.load(std::memory_order_relaxed),
                                       runtime_.tourSeen.load(std::memory_order_relaxed),
                                       runtime_.langChosen.load(std::memory_order_relaxed)});
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    juce::MemoryBlock paramsBlock;
    copyXmlToBinary(*xml, paramsBlock);
    const auto* pdata = static_cast<const std::uint8_t*>(paramsBlock.getData());
    chunks.set(scvb::state::kFourccPrms, std::vector<std::uint8_t>(pdata, pdata + paramsBlock.getSize()));

    // CFGS:Output 配置(group_id / 采集 / 输出 / 版本 / ui,[J66] 最小 T24 子集)。
    scvb::state::OutputState s;
    s.groupId = static_cast<scvb::u32>(groupId_);
    s.captureEnabled = captureEnabled_ ? 1u : 0u;
    s.outputEnabled = outputEnabled_ ? 1u : 0u;
    s.versionActive = static_cast<scvb::u32>(versionActive_);
    s.uiScale = static_cast<scvb::u32>(uiScale_);
    s.uiLanguage = uiLanguage_.toStdString();
    // [J69/U24] analysis 配置落盘(T35 #62 评审遗留):loudness_mode / center_slot_policy。
    s.loudnessMode = runtime_.loudnessMode.toStdString();
    s.centerSlotPolicy = runtime_.centerSlotPolicy.toStdString();
    s.unknownTail = preservedCfgsTail_; // 未来小版本追加字段原样回写(防静默丢字段)
    std::vector<std::uint8_t> cfg;
    if (!scvb::state::encodeOutputState(s, cfg))
    {
        return;
    }
    chunks.set(scvb::state::kFourccCfgs, std::move(cfg));

    // [J75] T43:ui.master_chart_mode 独立 UICF chunk(恒写 4 字节 u32,0=distribution | 1=trajectory)。
    std::vector<std::uint8_t> uicf;
    const std::uint32_t chartMode = (masterChartMode_ == "trajectory") ? scvb::state::kMasterChartModeTrajectory
                                                                       : scvb::state::kMasterChartModeDistribution;
    if (!scvb::state::encodeUiConfig(chartMode, uicf))
    {
        return;
    }
    chunks.set(scvb::state::kFourccUiConfig, std::move(uicf));

    // CRVS:段真身(版本名/段表/pan_curve)从 live crvsData_ 编码(T29;覆盖 loadedChunks_ 的旧 CRVS)。
    std::vector<std::uint8_t> crvs;
    if (scvb::state::encodeCrvs(crvsData_, crvs))
        chunks.set(scvb::state::kFourccCrvs, std::move(crvs));

    std::vector<std::uint8_t> blob;
    if (!scvb::state::encodeContainer(chunks, blob))
    {
        return;
    }
    destData.append(blob.data(), blob.size());
}

void ScvbOutputAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
    {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t size = static_cast<std::size_t>(sizeInBytes);

    // PR#53 R1:经 loadState 做 abi 判读 + 迁移(高 abi 拒载、低 abi 走迁移链;未知 chunk 原样保留)。
    scvb::state::StateChunks chunks;
    const scvb::state::StateLoadResult res = scvb::state::loadState(bytes, size, chunks);

    const juce::ScopedLock lock(lifecycleMutex_);

    // 冻结契约(CLAUDE.md §7.3 / STATE_SCHEMA):读到高版本 abi → 拒载并提示升级,绝不静默降级;
    // 同时保留原字节供 getStateInformation 原样回写(Input PR#51 红旗#1/重要#2 同款)。
    if (res.status == scvb::state::StateLoadStatus::RejectedNewer)
    {
        scvb::state::StateHeader hdr;
        stateAbiMismatch_ = true;
        stateAbiSeen_ = scvb::state::parseHeader(bytes, size, hdr) ? hdr.abi : 0u;
        preservedStateBlob_.assign(bytes, bytes + size);
        DBG("SCVB Output: state abi " << stateAbiSeen_ << " > current " << scvb::state::kCurrentAbi
                                      << "; refusing load (upgrade required)");
        return;
    }
    if (res.status != scvb::state::StateLoadStatus::Ok && res.status != scvb::state::StateLoadStatus::Migrated)
    {
        return; // Corrupt:不可信字节,拒载(不崩溃、不半填充)
    }

    stateAbiMismatch_ = false;
    preservedStateBlob_.clear();
    loadedChunks_ = chunks; // 保真 FEAT/CRVS/未知 fourcc 供 save 原样回写(T19 未知 fourcc 回写纪律)

    // PRMS:123 参数(宿主自动化面)。
    if (const scvb::state::Chunk* prms = chunks.find(scvb::state::kFourccPrms); prms != nullptr)
    {
        std::unique_ptr<juce::XmlElement> xml(
            getXmlFromBinary(prms->payload.data(), static_cast<int>(prms->payload.size())));
        if (xml != nullptr)
        {
            const juce::ValueTree loaded = juce::ValueTree::fromXml(*xml);
            // 首启已读位与参数同在 PRMS(见 OutputUiState.h);属性缺失 = 老工程 ⇒ 两位 false,
            // 用户会再走一遍首启,之后即落盘不再重放。
            const auto flags = scvb::output::readUiFlags(loaded);
            runtime_.guideSeen.store(flags.guideSeen, std::memory_order_relaxed);
            runtime_.tourSeen.store(flags.tourSeen, std::memory_order_relaxed);
            runtime_.langChosen.store(flags.langChosen, std::memory_order_relaxed);
            apvts.replaceState(loaded);
            handles_ = scvb::params::collectParamHandles(apvts);
        }
    }

    const scvb::state::Chunk* cfg = chunks.find(scvb::state::kFourccCfgs);
    if (cfg == nullptr)
    {
        return;
    }
    scvb::state::OutputState s;
    scvb::state::OutputDecodeReport report;
    if (!scvb::state::decodeOutputState(cfg->payload.data(), cfg->payload.size(), s, &report))
    {
        return; // 范围校验失败 → 拒载(CLAUDE.md §7.3)
    }
    preservedCfgsTail_ = std::move(s.unknownTail); // 未来小版本追加字段保留,getStateInformation 原样回写

    groupId_ = static_cast<int>(s.groupId);
    captureEnabled_ = s.captureEnabled != 0;
    outputEnabled_ = s.outputEnabled != 0;
    versionActive_ = static_cast<int>(s.versionActive);
    uiScale_ = static_cast<int>(s.uiScale);
    uiLanguage_ = juce::String::fromUTF8(s.uiLanguage.c_str(), static_cast<int>(s.uiLanguage.size()));
    // [J69/U24] analysis 配置落盘(T35 #62 评审遗留):未知枚举序号已由 codec 回落默认并计数。
    runtime_.loudnessMode = juce::String::fromUTF8(s.loudnessMode.c_str(), static_cast<int>(s.loudnessMode.size()));
    runtime_.centerSlotPolicy =
        juce::String::fromUTF8(s.centerSlotPolicy.c_str(), static_cast<int>(s.centerSlotPolicy.size()));
    if (report.loudnessModeFallbacks > 0 || report.centerSlotPolicyFallbacks > 0)
    {
        DBG("SCVB Output: analysis 枚举未知值回落默认(loudness_mode="
            << report.loudnessModeFallbacks << ", center_slot_policy=" << report.centerSlotPolicyFallbacks << ")");
    }

    // [J75] T43:ui.master_chart_mode 从独立 UICF chunk 读;缺失/长度非法/未知值均回落默认(§7.3)。
    std::uint32_t chartMode = scvb::state::kMasterChartModeDistribution;
    if (const scvb::state::Chunk* uicf = chunks.find(scvb::state::kFourccUiConfig); uicf != nullptr)
    {
        (void)scvb::state::decodeUiConfig(uicf->payload.data(), uicf->payload.size(), chartMode);
    }
    masterChartMode_ = (chartMode == scvb::state::kMasterChartModeTrajectory) ? juce::String("trajectory")
                                                                              : juce::String("distribution");
    session_.setCaptureEnabled(captureEnabled_);
    session_.setOutputEnabled(outputEnabled_);

    // CRVS:段真身解码(版本名/段表/pan_curve)。成功 → 替换;无 chunk 或解码失败 → 重置全新默认
    // (不残留旧编辑写回,PR#55 第10轮缺陷2)。无论结果都 +修订号刷新段表(PR#55 第8轮缺陷1)。
    bool crvsLoaded = false;
    if (const scvb::state::Chunk* crvs = chunks.find(scvb::state::kFourccCrvs); crvs != nullptr)
    {
        scvb::state::CrvsData decoded;
        if (scvb::state::decodeCrvs(crvs->payload.data(), crvs->payload.size(), decoded))
        {
            crvsData_ = std::move(decoded);
            crvsLoaded = true;
        }
    }
    if (!crvsLoaded)
    {
        crvsData_ = scvb::state::CrvsData{};
        crvsData_.versions[0].meta.name = "V1"; // 默认版本名([J05])
        crvsData_.versions[1].meta.name = "V2";
    }
    crvsRevision_.fetch_add(1, std::memory_order_release);

    // 加载 state 后 CRVS 已整体替换 → 清空 UndoManager,否则 undo() 会恢复加载前的旧 CRVS 快照,
    // 静默丢弃刚加载的段数据(PR#55 第12轮;关闭 #48 tech-debt「fromState 清 undo」的桥面同款)。
    // 桥的 UndoManager 只含 CRVS 事务(editSegment/setVersionName/copyVersion/setTrackManual/setPanCurve,
    // 均写 crvsData_),无其它事务类别 → 全清口径安全(在 lifecycleMutex_ 内)。
    authority_.undoManager().clearUndoHistory();

    // 绑定时序(03 §7.2):setStateInformation 后 claim;样本率等 prepareToPlay 提供。
    if (prepared_)
    {
        // PR#53 缺陷1:已 prepared 加载到不同 group 的 state → 走 changeGroup(释放旧 OutputSlot 与旧环
        // 绑定 → 新组 claim 后 attach 新环);直接 prepare() 会让旧 group OutputSlot 保持 active 且已绑
        // sources 继续读旧环。group 未变则 prepare() 幂等 re-claim。
        const scvb::u32 newGroup = static_cast<scvb::u32>(groupId_);
        if (session_.groupId() != newGroup)
        {
            session_.changeGroup(newGroup, static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                                 static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
            // [T44] viz 段随组切换 —— 与 setGroupId() 同口径:**只换指向,不建段**,建不建
            // 由下一拍的 syncVizSegment() 按 claim 态裁决。
            // 漏掉这一行时:宿主先 prepareToPlay(publisher 指向默认组 1)、再灌工程 chunk
            // (session 换到工程组 N),Output 于是把 viz 段发布在 g1,而 Monitor 在 gN 上等 ——
            // 永远 attach 不上,正是 v5 实测 P0-5「同工程 Output 在线却显示未连接」。
            // 离线测试恒是「先 setState 再 prepare」,这条路径从没被走到。
            vizPublisher_.setGroup(newGroup);
        }
        else
        {
            session_.prepare(static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                             static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
        }
        rebindVersion();
    }
    else
    {
        session_.setGroupId(static_cast<scvb::u32>(groupId_));
    }
}

void ScvbOutputAudioProcessor::setGroupId(int groupId)
{
    groupId = juce::jlimit(1, kGroupIdMax, groupId);
    const juce::ScopedLock lock(lifecycleMutex_);
    if (groupId == groupId_)
    {
        return;
    }
    groupId_ = groupId;
    session_.setGroupId(static_cast<scvb::u32>(groupId));
    // 改组(J66):释放旧组 OutputSlot → 新组 claim(期间输出直通,§4.2)。
    if (prepared_)
    {
        session_.changeGroup(static_cast<scvb::u32>(groupId),
                             static_cast<scvb::u32>(sampleRate_.load(std::memory_order_relaxed)),
                             static_cast<scvb::u32>(preparedMaxBlock_), scvb::steadyNowMs());
        // [T44] viz 段随组切换:**只换指向,不建段** —— 新组里本实例可能是 kObserver,
        // 建不建由下一拍的 syncVizSegment() 按 claim 态裁决([J66])。
        vizPublisher_.setGroup(static_cast<scvb::u32>(groupId));
    }
}

void ScvbOutputAudioProcessor::setCaptureEnabled(bool on)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    captureEnabled_ = on;
    session_.setCaptureEnabled(on);
}

void ScvbOutputAudioProcessor::setOutputEnabled(bool on)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    outputEnabled_ = on;
    session_.setOutputEnabled(on);
}

void ScvbOutputAudioProcessor::setVersionActive(int version)
{
    version = juce::jlimit(1, kVersionMax, version);
    const juce::ScopedLock lock(lifecycleMutex_);
    versionActive_ = version;
    if (prepared_)
    {
        rebindVersion();
    }
}

void ScvbOutputAudioProcessor::setMasterChartMode(const juce::String& mode)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // [J75] T43:仅 "trajectory" 是合法非默认档,其余(含未知值)一律回落默认 "distribution"。
    masterChartMode_ = (mode == "trajectory") ? juce::String("trajectory") : juce::String("distribution");
}

ScvbOutputAudioProcessor::ConnSnapshot ScvbOutputAudioProcessor::connSnapshot()
{
    const auto now = scvb::steadyNowMs(); // 取锁前采样,与 timerCallback 同一时钟口径
    const juce::ScopedLock lock(lifecycleMutex_);
    ConnSnapshot out;
    for (int t = 0; t < 15; ++t)
    {
        out.channels[static_cast<std::size_t>(t)] = session_.channelConn(static_cast<scvb::u32>(t + 1), now);
    }
    out.generation = session_.registryGeneration();
    out.readOnly = session_.state() == scvb::output::OutputClaimState::kObserver;
    return out;
}

void ScvbOutputAudioProcessor::bridgeSetUiLanguage(const juce::String& lang)
{
    const juce::ScopedLock lock(lifecycleMutex_); // 与 get/setStateInformation 的持久化路径串行
    uiLanguage_ = lang; // 已由桥层 normalize({zh,en,fr});getStateInformation 持久化
    // setLang 只在用户显式操作时才被调(首启语言卡的选择、设置页的语言按钮);web 启动时
    // 的语言回填走 setLang(..., {push:false}),不经桥。所以「桥的 setLang 被调过」就等价于
    // 「用户显式选过语言」—— 据此置位,首启语言卡此后不再问(v4 实测 P1-6)。
    runtime_.langChosen.store(true, std::memory_order_relaxed);
}

void ScvbOutputAudioProcessor::bridgeSetUiScalePercent(int percent)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // 边界真源 = scvb::bridge::Min/MaxUiScale(§1.28/§1.29:C++ 不得二次硬编码档位边界)。
    uiScale_ = juce::jlimit(juce::roundToInt(scvb::bridge::plugin::MinUiScale * 100.0f),
                            juce::roundToInt(scvb::bridge::plugin::MaxUiScale * 100.0f), percent);
}

void ScvbOutputAudioProcessor::bridgeSetGuideSeen(bool seen)
{
    const juce::ScopedLock lock(lifecycleMutex_); // 与 get/setStateInformation 的持久化路径串行
    runtime_.guideSeen.store(seen, std::memory_order_relaxed);
}

void ScvbOutputAudioProcessor::bridgeSetTourSeen(bool seen)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    runtime_.tourSeen.store(seen, std::memory_order_relaxed);
}

// createEditor() 与 createPluginFilter() 见 OutputPluginEntry.cpp:抽出去之后本 TU 不再引用
// OutputEditor,免 DAW 的宿主 harness 才能只编 Processor 而不链接 WebView2(见该文件头注)。

void ScvbOutputAudioProcessor::rebuildAllCurves()
{
    const double sr = sampleRate_.load(std::memory_order_relaxed); // 原子读(PR#55 第9轮)
    if (sr <= 0.0)
        return; // 未 prepare → 不构建不发布(防 NaN/inf CurveSegment,PR#55 第7轮缺陷2)

    // 曲线要变了 —— 在这里 +1 就覆盖了**全部**改曲线的路径(段编辑 / 手动写回 / 复制版本 /
    // 撤销重做 / 换版本 / 改 ramp / 分析回落 / 加载工程),不必逐个事务回调各记一笔、也不会漏。
    // 两个消费方:① 打印区间缓存(不刷新的话手动拖出旧包络之后,打印区间停在旧范围,
    // 播放头一走出去就回落 ARMED —— 自动化面与你听到的东西脱节);② viz 车道
    // (VizPublisher 的 needLanes,不刷新的话 Monitor 的车道停在编辑前的样子)。
    curvesRevision_.fetch_add(1, std::memory_order_release);

    for (int v = 1; v <= scvb::state::kNumVersions; ++v)
    {
        for (int t = 0; t < scvb::state::kNumTracks; ++t)
        {
            const auto& src =
                crvsData_.versions[static_cast<std::size_t>(v - 1)].tracks[static_cast<std::size_t>(t)].segments;
            if (src.empty())
            {
                authority_.setCurve(v, t, nullptr);
                continue;
            }

            std::vector<scvb::CurveSegment> curveSegments;
            curveSegments.reserve(src.size());
            for (const auto& s : src)
            {
                scvb::CurveSegment cs;
                cs.startSec = static_cast<double>(s.t0) / sr;
                cs.endSec = static_cast<double>(s.t1) / sr;
                cs.pan = static_cast<double>(s.pan);
                cs.volDb = static_cast<double>(s.volDb);
                curveSegments.push_back(cs);
            }

            scvb::TransitionConfig cfg;
            cfg.transitionRampSec = static_cast<double>(runtime_.transitionRampMs) / 1000.0;
            scvb::CurveEvaluator ev;
            ev.build(curveSegments, cfg);
            authority_.setCurve(v, t, &ev); // 不可变契约:setCurve 深拷贝
        }
    }
    // 打印器重取活动版本曲线(编辑后打印头立即反映新曲线真身,ADR-005)。
    printer_.setCurves(authority_.activeCurves());
}

std::uint8_t ScvbOutputAudioProcessor::probeGroupsOnline()
{
    std::uint8_t bitmap = 0;

    // 本组:直接读已映射的本组 OutputSlot 判定(state==active → 本组在线;observer → 同组已有主 Output)。
    const auto ownState = session_.state();
    if (ownState == scvb::output::OutputClaimState::kActive || ownState == scvb::output::OutputClaimState::kObserver)
        bitmap |= static_cast<std::uint8_t>(1u << (groupId_ - 1));

    // 异组:只读探测(契约 §2.4;失败=0 位,不重试不报错)。尺寸校验用 VirtualQuery(PR#55 缺陷1)。
    const scvb::u64 now = scvb::steadyNowMs();
    for (int g = 1; g <= scvb::kMaxGroups; ++g)
    {
        if (g == groupId_)
            continue; // 本组已判定
        if (scvb::probeRegistryGroupOnline(static_cast<scvb::u32>(g), now))
            bitmap |= static_cast<std::uint8_t>(1u << (g - 1));
    }
    return bitmap;
}

scvb::state::CrvsData ScvbOutputAudioProcessor::crvsSnapshot()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return crvsData_;
}

std::pair<juce::String, juce::String> ScvbOutputAudioProcessor::analysisConfigSnapshot()
{
    // 复评重要②:loudnessMode/centerSlotPolicy 的读写锁协议 —— setAnalysisConfig(写)、
    // getStateInformation(读)、本快照(读)三方均持 lifecycleMutex_,emitState 经此读,消除剩余竞态。
    const juce::ScopedLock lock(lifecycleMutex_);
    return {runtime_.loudnessMode, runtime_.centerSlotPolicy};
}

scvb::engine::PlayheadPod ScvbOutputAudioProcessor::playheadSnapshot() const
{
    scvb::engine::PlayheadPod pod{};
    // read 失败(写者正在写/读期间更新)时 pod 未被子写,保持零值 —— 是「撕裂混合值」,非「沿用上帧」;
    // 下一 25Hz tick 重读自愈(PR#55 建议④注释修正)。
    playheadShot_.read(pod);
    return pod;
}

scvb::engine::SetNameResult ScvbOutputAudioProcessor::setVersionName(int version, const juce::String& name,
                                                                     juce::String& effectiveOut)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    std::string eff;
    const scvb::engine::SetNameResult result = scvb::engine::normalizeVersionName(version, name.toStdString(), eff);
    effectiveOut = juce::String::fromUTF8(eff.c_str());
    if (result == scvb::engine::SetNameResult::InvalidIndex)
        return result;
    if (crvsData_.versions[static_cast<std::size_t>(version - 1)].meta.name == eff)
        return result; // 未变,不产生空撤销事务

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Rename V" + juce::String(version),
        [&] { crvsData_.versions[static_cast<std::size_t>(version - 1)].meta.name = eff; },
        [] {}); // 改名不影响曲线,不 rebuild
    return result;
}

scvb::engine::CopyVersionResult ScvbOutputAudioProcessor::copyVersion(int src, int dst)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    if (src < 1 || src > 2 || dst < 1 || dst > 2 || src == dst)
        return scvb::engine::CopyVersionResult::InvalidIndex;

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Copy V" + juce::String(src) + " -> V" + juce::String(dst),
        [&] {
            auto& d = crvsData_.versions[static_cast<std::size_t>(dst - 1)];
            const auto& s = crvsData_.versions[static_cast<std::size_t>(src - 1)];
            d.tracks = s.tracks; // 深拷贝段表(含 origin/locked)
            d.panCurve = s.panCurve;
            d.meta.copiedFrom = src; // name 不复制(保留目标名,J05)
            d.meta.copiedAtMs = static_cast<std::int64_t>(juce::Time::getCurrentTime().toMilliseconds());
        },
        [this] { rebuildAllCurves(); });
    return scvb::engine::CopyVersionResult::Ok;
}

scvb::state::SegmentEditResult ScvbOutputAudioProcessor::editSegment(int track,
                                                                     const scvb::state::SegmentEditArgs& args)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    // 结果门控:仅 Ok 才压入 undo 事务并 rebuild(PR#55 缺陷3)。
    return scvb::output::editSegmentTransactional(authority_.undoManager(), crvsData_, versionActive_, track, args,
                                                  [this] { rebuildAllCurves(); });
}

bool ScvbOutputAudioProcessor::setTrackManual(int ch, bool isPan, float value, int& replacedSegments,
                                              int& replacedLocked)
{
    const juce::ScopedLock lock(lifecycleMutex_);

    if (ch < 1 || ch > 15)
        return false;

    auto& track =
        crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)].tracks[static_cast<std::size_t>(ch - 1)];
    replacedSegments = static_cast<int>(track.segments.size());
    replacedLocked = 0;
    for (const auto& s : track.segments)
        if (scvb::state::segmentLocked(s.flags))
            ++replacedLocked; // 如实统计锁定段(PR#55 建议⑤)

    // 写一维必须保留另一维(§1.16 常值段的两个维度各自独立);构造与钳制口径见
    // makeManualConstantSegment 头注(T37 三轮 D 族回归点,单测直接断言该纯函数)。
    const scvb::state::Segment seg = scvb::output::makeManualConstantSegment(track.segments, isPan, value);

    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Track manual ch" + juce::String(ch),
        [&] { track.segments.assign(1, seg); }, [this] { rebuildAllCurves(); });

    // 手动值还必须落到**参数面**,否则冻结维度上它根本驱动不了声音。
    //
    // 取值仲裁(DspArbiter §2.3)对冻结维度读的是 rawPan/rawVol(host 参数),不读曲线;
    // 打印器对冻结车道也只是把参数自己的当前值重写成平直线(#68),不采样曲线。而 UI 在
    // 「手动写回成功」之后会**自动把该维度的 freeze 位置 1**(tab-tracks.js「拖动 = 接管手动」)。
    // 于是:第一次手动改能生效(此时 freeze 还是 0,曲线权威),UI 随即置 1,**之后每一次手动改
    // 都只写进曲线、对声音毫无作用** —— 而旋钮照样跟手,因为读回值取自段表。这正是 v4 实测
    // P1-4「先测有效、稍后再调完全无效」。
    //
    // 契约 §1.16 把本函数定义为「**冻结(freeze 对应位=1)时的手动静态值**」,PARAMETERS §freeze
    // 定义冻结维度为「引擎不驱动、host/**手动**权威」—— 手动值要当权威,就得落在冻结维度真正
    // 读的那个平面上。§1.16 的「零 gesture」照旧遵守:不 begin/endChangeGesture,只设值。
    const int v = versionActive_;
    if (auto* raw = isPan ? handles_.rawPan[static_cast<std::size_t>(v - 1)][static_cast<std::size_t>(ch - 1)]
                          : handles_.rawVol[static_cast<std::size_t>(v - 1)][static_cast<std::size_t>(ch - 1)];
        raw != nullptr)
    {
        const juce::String id = isPan ? scvb::params::panId(v, ch) : scvb::params::volId(v, ch);
        if (auto* p = apvts.getParameter(id))
        {
            const float applied = isPan ? seg.pan : seg.volDb;
            // **必须包 gesture**:裸 setValueNotifyingHost 在宿主看来是一次没有起止的孤立写入 ——
            // Cubase 这类宿主要么把它记成一个孤立自动化点、要么在自动化 Read 档下当场把值顶回去
            // (那样这条修复根本不生效)。begin/end 把它标成一次完整的用户编辑,宿主才会接受。
            // 这一点与 §1.16「零 gesture」的字面冲突,已在变更文档里作为裁定②登记。
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(applied)); // 工程值 → 归一化(§1.13 同款)
            p->endChangeGesture();
        }
    }
    return true;
}

void ScvbOutputAudioProcessor::setPanCurve(int version, const std::vector<scvb::PanCurvePoint>& points)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    if (version < 1 || version > scvb::state::kNumVersions)
        return; // 越界拒绝(public API 防 UB,PR#55 建议②)
    scvb::output::commitCrvsTransaction(
        authority_.undoManager(), crvsData_, "Set pan curve",
        [&] { crvsData_.versions[static_cast<std::size_t>(version - 1)].panCurve = points; },
        [] {}); // pan_curve 不参与 CurveEvaluator,不 rebuild
}

bool ScvbOutputAudioProcessor::setTransitionRamp(float ms)
{
    const juce::ScopedLock lock(lifecycleMutex_);
    const float clamped = juce::jlimit(20.0f, 300.0f, ms);
    if (runtime_.transitionRampMs == clamped)
        return false; // 未变,不重建

    runtime_.transitionRampMs = clamped;
    rebuildAllCurves(); // 新 ramp 立即烘焙进 CurveEvaluator 并重新发布(PR#55 第5轮缺陷2)
    return true;
}

bool ScvbOutputAudioProcessor::setAnalysisConfig(const juce::String& loudnessMode, const juce::String& centerSlotPolicy,
                                                 bool hasLoudness, bool hasCenter)
{
    // 复评重要①:持 lifecycleMutex_ 写 runtime_.loudnessMode/centerSlotPolicy,与 getStateInformation
    // (同锁读)串行化,消除 juce::String COW 跨线程竞态。传入值已由桥层白名单校验。
    const juce::ScopedLock lock(lifecycleMutex_);
    bool changed = false;
    if (hasLoudness)
    {
        changed |= loudnessMode != runtime_.loudnessMode;
        runtime_.loudnessMode = loudnessMode;
    }
    if (hasCenter)
    {
        changed |= centerSlotPolicy != runtime_.centerSlotPolicy;
        runtime_.centerSlotPolicy = centerSlotPolicy;
    }
    if (changed)
        ++runtime_.configSeq; // PR#55 缺陷4:变化才 bump
    return changed;
}

bool ScvbOutputAudioProcessor::undo()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return authority_.undoManager().undo();
}

bool ScvbOutputAudioProcessor::redo()
{
    const juce::ScopedLock lock(lifecycleMutex_);
    return authority_.undoManager().redo();
}

// ============================================================================
// 分析作业(§1.6/§1.7):后台线程跑 AnalysisPipeline,完成后回消息线程写 CRVS。
// ============================================================================

// 线程体只吃**快照**:起线程前在 [M] 持锁把范围内的特征切片拷出来,此后不再触碰
// FrameStore / runtime state / crvsData_ —— 分析期间用户照样可以采集、改配置、切 tab。
class ScvbOutputAudioProcessor::AnalysisJob final : public juce::Thread
{
public:
    AnalysisJob(ScvbOutputAudioProcessor& owner, std::array<scvb::analysis::PipelineTrackFeatures, 15> features,
                scvb::analysis::PipelineConfig config, std::uint32_t generation)
        : juce::Thread("scvb-analysis"), owner_(owner), features_(std::move(features)), config_(config),
          generation_(generation)
    {
    }

    void run() override
    {
        auto result = scvb::analysis::runAnalysisPipeline(
            features_, config_,
            // 进度是 atomic:本回调在 [W],读点在 [M] 的 25Hz emit。
            [this](float p) { owner_.runtime_.analysisProgress.store(p, std::memory_order_relaxed); },
            [this] { return threadShouldExit(); });

        if (threadShouldExit())
        {
            return; // 取消:结果整份丢弃,不碰 CRVS
        }

        // 交接给 [M]:结果放进槽 + 触发 AsyncUpdater。**不用裸 callAsync 捕获 owner 指针** ——
        // 那条路在「分析在途时删插件」下会把回调打在已析构对象上(见析构函数头注)。
        // AsyncUpdater 归 processor 自己所有,析构里 join + cancelPendingUpdate 能把它彻底堵死。
        {
            const juce::ScopedLock lock(owner_.pendingMutex_);
            owner_.pendingAnalysis_.result = std::move(result);
            owner_.pendingAnalysis_.rangeStartSample = config_.rangeStartSample;
            owner_.pendingAnalysis_.rangeEndSample = config_.rangeEndSample;
            owner_.pendingAnalysis_.generation = generation_;
            owner_.pendingAnalysis_.clearManual = owner_.analysisClearManual_;
            owner_.pendingAnalysis_.valid = true;
        }
        owner_.triggerAsyncUpdate();
    }

private:
    ScvbOutputAudioProcessor& owner_;
    std::array<scvb::analysis::PipelineTrackFeatures, 15> features_;
    scvb::analysis::PipelineConfig config_;
    std::uint32_t generation_ = 0;
};

// 退休作业的回收(定义必须在 AnalysisJob 类之后:unique_ptr 析构与 stopThread 都要完整类型)。
void ScvbOutputAudioProcessor::reapRetiredJobs()
{
    // 非阻塞:线程还在跑就留到下一拍。juce::Thread 的析构要求线程已停,故只析构停了的。
    for (auto it = retiredJobs_.begin(); it != retiredJobs_.end();)
    {
        if ((*it) != nullptr && (*it)->isThreadRunning())
        {
            ++it;
            continue;
        }
        it = retiredJobs_.erase(it);
    }
}

void ScvbOutputAudioProcessor::joinRetiredJobs()
{
    for (auto& job : retiredJobs_)
    {
        if (job != nullptr)
        {
            job->stopThread(2000);
        }
    }
    retiredJobs_.clear();
}

void ScvbOutputAudioProcessor::handleAsyncUpdate()
{
    PendingAnalysis pending;
    {
        const juce::ScopedLock lock(pendingMutex_);
        if (!pendingAnalysis_.valid)
        {
            return;
        }
        pending = std::move(pendingAnalysis_);
        pendingAnalysis_ = PendingAnalysis{};
    }

    // 代号不符 = 这份结果所属的作业已经被取消(或已被新作业顶替)→ 整份丢弃,不碰 CRVS。
    if (pending.generation != analysisGeneration_.load(std::memory_order_acquire))
    {
        return;
    }
    finishAnalysis(std::move(pending.result), pending.rangeStartSample, pending.rangeEndSample, pending.clearManual);
}

ScvbOutputAudioProcessor::AnalyzeAccepted ScvbOutputAudioProcessor::previewAnalysis(std::uint16_t tracksMask,
                                                                                    double startS, double endS)
{
    AnalyzeAccepted a;
    if (!(endS > startS))
    {
        return a; // ok=false + affected 全 0(§1.6 拒绝态行,不带 reason)
    }
    const juce::ScopedLock lock(lifecycleMutex_);
    const double hopS = featHopSeconds();
    const scvb::analysis::HopRange range{static_cast<std::uint64_t>(std::max(0.0, startS) / hopS),
                                         static_cast<std::uint64_t>(std::max(0.0, endS) / hopS)};
    if (range.end <= range.begin)
    {
        return a;
    }

    for (int t = 0; t < 15; ++t)
    {
        if (tracksMask != 0 && (tracksMask & (1u << t)) == 0)
        {
            continue;
        }
        if (!runtime_.channels[static_cast<std::size_t>(t)].enabled)
        {
            continue;
        }
        if (session_.frameStore().channel(static_cast<scvb::u32>(t + 1)).coveredHops(range) > 0)
        {
            ++a.tracks;
        }
        // 用户段(user_edited / locked)重分析不覆盖(ADR-008)—— 如实计数供确认条显示。
        const auto& segs = crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)]
                               .tracks[static_cast<std::size_t>(t)]
                               .segments;
        for (const auto& s : segs)
        {
            if (scvb::state::segmentOrigin(s.flags) != scvb::state::SegmentOrigin::Auto ||
                scvb::state::segmentLocked(s.flags))
            {
                ++a.manualKept;
            }
        }
    }
    a.ok = a.tracks > 0;
    return a;
}

ScvbOutputAudioProcessor::AnalyzeAccepted
ScvbOutputAudioProcessor::startAnalysis(std::uint16_t tracksMask, double startS, double endS, bool clearManual)
{
    AnalyzeAccepted a;
    if (analysisRunning_.load(std::memory_order_acquire))
    {
        a.busy = true; // §1.6/§7 manifest:analyze 唯一登记的 reason
        return a;
    }
    if (!isPrepared())
    {
        return a; // 未 prepare:按「无可分析数据」回 {ok:false, affected:{0,0,0}}
    }

    const juce::ScopedLock lock(lifecycleMutex_);

    const double sr = sampleRate_.load(std::memory_order_relaxed);
    const double hopS = featHopSeconds();
    const std::int64_t hopSamples = static_cast<std::int64_t>(std::llround(hopS * sr));
    if (hopSamples <= 0 || !(endS > startS))
    {
        return a;
    }

    const std::uint64_t firstHop = static_cast<std::uint64_t>(std::max(0.0, startS) / hopS);
    const std::uint64_t lastHop = static_cast<std::uint64_t>(std::max(0.0, endS) / hopS);
    if (lastHop <= firstHop)
    {
        return a;
    }
    const std::size_t numHops = static_cast<std::size_t>(lastHop - firstHop);

    // 取样:把范围内每轨的 kw/peak 拷成线程私有快照(30s × 15 轨 ≈ 180KB,量级可忽略)。
    std::array<scvb::analysis::PipelineTrackFeatures, 15> features;
    for (int t = 0; t < 15; ++t)
    {
        auto& f = features[static_cast<std::size_t>(t)];
        if (tracksMask != 0 && (tracksMask & (1u << t)) == 0)
        {
            continue;
        }
        if (!runtime_.channels[static_cast<std::size_t>(t)].enabled)
        {
            continue;
        }
        const auto& frames = session_.frameStore().channel(static_cast<scvb::u32>(t + 1));
        if (frames.coveredHops(scvb::analysis::HopRange{firstHop, lastHop}) == 0)
        {
            continue; // 该轨范围内无采集数据
        }

        f.kwMs.assign(numHops, 0.0f);
        f.peak.assign(numHops, 0.0f);
        f.covered.assign(numHops, 0u);
        for (std::size_t i = 0; i < numHops; ++i)
        {
            const std::uint64_t hop = firstHop + i;
            if (!frames.hasHop(hop))
            {
                continue; // 未覆盖 hop 留静音(0),VAD 自然判成静音
            }
            f.kwMs[i] = frames.kwMs(hop);
            f.peak[i] = frames.peak(hop);
            f.covered[i] = 1u;
            f.anyCovered = true;
        }
        if (f.anyCovered)
        {
            ++a.tracks;
        }
    }

    if (a.tracks == 0)
    {
        return a; // 范围 ∩ 覆盖 = ∅
    }

    // 配置:VAD/分段参数取 runtime state(桥面 setVadParams/setSegmentation 写的那份)。
    scvb::analysis::PipelineConfig cfg;
    cfg.sampleRate = sr;
    cfg.hopMs = static_cast<int>(scvb::output::OutputSession::featHopMs());
    cfg.rangeStartSample = static_cast<std::int64_t>(firstHop) * hopSamples;
    cfg.rangeEndSample = static_cast<std::int64_t>(lastHop) * hopSamples;
    // 单位换算(**两侧不是同一个量**,直接透传会让 VAD 一段都切不出来):
    //   · state 的 analysis.vad.threshold_db 是**绝对**静音门限,UI 档位 −60..−10 dB、默认 −38;
    //   · VadParams::thresholdDb 是**自适应基准之下的深度**(T_on = A − thresholdDb),默认 30。
    // 以两侧各自的默认值为锚做线性对位:depth = kVadUiRefDb − ui,取 kVadUiRef = −8 使
    // ui=−38 恰好落回 depth=30。方向也对:ui 越低(越想收更轻的声)→ depth 越大 → 门限越低。
    // 直接把 −45 当 depth 用会得到 T_on = A + 45 —— 门限**高过**基准,任何素材都过不去,
    // 于是分析跑完却零段(v4 实测 P0-1 的次生现象)。
    constexpr float kVadUiRefDb = -8.0f;
    cfg.vad.thresholdDb = juce::jlimit(1.0f, 80.0f, kVadUiRefDb - runtime_.vadThresholdDb);
    cfg.vad.hysteresisDb = runtime_.vadHysteresisDb;
    cfg.vad.hangoverMs = runtime_.vadHangoverMs;
    cfg.vad.paddingPreMs = runtime_.vadPaddingPreMs;
    cfg.vad.paddingPostMs = runtime_.vadPaddingPostMs;
    cfg.vad.minSegmentMs = runtime_.segmentationMinSegmentMs;
    cfg.segmentation.minSegmentMs = static_cast<double>(runtime_.segmentationMinSegmentMs);
    cfg.segmentation.sensitivity = static_cast<double>(runtime_.segmentationSensitivity);
    cfg.balance.panCurve = crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)].panCurve;
    // J69/02 §5.6:中心槽策略取 state(Tab4 那一档此前零消费方,拧了不生效)。
    if (runtime_.centerSlotPolicy == "lead_exclusive")
        cfg.assign.centerSlotPolicy = scvb::analysis::CenterSlotPolicy::LeadExclusive;
    else if (runtime_.centerSlotPolicy == "even_spread")
        cfg.assign.centerSlotPolicy = scvb::analysis::CenterSlotPolicy::EvenOffset;
    else
        cfg.assign.centerSlotPolicy = scvb::analysis::CenterSlotPolicy::PriorityQueue;

    // §1.6「重新识别(含手动段)」= clearManual:除了不再保留用户段(见 finishAnalysis),还必须
    // **把 freeze 位清零**。此前只清段不清位,于是:
    //   ① 指派层看见 freeze&1 仍为 1,把该轨当 manual 处理 → pan 直接取 currentPan(参数面上那个
    //      手动值)→ 新产出的 auto 段把手动值**原样烘焙**进去;
    //   ② DspArbiter 对冻结维度读的仍是 rawPan/rawVol,曲线怎么换都不影响声音。
    // 两条合起来就是 v5 实测 P0-3「点了重新识别、再分析或关冻结,pan 还是那个手动值」——
    // 关冻结之所以也没用,是因为曲线里已经是同一个数了。
    // 只对**本次真会被分析的轨**(在 scope 内、启用、范围内有覆盖)动手:范围内无数据的轨清了位
    // 就会掉进一条空曲线,那是把「保持现状」改成了静默丢值。
    if (clearManual)
    {
        for (int t = 0; t < 15; ++t)
        {
            if (!features[static_cast<std::size_t>(t)].anyCovered)
            {
                continue;
            }
            const auto* frzRaw =
                handles_.rawFrz[static_cast<std::size_t>(versionActive_ - 1)][static_cast<std::size_t>(t)];
            if (frzRaw == nullptr || juce::roundToInt(frzRaw->load(std::memory_order_relaxed)) == 0)
            {
                continue; // 本就未冻结:不发多余的 gesture(宿主自动化里会多出一个空写入点)
            }
            auto* p = apvts.getParameter(scvb::params::freezeId(versionActive_, t + 1));
            if (p == nullptr)
            {
                continue;
            }
            // gesture 三段式与 setTrackManual 的参数面写入同口径:宿主要看见一次完整的用户编辑,
            // 否则 Read 档下会把值当场顶回去。
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(0.0f));
            p->endChangeGesture();
        }
    }

    for (int t = 0; t < 15; ++t)
    {
        const auto& c = runtime_.channels[static_cast<std::size_t>(t)];
        auto& tc = cfg.tracks[static_cast<std::size_t>(t)];
        tc.enabled = c.enabled;
        tc.priority = static_cast<double>(c.priority);
        tc.pairId = c.pairId;
        tc.leadLock = c.leadLock;
        tc.leadVolExempt = c.leadVolExempt;
        tc.participateInAutoPan = c.participatesInAutoPan();
        tc.source =
            c.sourceChannels == 2 ? scvb::analysis::SourceChannels::Stereo : scvb::analysis::SourceChannels::Mono;
        const auto* frz = handles_.rawFrz[static_cast<std::size_t>(versionActive_ - 1)][static_cast<std::size_t>(t)];
        tc.freeze = frz != nullptr ? juce::jlimit(0, 3, juce::roundToInt(frz->load(std::memory_order_relaxed))) : 0;
        if (clearManual && features[static_cast<std::size_t>(t)].anyCovered)
        {
            tc.freeze = 0; // 上面刚清过位;不靠参数原子的回读时序,直接照本次意图取值
        }
        // 「现值」在 AutoAssign 里服务**两个分支**(AutoAssign.cpp:235-244),两者的权威面不同,
        // 必须**分开取源** —— 一视同仁会把另一个分支的权威顶掉:
        //   · freeze&1(冻结 pan)→ 权威是**参数面**。DspArbiter 对冻结维度读的就是 rawPan
        //     (setTrackManual 头注已裁定这一点)。用户在宿主里拧冻结 pan 是该维度上唯一有效的
        //     路径,若这里改取段表旧值,一点分析就把用户刚拧的值改写回去 —— 那正是清单上挂着的
        //     P1-D 那一族现象,不能在这轮再给它加一条新成因。
        //   · !participate(不参与)→ 权威是**段表**。这条轨压根不进指派,它该保持自己原来的
        //     声像;而参数面在从未打印过的工程里恒是 0,拿它当现值等于把每条不参与的轨按到
        //     正中再烘焙进段表(v5.1 实测 P0-B 的放大器)。
        //
        // 段表一侧取**与本次分析范围起点相交(否则最近)**的那一段,而不是整轨第一段:
        // 轨内 pan 随时间变化时,首段的值与范围所在处可能毫无关系。
        const auto* rawPan = handles_.rawPan[static_cast<std::size_t>(versionActive_ - 1)][static_cast<std::size_t>(t)];
        const double rawPanValue =
            rawPan != nullptr ? static_cast<double>(rawPan->load(std::memory_order_relaxed)) : 0.0;
        if ((tc.freeze & 1) != 0)
        {
            tc.currentPan = rawPanValue; // 冻结维度:参数面是权威
        }
        else
        {
            const auto& existing = crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)]
                                       .tracks[static_cast<std::size_t>(t)]
                                       .segments;
            tc.currentPan = rawPanValue;
            const std::int64_t rangeT0 = cfg.rangeStartSample;
            const scvb::state::Segment* best = nullptr;
            for (const auto& sg : existing)
            {
                if (sg.t0 <= rangeT0 && rangeT0 < sg.t1)
                {
                    best = &sg; // 命中范围起点所在段:直接用它
                    break;
                }
                if (sg.t1 <= rangeT0)
                {
                    best = &sg; // 起点之前最近的一段(段表按 t0 升序,越后越近)
                }
                else if (best == nullptr)
                {
                    best = &sg; // 范围整体落在首段之前:退而取首段
                    break;
                }
            }
            if (best != nullptr)
            {
                tc.currentPan = static_cast<double>(best->pan);
            }
        }
    }

    // 旧作业对象在**这里**回收,而不是在它自己的完成回调里 —— 在 run() 尚未返回时销毁
    // juce::Thread,debug 构建会撞 ~Thread 的 jassert(!isThreadRunning())。
    if (analysisJob_ != nullptr)
    {
        // 只 signal 不 join:join 在消息线程上最多堵 2 秒。退休区 + 每拍非阻塞回收。
        analysisJob_->signalThreadShouldExit();
        retiredJobs_.push_back(std::move(analysisJob_));
    }

    runtime_.analysisRunning = true;
    runtime_.analysisHasProgress = true;
    runtime_.analysisProgress.store(0.0f, std::memory_order_relaxed);
    analysisRunning_.store(true, std::memory_order_release);

    const std::uint32_t gen = analysisGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    analysisClearManual_ = clearManual;
    analysisJob_ = std::make_unique<AnalysisJob>(*this, std::move(features), cfg, gen);
    analysisJob_->startThread();

    a.ok = true;
    a.intervals = 0; // 受理回执:区间数要跑完才知道,不谎报
    return a;
}

void ScvbOutputAudioProcessor::cancelAnalysis()
{
    // 先 bump 代号:即便 run() 已经把结果放进槽并触发了 AsyncUpdater,那份结果的代号也已过期,
    // handleAsyncUpdate 会整份丢弃 —— 与「取消 = 结果整份丢弃,不碰 CRVS」的承诺对齐。
    analysisGeneration_.fetch_add(1, std::memory_order_acq_rel);

    if (analysisJob_ != nullptr)
    {
        // 同 startAnalysis:消息线程不 join(cancelAnalyze 是 web 直达的入口,堵它就是堵 UI)。
        analysisJob_->signalThreadShouldExit();
        retiredJobs_.push_back(std::move(analysisJob_));
    }
    {
        const juce::ScopedLock lock(pendingMutex_);
        pendingAnalysis_ = PendingAnalysis{};
    }
    const juce::ScopedLock lock(lifecycleMutex_);
    runtime_.analysisRunning = false;
    runtime_.analysisProgress.store(0.0f, std::memory_order_relaxed);
    analysisRunning_.store(false, std::memory_order_release);
}

void ScvbOutputAudioProcessor::finishAnalysis(scvb::analysis::PipelineResult result, std::int64_t rangeStartSample,
                                              std::int64_t rangeEndSample, bool clearManual)
{
    {
        const juce::ScopedLock lock(lifecycleMutex_);

        if (!result.cancelled)
        {
            auto& version = crvsData_.versions[static_cast<std::size_t>(versionActive_ - 1)];

            for (int t = 0; t < 15; ++t)
            {
                const auto& src = result.segments[static_cast<std::size_t>(t)];
                if (src.empty())
                {
                    continue; // 无产出的轨保持原样(不清空既有段)
                }
                auto& dst = version.tracks[static_cast<std::size_t>(t)].segments;

                // 保留两类既有段:
                //   ① 用户段(user_edited / user_created / locked)—— ADR-008「重分析不覆盖」;
                //   ② **完全落在本次分析范围之外**的 auto 段 —— 局部重分析只对
                //      (轨道 × 时间区间)失效,绝不能触碰其他区间的已有结果(ADR-008 / §4.4)。
                // 修复前这里是「本次产出 + 用户段」整表替换,于是「划了循环区 → 点分析」
                // 会把循环区**之外**先前分析出来的段全部静默抹掉,UI 上毫无提示。
                // 与范围**相交**的 auto 段(含跨边界者)由本次产出取代 —— 那正是被重分析的区间。
                std::vector<scvb::state::Segment> kept;
                for (const auto& sg : dst)
                {
                    // clearManual(§1.6 opts「重新识别(含手动段)」)= 连用户段一并重算:
                    // 用户读了二次确认文案、点了确认,就该真的清掉手动段 —— 此前 opts 整个被丢弃,
                    // 行为与普通分析逐字节相同,是「按钮亮着、点了没用」。范围外的段仍然保留。
                    const bool isUser =
                        !clearManual && (scvb::state::segmentOrigin(sg.flags) != scvb::state::SegmentOrigin::Auto ||
                                         scvb::state::segmentLocked(sg.flags));
                    const bool outsideRange = sg.t1 <= rangeStartSample || sg.t0 >= rangeEndSample;
                    if (isUser || outsideRange)
                    {
                        kept.push_back(sg);
                    }
                }

                std::vector<scvb::state::Segment> next;
                next.reserve(src.size() + kept.size());
                for (const auto& as : src)
                {
                    scvb::state::Segment seg;
                    seg.t0 = as.t0Samples;
                    seg.t1 = as.t1Samples;
                    seg.pan = juce::jlimit(-100.0f, 100.0f, static_cast<float>(as.pan));
                    seg.volDb = juce::jlimit(-24.0f, 12.0f, static_cast<float>(as.volDb));
                    seg.flags = scvb::state::makeSegmentFlags(scvb::state::SegmentOrigin::Auto, false);
                    // 与保留段重叠则让位(用户段优先;范围外 auto 段本就不该与范围内产出重叠)。
                    bool clash = false;
                    for (const auto& k : kept)
                    {
                        if (seg.t0 < k.t1 && k.t0 < seg.t1)
                        {
                            clash = true;
                            break;
                        }
                    }
                    if (!clash)
                    {
                        next.push_back(seg);
                    }
                }
                next.insert(next.end(), kept.begin(), kept.end());
                std::sort(next.begin(), next.end(),
                          [](const scvb::state::Segment& x, const scvb::state::Segment& y) { return x.t0 < y.t0; });
                dst = std::move(next);
            }

            // §1.6 撤销行逐字「否(分析产物变更不入撤销栈)」——**不走 commitCrvsTransaction**
            // (它内部 beginNewTransaction + perform,会把分析产物压进 undo)。直接写 + 重建曲线。
            // 顺带:入栈还会让 Ctrl+Z 以 reason:"undo" 重发段表,和分析完成的 reason:"analyze"
            // 打架,UI 的分析态更难对齐。
            rebuildAllCurves();
        }

        runtime_.analysisRunning = false;
        runtime_.analysisProgress.store(result.cancelled ? 0.0f : 1.0f, std::memory_order_relaxed);
        analysisDone_ = !result.cancelled; // editor 据此以 reason:"analyze" 重发段表(§2.8)
        crvsRevision_.fetch_add(1, std::memory_order_release);
    }
    analysisRunning_.store(false, std::memory_order_release);
}
