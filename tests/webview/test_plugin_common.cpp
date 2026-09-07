// SPDX-License-Identifier: GPL-3.0-or-later
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <BridgeBase.h>
#include <FallbackPanel.h>
#include <PlatformWebView.h>
#include <WebViewRevealGate.h>
#include <ResourceProvider.h>
#include <WebViewHost.h> // 只取看门狗预算/事件名常量(全是 constexpr,不需要编 WebViewHost.cpp)

#include <catch2/catch_test_macros.hpp>

namespace
{
// 仿造 juce_add_binary_data 生成的命名空间(最小 Source),验证 provide 按原始文件名命中。
const char* kResourceNames[] = {"index_html"};
const char* kOriginalFilenames[] = {"index.html"};

const char* fakeGetResource(const char* resourceName, int& size)
{
    if (juce::String(resourceName) == "index_html")
    {
        size = 5;
        return "hello";
    }
    size = 0;
    return nullptr;
}
} // namespace

TEST_CASE("ResourceProvider MIME mapping covers embedded asset types")
{
    using scvb::webview::ResourceProvider;
    CHECK(juce::String(ResourceProvider::mimeForExtension("html")) == "text/html");
    CHECK(juce::String(ResourceProvider::mimeForExtension("htm")) == "text/html");
    CHECK(juce::String(ResourceProvider::mimeForExtension("css")) == "text/css");
    CHECK(juce::String(ResourceProvider::mimeForExtension("js")) == "text/javascript");
    CHECK(juce::String(ResourceProvider::mimeForExtension("mjs")) == "text/javascript"); // ES module 必须是 JS MIME
    CHECK(juce::String(ResourceProvider::mimeForExtension("json")) == "application/json");
    CHECK(juce::String(ResourceProvider::mimeForExtension("svg")) == "image/svg+xml");
    CHECK(juce::String(ResourceProvider::mimeForExtension("woff2")) == "font/woff2");
    CHECK(juce::String(ResourceProvider::mimeForExtension("woff")) == "font/woff");
    CHECK(juce::String(ResourceProvider::mimeForExtension("ttf")) == "font/ttf");
    CHECK(juce::String(ResourceProvider::mimeForExtension("png")) == "image/png");
    CHECK(juce::String(ResourceProvider::mimeForExtension("bin")) == "application/octet-stream");
}

TEST_CASE("ResourceProvider empty source returns nullopt (no fake resources)")
{
    scvb::webview::ResourceProvider provider({});
    CHECK_FALSE(provider.provide("/").has_value());
    CHECK_FALSE(provider.provide("/index.html").has_value());
    CHECK_FALSE(provider.provide("/js/juce/index.js").has_value());
}

TEST_CASE("ResourceProvider serves by original filename (root-relative + full URL)")
{
    scvb::webview::ResourceProvider::Source src;
    src.resourceCount = 1;
    src.originalFilenames = kOriginalFilenames;
    src.resourceNames = kResourceNames;
    src.getNamedResource = &fakeGetResource;

    scvb::webview::ResourceProvider provider(src);

    // root-relative 口径(JUCE 8.0.8 的 Windows/mac 后端调 provider 前已剥 origin,根请求给 "/")。
    REQUIRE(provider.provide("/index.html").has_value());
    REQUIRE(provider.provide("/").has_value()); // 根文档 → index.html

    // full-URL 口径(防御性兜底,§6.1 机制 4,保证跨后端/跨版本一致)。
    REQUIRE(provider.provide("https://juce.backend/").has_value());
    REQUIRE(provider.provide("https://juce.backend/index.html").has_value());

    const auto res = provider.provide("/index.html");
    REQUIRE(res.has_value());
    CHECK(res->mimeType == juce::String("text/html"));
    CHECK(res->data.size() == 5);

    CHECK_FALSE(provider.provide("/other.css").has_value()); // 未命中 → nullopt
}

TEST_CASE("clampUiScale matches Bridge bounds")
{
    using scvb::bridge::clampUiScale;
    CHECK(clampUiScale(1.0f) == 1.0f);
    CHECK(clampUiScale(0.1f) == scvb::bridge::plugin::MinUiScale);
    CHECK(clampUiScale(9.0f) == scvb::bridge::plugin::MaxUiScale);
}

