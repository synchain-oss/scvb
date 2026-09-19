// SPDX-License-Identifier: GPL-3.0-or-later
// test_mix_source —— ShmRingMixSource 读环语义(covered/换代/失准)+ MixMath DSP 原语单测。

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "output/BusXfade.h"
#include "output/MeterShot.h"
#include "output/MixMath.h"
#include "output/ShmRingMixSource.h"

using scvb::AudioRingHeader;
using scvb::kScvbAbi;
using scvb::kScvbMagic;
using scvb::u32;

// [SL-442] 「没有 pan 曲线」的 G 视图:两个表都为 null ⇒ panCurveGainDb 恒回 0 dB。
// 既有这四条断言的期望值一个都没改 —— 那正是「老工程零影响」的直接证据。
static const scvb::PanCurveXfade kNoCurve{};
using scvb::u64;
using scvb::output::ShmRingMixSource;

namespace
{
// 建一个已初始化的 2^k 环(heap,非共享内存),返回数据缓冲引用。
struct RingFixture
{
    AudioRingHeader header{};
    std::vector<float> data;

    explicit RingFixture(u32 frames = 64, u32 channels = 2)
    {
        header.magic.store(kScvbMagic, std::memory_order_release);
        header.abi.store(kScvbAbi, std::memory_order_release);
        header.sample_rate = 48000;
        header.ring_frames = frames;
        header.channels = channels;
        header.write_head_samples.store(0, std::memory_order_release);
        header.epoch.store(0, std::memory_order_release);
        data.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
    }
};
} // namespace

TEST_CASE("ShmRingMixSource bind 校验(非法几何拒绝)", "[mix][ring]")
{
    RingFixture f;
    ShmRingMixSource src;

    // channels=3 非法。
    f.header.channels = 3;
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());

    // ring_frames 非 2^k。
    f.header.channels = 2;
    f.header.ring_frames = 63;
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());

    // magic 不符。
    f.header.ring_frames = 64;
    f.header.magic.store(0x12345678, std::memory_order_release);
    src.bind(&f.header, f.data.data());
    REQUIRE_FALSE(src.bound());
}

TEST_CASE("ShmRingMixSource stereo interleaved 读取(covered)", "[mix][ring]")
{
    RingFixture f;
    for (u32 i = 0; i < 64; ++i)
    {
        f.data[static_cast<std::size_t>(i) * 2] = static_cast<float>(i);
        f.data[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(i * 100);
    }
    f.header.write_head_samples.store(64, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    REQUIRE(src.channels() == 2);
    REQUIRE(src.ringFrames() == 64);

    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));
    for (int i = 0; i < 8; ++i)
    {
        CHECK(out[static_cast<std::size_t>(i) * 2] == static_cast<float>(i));
        CHECK(out[static_cast<std::size_t>(i) * 2 + 1] == static_cast<float>(i * 100));
    }
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 冷启动(写方尚未追上)不计失准", "[mix][ring]")
{
    // T37 三轮 A 族回归:刚 attach 的空环 / 起播瞬间 / 宿主先渲染 Output 再渲染 Input,
    // 都表现为「write_head 还没覆盖本块」。这不是失准,是**尚未上线** —— 若计数,所有
    // 注入轨会在同一块同时 +1,UI 立刻报「5 轨检测到时间线缺口」(真机症状 L-6)。
    RingFixture f;
    f.header.write_head_samples.store(0, std::memory_order_release); // 写方一帧未写

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::vector<float> out(8 * 2, 1.0f);
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE_FALSE(src.read(0, out.data(), 8)); // 该块静音直通
    }
    CHECK(src.gapCount() == 0); // 一次都不计

    // 写方追上 → 首次成功读(primed),此后才进入失准判定。
    f.header.write_head_samples.store(8, std::memory_order_release);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 写头停滞 → 记饿读,不记失准(v5 P1-7)", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(8, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    // 先成功读一次:本代确有可读数据,此后 covered 失败才需要分类。
    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE(src.read(0, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 0);

    // 写头停滞而读位置前移 = **写方没在写**(bypass / 宿主在静音段跳过该轨)。
    // 这归 OutputSession 的 CH_SUSPENDED 判定管,**不是失准** —— 老实现在这里记缺口,
    // 于是每个静音边界都闪一次「失准」再自愈(v5 实测 P1-7)。
    REQUIRE_FALSE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 1);

    // 停滞持续:饿读继续留痕,失准仍然为 0。
    REQUIRE_FALSE(src.read(16, out.data(), 8));
    CHECK(src.gapCount() == 0);
    CHECK(src.stallFailCount() == 2);

    // 写头推进覆盖后恢复。
    f.header.write_head_samples.store(24, std::memory_order_release);
    REQUIRE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("ShmRingMixSource 写方套圈 → 真失准计数", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(8, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::vector<float> out(8 * 2, 1.0f);
    REQUIRE(src.read(0, out.data(), 8));
    REQUIRE(src.gapCount() == 0);

    // 写头远远越过读位置(超一整个环距)= 要读的数据已被覆盖 —— 这才是真失准,照计。
    // 与上一条用例的对照点:两者 covered 都失败,但物理成因相反,不能混判。
    f.header.write_head_samples.store(static_cast<scvb::u64>(f.header.ring_frames) * 4 + 8, std::memory_order_release);
    REQUIRE_FALSE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 1);
}

TEST_CASE("ShmRingMixSource epoch 跳变 → 本代数据从此刻起算", "[mix][ring]")
{
    RingFixture f;
    f.header.write_head_samples.store(64, std::memory_order_release);

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());

    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));

    // epoch 跳变后,validFrom = 当前块起点;旧区间(仍被写头覆盖但属旧代)按缺口处理。
    f.header.epoch.fetch_add(1, std::memory_order_release);
    // 写头保持 64:新代区间从 8 起算,[8,16) 被 64 覆盖 → 有效。
    REQUIRE(src.read(8, out.data(), 8));
    CHECK(src.gapCount() == 0);
}

