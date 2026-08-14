// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "analysis/PanCurve.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scvb::analysis
{

// 源声道类型(J57/J60)。
enum class SourceChannels : uint8_t
{
    Mono = 0,
    Stereo = 1,
};

// 中心槽策略(J69,02 §5.6)。三档枚举与默认值只在 02 定义一次(T12/T13 同源)。
enum class CenterSlotPolicy : uint8_t
{
    PriorityQueue = 0, // 按优先级排队(默认 = 方案 A)
    LeadExclusive = 1, // 主唱独占
    EvenOffset = 2, // 均分微偏
};

// J69 ④:未知/越界 center_slot_policy 回落默认档(priority_queue)并记 warning(03 §6.3 字符串层语义的枚举等价)。
CenterSlotPolicy sanitizeCenterSlotPolicy(int rawValue, bool* warnedOut = nullptr);

// 单轨分析期元数据(02-dsp-spec §5.1 输入;持久化真源 = state channels[] 配置,由 T19 编码)。
// 本卡只消费「槽位/指派」所需的字段;z / leadVolExempt / width / source 透传给 T13(平衡)。
struct TrackMeta
{
    int channelIndex = 0; // 全局 channel 序:ε 平局与 pair「低序取左」用。
    double priority = 5.0; // [0,10];p̂ = priority/10 ∈ [0,1](02 §5.3)。
    double z = 1.0; // 段响度线性能量(§4;T13 平衡的总能量口径)。
    double zL = 0.0; // stereo L 通道能量(02 §6.1;mono 忽略:z_iL=z、z_iR=0)。
    double zR = 0.0; // stereo R 通道能量;两字段均 0(未设)且 stereo → 等分 z_iL=z_iR=z/2。
    int pairId = 0; // 0=无对;>0 = 与同 pairId 的另一轨互为镜像对(超级节点)。
    bool leadLock = false; // 集合 C(分析期主唱锁):恒 P=0、不占槽(02 §5.2)。
    bool leadVolExempt = false; // J58 独立、零联动(本卡只透传)。
    int freeze = 0; // J65:0=全自动;1=冻结 pan;2=冻结 vol;3=全冻结(pan 维冻结 = bit0)。
    bool participateInAutoPan = true; // J60:mono 默认 true、stereo 默认 false(默认值归 state 层)。
    SourceChannels source = SourceChannels::Mono; // 透传(参与/默认值由 participate 布尔承载)。
    double currentPan = 0.0; // 现值(manual / nonpart 轨保持此值,不被改写)。
    double width = 0.0; // 每轨张开度(stereo 源;点语义 → 不进代价函数,只透传)。
    bool hasPrev = false; // 是否携带上一区间解;false → w_cont/w_side 项为 0(首区间)。
    double prevPan = 0.0; // P_i^{t−1}(上一区间名义 pan)。
};

// 内部权重(02 §0.3 出厂默认)。全部可由测试/上层覆盖。
struct AutoAssignConfig
{
    double pBase = 60.0; // P_base(width=100% 最大分配 pan)。
    double minSep = 8.0; // 槽位最小间隔(P 单位)。
    double wPrio = 1.0; // 优先级项权重。
    double wCont = 0.6; // 连续性项权重(上一区间)。
    double wSide = 0.8; // 跨侧翻转项权重。
    double wLink = 100.0; // link 硬约束(已由超级节点预合并,恒 0,保留字段)。
    double wBal = 0.3; // 平衡感知二次指派项(02 §5.3/T13 第二趟)。
    double epsilonBase = 1e-6; // ε 平局扰动基值(02 §5.3;实现见 .cpp)。
    CenterSlotPolicy centerSlotPolicy = CenterSlotPolicy::PriorityQueue; // J69(02 §5.6)。
    double deltaCenter = 4.0; // even_offset 的 δ_c = minSep/2(02 §5.6/§0.3)。
};

// 槽位(名义域,width=100% 基准;02 §5.2)。
struct Slot
{
    double pan = 0.0; // 名义 pan 值。
    int ringRank = 0; // 环序:1=最外;中心 = 最内环序(奇数分支)。
    bool isCenter = false;
};

// 每全局区间指派结果(02 §5.1 输出中的 pan 部分;u 由 T13 solveBalance 填充)。
struct AssignResult
{
    std::vector<double> pans; // 每轨名义 pan(与输入 tracks 同序;fixed=0,manual/nonpart=现值)。
    std::vector<double> u; // 每轨音量修正 dB(本卡恒 0;T13 填充)。
    std::vector<Slot> slots; // 生成的槽位(按 §5.2 冻结排序:环外→内、同环先左后右、中心最后)。
    double totalCost = 0.0; // 指派代价(仅 w_prio/w_cont/w_side/w_link,不含 ε)。
    std::string reason; // 退化文案,如「独唱段居中」;正常为空。
    bool widthWarning = false; // 宽度不足(§5.4,UI 提示「宽度不足以容纳 n 轨」)。
};

// 平衡感知二次指派提示(02 §5.3 第二趟,由 T13 回退链 step 2 填充)。
// 第二趟代价加 wBal · signD0 · zHat[i] · ρ(P(j)):把响的轨推向弱侧。
struct BalanceAwareHint
{
    double signD0 = 0.0; // sign(D₀)(u=0 预估)。
    std::vector<double> zHat; // ẑ_i = z_i/Σz(与 tracks 同序)。
};

// 平衡参数(02 §6.2/§6.4)。
struct BalanceConfig
{
    double tol = 0.3; // 收敛容差 LU。
    int maxIters = 6; // 最大迭代数。
    double eta = 1.0; // 学习率 η。
    double uMax = 4.0; // u_max(dB)。
    double volSoftMin = -18.0; // vol_soft_window 下界。
    double volSoftMax = 9.0; // vol_soft_window 上界。
    double uHardMin = -24.0; // 参数硬范围下界(仅文档/截断参照)。
    double uHardMax = 12.0; // 参数硬范围上界。
    std::vector<PanCurvePoint> panCurve; // 曲线 G(P);空 → G≡0(c≡1)。
    double deltaMax = 15.0; // δ ∈ [−15,+15](§6.4 step 3)。
    double deltaPrecision = 0.1; // δ 步进精度。
    int deltaBinaryIters = 20; // δ 二分 ≤20 次。
};

// 平衡结果(02 §6.2/§6.4)。
struct BalanceResult
{
    std::vector<double> uBalance; // 平衡解(共模前);BAL-1/2/4/7 断言它。
    std::vector<double> u; // 最终 u(共模 + 软窗截断后)。
    std::vector<double> pans; // 最终每轨名义 pan(solveBalance 回显输入;回退链含 δ 平移结果)。
    double delta = 0.0; // 共模 Δ。
    double d0 = 0.0; // 解前 D。
    double dBalance = 0.0; // 平衡迭代后 D(共模前);BAL-4 的「residualD≈4」即此值。
    double dFinal = 0.0; // 共模后 D;converged 判据(§6.2/BAL-5)。
    bool converged = false; // |dFinal| < tol。
    bool capped = false; // 平衡迭代中任一 adjustable 轨 u 触 ±u_max。
    int iters = 0; // 实际迭代数。
    int fallbackLevel = 1; // 回退链最终所在级(1..4)。
    std::vector<int> fallbackPath; // 已尝试的级(如 [1,2,3,4])。
    std::vector<std::string> warnings; // 软窗截断 / 回退链 4 警告文案。
};

// §5.2 槽位生成(默认 center_slot_policy = priority_queue = 方案 A)。
// n = 自由轨折算数(超级节点计 2),值域 [0,15](J59)。n=0 → 空槽。
// hasLeadLock:区间内是否存在 lead_lock 轨(C≠∅;仅 lead_exclusive 档用,02 §5.6)。
// widthWarningOut 非空时写「宽度不足」提示标志。
std::vector<Slot> generateSlots(int n, const AutoAssignConfig& cfg, bool* widthWarningOut = nullptr,
                                bool hasLeadLock = false);

// §5.1/§5.3 单全局区间指派:
//   fixed(lead_lock)= P0 不占槽;manual(freeze pan 维)= 保持现值;nonpart(!participate)= 保持现值;
//   free(含 pair 超级节点)= 经匈牙利(无 pair)或枚举分支法(有 pair,§5.3)指派到槽位。
//   balHint 非空 → 第二趟平衡感知代价(§5.3/T13 回退链 step 2)。
// 确定性:同输入逐位一致(ε 平局;pair 枚举取最小代价、平局取环序字典序小者)。
AssignResult assignInterval(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& cfg,
                            const BalanceAwareHint* balHint = nullptr);

// §6.2 核心平衡迭代 + 共模:pans 与 tracks 同序(每轨名义 pan)。不触发回退链。
BalanceResult solveBalance(const std::vector<TrackMeta>& tracks, const std::vector<double>& pans,
                           const BalanceConfig& cfg);

// §6.4 完整平衡 + 四级回退链:1) solveBalance 收敛 → 完成;2) 第二趟平衡感知指派 → 重新
// solveBalance;3) 最外非 pair 环 δ 平移(BAL-8 跳过 pair 环);4) 无杠杆 → UI 警告保留当前解。
BalanceResult solveBalanceWithFallback(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& assignCfg,
                                       const BalanceConfig& balanceCfg);

} // namespace scvb::analysis