// [SL-234] 百分比档位 clamp:桥面 setUiScale 与**加载期** CFGS.uiScale 共用的那一个。
// 边界必须由 Min/MaxUiScale 换算出来,不是写死的 33/300 —— 用常量表达断言,常量改了用例跟着走。
TEST_CASE("clampUiScalePercent matches Bridge bounds (SL-234)")
{
    using scvb::bridge::clampUiScalePercent;
    const int lo = juce::roundToInt(scvb::bridge::plugin::MinUiScale * 100.0f);
    const int hi = juce::roundToInt(scvb::bridge::plugin::MaxUiScale * 100.0f);

    // 区间内原样(反向验证:不是恒返回边界)。
    CHECK(clampUiScalePercent(100) == 100);
    CHECK(clampUiScalePercent(lo) == lo);
    CHECK(clampUiScalePercent(hi) == hi);

    // 越界夹到边界。
    CHECK(clampUiScalePercent(lo - 1) == lo);
    CHECK(clampUiScalePercent(hi + 1) == hi);
    CHECK(clampUiScalePercent(0) == lo);
    CHECK(clampUiScalePercent(-1) == lo);
    CHECK(clampUiScalePercent(100000) == hi);

    // u32 全宽:CFGS 里 uiScale 是 u32,入参取 int64 才不会先溢出成 -1 再"恰好"夹到下界 ——
    // 4294967295 是**大**值,必须夹到上界。
    CHECK(clampUiScalePercent(static_cast<std::int64_t>(0xFFFFFFFFu)) == hi);
    CHECK(clampUiScalePercent(static_cast<std::int64_t>(0x80000000u)) == hi);
}

TEST_CASE("designBoxWindowSize rounds DESIGN × scale (zoom mechanism)")
{
    using scvb::bridge::designBoxWindowSize;

    const auto out1 = designBoxWindowSize("output", 1.0f);
    CHECK(out1.width == 1180);
    CHECK(out1.height == 780);

    const auto out15 = designBoxWindowSize("output", 1.5f);
    CHECK(out15.width == 1770);
    CHECK(out15.height == 1170);

    const auto in05 = designBoxWindowSize("input", 0.5f);
    CHECK(in05.width == 230);
    CHECK(in05.height == 280);
}

TEST_CASE("FallbackPanel missing-runtime variant shows install + retry and fires callbacks")
{
    juce::ScopedJuceInitialiser_GUI gui; // FallbackPanel(Component)构造需要 MessageManager
    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Output";
    options.message = "runtime missing";
    options.showInstall = true;
    bool installFired = false;
    bool retryFired = false;
    options.onInstall = [&] { installFired = true; };
    options.onRetry = [&] { retryFired = true; };

    scvb::webview::FallbackPanel panel(std::move(options));
    CHECK(panel.getNumChildComponents() == 5); // title + message + details + install + retry

    // 没给 details -> 组件建了但不可见(不占版面,也不留一条空行)
    auto* details = panel.findChildWithID("fallback.details");
    REQUIRE(details != nullptr);
    CHECK_FALSE(details->isVisible());

    // triggerClick() 走 postCommandMessage(异步,需消息循环);此处直接调 onClick 验证接线。
    auto* installBtn = dynamic_cast<juce::TextButton*>(panel.findChildWithID("fallback.install"));
    REQUIRE(installBtn != nullptr);
    installBtn->onClick();
    CHECK(installFired);

    auto* retryBtn = dynamic_cast<juce::TextButton*>(panel.findChildWithID("fallback.retry"));
    REQUIRE(retryBtn != nullptr);
    retryBtn->onClick();
    CHECK(retryFired);
}

TEST_CASE("FallbackPanel load-timeout variant hides install")
{
    juce::ScopedJuceInitialiser_GUI gui; // FallbackPanel(Component)构造需要 MessageManager
    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Input";
    options.message = "timed out";
    options.showInstall = false;

    scvb::webview::FallbackPanel panel(std::move(options));
    CHECK(panel.getNumChildComponents() == 4); // title + message + details + retry
    CHECK(panel.findChildWithID("fallback.install") == nullptr);
    CHECK(panel.findChildWithID("fallback.retry") != nullptr);
}

TEST_CASE("FallbackPanel shows the diagnostics line when one is supplied")
{
    juce::ScopedJuceInitialiser_GUI gui;
    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Output";
    options.message = "timed out";
    options.details = "waited 15003 ms  |  nav finished  |  WebView2 137.0.3296.83";

    scvb::webview::FallbackPanel panel(std::move(options));
    panel.setSize(1180, 780);

    auto* details = dynamic_cast<juce::Label*>(panel.findChildWithID("fallback.details"));
    REQUIRE(details != nullptr);
    CHECK(details->isVisible());
    CHECK(details->getText().contains("WebView2 137.0.3296.83"));
    // 诊断行占了版面,重试按钮仍要在面板内 —— 它是用户唯一能按的东西。
    CHECK(panel.getLocalBounds().contains(panel.findChildWithID("fallback.retry")->getBounds()));
}

