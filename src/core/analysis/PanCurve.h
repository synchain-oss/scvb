// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>

// PanCurve:pan 角度域增益曲线 G: P ∈ [−100,+100] → dB(02 §7)。
// bell/shelf/cut 三种形状,2049 点 LUT;UI 与 DSP 共用本实现(曲线同源)。
namespace scvb
{

enum class PanCurveShape : int
{
    bell = 0,
    shelf = 1,
    cut = 2
};

// [J07] shelf/cut 的方向;bell 忽略 side。
enum class PanCurveSide : int
{
    out = 0,
    left = 1,
    right = 2
};

// params-v0 v1 的 points[] 单点:{angle(=P₀), gain_db(=A), shape, q, side}。
struct PanCurvePoint
{
    float angle = 0.0f; // P₀ ∈ [−100, +100]
    float gainDb = 0.0f; // A(dB)
    PanCurveShape shape = PanCurveShape::bell;
    float q = 1.5f; // bell/shelf 为 Q ∈ [0.5, 10];cut 为 slope(dB/oct,6|12|18|24,默认 12)
    PanCurveSide side = PanCurveSide::out;
};

// LUT 点数:−100..+100,步长 ≈0.0061(32769 点,奇数保证 0° 恰为格点)。
// cut slope 的 smoothstep 斜坡在角度域极窄(A=−3,s=24 → u_b=0.125 oct,斜坡仅 ~0.09°),
// 2049 点(步长 0.0977)在该区间只有 ~1 个采样点,线性插值最坏误差可超 0.03 dB(CI 实测 0.034)。
// 提密度至 32769(步长 0.0061)后斜坡内 ~15 点,最坏误差 ≈0.010 dB(模拟实测),
// 稳过 02 §7.2 的 ≤0.03 dB 上限(留 ≥2× 余量)。
inline constexpr int kPanCurveLutSize = 32769;

// cut slope 模型的侧向距离地板:d = max(|d|, d0),d0 = 1.0(度)。
// d<d0(切点自身及 1° 内)→ G=0,斜坡自 1° 起。
inline constexpr double kPanCurveCutD0Deg = 1.0;

// 曲线整体 clamp 边界(02 §7.1)。
inline constexpr float kPanCurveMinDb = -24.0f;
inline constexpr float kPanCurveMaxDb = 12.0f;

// [SL-442 第2轮] 点的有限性 / 值域守卫 —— **两条入口共用这一份**,别各写各的。
//
// 为什么在 SL-442 才加:此前 pan_curve 只喂分析与 UI,坏数据的后果是「分析结果不对 / 画歪」;
// 接进实时链之后,同一份坏数据会**变成灌进宿主母线的 NaN**。
// ⇒ 把一个数据从非实时路搬进实时路时,它的信任要求必须跟着搬 —— 这条守卫就是那个要求。
//
// ⚠ 承重的是**有限性**,不是值域:NaN 会一路穿过 clampDb(`NaN < min` 与 `NaN > max` 都是
//    false,于是原样返回),进 LUT,再乘进样本。值域只是额外收窄。
// ⚠ 「看起来挡住了」比「没挡」更危险:桥面原有的
//    `p.q <= 0.0f || p.angle < -100.0f || p.angle > 100.0f` 读起来像覆盖了 angle 与 q,
//    但 **`NaN <= 0`、`NaN < -100`、`NaN > 100` 三个比较全为 false** —— NaN 从每一条里穿过去。
//    所以必须**显式** isfinite,不能靠范围比较兜底。
//
// 值域取真源,不写字面量(SL-416 判例:字面量与规格各写一份必漂):
//   · angle ∈ [-100, +100] —— 宪法 params-v0「pan_curve.points[]: {angle: -100..100, ...}」;
//   · q > 0                —— 02 §7.1(Δ = 100/Q;cut 的 q 承载 slope,契约 q>0);
//   · gain_db              —— **宪法未声明每点值域**,故只查有限性。JS 编辑器的 ±12
//                             (curve-editor.js GAIN_DB_MIN/MAX)是 **UI 交互约束**,拿它去
//                             拒绝格式上合法的工程会误伤;整条曲线的 [-24,+12] 由 evalCurve
//                             的 clampDb 兜 —— 那是求值后的 clamp,不是入口校验,两回事。
inline constexpr float kPanCurvePointAngleMin = -100.0f; // params-v0 pan_curve.points[].angle
inline constexpr float kPanCurvePointAngleMax = 100.0f;

bool isPanCurvePointUsable(const PanCurvePoint& point);

// 整表校验:任一点不合格即 false。**整表拒绝,不逐点丢弃** —— 静默丢点会让用户画的曲线
// 少一块而没有任何提示,比拒绝整次提交更难排查。
bool arePanCurvePointsUsable(const std::vector<PanCurvePoint>& points);

// 半宽 Δ = 100 / Q(P 单位)。
double panCurveHalfWidth(float q);

// 单点形状求值(dB,不 clamp)。
// bell:u = (P − P₀)/Δ(side 忽略);
// shelf:u 由 side 决定 —— right → (P − P₀)/Δ、left → (P₀ − P)/Δ、
//        out → P₀≥0 按 right / P₀<0 按 left(对任意 P₀ 确定性求值,无未定义区)。
// cut:slope 模型 —— d = 侧向距离(right→P−P₀、left→P₀−P、out→sign(P₀) 定),
//      d = max(|d|, d0)(d0=1°),u = log2(d/d0),u_b = |A|/s(s=point.q),
//      G = A·smoothstep(u/u_b) 并 clamp 到 [A, 0];d≤0(保留侧)→ G=0。
double evalShape(const PanCurvePoint& point, double pan);

// 整条曲线解析求值:Σ 各点 + clamp 到 [−24, +12] dB。
double evalCurve(const std::vector<PanCurvePoint>& points, double pan);

// 2049 点 LUT:参数变化时重建(消息线程),音频线程只读;clamp + 线性插值。
class PanCurveLut
{
public:
    PanCurveLut() { m_lut.fill(0.0f); }

