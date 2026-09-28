// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputUiState —— 首启已读位在 PRMS(APVTS ValueTree)里的读写(T37 真机 bug A-3),
// 以及同处的 `session_guid`([SL-215])与 `ui.active_tab`([J148])。
//
// 为什么放 PRMS 而不是 CFGS:**STATE_SCHEMA §三 的 chunk 表把本模块经手的这几位只登记在
// PRMS 名下** —— `ui.guide_seen` / `ui.tour_seen` / `ui.lang_chosen`([J81]) / `ui.active_tab`,
// 外加下面那个 `session_guid`([SL-215])。这条不依赖任何版本假设,是本模块的真正依据。
// (限 Output 侧:Input 的 `ui.guide_seen` 是**另一个**位 —— 契约上归 Input state 的 CFGS 尾扩
// (STATE_SCHEMA §三 Input 条,[J81]/J80),但**编码落点尚未落地**:当前 `InputStateCodec` 的
// payload 只到语言字节为止(4×u32 头 + langBytes),且是 `kHeaderBytes + langBytes != size` 的
// 严格等长、连尾部都不容忍。[SL-411 R5] 顺手核过:该断言在 `InputStateCodec.cpp` 的
// `decodeInputState` 里逐字成立(那一行今天仍在),**本 PR 把容器 abi 推到 4 之后它也没变** ——
// Input 侧没有新的 CFGS 尾档,别把「共用容器 abi」与「两边布局同形」混成一件事。
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
// 即拒载),header 那 6 个 u32 的偏移至今冻结、不许就地插字段;其后是**五级尾档**
// ([SL-472] 更新:abi=1 时是「8 字节枚举尾少了才拒载」,SL-279 推到 16、SL-411 推到 28、
// SL-416 推到 52、SL-472 推到 1912,下面这句才是今天的口径)——
//   · 尾长 0(= 旧 abi=1 payload,整段缺失)→ 全部尾字段回落默认;
//   · 尾长 8(`loudness_mode` + `center_slot_policy`)/ 16(再 + `applied.*`)/ 28(再 + `segmentation.*`)/
//     52(再 + `vad.*` 五项与 `transition_ramp_ms`)/ 1912(再 + `channels[15]` 七项,15 × 124 字节)
//     —— 各档**内**少一个字节即拒载:`(0,8)` / `(8,16)` / `(16,28)` / `(28,52)` / `(52,1912)` 五个空洞
//     一律整块拒载([SL-279 复审] 起的纪律,SL-411 R8 把措辞收敛成「档内不许半截」);
//   · **≥ 1912 之后**多出来的字节走 unknownTail 保留回写、并不拒载(任意长度都收)。
// 真源 = `src/core/state/OutputStateCodec.h` 头注与 `tests/core/test_output_session.cpp` 的
// 长度断言(`24u + 2u + 52u + 1860u` 等);这里只记「它已经不是 8 字节那版口径」这件事,别再按 8 字节推理。
//
// 背景(与字段该放哪一节无关):跨 abi 是**整块**拒载 —— `loadState` 的 abi 判读排在
// `decodeContainer` 之前就 return,pre-J69 构建(kCurrentAbi=1)读到 abi=2 的工程走 RejectedNewer,
// PRMS 和 CFGS 一起没进解码(原始字节由 preservedStateBlob_ 原样回写,工程不会被写坏)。
// 这对任何 abi=2 工程都无条件发生,所以它论证不了「所以放 PRMS」。
//
// ValueTree(XML)在**同 abi** 内两个方向都天生容忍字段增删:
//   • 旧构建读新工程 —— 多出来的这几个属性被忽略,其余参数照常加载;
//   • 新构建读旧工程 —— 属性不存在,这几位取默认 false。
// 无需升 abi、无需迁移函数。§1.31 的 `ui.active_tab` 就是这么补上的([J148],见文件末那一节)。

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <cstdint>
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

// [J150] channels[15].auto_label —— 每轨「最近一次自动填进 label 的 DAW 轨道名」(没自动填过为空串)。
// 载入时 label 为空或与它相等 ⇒ 继续跟随;不等 ⇒ 用户命名。
// 编码:一个 JSON 数组(15 个字符串),落在 PRMS 根节点属性面而不是 CFGS 的 channels 档:
//   · CFGS 是定长布局,加字段要么升容器 abi(Input / Output / Monitor 三个插件共用这一个 abi,升了之后
//     **三个**插件的新工程在旧构建里都整块拒载),要么走尾部 unknownTail;
//   · 存「名字」而不存「是不是用户起的」一位标志,是为了**自证**:旧构建不认识这个属性,却会经
//     APVTS replaceState / copyState 把它原样带回来 —— 若存的是标志位,用户在旧构建里改过名字、回到
//     新构建时标志还说「自动」,轨道名就会把用户的名字覆盖掉;存名字时 label 已经不等于它,仍判成
//     用户命名。
// 与 session_guid / ui_* 同一条理由(同 abi 内两个方向都容忍增删,无需升 abi、无需迁移函数)。
inline const juce::Identifier kAutoLabelsProp{"channels_auto_label"};
inline constexpr int kAutoLabelCount = 15;