namespace
{
// WCAG 2.x 的相对亮度与对比度(sRGB)。只放在测试侧:产品代码需要的只是几个固定取值,
// 需要「算得对」的是这条断言本身。公式逐字来自 WCAG 2.1 定义(relative luminance /
// contrast ratio),不是近似式。
double srgbLinear(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(juce::Colour c)
{
    return 0.2126 * srgbLinear(c.getFloatRed()) + 0.7152 * srgbLinear(c.getFloatGreen()) +
           0.0722 * srgbLinear(c.getFloatBlue());
}

double contrastRatio(juce::Colour a, juce::Colour b)
{
    const auto la = relativeLuminance(a);
    const auto lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

// [SL-370] shellBackdrop() 从深色改成浅色之后,兜底面板的三行标签必须跟着变深墨。
// 这块面板是 WebView2 起不来时**唯一**还能告诉用户发生了什么的东西:配色一旦同明暗,
// 用户看到的就是一块什么都没有的浅色板,而这正是本卡改底色带出来的风险面。
// 判据钉的是**对比度**,不是某个具体色值 —— 底色或字色任一侧改了都由它兜住,
// 且不会因为换一个同样可读的色号就假红。
TEST_CASE("FallbackPanel label colours stay readable on shellBackdrop()")
{
    juce::ScopedJuceInitialiser_GUI gui;
    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Output";
    options.message = "WebView2 runtime missing";
    options.details = "waited 15003 ms  |  no nav";

    scvb::webview::FallbackPanel panel(std::move(options));
    const auto bg = scvb::webview::shellBackdrop();

    // 三行标签都画在 FallbackPanel::paint 的 fillAll(shellBackdrop()) 上。
    // 4.5:1 = WCAG AA 正文档;诊断行 11px 比正文更小,同样按 4.5 判(不放宽到 3:1)。
    for (const auto* id : {"fallback.title", "fallback.message", "fallback.details"})
    {
        auto* label = dynamic_cast<juce::Label*>(panel.findChildWithID(id));
        REQUIRE(label != nullptr);
        const auto fg = label->findColour(juce::Label::textColourId);
        const auto ratio = contrastRatio(fg, bg);
        INFO("id=" << id << " fg=" << fg.toDisplayString(true).toStdString()
                   << " bg=" << bg.toDisplayString(true).toStdString() << " ratio=" << ratio);
        CHECK(ratio >= 4.5);
    }

    // 反向哨兵:白字在这块浅底上远远不够 —— 这一行同时证明上面那三格不是恒真
    // (SL-370 之前 title 用的正是 Colours::white)。
    CHECK(contrastRatio(juce::Colours::white, bg) < 4.5);
}

// -----------------------------------------------------------------------------
// [SL-370 / SL-376] 开窗遮挡闸(RevealGate)。
//
// WebViewHost.cpp **不进任何测试目标**(它只在随插件 target 编译的 INTERFACE 库里),
// 所以「WebView 此刻该不该待在可视区外」这个判定被抽成了纯逻辑的 RevealGate,
// 才有下面这几格。接线那一半(谁调 onNavigationStarted / onFirstFrame / onTick)
// 编译期与运行期都够不着,只能靠 WebViewHost.cpp 里的调用点与真机验收 —— 这是
// 已登记的覆盖缺口(与 SL-282 同族),别把这几格读成「整条链都验过了」。
//
// ⚠ 这条缺口在 [SL-376] 之后**更要紧**:本卡改的正是接线面上的一句
// (WebViewHost::onNavigationFinished 里去掉 applyRevealGate()/noteRevealed()),
// 而那一句删没删干净,下面一格都照不到。真机验收数的是放行原因表,不是这几格。
// -----------------------------------------------------------------------------
TEST_CASE("[SL-370] RevealGate parks on navigation start and reveals on the first-frame signal")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    CHECK_FALSE(gate.parked()); // 控制器建好之前必须留在原位:SL-271 的重试泵挂在 paint 上

    gate.onNavigationStarted(1000);
    CHECK(gate.parked());

    // [SL-376] 信号本身只武装,放行落在后面的 tick 上(tick 数 ∧ 毫秒下界)—— 那两格单独在下面钉。
    gate.onFirstFrame(1000);
    gate.onTick(1000 + scvb::webview::RevealGate::kRevealSettleMs);
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

// [SL-376] **navFinished 不再是一条放行路。**
//
// 定谳:pageFinishedLoading 只说明文档下载完,不保证任何一帧已经合成;SL-370 的 pluginval
// 数表里它有 4/10 次抢在首帧信号前 3–6 ms 放行,放回来露出的就是 WebView2 宿主 HWND 首帧
// 之前那一层(用户 v5.6.10:「粉 → 白一瞬 → 内容」)。
// 删除式:把 onNavigationFinished() 改回 `reveal("navFinished")`,本格立刻红。
TEST_CASE("[SL-376] RevealGate never reveals on navigation finished, it only records it")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    CHECK_FALSE(gate.navigationFinishedSeen());

    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onNavigationFinished();
    CHECK(gate.parked()); // ← 放行路被拿掉的那一格
    CHECK(gate.navigationFinishedSeen()); // 但要记账:超时那一行诊断靠它分两种失败
    CHECK(juce::String(gate.lastRevealReason()).isEmpty());

    // 之后一路 tick 到超时前一毫秒都还得按住 —— 「不放行」不能靠「还没 tick 过」蒙混。
    gate.onTick(1500);
    gate.onTick(1000 + scvb::webview::RevealGate::kRevealFallbackMs - 1);
    CHECK(gate.parked());
}

// [SL-376] 首帧信号到达后要再压一拍才放行 —— **tick 数**那一半。
//
// 两层 rAF 保证的是「帧已提交给合成器」,提交到上屏还差一拍 —— 信号一到就挪回来,
// 露出的仍是 WebView2 的底(用户看到的那一瞬白)。
// 删除式:让 onFirstFrame() 直接 reveal("firstFrame"),第一条 CHECK 立刻红。
TEST_CASE("[SL-376] RevealGate holds at least one more tick after the first-frame signal")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onFirstFrame(1000);
    CHECK(gate.parked()); // ← 那一拍:信号到了,还没放
    CHECK(juce::String(gate.lastRevealReason()).isEmpty());

    // 毫秒下界早已满足(每个 tick 都远在其后),所以这里量的纯粹是 tick 数那一半。
    for (int i = 0; i < scvb::webview::RevealGate::kRevealSettleTicks; ++i)
    {
        CHECK(gate.parked());
        gate.onTick(1000 + scvb::webview::RevealGate::kRevealSettleMs + i);
    }
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

// [SL-376] 那一拍的**毫秒下界**那一半(#247 复审【重要】②)。
//
// 只数 tick 是不够的:信号到达点相对 25Hz tick 的相位是随机的,「下一个 tick」离信号可以只有
// 1 ms —— 本卡自己的 pluginval 数表里最小一次就是 4 ms,**小于**一个 60Hz 合成帧。那样一来
// 头注承诺的「等出合成那一拍」在相当一部分开窗上根本没兑现,而没有任何判据会红。
// 删除式:把 onTick 里那个 msDone 条件删掉(只留 ticksDone),本格第一条 CHECK 立刻红。
TEST_CASE("[SL-376] RevealGate holds the first-frame settle for a millisecond floor, not just a tick")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onFirstFrame(2000);

    // 紧跟着就来一个 tick(只差 1 ms):tick 数够了,毫秒下界还差得远 ⇒ **不许放**。
    gate.onTick(2001);
    CHECK(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()).isEmpty());