    void rebuild(const std::vector<PanCurvePoint>& points);

    // §7.2 gainDb:clamp + 线性插值。
    float gainDb(float pan) const;

    // 供 WebView bridge / 测试读取原始 LUT(不插值)。
    const float* data() const noexcept { return m_lut.data(); }

private:
    std::array<float, kPanCurveLutSize> m_lut;
};

// [SL-442] 换表瞬间的交叉淡入视图(02 §8.1 步骤 5,窗口时长取 03 §2.4 的 30ms 切换档)。
//
// 为什么需要它:G 是 P 的静态映射,pan 移动时 G 跟着 P 连续变,不会咔哒。但**曲线本身被改**
// (或换版本)时,同一个 P 上的增益会**瞬时跳变**。实测单次 setPanCurve 提交的跳变上界
// 是 12 dB(单点 gain 值域 ±12,拖到底就能打满),最温和的手势(滚轮拧一格 Q)也有 ~1 dB
// —— 没有「小到不可闻」那一档,所以换表必须淡入。
//
// 窗口外 previous 恒为 null,`panCurveGainDb` 退化成只查一张表,与不做淡入时逐位相同。
struct PanCurveXfade
{
    const PanCurveLut* target = nullptr; // 新表;null → G≡0
    // 旧表。**null 有两种含义,靠 `fading` 区分** —— 这正是第一版的缺陷所在:
    // 当时拿 `previous == nullptr` 兼作「窗口关着」,而 null 同时也是「从没画过曲线」的状态,
    // 于是**用户第一次画曲线那一次恰好走了不淡入的分支** —— 花整轮避免的 12 dB 硬跳,
    // 原样留在了最常见的入口上。
    const PanCurveLut* previous = nullptr; // 窗口内的旧表;null + fading=true ⇒ 从 G≡0 淡入
    float mix = 1.0f; // previous→target 的权重,0=全旧、1=全新;窗口外恒 1
    bool fading = false; // 窗口是否开着。**唯一**的窗口判据,别再用 previous 是否为 null 代替
};

// 音频线程:clamp + 线性插值 +(窗口内)dB 域交叉淡入。零分配、零锁、无分支跳表。
// 在 **dB 域**而不是线性域插值,与 02 §8.2 对 v 的口径一致(「v(t) = lerp(...) 在 dB 域插值」);
// 凸组合的直接好处是:P 固定时,窗口内的值必落在新旧两表该点取值围成的区间内,不会超调。
// (P 同时在动时两个输入一起变,那时的轨迹不由这条保证 —— 见 tests 里的 SL-442 合成轨迹格。)
inline float panCurveGainDb(const PanCurveXfade& x, float pan) noexcept
{
    const float now = (x.target != nullptr) ? x.target->gainDb(pan) : 0.0f;
    if (!x.fading)
    {
        return now; // 窗口外:只查一张表,与不做淡入时逐位相同
    }
    // 窗口内。previous==null 表示**从 G≡0 淡入** —— 第一次画曲线走的就是这条,
    // 它同样要淡,不能直接跳到 now。
    const float before = (x.previous != nullptr) ? x.previous->gainDb(pan) : 0.0f;
    return before + (now - before) * x.mix;
}

} // namespace scvb