inline void writeAutoLabels(juce::ValueTree& apvtsState, const std::array<juce::String, kAutoLabelCount>& labels)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    juce::Array<juce::var> arr;
    for (const auto& l : labels)
    {
        arr.add(l);
    }
    apvtsState.setProperty(kAutoLabelsProp, juce::JSON::toString(juce::var(arr), /*allOnOneLine=*/true), nullptr);
}

// 读回(state 字节不可信,§7.3):不是 JSON 数组 ⇒ 全空;数组短于 15 ⇒ 缺的那几条记空串,长出来的忽略
// (只按下标取、绝不越界)。**不逐条校验内容**:这里的值只拿来与 label 比「等不等」、从不写进 label,
// 一条畸形值最坏让那条 label 被当成「自动」、下次宿主改名时跟着轨道名变 —— 不涉及内存安全,也不改写
// 任何别的字段(label 本身已由 CFGS 解码校验过);逐条过滤能挡的只有这一种无害情形,不值一道判据。
inline std::array<juce::String, kAutoLabelCount> readAutoLabels(const juce::ValueTree& apvtsState)
{
    std::array<juce::String, kAutoLabelCount> out;
    if (!apvtsState.isValid() || !apvtsState.hasProperty(kAutoLabelsProp))
    {
        return out;
    }
    const juce::var parsed = juce::JSON::parse(apvtsState.getProperty(kAutoLabelsProp).toString());
    const juce::Array<juce::var>* arr = parsed.getArray();
    if (arr == nullptr)
    {
        return out;
    }
    const int n = juce::jmin(arr->size(), kAutoLabelCount);
    for (int t = 0; t < n; ++t)
    {
        out[static_cast<std::size_t>(t)] = arr->getReference(t).toString();
    }
    return out;
}

// [J148] `ui.active_tab` —— 契约 §1.31「写 state ui.active_tab(重开面板恢复上次 tab)」的落盘那一半。
// 此前它只活在 `OutputRuntimeState` 里(同一会话里关窗再开能恢复,重开工程一律回到 Tab1),
// 而契约与 STATE_SCHEMA §三 PRMS 行都写着它随工程走。落点与上面三个 ui_ 位、session_guid 同在
// PRMS 根节点属性面,理由逐字相同(本文件头注):abi 不动、不写迁移函数。
//
// 运行期用**序号**而不是 juce::String 承载:这个值会被宿主线程的 get/setStateInformation 与消息线程
// 25Hz 的快照 emit 同时碰,序号能装进一个 atomic,读方就不必为了它去抢 lifecycleMutex_
// (与 guideSeen/tourSeen 同一条理由,见 OutputRuntimeState 里那两位的注释)。
// 落盘写**名字**不写序号:工程文件里存的是 §1.31 的冻结枚举字面量,本枚举的序号哪天重排了,
// 已存的工程照样读得回来。
enum class OutputActiveTab : std::uint8_t
{
    kMaster = 0,
    kTracks = 1,
    kWave = 2,
    kSettings = 3,
};

inline const juce::Identifier kUiActiveTabProp{"ui_active_tab"};

// 序号 → §1.31 枚举字面量。越界序号不可能来自正常路径(写入口只有 parseActiveTab 与
// readActiveTab 两处,都只产出四值之一),万一出现也按默认档输出,绝不输出第五个值。
inline const char* activeTabName(OutputActiveTab tab)
{
    switch (tab)
    {
    case OutputActiveTab::kTracks:
        return "tracks";
    case OutputActiveTab::kWave:
        return "wave";
    case OutputActiveTab::kSettings:
        return "settings";
    case OutputActiveTab::kMaster:
    default:
        return "master";
    }
}

// §1.31 枚举字面量 → 序号。四值之外(含空串、大小写不同)一律返回 false 且不动 out,
// 由调用方决定怎么处理:桥面回 badArg,加载侧回落默认。
inline bool parseActiveTab(const juce::String& name, OutputActiveTab& out)
{
    if (name == "master")
        out = OutputActiveTab::kMaster;
    else if (name == "tracks")
        out = OutputActiveTab::kTracks;
    else if (name == "wave")
        out = OutputActiveTab::kWave;
    else if (name == "settings")
        out = OutputActiveTab::kSettings;
    else
        return false;
    return true;
}

// 写入 APVTS 快照树的根节点(getStateInformation:copyState() 之后、序列化之前)。
inline void writeActiveTab(juce::ValueTree& apvtsState, OutputActiveTab tab)
{
    if (!apvtsState.isValid())
    {
        return;
    }
    apvtsState.setProperty(kUiActiveTabProp, juce::String(activeTabName(tab)), nullptr);
}

// 属性缺失(老工程 / 本版之前存的工程)或取值不在四值里(手改工程文件、不可信字节)⇒ Tab1
// `master`,不报错、不提示 —— 与 `ui.master_chart_mode` 读到未知值回落默认同一口径。
// **调用方必须把这个返回值写回运行期**,不能「属性缺失就不动」:否则上一个工程停在哪个 tab,
// 载入这份老工程后就还停在哪,下次保存再把它写进这份工程(#96 陈旧值那一族)。
inline OutputActiveTab readActiveTab(const juce::ValueTree& apvtsState)
{
    OutputActiveTab tab = OutputActiveTab::kMaster;
    if (apvtsState.isValid())
    {
        (void)parseActiveTab(apvtsState.getProperty(kUiActiveTabProp, juce::String()).toString(), tab);
    }
    return tab;
}

} // namespace scvb::output