    // 下界前一毫秒仍然按住 —— 边界格:少了它,把判据写成 `> 0` 也照绿。
    gate.onTick(2000 + scvb::webview::RevealGate::kRevealSettleMs - 1);
    CHECK(gate.parked());

    // 到点才放。
    gate.onTick(2000 + scvb::webview::RevealGate::kRevealSettleMs);
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

// [SL-376] 毫秒下界也要**回绕安全**(与 kRevealFallbackMs 的差值同一手法)。
//
// 信号时刻取 **2^32 - 8** —— 距回绕点只有 8 ms,**比 kRevealSettleMs 还近**。这一条是本格
// 能不能钉住东西的全部:回绕点再远一点(比如 0xffffff00),写成加法式 settleAtMs_ + 下界
// 也不会溢出,两种写法的结果处处相同,本格就成了一个永远绿的摆设(第一版正是如此,
// 反向注入跑出来是绿的才发现)。
// 删除式:把 msDone 改成加法式
//   nowMs >= settleAtMs_ + (uint32)kRevealSettleMs
// —— 那时 settleAtMs_ + 32 溢出成一个极小的数,下面「信号后 1 ms 就来一个 tick」那一格
// 会当场放行 ⇒ 红。
TEST_CASE("[SL-376] RevealGate settle floor survives the millisecond counter wrapping around")
{
    using Gate = scvb::webview::RevealGate;
    Gate gate;
    const std::uint32_t nearWrap = 0xfffffff8u; // 距回绕点 8 ms < kRevealSettleMs
    gate.beginLoadAttempt();
    gate.onNavigationStarted(nearWrap - 1000); // 离超时线还远,不会被 timeout 抢走
    REQUIRE(gate.parked());

    gate.onFirstFrame(nearWrap);

    // 信号后 1 ms 就来一个 tick:真差值是 1,远不到下界 ⇒ **不许放**。
    // 加法式在这里会算成「早就够了」,因为 settleAtMs_ + 32 已经溢出回绕。
    gate.onTick(nearWrap + 1);
    CHECK(gate.parked());

    // 跨过回绕点之后仍要按同一个下界判(边界前一毫秒按住、到点才放)。
    gate.onTick(nearWrap + static_cast<std::uint32_t>(Gate::kRevealSettleMs) - 1);
    CHECK(gate.parked());
    gate.onTick(nearWrap + static_cast<std::uint32_t>(Gate::kRevealSettleMs));
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

// [SL-376] 信号踩在 3s 线上到达时,结算那一拍**必须先于**超时判定。
// 否则数表里会凭空多出一次「信号缺席」,而真机验收数的就是这张表。
// 删除式:把 onTick 里的 settling 分支挪到超时判定之后,本格的 reason 变成 "timeout" 即红。
TEST_CASE("[SL-376] RevealGate settle tick beats the timeout when the signal lands on the deadline")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onFirstFrame(1000 + scvb::webview::RevealGate::kRevealFallbackMs - 1); // 信号在超时线之前一瞬到达
    for (int i = 0; i < scvb::webview::RevealGate::kRevealSettleTicks; ++i)
        // 已经过了超时线,且毫秒下界也已满足 —— 唯一还能决定 reason 的就是分支序。
        gate.onTick(1000 + scvb::webview::RevealGate::kRevealFallbackMs + scvb::webview::RevealGate::kRevealSettleMs +
                    i);

    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

TEST_CASE("[SL-370] RevealGate reveals on the timeout fallback and never stays parked forever")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    // 上界前一毫秒仍然按住 —— 边界格:少了它,把判据写成 `> 0` 也照绿。
    gate.onTick(1000 + scvb::webview::RevealGate::kRevealFallbackMs - 1);
    CHECK(gate.parked());

    gate.onTick(1000 + scvb::webview::RevealGate::kRevealFallbackMs);
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "timeout");
    // [SL-376] 超时那一行诊断要靠这一位分「页面 load 完了但信号没发」与「导航压根没走完」。
    CHECK_FALSE(gate.navigationFinishedSeen());
}

