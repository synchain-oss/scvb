// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputAuthority.h"

#include <algorithm>
#include <chrono>

#include "SegmentEditService.h" // configureCrvsUndoBudget:CRVS 撤销预算的唯一真源

namespace
{

// 逐字段比对两份点列表。用逐字段而不是 memcmp:结构体有 enum 成员与可能的填充字节,memcmp 会
// 被填充里的垃圾骗成「不同」。
// ⚠ 用 `==` 比较 float 安全的前提是调用方**先**过了 arePanCurvePointsUsable(NaN 的 `==` 恒假,
//   NaN 能走到这儿就会恒判「变了」)。失败方向是安全的:精确相等判错只会「多重建一次」,
//   不会「漏掉一次真改动」—— 反过来写(给容差)才危险。
bool samePanCurvePoints(const std::vector<scvb::PanCurvePoint>& a, const std::vector<scvb::PanCurvePoint>& b)
{
    const auto samePoint = [](const scvb::PanCurvePoint& x, const scvb::PanCurvePoint& y) {
        return x.angle == y.angle && x.gainDb == y.gainDb && x.shape == y.shape && x.q == y.q && x.side == y.side;
    };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), samePoint);
}

double steadyMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 版本重命名撤销动作:捕获旧名,undo 时还原。
class RenameVersionAction final : public juce::UndoableAction
{
public:
    RenameVersionAction(scvb::engine::VersionStore& store, int version, std::string newName)
        : m_store(store), m_version(version), m_newName(std::move(newName)), m_oldName(store.versionName(version))
    {
    }

    bool perform() override
    {
        m_store.setVersionName(m_version, m_newName);
        return true;
    }

    bool undo() override
    {
        m_store.setVersionName(m_version, m_oldName);
        return true;
    }

    int getSizeInUnits() override { return 1; }

private:
    scvb::engine::VersionStore& m_store;
    int m_version = 0;
    std::string m_newName;
    std::string m_oldName;
};

} // namespace

