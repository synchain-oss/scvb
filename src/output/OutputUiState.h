// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputUiState —— 首启已读位在 PRMS(APVTS ValueTree)里的读写(T37 真机 bug A-3)。
//
// 为什么放 PRMS 而不是 CFGS:**STATE_SCHEMA §三 的 chunk 表把本模块经手的这几位只登记在
// PRMS 名下** —— `ui.guide_seen` / `ui.tour_seen` / `ui.lang_chosen`([J81]),外加下面那个
// `session_guid`([SL-215])。这条不依赖任何版本假设,是本模块的真正依据。
// (限 Output 侧:Input 的 `ui.guide_seen` 是**另一个**位 —— 契约上归 Input state 的 CFGS 尾扩
// (STATE_SCHEMA §三 Input 条,[J81]/J80),但**编码落点尚未落地**:当前 `InputStateCodec` 的
// payload 只到语言字节为止,且是 `kHeaderBytes + langBytes != size` 的严格等长、连尾部都不容忍。
// 别拿那一行来推翻这里,也别照它去 InputStateCodec 里找字段。)
// 别拿 `ui.scale` / `ui.language` 举证:那两个在 PRMS 与 CFGS 两行**都**登记着,证不出该放哪边。
//
// 当年(T37)还有一条机制上的理由,今天只剩一半,别再照旧口径记:那时 CFGS 的
// OutputStateCodec 是 `kHeaderBytes + langBytes != size` 的严格等长解码,尾部**加不进**字段,
// 要加就得升容器 abi;而 ValueTree 加属性不用动 abi。[J69/U24] 之后 CFGS 补了 unknownTail
// (已知字段之后的未知尾部解码保留、编码原样回写,正是给「未来小版本追加」留的口子,
// 见 docs/contract-changes/20260825-cfgs-persistence.md),所以**同 abi 内**两边如今都容忍
// 尾部/属性增删 —— 这条机制差已经不构成区分度,留着只当历史记录看。
//
// 当前长度纪律(CFGS):24 字节 header(6×u32)+ langBytes 语言字节是严格长度(`base > size`
// 即拒载),header 那 6 个 u32 的偏移至今冻结、不许就地插字段;其后 8 字节枚举尾**少了**才拒载
// (`0 < remaining < 8`),整段缺失(旧 abi=1 payload)回落默认,而**多出来**的字节走 unknownTail
// 保留回写、并不拒载。
//
// 背景(与字段该放哪一节无关):跨 abi 是**整块**拒载 —— `loadState` 的 abi 判读排在
// `decodeContainer` 之前就 return,pre-J69 构建(kCurrentAbi=1)读到 abi=2 的工程走 RejectedNewer,
// PRMS 和 CFGS 一起没进解码(原始字节由 preservedStateBlob_ 原样回写,工程不会被写坏)。
// 这对任何 abi=2 工程都无条件发生,所以它论证不了「所以放 PRMS」。
//
// ValueTree(XML)在**同 abi** 内两个方向都天生容忍字段增删:
//   • 旧构建读新工程 —— 多出来的这几个属性被忽略,其余参数照常加载;
//   • 新构建读旧工程 —— 属性不存在,这几位取默认 false。
// 无需升 abi、无需迁移函数;将来补 §1.31 的 ui.active_tab 同样零成本。

#include <juce_data_structures/juce_data_structures.h>

#include <string>

#include "state/FeaturesCodec.h" // isValidSessionGuid(不可信 state 字节的 guid 形状校验)