TEST_CASE("[SL-370] RevealGate timeout survives the millisecond counter wrapping around")
{
    // getMillisecondCounter 每 ~49 天回绕一次。回绕点上直接比大小会把「刚挪走」算成
    // 「早该放行」(或反过来永不放行),故判据走 uint32 差值再转 int32。
    scvb::webview::RevealGate gate;
    const std::uint32_t nearWrap = 0xffffff00u;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(nearWrap);
    REQUIRE(gate.parked());

    gate.onTick(nearWrap + static_cast<std::uint32_t>(scvb::webview::RevealGate::kRevealFallbackMs) - 1);
    CHECK(gate.parked());
    gate.onTick(nearWrap + static_cast<std::uint32_t>(scvb::webview::RevealGate::kRevealFallbackMs));
    CHECK_FALSE(gate.parked());
}

TEST_CASE("[SL-370] RevealGate never re-parks after it has revealed once")
{
    // 页面自己再导航一次时重新挪走 = 把一个已经画好的界面换成占位底色,比那点白更难看。
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    gate.onFirstFrame(1000);
    gate.onTick(1000 + scvb::webview::RevealGate::kRevealSettleMs); // [SL-376] 放行在信号之后那一拍(tick 数 ∧ 毫秒下界)
    REQUIRE_FALSE(gate.parked());

    gate.onNavigationStarted(2000);
    CHECK_FALSE(gate.parked());

    // 只有一次新的加载尝试(构造 / retry)才重新武装。
    gate.beginLoadAttempt();
    gate.onNavigationStarted(3000);
    CHECK(gate.parked());
}

TEST_CASE("[SL-370] RevealGate takes a first-frame signal that arrives before the navigation callback")
{
    // 信号先于 pageAboutToLoad 到达(消息线程投递顺序不由我们决定)时,不能反过来把
    // 已经画好的页面挪走 —— onFirstFrame 即使在没挪走时也要记账。
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onFirstFrame(900);
    gate.onNavigationStarted(1000);
    CHECK_FALSE(gate.parked());
}

TEST_CASE("[SL-370] RevealGate releases when the fallback panel takes over")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onFallbackShown();
    CHECK_FALSE(gate.parked()); // 面板铺满本组件,retry 回来时 bounds 不能还停在可视区外
}

TEST_CASE("[SL-370] parkedBounds keeps the size and lands outside the visible rectangle")
{
    const juce::Rectangle<int> visible{0, 0, 1180, 780};
    const auto parked = scvb::webview::parkedBounds(visible);

    // 尺寸一字不改:视口不变 ⇒ 页面不 reflow、Chromium 仍按真实尺寸出帧(rAF 照跑)。
    CHECK(parked.getWidth() == visible.getWidth());
    CHECK(parked.getHeight() == visible.getHeight());
    // 真的挪出去了:与可视区零交集。删掉平移这一步,本行立刻红。
    CHECK_FALSE(parked.intersects(visible));

    // 宽度为 0(还没 resizeToDesignBox)时也必须挪得动,否则原地不动 = 遮挡完全失效。
    const juce::Rectangle<int> empty{0, 0, 0, 40};
    CHECK(scvb::webview::parkedBounds(empty).getX() > empty.getX());
}