TEST_CASE("MixMath ms_balance 端点语义", "[mix][math]")
{
    SECTION("ms_balance=0 → 矩阵恒等(逐位透传)")
    {
        float l = 0.25f;
        float r = -0.75f;
        scvb::output::applyMsBalance(0.0f, l, r);
        CHECK(l == 0.25f);
        CHECK(r == -0.75f);
    }

    SECTION("ms_balance=-100 → 纯 M(mono)")
    {
        float l = 0.5f;
        float r = -0.5f;
        scvb::output::applyMsBalance(-100.0f, l, r);
        CHECK(l == Catch::Approx(0.0f).margin(1e-6)); // M = (L+R)/2 = 0
        CHECK(r == Catch::Approx(0.0f).margin(1e-6));
    }

    SECTION("ms_balance=+100 → 纯 S(side)")
    {
        float l = 0.5f;
        float r = -0.5f;
        scvb::output::applyMsBalance(100.0f, l, r);
        CHECK(l == Catch::Approx(0.5f).margin(1e-6)); // S = (L-R)/2 = 0.5
        CHECK(r == Catch::Approx(-0.5f).margin(1e-6)); // R' = -S = -0.5
    }
}

TEST_CASE("MixMath mono equal-power pan", "[mix][math]")
{
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixMonoSample(1.0f, 0.0f, 0.0f, 100.0f, 1.0f, kNoCurve, l, r);
    CHECK(l == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(r == Catch::Approx(0.70710678f).margin(1e-5));

    float hl = 0.0f;
    float hr = 0.0f;
    scvb::output::mixMonoSample(1.0f, -100.0f, 0.0f, 100.0f, 1.0f, kNoCurve, hl, hr); // 硬左
    CHECK(hl == Catch::Approx(1.0f).margin(1e-5));
    CHECK(hr == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("MixMath stereo dual-pan + width", "[mix][math]")
{
    // width=100、pan=0 → L 源 → L、R 源 → R(源宽度原样,不互换)。
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.5f, 0.0f, 0.0f, 100.0f, 100.0f, 1.0f, kNoCurve, l, r);
    CHECK(l == Catch::Approx(1.0f).margin(1e-5)); // 子声像 P_L=-100:gL_L=1、gL_R=0
    CHECK(r == Catch::Approx(0.5f).margin(1e-5)); // 子声像 P_R=+100:gR_L=0、gR_R=1

    // width=0 → 双子声像重合(塌成 mono)。
    float ml = 0.0f;
    float mr = 0.0f;
    scvb::output::mixStereoSample(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 1.0f, kNoCurve, ml, mr);
    CHECK(ml == Catch::Approx(0.70710678f).margin(1e-5));
    CHECK(mr == Catch::Approx(0.70710678f).margin(1e-5));
}

TEST_CASE("ShmRingMixSource bind/unbind 与 read 并发不崩(原子快照)", "[mix][ring][concurrency]")
{
    RingFixture f(64, 2);
    f.header.write_head_samples.store(64, std::memory_order_release);
    for (u32 i = 0; i < 64; ++i)
    {
        f.data[static_cast<std::size_t>(i) * 2] = static_cast<float>(i);
        f.data[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(i);
    }

    ShmRingMixSource src;
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());

    std::atomic<bool> stop{false};
    std::vector<float> buf(8 * 2);

    std::thread reader([&] {
        int64_t t0 = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            (void)src.read(t0, buf.data(), 8);
            t0 = (t0 + 8) & 63; // 在 64 帧环内循环(covered)
        }
    });

    // 主线程反复 bind/unbind 抖动;reader 并发 read —— 原子快照下不得空指针解引用/撕裂。
    for (int i = 0; i < 2000; ++i)
    {
        src.unbind();
        src.bind(&f.header, f.data.data());
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    // 重新稳定绑定,确认仍可读(无永久损坏)。
    src.unbind();
    src.bind(&f.header, f.data.data());
    REQUIRE(src.bound());
    std::vector<float> out(8 * 2);
    REQUIRE(src.read(0, out.data(), 8));
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] == 0.0f);
}