namespace scvb::output
{

void OutputAuthority::prepare(double sampleRate, const scvb::params::ParamHandles& handles)
{
    m_handles = handles;
    m_arbiter.prepare(sampleRate);
    // version_active 是用户 state,prepare(含采样率变化重入)不重置它。
    m_prepared = true;
    rebindSources();
}

void OutputAuthority::setVersionActive(int version)
{
    const int before = m_versions.versionActive();
    m_versions.setVersionActive(version); // 越界钳制 + warning 计数(不静默取模)
    if (m_versions.versionActive() == before)
        return; // 版本号未变则不重发快照(避免虚假 versionChanged),预览也原样留着
    // [J157] 预览属于旧版本:换过去就作废。只靠 rebindSources 按版本号挑表不够 ——
    // 切回旧版本时那张陈旧预览会原样复活,而那一刻已经没有人在拖。
    (void)dropPanCurvePreview();
    if (m_prepared)
        rebindSources();
}

OutputAuthority::OutputAuthority()
{
    // 撤销预算的**唯一**装配点。
    //
    // 本文件匿名 namespace 里的 `RenameVersionAction` 的 getSizeInUnits 恒回 1,在这套字节口径下
    // 等于「几乎不占预算」—— 对**这个 action** 是正确的:它持有的只是新旧两个名字,与整表 CRVS
    // 快照不在一个量级(版本复制原先也有一个同类 action,随零调用点的 `copyVersion` 一并删除,SL-510)。
    // ⚠ 别据此以为「版本改名/复制对撤销预算免费」:**生产路径**上的
    // `ScvbOutputAudioProcessor::setVersionName` / `copyVersion` 走的是 `commitCrvsTransaction`
    // → `CrvsTransactionAction`,同样**按整表段数记全字节**。这里说的只是 authority 自带的
    // 版本层 action(#152 第三轮复审【建议】3)。
    scvb::output::configureCrvsUndoBudget(m_undoManager);
}

int OutputAuthority::versionActive() const
{
    return m_versions.versionActive();
}

void OutputAuthority::setCurve(int version, int track, const scvb::CurveEvaluator* curve)
{
    // 不可变契约:VersionStore 深拷贝 src 进新建对象再替换,绝不原地改写已发布曲线;
    // 曲线生命周期由 VersionStore 与发布出的快照(shared_ptr)共同保证。
    m_versions.setCurve(version, track, curve);
    if (version == m_versions.versionActive() && m_prepared)
        rebindSources();
}

void OutputAuthority::setPanCurve(int version, const std::vector<scvb::PanCurvePoint>& points)
{
    if (version < 1 || version > kNumVersions)
        return; // 越界拒绝(与 setVersionActive 的钳制不同:这里没有「合理的邻近值」可退)

    // [SL-442 第2轮] **进表之前**挡住不可用的点 —— 这是「数据进实时链」的最后一道闸。
    // 两条入口(桥面 / 解码)各自也挡了,这里是**兜底**:将来任何新入口(复制版本、脚本、
    // preset 导入……)接上来时,不必记得去补校验 —— 烘表这一步一定会走到。
    // 不在音频线程里逐样本判有限性:那是热路径,而且那时已经晚了(表已经被污染)。
    //
    // ⚠ **这条兜底是静默的**:没有返回值、没有计数器、没有日志。走到它的时候
    //   `crvsData_.versions[v].panCurve` 已被事务写成新点列表 ⇒ **界面画的是新曲线、
    //   存盘存的是新曲线,而音频线程仍在用上一张表**,没有任何东西会说出这件事。
    // ⚠ 它**今天走不到**,靠的是上游两道守卫先各自挡住:
    //     · 桥面 `parsePanCurvePointsArg`(BridgeArgs.h;setPanCurve 与 previewPanCurve 两个
    //       handler 共用)—— 坏点 → badArg,整次提交被拒;
    //     · 解码 `scvb::state::decodeCrvs` —— 坏点 → 整份 state 拒载。
    // ⚠ **上游任意一道放宽,这里就变成「画的和听的不一致」且无人知晓。**
    //   「今天不可达」这种断言会自己过期,而过期时没有任何东西会红 —— 所以这里记的是
    //   它**依赖谁**,不是它安不安全。放宽上游的人请连这一处一起看。
    if (!scvb::arePanCurvePointsUsable(points))
        return; // 整表拒绝,保留上一张表 —— 宁可曲线不更新,也不让母线收到 NaN

    auto& baked = m_panCurvePoints[static_cast<std::size_t>(version - 1)];

    // 逐字段比对上次烘过的点列表;相同 ⇒ 整个调用 no-op(不重建、不重发、LUT 对象指针不变)。
    // 这条不只是省 CPU:`rebuildAllCurves` 在**每次**段编辑 / 改 ramp / 撤销重做时都会跑到这儿,
    // 而 pan_curve 绝大多数时候没动 —— 无谓重建会让 LUT 指针每次都变,把「这次到底换表没有」
    // 这个判据抖成恒真。为什么逐字段、为什么 `==` 比 float 安全,见 samePanCurvePoints。
    // ⚠ **前提是上面那道有限性守卫排在它前面** —— 别调换这两段的顺序。
    // ⚠ [J157] no-op 这条路**不碰预览**:段编辑在拖动途中跑到这儿(分析完成、另一处改了段)
    //   时,预览必须原样留着 —— 撤掉的话音频弹回已提交那张,而手指停着就不会再有新预览。
    if (samePanCurvePoints(baked, points))
        return;

    // 不可变契约(与 setCurve 同一套):新建一张表、烘满之后才让任何人看见,
    // 绝不原地重建已经发布出去的那张 —— 音频线程可能正在读它。
    //
    // [J157] 松手提交的点表 == 正在发布的那份预览 ⇒ **沿用那张表**:同一组点烘出来本来就逐位
    // 相同(rebuild 是确定的),沿用省掉一次烘表(16 个 bell 约 16 ms),而且 LUT 指针不变 ⇒
    // 音频线程判「没换表」、不开淡入窗口 ⇒ 松手那一下输出逐位不变。
    std::shared_ptr<const scvb::PanCurveLut> lut;
    if (m_preview.live && m_preview.liveVersion == version && m_preview.lut != nullptr &&
        samePanCurvePoints(m_preview.livePoints, points))
    {
        lut = m_preview.lut;
    }
    else
    {
        auto fresh = std::make_shared<scvb::PanCurveLut>();
        fresh->rebuild(points); // 点列表为空 → 全 0 dB ⇒ G≡0 ⇒ 增益恒 1
        lut = std::move(fresh);
    }
    m_panCurveLut[static_cast<std::size_t>(version - 1)] = std::move(lut);
    baked = points;

    // [J157] 这一版的**已提交**曲线变了(松手提交 / 撤销重做 / 载入工程)⇒ 这一版上的预览作废:
    // 预览是「叠在旧的已提交曲线上的一次拖动」,底换了它就没有意义了。别的版本上的预览不动
    // (只可能是 live=false 的残留记账:换版本时已经作废过)。
    if ((m_preview.live && m_preview.liveVersion == version) ||
        (m_preview.pending && m_preview.pendingVersion == version))
        (void)dropPanCurvePreview();

    // 非活动版本:只更新本地那张,不发快照;换到该版本时 rebindSources 自会取走。
    if (version == m_versions.versionActive() && m_prepared)
        rebindSources();
}

OutputAuthority::PanCurvePreviewRequest
OutputAuthority::requestPanCurvePreview(int version, const std::vector<scvb::PanCurvePoint>& points)
{
    if (version != m_versions.versionActive())
        return PanCurvePreviewRequest::staleVersion;
    // 与 setPanCurve 同一道闸,理由同那里:预览表同样进实时链,坏点在这里拦就不会有「音频线程
    // 读到 NaN」这回事。这里拦下是**有声的**(返回值 → 桥面 badArg),不是 setPanCurve 那种静默兜底。
    if (!scvb::arePanCurvePointsUsable(points))
        return PanCurvePreviewRequest::badPoints;
    m_preview.pending = true;
    m_preview.pendingVersion = version;
    m_preview.pendingPoints = points;
    return PanCurvePreviewRequest::accepted;
}

bool OutputAuthority::dropPanCurvePreview()
{
    const bool wasLive = m_preview.live;
    m_preview.pending = false;
    m_preview.pendingPoints.clear();
    m_preview.live = false;
    m_preview.livePoints.clear();
    m_preview.lut.reset();
    return wasLive;
}

void OutputAuthority::cancelPanCurvePreview()
{
    if (dropPanCurvePreview() && m_prepared)
        rebindSources(); // 按已提交那张重发;音频线程见 LUT 指针变了,照常 30 ms 淡回
}

bool OutputAuthority::pumpPanCurvePreview(double nowMs)
{
    auto& p = m_preview;
    if (!p.pending)
        return false;
    if (p.pendingVersion != m_versions.versionActive())
    {
        // 请求之后换过版本(setVersionActive 已经 drop 过,这里只是兜底):这份点表属于旧版本。
        p.pending = false;
        p.pendingPoints.clear();
        return false;
    }
    if (!m_prepared)
        return true; // 没 prepare 就没有音频线程在听;留着,等 prepare 之后的下一拍
    // ① 限速(≤ 20 Hz;烘表贵时更低,见 kPanCurvePreviewMaxDuty)。
    if (p.stats.publishes > 0 && nowMs - p.lastPublishMs < p.stats.intervalMs)
        return true;
    // ② 回收闸:上一份预览快照音频线程还没越过 ⇒ 不发。**这一条是「音频线程停着时池不涨」的
    //    全部依据** —— 删掉它,拖动期间每 50 ms 就多钉一张 128 KiB 的表直到音频再跑起来。
    if (p.lastSeq != 0 && m_arbiter.oldestHeldSeq() < p.lastSeq)
        return true;

    const int v = p.pendingVersion;
    const auto& committedPoints = m_panCurvePoints[static_cast<std::size_t>(v - 1)];
    if (p.live && samePanCurvePoints(p.livePoints, p.pendingPoints))
    {
        // 与正在发布的那份相同:什么都不用做(一拍里来回拖回同一点)。
        p.pending = false;
        p.pendingPoints.clear();
        return false;
    }
    std::shared_ptr<const scvb::PanCurveLut> lut;
    if (samePanCurvePoints(committedPoints, p.pendingPoints))
    {
        // 拖回了已提交的样子:借用已提交那张,不烘(可能是 null ⇒ G≡0,与已提交同义)。
        lut = m_panCurveLut[static_cast<std::size_t>(v - 1)];
    }
    else
    {
        const auto clock = p.bakeClock ? p.bakeClock : std::function<double()>(&steadyMs);
        const double t0 = clock();
        auto fresh = std::make_shared<scvb::PanCurveLut>();
        fresh->rebuild(p.pendingPoints);
        const double bakeMs = std::max(0.0, clock() - t0);
        lut = std::move(fresh);
        ++p.stats.bakes;
        p.stats.lastBakeMs = bakeMs;
        p.stats.maxBakeMs = std::max(p.stats.maxBakeMs, bakeMs);
        // 占空比上限:下一次最早在 bakeMs / 10% 之后。典型曲线(≤ 4 个 bell,≤ 4 ms)仍是 50 ms;
        // 16 个 bell(约 16 ms)拉到约 160 ms ⇒ 约 6 Hz。宁可极端曲线预览粗一点,也不让宿主界面
        // 线程在拖动期间被吃掉三分之一。
        p.stats.intervalMs = std::max(kPanCurvePreviewMinIntervalMs, bakeMs / kPanCurvePreviewMaxDuty);
    }

    p.live = true;
    p.liveVersion = v;
    p.livePoints = std::move(p.pendingPoints);
    p.lut = std::move(lut);
    p.pending = false;
    p.pendingPoints.clear();

    rebindSources();
    p.lastSeq = m_nextSnapshotSeq; // 刚发的那份
    p.lastPublishMs = nowMs;
    ++p.stats.publishes;
    return false;
}

scvb::engine::SetNameResult OutputAuthority::setVersionName(int version, const juce::String& name)
{
    std::string effective;
    const scvb::engine::SetNameResult result =
        scvb::engine::normalizeVersionName(version, name.toStdString(), effective);
    if (result == scvb::engine::SetNameResult::InvalidIndex)
        return result;

    // 归一化后与当前名一致 → 无操作(不产生空撤销事务)。
    if (m_versions.versionName(version) == effective)
        return result;

    m_undoManager.beginNewTransaction("Rename V" + juce::String(version));
    m_undoManager.perform(new RenameVersionAction(m_versions, version, effective));

    return result;
}

juce::String OutputAuthority::versionName(int version) const
{
    return juce::String(m_versions.versionName(version));
}

juce::ValueTree OutputAuthority::toState() const
{
    // T18 版本层 state 最小往返(名称 + active)。T19 将把这些字段并入 CRVS chunk(StateCodec)。
    juce::ValueTree state("SCVB-VERSIONS");
    state.setProperty("v1_name", juce::String(m_versions.versionName(1)), nullptr);
    state.setProperty("v2_name", juce::String(m_versions.versionName(2)), nullptr);
    state.setProperty("active", m_versions.versionActive(), nullptr);
    return state;
}

void OutputAuthority::fromState(const juce::ValueTree& state)
{
    if (!state.hasType("SCVB-VERSIONS"))
        return;

    for (int v = 1; v <= kNumVersions; ++v)
    {
        const juce::String key = "v" + juce::String(v) + "_name";
        const juce::String fallback = juce::String(m_versions.versionName(v));
        m_versions.setVersionName(v, state.getProperty(key, fallback).toString().toStdString());
    }

    const int before = m_versions.versionActive();
    m_versions.setVersionActive(static_cast<int>(state.getProperty("active", m_versions.versionActive())));
    if (m_versions.versionActive() != before)
        (void)dropPanCurvePreview(); // [J157] 同 setVersionActive:预览属于旧版本

    if (m_prepared)
        rebindSources();
}

int OutputAuthority::warningCount() const
{
    return m_versions.warningCount();
}

std::array<const scvb::CurveEvaluator*, OutputAuthority::kNumTracks> OutputAuthority::activeCurves() const
{
    std::array<const scvb::CurveEvaluator*, kNumTracks> out{};
    const int v = m_versions.versionActive();
    for (int t = 0; t < kNumTracks; ++t)
    {
        out[static_cast<std::size_t>(t)] = m_versions.curve(v, t);
    }
    return out;
}

std::shared_ptr<const scvb::PanCurveLut> OutputAuthority::activePanCurveLut() const
{
    return m_panCurveLut[static_cast<std::size_t>(m_versions.versionActive() - 1)];
}

std::array<scvb::engine::DspArbiter::TrackValues, OutputAuthority::kNumTracks>
OutputAuthority::processBlock(bool engineAuthority, double tSec)
{
    return m_arbiter.processBlock(engineAuthority, tSec);
}

std::array<scvb::engine::DspArbiter::TrackValues, OutputAuthority::kNumTracks> OutputAuthority::nextSample()
{
    return m_arbiter.nextSample();
}

void OutputAuthority::rebindSources()
{
    const int v = m_versions.versionActive() - 1; // 0-based

    // 构建不可变快照,完整构造后原子发布;旧快照进池保活,音频线程确认后在下面释放([SL-445])。
    auto snap = std::make_unique<scvb::engine::DspArbiter::Snapshot>();
    const auto curves = m_versions.snapshotCurves(m_versions.versionActive());
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& s = snap->sources[static_cast<std::size_t>(t)];
        s.rawPan = m_handles.rawPan[v][t];
        s.rawVol = m_handles.rawVol[v][t];
        s.rawTrkW = m_handles.rawTrkW[v][t];
        s.rawFrz = m_handles.rawFrz[v][t];
        s.curve = curves[static_cast<std::size_t>(t)]; // shared_ptr 保活曲线(不可变契约)
    }
    snap->rawLeadSelect = m_handles.rawLeadSelect;
    // 活动版本的 G 查表(02 §8.1 步骤 5)。从没设过 pan_curve 的版本这里是 null ⇒ 音频线程按 G≡0 走。
    // [J157] 拖动预览在发布中 ⇒ 用预览那张。**所有**重发(段编辑、prepare、换 ramp……)都走到这里,
    // 所以拖动途中别的东西触发一次重发,也不会把音频弹回已提交那张。
    const bool previewHere = m_preview.live && m_preview.liveVersion == v + 1;
    snap->panCurveLut = previewHere ? m_preview.lut : m_panCurveLut[static_cast<std::size_t>(v)];

    snap->seq = ++m_nextSnapshotSeq;

    m_arbiter.publish(snap.get()); // release-store
    m_snapshotPool.push_back(std::move(snap));

    // [SL-445] 回收:音频线程已确认不会再碰的(seq 严格小于它报的数)从队头放掉。刚发布的那份
    // seq 最大、不可能小于确认值(确认值只来自已发布的快照),所以 size>1 只是多一道保险。
    const std::uint64_t held = m_arbiter.oldestHeldSeq();
    while (m_snapshotPool.size() > 1 && m_snapshotPool.front()->seq < held)
        m_snapshotPool.pop_front();
}

} // namespace scvb::output