TEST_CASE("majorVersionOf parses the WebView2 runtime version string")
{
    using scvb::webview::PlatformWebView;
    CHECK(PlatformWebView::majorVersionOf("137.0.3296.83") == 137);
    CHECK(PlatformWebView::majorVersionOf("86.0.616.0") == 86);
    CHECK(PlatformWebView::majorVersionOf(" 91.0.864.41 ") == 91);
    // 解析不出 -> -1,调用方据此**放行**而非判 tooOld(见 PlatformWebViewRuntime.cpp 注释:
    // 宁可让看门狗按超时兜底,也不要把一台能用的机器挡在「请升级」面板后面)。
    CHECK(PlatformWebView::majorVersionOf("") == -1);
    CHECK(PlatformWebView::majorVersionOf("dev") == -1);
    CHECK(PlatformWebView::majorVersionOf("v137.0") == -1);
    // 下限本身:低于 86 的运行时缺 JUCE 8.0.8 硬依赖的首发 GA 接口,必须走「需升级」分支。
    CHECK(PlatformWebView::kMinRuntimeMajor == 86);
    CHECK(PlatformWebView::majorVersionOf("85.0.564.68") < PlatformWebView::kMinRuntimeMajor);
}

// [SL-376 / SL-364] 「DefaultBackgroundColor 这一层在不在」的三态判定。
//
// JUCE 取 ICoreWebView2Controller2 是 QueryInterface + `if (ptr != nullptr)`,取不到就静默跳过
// put_DefaultBackgroundColor —— 真机上完全不可观测,SL-364 卡的就是这一点。本判定把它变成
// 一行可抓的诊断;**它证的是「运行时有没有这个接口」,不是「那次 QueryInterface 真成功了」**
// (理由见 PlatformWebView.h 的头注)。
TEST_CASE("[SL-376] backgroundColourSupport classifies the WebView2 runtime three ways")
{
    using PWV = scvb::webview::PlatformWebView;
    using Support = PWV::BackgroundColourSupport;
    const auto ok = PWV::RuntimeStatus::ok;

    // 用户机实测版本(SL-370 记录):远高于下限 ⇒ 接口在。
    CHECK(PWV::backgroundColourSupport({ok, "152.0.4191.66"}) == Support::available);
    // 下限本身与它下面一档:边界格,少了它把判据写成 `>` 也照绿。
    CHECK(PWV::backgroundColourSupport({ok, "87.0.664.66"}) == Support::available);
    CHECK(PWV::backgroundColourSupport({ok, "86.0.616.0"}) == Support::unavailable);
    // 版本串解析不出 -> **不猜**,如实说不知道(与 runtimeInfo 那边同一个取舍)。
    CHECK(PWV::backgroundColourSupport({ok, "dev"}) == Support::unknown);
    CHECK(PWV::backgroundColourSupport({ok, ""}) == Support::unknown);
    // 压根没探到运行时:这条路上不会去建控制器,这一层无从谈起。
    CHECK(PWV::backgroundColourSupport({PWV::RuntimeStatus::missing, ""}) == Support::unknown);

    // 下限必须严格高于 kMinRuntimeMajor,否则「运行时够跑但底色层缺席」这一档根本不存在,
    // 上面那格 unavailable 就成了永远走不到的死代码(而 SL-364 备忘正是为这一档立的)。
    CHECK(PWV::kBackgroundColourMinRuntimeMajor > PWV::kMinRuntimeMajor);
}

TEST_CASE("Watchdog budgets give cold start more room than a warm reopen")
{
    using scvb::webview::WebViewHost;
    // 冷启动要覆盖 msedgewebview2.exe 进程组拉起 + user-data 目录首建;热启动只是新建一个
    // WebView。两者相等就说明有人把常量改回了单一预算,冷启动误报会立刻回来。
    CHECK(WebViewHost::kColdLoadBudgetMs > WebViewHost::kWarmLoadBudgetMs);
    CHECK(WebViewHost::kColdLoadBudgetMs >= 15000);
    CHECK(WebViewHost::kAfterNavBudgetMs >= 5000);
    // 事件名是 web 侧 index.html boot 守卫的逐字引用面(smoke-embedded-resources.mjs 对拍)。
    CHECK(juce::String(WebViewHost::kBootErrorEventId) == "__scvb__bootError");
}
TEST_CASE("normalizeLang accepts {zh,en,fr} and falls back to zh (§1.30)")
{
    using scvb::bridge::normalizeLang;
    CHECK(normalizeLang("zh") == "zh");
    CHECK(normalizeLang("en") == "en");
    CHECK(normalizeLang("fr") == "fr");
    CHECK(normalizeLang("de") == "zh"); // 未知 code 回退 zh
    CHECK(normalizeLang("") == "zh");
}