TEST_CASE("BusXfade 直通⇄混音等功率交叉(无别名/无 +3dB 泵感)", "[mix][busxfade]")
{
    scvb::output::BusXfade xf;
    xf.prepare(48000.0, 80.0);
    const int n = 512;

    std::vector<float> in(static_cast<std::size_t>(n), 1.0f); // 直通源 = 单位 DC
    std::vector<float> mix(static_cast<std::size_t>(n), 0.0f); // 自有混音 = 静音(模拟最后一混音块)
    std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
    const float* ip[2] = {in.data(), in.data()};
    const float* mp[2] = {mix.data(), mix.data()};
    float* op[2] = {out.data(), out.data()};

    // 1) 稳态直通:theta=0 → render(false) 零开销,不写输出。
    out.assign(static_cast<std::size_t>(n), 7.0f);
    xf.render(op, ip, mp, n, false);
    REQUIRE(xf.isSettledPassthrough());
    REQUIRE(out[0] == 7.0f);

    // 2) 直通→混音:out = in*cos(θ)+mix*sin(θ)=cos(θ)(in=1,mix=0),单调降且恒 ≤1(无 +3dB 泵感)。
    xf.render(op, ip, mp, n, true);
    REQUIRE_FALSE(xf.isSettledMix());
    float prev = 2.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = out[static_cast<std::size_t>(i)];
        REQUIRE(v <= 1.0f + 1e-6f);
        REQUIRE(v <= prev + 1e-6f); // cos 单调降
        prev = v;
    }

    // 3) 推至稳态混音:余下样本直接拷贝 mix(此处 mix=0)。
    for (int k = 0; k < 20; ++k)
    {
        xf.render(op, ip, mp, n, true);
    }
    REQUIRE(xf.isSettledMix());
    REQUIRE(out[0] == 0.0f); // mix=0 → 稳态输出 0

    // 4) 混音→直通:out = cos(θ)(θ 从 π/2 降),单调升、恒 ≤1;首样本≈mix(无硬切)。
    xf.render(op, ip, mp, n, false);
    REQUIRE_FALSE(xf.isSettledPassthrough());
    prev = -1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float v = out[static_cast<std::size_t>(i)];
        REQUIRE(v <= 1.0f + 1e-6f);
        REQUIRE(v >= prev - 1e-6f); // cos 单调升
        prev = v;
    }
    REQUIRE(out[0] == Catch::Approx(0.0f).margin(1e-4f)); // 首样本≈mix=0,与上一块稳态混音连续(无阶跃)
}

