// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// 单轨分析期元数据(02-dsp-spec §5.1 输入;持久化真源 = state channels[] 配置,由 T19 编码)。
// 本卡只消费「槽位/指派」所需的字段;z / leadVolExempt / width / source 透传给 T13(平衡)。
struct TrackMeta
{
    int channelIndex = 0; // 全局 channel 序:ε 平局与 pair「低序取左」用。
    double priority = 5.0; // [0,10];p̂ = priority/10 ∈ [0,1](02 §5.3)。
    double z = 1.0; // 段响度线性能量(§4;本卡只透传,平衡由 T13)。
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
    double wBal = 0.3; // 平衡感知二次指派项(T13 第二趟,本卡未用)。
    double epsilonBase = 1e-6; // ε 平局扰动基值(02 §5.3;实现为 Monge 交叉项,见 .cpp)。
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

// §5.2 槽位生成(默认 center_slot_policy = priority_queue = 方案 A)。
// n = 自由轨折算数(超级节点计 2),值域 [0,15](J59)。n=0 → 空槽。
// widthWarningOut 非空时写「宽度不足」提示标志。
std::vector<Slot> generateSlots(int n, const AutoAssignConfig& cfg, bool* widthWarningOut = nullptr);

// §5.1/§5.3 单全局区间指派:
//   fixed(lead_lock)= P0 不占槽;manual(freeze pan 维)= 保持现值;nonpart(!participate)= 保持现值;
//   free(含 pair 超级节点)= 经匈牙利(无 pair)或枚举分支法(有 pair,§5.3)指派到槽位。
// 确定性:同输入逐位一致(ε 平局;pair 枚举取最小代价、平局取环序字典序小者)。
AssignResult assignInterval(const std::vector<TrackMeta>& tracks, const AutoAssignConfig& cfg);

} // namespace scvb::analysis