TEST_CASE("parseUiScaleArg rejects non-numeric/off-preset and accepts preset (§1.28)")
{
    using scvb::bridge::parseUiScaleArg;
    float out = 0.0f;

    juce::Array<juce::var> nonNumeric;
    nonNumeric.add(juce::var("abc"));
    CHECK_FALSE(parseUiScaleArg(nonNumeric, "output", out)); // 非数字 → badArg

    juce::Array<juce::var> empty;
    CHECK_FALSE(parseUiScaleArg(empty, "output", out)); // 缺参 → badArg

    // Output 档位表 = [0.5,0.65,0.8,1,1.25,1.5,2]
    juce::Array<juce::var> offPreset;
    offPreset.add(juce::var(1.3));
    CHECK_FALSE(parseUiScaleArg(offPreset, "output", out)); // 非档位 → badArg

    juce::Array<juce::var> outOfRange;
    outOfRange.add(juce::var(2.5));
    CHECK_FALSE(parseUiScaleArg(outOfRange, "output", out)); // 超 Output 上限 2.0 → badArg

    juce::Array<juce::var> okOutput;
    okOutput.add(juce::var(1.5));
    REQUIRE(parseUiScaleArg(okOutput, "output", out));
    CHECK(out == 1.5f);

    // Input 档位表 = [0.33,0.5,0.75,1,1.25,1.5,1.75,2,2.5,3];0.33 与 2.5 合法
    juce::Array<juce::var> in033;
    in033.add(juce::var(0.33));
    REQUIRE(parseUiScaleArg(in033, "input", out));
    CHECK(out == 0.33f);

    juce::Array<juce::var> in25;
    in25.add(juce::var(2.5));
    REQUIRE(parseUiScaleArg(in25, "input", out));
    CHECK(out == 2.5f);
}

TEST_CASE("badArgResponse has {ok:false, reason:badArg} shape (§1.28)")
{
    const auto v = scvb::bridge::badArgResponse();
    auto* obj = v.getDynamicObject();
    REQUIRE(obj != nullptr);
    CHECK_FALSE(static_cast<bool>(obj->getProperty("ok")));
    CHECK(obj->getProperty("reason").toString() == "badArg");
}

TEST_CASE("buildUiSeedPairs/buildUiSnapshot share Init keys (state pushback §1.30)")
{
    scvb::bridge::UiSeed seed;
    seed.role = "output";
    seed.version = "0.1.0";
    seed.lang = "fr";
    seed.uiScale = 1.25f;
    seed.channelLimit = 15;

    const auto pairs = scvb::bridge::buildUiSeedPairs(seed);
    REQUIRE(pairs.size() == 5);
    CHECK(pairs[0].first == scvb::bridge::Init::Version);
    CHECK(pairs[1].first == scvb::bridge::Init::Role);
    CHECK(pairs[2].first == scvb::bridge::Init::ChannelLimit);
    CHECK(pairs[3].first == scvb::bridge::Init::Lang);
    CHECK(pairs[4].first == scvb::bridge::Init::UiScale);

    // setLang 写入后经本快照回推实际生效值(scvb.state)。
    const auto snap = scvb::bridge::buildUiSnapshot(seed);
    auto* obj = snap.getDynamicObject();
    REQUIRE(obj != nullptr);
    CHECK(obj->getProperty(scvb::bridge::Init::Lang).toString() == "fr");
    CHECK(obj->getProperty(scvb::bridge::Init::Role).toString() == "output");
    CHECK(static_cast<int>(obj->getProperty(scvb::bridge::Init::ChannelLimit)) == 15);
    CHECK(obj->getProperty(scvb::bridge::Init::Version).toString() == "0.1.0");
}