// ---------------------------------------------------------------------------
// T37 三轮 B 族回归:电平快照通道(scvb.meters 的数据面)。
// 真机症状 L-2/L-3:识别出了轨道数,但玻璃管液柱一直最低、电平表不跳、无峰值条。
// 根因是 emitMeters 把 15 轨与总线全部硬编码在 -60dB 地板(T29 占位),叠加 0.3dB 阈值门
// 后该事件首帧发一次就再不发。web 侧一直是好的,缺的是这条从音频线程上来的数据。
// ---------------------------------------------------------------------------

TEST_CASE("MeterShot:seqlock 往返 + 静音发布回地板(T37 三轮 B 族)", "[meters][t37]")
{
    scvb::output::MeterShot shot;

    // 未发布过:读到全零 = 静音。emitMeters 的 toDb(0) 落 -60dB 地板,液柱在底部。
    scvb::output::MeterPod pod{};
    REQUIRE(shot.read(pod));
    CHECK(pod.trackRms[0] == 0.0f);
    CHECK(pod.busPeak[1] == 0.0f);

    // 发布一帧真实电平(线性幅度;dB 换算刻意留在消息线程)。
    scvb::output::MeterPod tx{};
    tx.trackRms[0] = 0.5f;
    tx.trackPeak[0] = 0.75f;
    tx.trackRms[14] = 0.125f;
    tx.busRms[0] = 0.25f;
    tx.busPeak[1] = 1.0f;
    shot.publish(tx);

    scvb::output::MeterPod rx{};
    REQUIRE(shot.read(rx));
    CHECK(rx.trackRms[0] == 0.5f);
    CHECK(rx.trackPeak[0] == 0.75f);
    CHECK(rx.trackRms[14] == 0.125f);
    CHECK(rx.busRms[0] == 0.25f);
    CHECK(rx.busPeak[1] == 1.0f);

    // publishSilentMeters 的语义:发全零 → 电平表落回地板,而不是冻在上一块的值。
    shot.publish(scvb::output::MeterPod{});
    REQUIRE(shot.read(rx));
    CHECK(rx.trackRms[0] == 0.0f);
    CHECK(rx.trackPeak[0] == 0.0f);
    CHECK(rx.busPeak[1] == 0.0f);

    // 写者在临界区内(seq 为奇)时读方必须报撕裂,由调用方沿用上帧而不是拿到半新半旧的值。
    shot.seq.fetch_add(1, std::memory_order_release);
    CHECK_FALSE(shot.read(rx));
    shot.seq.fetch_add(1, std::memory_order_release);
    CHECK(shot.read(rx));
}

// =============================================================================
// [SL-442] pan 角度域曲线 G 进实时链(02 §8.1 步骤 5)—— 施加环节的判据。
// 主判据是 §7 那句原话:「UI 与 DSP 共用本实现……杜绝『画的和听的不一致』」。
// 接上 G 之前这句只成立一半(前端有、DSP 没有),下面钉的就是缺的那一半。
// =============================================================================

