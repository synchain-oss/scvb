// SPDX-License-Identifier: GPL-3.0-or-later
// test_ui_defaults_store —— 系统级 UI 全局默认的真实落盘往返(T37 真机 bug A-3 的全局那一层)。
//
// 为什么要真跑文件:契约 §1.32/§1.33 的「不再显示」是**跨工程**承诺,此前 §1.1 快照里的
// guide_seen_global / tour_seen_global 是硬编码 false、persistUiScaleAsDefault 是空实现 ——
// 用户勾了「不再显示」,下个工程照旧弹。本测试写一次、另开一份 PropertiesFile 读回来,
// 等价于「换一个插件实例 / 换一个工程」再读全局位。
//
// 纪律:测试会动本机的真实设置文件,故先读原值、结束时逐项还原(不给测试机留下副作用)。

#include <catch2/catch_test_macros.hpp>

#include "UiDefaultsStore.h"

namespace ud = scvb::output::uidefaults;

namespace
{
// 结束时还原原值(含异常路径),免得跑一次单测就把测试机的引导页永久关掉。
struct RestoreDefaults
{
    bool guide = ud::guideSeenGlobal();
    bool tour = ud::tourSeenGlobal();
    int scale = ud::uiScalePercent();
    ~RestoreDefaults()
    {
        ud::setGuideSeenGlobal(guide);
        ud::setTourSeenGlobal(tour);
        // 原本没设置过(0)时写回 100:接口没有「清除」语义,而 100 与「未设置」对调用方
        // 等效(processor 只在 >0 时采用),不给测试机留下一个莫名其妙的默认缩放档。
        ud::setUiScalePercent(scale > 0 ? scale : 100);
    }
};
} // namespace

TEST_CASE("UiDefaultsStore:全局默认写一次、换实例读得回(T37 A-3)", "[output][uidefaults][t37]")
{
    RestoreDefaults restore;

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

    // 档位越界 = 不可信值:既不写入,也不当作「已设置」读出(调用方沿用自己的 100)。
    ud::setUiScalePercent(5000);
    REQUIRE(ud::uiScalePercent() == 125);
}