TEST_CASE("makeWebViewOptions selects WebView2 backend + per-plugin userDataFolder (§9/#5)")
{
    using WBC = juce::WebBrowserComponent;
    using scvb::webview::PlatformWebView;

    const auto in1 = PlatformWebView::makeUserDataFolder("SCVBInputWV2");
    const auto in2 = PlatformWebView::makeUserDataFolder("SCVBInputWV2");
    const auto out1 = PlatformWebView::makeUserDataFolder("SCVBOutputWV2");

    // **同插件的多个实例必须拿到同一个目录**:WebView2 的浏览器进程组按 user-data 目录共享,
    // 每实例一个目录会让进程组永不复用 —— 于是「热启动」判定形同虚设(第二次开窗其实还是
    // 完整冷启动却按 5s 热预算计时),而且每开一个编辑器就多一整套 msedgewebview2 进程。
    CHECK(in1 == in2);
    // 两个插件之间仍然分开(各自的会话/缓存互不干扰)。
    CHECK_FALSE(in1 == out1);

#if JUCE_WINDOWS
    const auto o1 = PlatformWebView::makeWebViewOptions(WBC::Options{}, in1);
    CHECK(o1.getBackend() == WBC::Options::Backend::webview2); // 机制 1:显式选 WebView2
    CHECK(o1.getWinWebView2BackendOptions().getUserDataFolder() == in1);

    // [SL-253] WebView2 在所有 web 内容**之下**铺的那一层必须是**不透明**的 shellBackdrop()。
    // [SL-370] 原话是「不透明暗色」——「暗」已经不对了(那正是用户看见的那段黑),
    // 现在这一层与成品外壳同明暗;本断言从来只钉「等于 shellBackdrop() 且不透明」,取值本身
    // 由 web-preview/tests/smoke-embedded-resources.mjs 的 ⑥c 对着 --page-gradient 判。
    // 不设的话它是默认构造的 juce::Colour = ARGB 0x00000000(全透明),JUCE 会把这个值
    // 原样 put 进 put_DefaultBackgroundColor —— 于是从控制器建好到 tokens.css/base.css
    // 解析完为止这一层什么都不挡,露的是窗口的白(用户实测「开窗一瞬全白」)。
    const auto bg = o1.getWinWebView2BackendOptions().getBackgroundColour();
    CHECK(bg == scvb::webview::shellBackdrop());
    CHECK(bg.isOpaque()); // JUCE 只接受全不透明或全透明;半透明会在其内部断言
    CHECK_FALSE(bg == juce::Colour()); // ★ 反向哨兵:退回默认构造(全透明)即红
    CHECK(in1.getFileName() == "SCVBInputWV2");

    // 目录名里不许再出现 PID / 实例序号:那正是「每实例一个目录」的残留特征。
    CHECK_FALSE(in1.getFileName().contains("_p"));

    // 必须落在 **Local** AppData:%TEMP% 会被磁盘清理扫掉;Roaming
    // (juce 的 userApplicationDataDirectory)会让浏览器缓存跟着漫游配置文件同步。
    CHECK(in1.isAChildOf(juce::File::getSpecialLocation(juce::File::windowsLocalAppData)));
    CHECK_FALSE(in1.isAChildOf(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)));
    CHECK_FALSE(in1.isAChildOf(juce::File::getSpecialLocation(juce::File::tempDirectory)));

    // PID 仍要拿得到 —— 它进诊断行(与任务管理器对照),只是不再进目录名。
    CHECK(PlatformWebView::processId() > 0);

    // 可写性探针:目录应当可建可写;探针文件不留痕。
    CHECK(PlatformWebView::probeUserDataFolder(in1).isEmpty());
    CHECK_FALSE(in1.getChildFile(".scvb-write-probe").existsAsFile());
#else
    const auto o1 = PlatformWebView::makeWebViewOptions(WBC::Options{}, in1);
    CHECK(o1.getBackend() == WBC::Options::Backend::defaultBackend); // 非 Windows 走系统默认
#endif
}

TEST_CASE("FallbackPanel deferred retry avoids use-after-free (SafePointer + callAsync)")
{
    juce::ScopedJuceInitialiser_GUI gui;

    // holder 用 shared_ptr 持有,保证 pending callAsync 捕获的 holder 不悬垂;panel 销毁后
    // SafePointer 自动置空,pending callAsync 安全 no-op。
    struct RetryHolder
    {
        std::unique_ptr<scvb::webview::FallbackPanel> panel;
        juce::Component::SafePointer<scvb::webview::FallbackPanel> safe;
    };
    auto holder = std::make_shared<RetryHolder>();

    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Output";
    options.message = "defer";
    // 复刻 WebViewHost::showFallback 的 onRetry 接线:onClick 内**只调度**延后销毁,不同步 reset,
    // 否则会销毁正执行 onClick 的按钮(use-after-free)。
    options.onRetry = [holder] {
        juce::MessageManager::callAsync([holder] {
            if (holder->safe != nullptr)
                holder->panel.reset();
        });
    };

    holder->panel = std::make_unique<scvb::webview::FallbackPanel>(std::move(options));
    holder->safe = juce::Component::SafePointer<scvb::webview::FallbackPanel>(holder->panel.get());

    auto* retryBtn = dynamic_cast<juce::TextButton*>(holder->panel->findChildWithID("fallback.retry"));
    REQUIRE(retryBtn != nullptr);
    retryBtn->onClick(); // 只调度延后销毁

    // 同步点:面板必须仍存活(延后未执行)——防 use-after-free 的关键断言。
    CHECK(holder->panel != nullptr);
    CHECK(holder->safe != nullptr);

    // 手动清理(不依赖 modal loop,本测试进程 JUCE_MODAL_LOOPS_PERMITTED=0):删除面板后
    // SafePointer 置空,残留的 pending callAsync 据此安全 no-op。
    holder->panel.reset();
    CHECK(holder->safe == nullptr);
}