namespace
{

// bell A=-9, P0=+30, Q=6(Δ=100/6≈16.67)。三个选择都是为了让判据钉得住:
//   A=-9   —— 深槽,「施加 / 不施加」的差别远大于任何容差;
//   P0=+30 —— 不在中心,「查名义角」与「查有效角」才会落到曲线上差很多的两处;
//   Q=6    —— 钟够窄。Q=3(Δ=33.33)时名义角 +60 与有效角 +30 只差 3.87 dB,
//             我第一版就栽在这儿:差值不够大,「这个输入能否分辨两种实现」的前置断言当场红。
//             Q=6 时这个差拉到 8 dB。
std::vector<scvb::PanCurvePoint> sl442Curve()
{
    scvb::PanCurvePoint p;
    p.angle = 30.0f;
    p.gainDb = -9.0f;
    p.shape = scvb::PanCurveShape::bell;
    p.q = 6.0f;
    p.side = scvb::PanCurveSide::out;
    return {p};
}

void monoPair(float pan, float volDb, float globalWidth, const scvb::PanCurveXfade& curve, float& l, float& r)
{
    l = 0.0f;
    r = 0.0f;
    scvb::output::mixMonoSample(1.0f, pan, volDb, globalWidth, 1.0f, curve, l, r);
}

// 实时链实际施加的 dB = 有曲线 / 无曲线 的**能量**比。
// ⚠ 不能只拿单个通道做分母:equal-power 下 pan=+100 时 gL = cos(π/2),float 里是 -0.0f,
// 比值当场没意义(第一版我就是这么写的,被 `bare > 0` 那条前置断言拦下)。
// 而 gL²+gR² ≡ 1(ADR-010),所以能量比恰好就是增益比,与 pan 落在哪儿无关。
double monoAppliedDb(float pan, float globalWidth, const scvb::PanCurveXfade& curve)
{
    float l = 0.0f;
    float r = 0.0f;
    float bl = 0.0f;
    float br = 0.0f;
    monoPair(pan, 0.0f, globalWidth, curve, l, r);
    monoPair(pan, 0.0f, globalWidth, kNoCurve, bl, br);
    const double withG = std::sqrt(static_cast<double>(l) * l + static_cast<double>(r) * r);
    const double bare = std::sqrt(static_cast<double>(bl) * bl + static_cast<double>(br) * br);
    return 20.0 * std::log10(withG / bare);
}

// stereo:**只喂一路源**(另一路给 0),于是输出里只剩那一个子声像的贡献,
// 比值恰好等于该子声像各自的 G 增益。两路一起喂的话,单个输出通道里混着两个子声像,
// 比值是两者的加权和 —— 钉不住「每个子声像各查各的」(第一版我就是这么写的,推理太松)。
double stereoSubImageAppliedDb(bool useLeftSource, float pan, float trkWidth, float globalWidth,
                               const scvb::PanCurveXfade& curve)
{
    const float sL = useLeftSource ? 1.0f : 0.0f;
    const float sR = useLeftSource ? 0.0f : 1.0f;
    float l = 0.0f;
    float r = 0.0f;
    scvb::output::mixStereoSample(sL, sR, pan, 0.0f, trkWidth, globalWidth, 1.0f, curve, l, r);
    float bl = 0.0f;
    float br = 0.0f;
    scvb::output::mixStereoSample(sL, sR, pan, 0.0f, trkWidth, globalWidth, 1.0f, kNoCurve, bl, br);
    // 取两个输出通道的能量比,免得挑到 equal-power 增益恰为 0 的那一端。
    const double withG = std::sqrt(static_cast<double>(l) * l + static_cast<double>(r) * r);
    const double bare = std::sqrt(static_cast<double>(bl) * bl + static_cast<double>(br) * br);
    return 20.0 * std::log10(withG / bare);
}

} // namespace

TEST_CASE("SL442-MIX-1 空曲线 / 未接线时实时链逐位不变(老工程零影响)", "[mix][math][pancurve]")
{
    // 空点列表烘出来的表,和「压根没有表」必须走到同一个结果 —— 而且是**逐位**,不是近似:
    // 增益 = 10^(G/20),G 恰为 0 时因子恰为 1.0f,乘法按位恒等。
    scvb::PanCurveLut empty;
    empty.rebuild({});
    const scvb::PanCurveXfade withEmpty{&empty, nullptr, 1.0f};

    for (const float pan : {-100.0f, -37.5f, 0.0f, 30.0f, 100.0f})
    {
        for (const float vol : {-12.0f, 0.0f, 6.0f})
        {
            for (const float gw : {0.0f, 50.0f, 100.0f, 150.0f})
            {
                float bl = 0.0f;
                float br = 0.0f;
                float cl = 0.0f;
                float cr = 0.0f;
                monoPair(pan, vol, gw, kNoCurve, bl, br);
                monoPair(pan, vol, gw, withEmpty, cl, cr);
                REQUIRE(cl == bl); // 逐位,无容差
                REQUIRE(cr == br);
            }
        }
    }

    // stereo 同理 —— 它的求和式结构最容易在重构里被拆成 a*c+b*c 而丢掉按位恒等。
    for (const float w : {0.0f, 35.0f, 100.0f})
    {
        float bl = 0.0f;
        float br = 0.0f;
        scvb::output::mixStereoSample(0.75f, -0.3f, 20.0f, -4.0f, w, 80.0f, 1.0f, kNoCurve, bl, br);
        float cl = 0.0f;
        float cr = 0.0f;
        scvb::output::mixStereoSample(0.75f, -0.3f, 20.0f, -4.0f, w, 80.0f, 1.0f, withEmpty, cl, cr);
        REQUIRE(cl == bl);
        REQUIRE(cr == br);
    }
}

