// SPDX-License-Identifier: GPL-3.0-or-later
// test_ui_defaults_store —— 系统级 UI 全局默认的真实落盘往返(T37 真机 bug A-3 的全局那一层)
//                            + 首启已读位在 PRMS ValueTree 上的双向兼容。
//
// 为什么要真跑文件:契约 §1.32/§1.33 的「不再显示」是**跨工程**承诺,此前 §1.1 快照里的
// guide_seen_global / tour_seen_global 是硬编码 false、persistUiScaleAsDefault 是空实现 ——
// 用户勾了「不再显示」,下个工程照旧弹。本测试写一次、另开一份 PropertiesFile 读回来,
// 等价于「换一个插件实例 / 换一个工程」再读全局位。
//
// 落盘目录经 setStorageDirForTesting 指到临时目录:单测绝不碰真实用户设置
// (崩溃即残留会让开发机的引导页永久不弹,并行 worktree 之间也会串扰)。

#include <catch2/catch_test_macros.hpp>

#include "OutputUiState.h"
#include "UiDefaultsStore.h"

namespace ud = scvb::uidefaults;

namespace
{
// 每个 TEST_CASE 独享一个临时目录,结束即整个删掉(含异常路径)。
struct TempStore
{
    juce::File dir =
        juce::File::createTempFile("scvb-uidefaults-test")
            .getSiblingFile("scvb-uidefaults-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempStore()
    {
        dir.createDirectory();
        ud::setStorageDirForTesting(dir);
    }
    ~TempStore()
    {
        ud::setStorageDirForTesting({}); // 恢复生产位置
        dir.deleteRecursively();
    }
};
} // namespace

TEST_CASE("[SL258] guide_seen 全局位按侧分键 —— Output 与 Input 各存一份、互不串扰",
          "[uidefaults][SL258]")
{
    TempStore store;

    // 为什么必须分键(契约 §3.1 语义行逐字):两侧引导讲的是两个界面、两套内容。
    // 共用一个位的话,先装 Output 的用户在 Output 里勾过「不再显示」之后,**永远看不到
    // Input 的引导** —— 而那正是 J80 立 T48 的全部理由。本用例就是钉这一条。
    REQUIRE_FALSE(ud::guideSeenGlobal());
    REQUIRE_FALSE(ud::guideSeenGlobalInput());

    // ① Output 勾了「不再显示」⇒ **不得**连带把 Input 的位也置上。
    ud::setGuideSeenGlobal(true);
    CHECK(ud::guideSeenGlobal());
    CHECK_FALSE(ud::guideSeenGlobalInput()); // 反向:合并成一个键时这里必红

    // ② 反过来同理:Input 置位不影响 Output(且两位可各自独立取值)。
    ud::setGuideSeenGlobalInput(true);
    ud::setGuideSeenGlobal(false);
    CHECK(ud::guideSeenGlobalInput());
    CHECK_FALSE(ud::guideSeenGlobal());

    // ③ 真过磁盘:每次调用现开一份 PropertiesFile,读得回 = 换实例/换工程也读得回
    //    (Input 侧的跨工程「不再显示」承诺就靠这一条,§3.8 的 alsoGlobal)。
    ud::setGuideSeenGlobalInput(false);
    CHECK_FALSE(ud::guideSeenGlobalInput());
}

TEST_CASE("UiDefaultsStore:全局默认写一次、换实例读得回(T37 A-3)", "[output][uidefaults][t37]")
{
    TempStore store;

    // 干净起点:从没写过 ⇒ 两位 false、缩放档 0(= 未设置,调用方沿用自己的 100)。
    REQUIRE_FALSE(ud::guideSeenGlobal());
    REQUIRE_FALSE(ud::tourSeenGlobal());
    REQUIRE(ud::uiScalePercent() == 0);

    ud::setGuideSeenGlobal(true);
    ud::setTourSeenGlobal(true);
    // 每次调用都现开一份 PropertiesFile —— 读到 true 即证明值真的过了磁盘,
    // 而不是活在某个进程内单例里(换插件实例/换工程同样读得到)。
    REQUIRE(ud::guideSeenGlobal());
    REQUIRE(ud::tourSeenGlobal());

    ud::setGuideSeenGlobal(false);
    REQUIRE_FALSE(ud::guideSeenGlobal());
    REQUIRE(ud::tourSeenGlobal()); // 两位互不干扰

    ud::setUiScalePercent(125);
    REQUIRE(ud::uiScalePercent() == 125);

    // 档位越界 = 不可信值:既不写入,也不当作「已设置」读出。
    ud::setUiScalePercent(5000);
    REQUIRE(ud::uiScalePercent() == 125);
    ud::setUiScalePercent(1);
    REQUIRE(ud::uiScalePercent() == 125);
}

TEST_CASE("UiDefaultsStore:语言值全局镜像覆盖 §1.30 归一集 {zh,en,fr}", "[output][uidefaults][v5]")
{
    TempStore store;

    // 干净起点:从没选过 ⇒ 布尔 false、语言空串(= 未设置,调用方沿用自己的默认)。
    REQUIRE_FALSE(ud::langChosenGlobal());
    REQUIRE(ud::langGlobal().isEmpty());

    // **三种语言逐一往返**。fr 这一条是必须的:白名单漏掉它时,法文用户的写入会静默
    // return,而 setLangChosenGlobal(true) 照写不误 —— 移除插件重加载后语言回落 en,
    // 语言起始卡又被 lang_chosen_global 挡住,P1-6 在 fr 上原样复现。
    for (const juce::String lang : {juce::String("zh"), juce::String("en"), juce::String("fr")})
    {
        ud::setLangGlobal(lang);
        REQUIRE(ud::langGlobal() == lang); // 每次调用现开一份 PropertiesFile → 真的过了磁盘
    }

    // 归一集之外的值:既不写入,也不覆盖上一次的有效值(磁盘上的 XML 用户可编辑)。
    ud::setLangGlobal("de");
    REQUIRE(ud::langGlobal() == "fr");
    ud::setLangGlobal("");
    REQUIRE(ud::langGlobal() == "fr");
    ud::setLangGlobal("ZH"); // 大小写不做兜底:归一化是 §1.30 桥层的职责,本层只复核
    REQUIRE(ud::langGlobal() == "fr");

    // 布尔与语言值是两件事,互不代替(只写布尔不写值 = 卡被挡住却回英文,正是 v5 P1-6)。
    ud::setLangChosenGlobal(true);
    REQUIRE(ud::langChosenGlobal());
    REQUIRE(ud::langGlobal() == "fr");
}

TEST_CASE("PRMS ui 首启已读位:往返 + 新旧构建双向兼容(T37 A-3)", "[output][state][t37]")
{
    using namespace scvb::output;

    // 参数树的形状按 APVTS 的实际样子搭:根节点 + 若干参数子节点。两位挂根节点属性面。
    juce::ValueTree tree("PARAMETERS");
    juce::ValueTree p("PARAM");
    p.setProperty("id", "out_width", nullptr);
    p.setProperty("value", 0.5f, nullptr);
    tree.appendChild(p, nullptr);

    // ① 老工程(从没落过这两位)⇒ 读回 false,该走首启。
    const auto fresh = readUiFlags(tree);
    REQUIRE_FALSE(fresh.guideSeen);
    REQUIRE_FALSE(fresh.tourSeen);

    // ② 写入 → XML 往返(= getStateInformation / setStateInformation 走的那条路)。
    writeUiFlags(tree, {true, false});
    const std::unique_ptr<juce::XmlElement> xml(tree.createXml());
    REQUIRE(xml != nullptr);
    const juce::ValueTree reloaded = juce::ValueTree::fromXml(*xml);
    const auto flags = readUiFlags(reloaded);
    REQUIRE(flags.guideSeen);
    REQUIRE_FALSE(flags.tourSeen);
    // 参数子节点未被这两位挤掉。
    REQUIRE(reloaded.getNumChildren() == 1);
    REQUIRE(reloaded.getChild(0).getProperty("id").toString() == "out_width");

    // ③ **正向兼容**(这是本设计相对「CFGS 尾部追加」的关键收益):旧构建不认识这两个属性,
    //    照旧只读它认识的参数 —— 属性多了不影响解析,工程不会被静默打回默认值。
    //    这里用「删掉两个属性」模拟旧构建的视角,验证其余内容原封不动。
    juce::ValueTree asOldBuildSees = reloaded.createCopy();
    asOldBuildSees.removeProperty(kUiGuideSeenProp, nullptr);
    asOldBuildSees.removeProperty(kUiTourSeenProp, nullptr);
    REQUIRE(asOldBuildSees.getNumChildren() == 1);
    REQUIRE(static_cast<float>(asOldBuildSees.getChild(0).getProperty("value")) == 0.5f);

    // ④ 无效树不崩、按默认返回。
    const juce::ValueTree invalid;
    REQUIRE_FALSE(readUiFlags(invalid).guideSeen);
    juce::ValueTree stillInvalid;
    writeUiFlags(stillInvalid, {true, true}); // no-op,不构造节点
    REQUIRE_FALSE(stillInvalid.isValid());
}