namespace scvb::output
{

// 属性名带 ui_ 前缀,与 APVTS 自己写在根节点上的参数子节点不同名(APVTS 的参数是**子节点**,
// 根节点属性面归本模块与将来的 ui.* 使用)。
inline const juce::Identifier kUiGuideSeenProp{"ui_guide_seen"};
inline const juce::Identifier kUiTourSeenProp{"ui_tour_seen"};
// 「用户显式选过语言」位:首启语言选择卡的唯一抑制条件。此前它只活在 web 的
// store.session 里(随 WebView 一起销毁),于是每次开窗都重新问一遍(v4 实测 P1-6)。
inline const juce::Identifier kUiLangChosenProp{"ui_lang_chosen"};

struct OutputUiFlags
{
    bool guideSeen = false;
    bool tourSeen = false;
    bool langChosen = false;
};

// 写入 APVTS 快照树的根节点(getStateInformation:copyState() 之后、序列化之前)。
inline void writeUiFlags(juce::ValueTree& apvtsState, const OutputUiFlags& flags)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    apvtsState.setProperty(kUiGuideSeenProp, flags.guideSeen, nullptr);
    apvtsState.setProperty(kUiTourSeenProp, flags.tourSeen, nullptr);
    apvtsState.setProperty(kUiLangChosenProp, flags.langChosen, nullptr);
}

// 从工程里解出的 APVTS 树读回(setStateInformation:replaceState 之前/之后皆可)。
// 属性缺失 = 老工程 / 从未落过盘 ⇒ 这几位为 false(= 该走首启)。
inline OutputUiFlags readUiFlags(const juce::ValueTree& apvtsState)
{
    OutputUiFlags flags;
    if (!apvtsState.isValid())
    {
        return flags;
    }
    flags.guideSeen = static_cast<bool>(apvtsState.getProperty(kUiGuideSeenProp, false));
    flags.tourSeen = static_cast<bool>(apvtsState.getProperty(kUiTourSeenProp, false));
    flags.langChosen = static_cast<bool>(apvtsState.getProperty(kUiLangChosenProp, false));
    return flags;
}

// [SL-215] 会话 GUID —— sidecar **目录**隔离的根基:SidecarStore 按 `<base>/sessions/<GUID>/`
// 分目录,目录内是固定的三个文件名 features.bin.gz / manifest.json / owner.lock(04 §5.4/§5.5)。
// 也就是说隔离靠的是目录名而非文件名 —— 同一 baseDir 下各会话各占一个 GUID 目录,互不覆盖。
// 它此前**根本没有生产落点**:桥面快照里写死一串全零字面量(OutputEditor 的 `session_guid`),
// 设置页于是恒显示 session 00000000-0000-0000-0000-000000000000。
//
// **生成点只有一处**:`ScvbOutputAudioProcessor` 构造期的 `juce::Uuid().toDashedString()`,
// 口径见 STATE_SCHEMA §4.3(该节点名的就是 juce::Uuid)。注意 `SidecarStore` 里另有一个
// `generateSessionGuid()` —— 它产出的也是合法 dashed v4 UUID,但**不是**本 GUID 的生成点,
// 目前只在 SidecarStore 内部(CoW 换新 GUID)被用到;别把两者当成同一个入口。
//
// 落在 PRMS 根节点属性面上,理由与上面三个 ui_ 位逐字相同(见本文件头注:STATE_SCHEMA §三
// 把它连同那三位一并登记在 PRMS 名下;同 abi 内 ValueTree 增删字段两个方向都容忍,无需升
// abi、无需迁移函数),也就不动 STATE_SCHEMA 的冻结布局。
inline const juce::Identifier kSessionGuidProp{"session_guid"};

inline void writeSessionGuid(juce::ValueTree& apvtsState, const juce::String& guid)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    apvtsState.setProperty(kSessionGuidProp, guid, nullptr);
}

// 读回并**校验形状**:state 字节不可信(§7.3),而这个 guid 会被 SidecarStore 拿去拼路径。
// 非 36 字符 dashed UUID 一律当「没有」处理,由调用方重新生成 —— 绝不把畸形串带进目录名。
inline juce::String readSessionGuid(const juce::ValueTree& apvtsState)
{
    if (!apvtsState.isValid())
    {
        return {};
    }
    const juce::String guid = apvtsState.getProperty(kSessionGuidProp, juce::String()).toString();
    return scvb::state::isValidSessionGuid(guid.toStdString()) ? guid : juce::String();
}

} // namespace scvb::output