TEST_CASE("SL442-MIX-2 画的就是听的:实时链施加的增益 == UI 那条曲线", "[mix][math][pancurve]")
{
    const auto points = sl442Curve();
    scvb::PanCurveLut lut;
    lut.rebuild(points);
    const scvb::PanCurveXfade curve{&lut, nullptr, 1.0f};

    // 全局 width=100 ⇒ P_eff == P,此时「实时链施加的 dB」可以直接和解析式对。
    for (const double pan : {-100.0, -30.0, 0.0, 30.0, 63.33, 100.0})
    {
        // 容差 0.05 dB:LUT 插值上限 0.03(02 §7.3 CURVE-4)+ float 往返余量。
        // 被测量跨 -9..0 dB,谷底 -9 dB 是容差的 180 倍 —— 「同值」不可能靠容差蒙到。
        REQUIRE(monoAppliedDb(static_cast<float>(pan), 100.0f, curve)
                == Catch::Approx(scvb::evalCurve(points, pan)).margin(0.05));
    }
    // 判别量:曲线底部确实压下去了 9 dB,不是压了个 0(全 0 的实现会让上面那圈全绿)。
    REQUIRE(monoAppliedDb(30.0f, 100.0f, curve) == Catch::Approx(-9.0).margin(0.05));
}

TEST_CASE("SL442-MIX-3 G 查在 P_eff 上,不是名义角(width≠100 才分得出)", "[mix][math][pancurve]")
{
    const auto points = sl442Curve(); // 谷底在 P=+30
    scvb::PanCurveLut lut;
    lut.rebuild(points);
    const scvb::PanCurveXfade curve{&lut, nullptr, 1.0f};

    // ---- mono:全局 width=50 ⇒ P_eff = P/2。取名义 P=+60 ⇒ P_eff=+30 = 谷底 ----
    // 查有效角 → 施加 -9 dB(谷底);查名义角 → 施加 evalCurve(+60) ≈ -1 dB。
    const double atNominal = scvb::evalCurve(points, 60.0);
    const double atEffective = scvb::evalCurve(points, 30.0);
    REQUIRE(std::fabs(atNominal - atEffective) > 5.0); // 先确认这个输入真的能分辨两种实现

    const double appliedDb = monoAppliedDb(60.0f, 50.0f, curve);
    REQUIRE(appliedDb == Catch::Approx(atEffective).margin(0.05)); // 是有效角
    REQUIRE(appliedDb != Catch::Approx(atNominal).margin(0.05)); // 且确实不是名义角

    // ---- stereo:两个子声像各查各的 ----
    // pan=0、w_t=60、全局 width=50 ⇒ 名义子声像 (-60,+60) → 缩放后 (-30,+30)。
    // 右子声像正落谷底(-9 dB),左子声像几乎不受影响(≈0 dB)。
    const double dLeftSub = stereoSubImageAppliedDb(true, 0.0f, 60.0f, 50.0f, curve);
    const double dRightSub = stereoSubImageAppliedDb(false, 0.0f, 60.0f, 50.0f, curve);
    REQUIRE(dLeftSub == Catch::Approx(scvb::evalCurve(points, -30.0)).margin(0.05));
    REQUIRE(dRightSub == Catch::Approx(scvb::evalCurve(points, 30.0)).margin(0.05));

    // 可分辨性:拿弧中心查一次再共用的实现,两个子声像会得到**同一个**增益。
    // 这条要求两者差得够开 —— 差不开的话上面两条对那种实现也可能同时成立。
    REQUIRE(std::fabs(dLeftSub - dRightSub) > 5.0);
    // 而共用弧中心的实现会给出 evalCurve(P_eff=0),两边都是这个值 —— 与上面两条都不符。
    const double atArcCentre = scvb::evalCurve(points, 0.0);
    REQUIRE(std::fabs(dRightSub - atArcCentre) > 5.0);
}
