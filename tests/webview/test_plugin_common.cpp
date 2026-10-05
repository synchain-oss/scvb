// SPDX-License-Identifier: GPL-3.0-or-later
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <BridgeBase.h>
#include <FallbackPanel.h>
#include <PlatformLog.h> // [B 线 M07]
#include <PlatformWebView.h>
#include <WebViewRevealGate.h>
#include <ResourceProvider.h>
#include <WebViewHost.h> // 只取看门狗预算/事件名常量(全是 constexpr,不需要编 WebViewHost.cpp)

#if JUCE_MAC
// [B 线 M07] mac 上本目标链 scvb_core(tests/CMakeLists.txt),IpcDiag 与 SCVB_HAS_POSIX_SHM 都从那里来;
// 宏若没到,[mac] 那一格「sink 去重」会被 #if 静默拿掉 —— 宁可当场编译红。
#if !SCVB_HAS_POSIX_SHM
#error "scvb_plugin_common_tests on macOS must link scvb_core (SCVB_HAS_POSIX_SHM / IpcDiag)"
#endif
#include <ipc/IpcDiag.h>
#include <unistd.h> // getpid
#endif

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

// [SL-370] 占位底从深色改成浅色(现为浅色**渐变**,C++ 真源 = kShellBackdropStops /
// shellBackdropMid(),PlatformWebView.h)之后,兜底面板的三行标签必须跟着变深墨。
// 这块面板是 WebView2 起不来时**唯一**还能告诉用户发生了什么的东西:配色一旦同明暗,
// 用户看到的就是一块什么都没有的浅色板,而这正是本卡改底色带出来的风险面。
// 判据钉的是**对比度**,不是某个具体色值 —— 底色或字色任一侧改了都由它兜住,
// 且不会因为换一个同样可读的色号就假红。
TEST_CASE("FallbackPanel label colours stay readable on the fallback panel background")
{
    juce::ScopedJuceInitialiser_GUI gui;
    scvb::webview::FallbackPanel::Options options;
    options.title = "SCVB Output";
    options.message = "WebView2 runtime missing";
    options.details = "waited 15003 ms  |  no nav";

    scvb::webview::FallbackPanel panel(std::move(options));
    // [SL-402] 面板底不再与开窗占位同源:占位升成渐变后,面板用的是自己的
    // kFallbackPanelArgb(FallbackPanel.h)。判据照旧钉**对比度**,钉住「底换谁都不许把
    // 标签变成白字浅底」这一件事。
    const auto bg = juce::Colour(scvb::webview::kFallbackPanelArgb);

    // 三行标签都画在 FallbackPanel::paint 的 fillAll(kFallbackPanelArgb) 上。
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
// 两层 rAF 保证的是「**已绘的**那一帧已提交给合成器」([SL-429] 起「已绘」由页内的 paint
// 记录保证,两层 rAF 自己并不保证这件事),提交到上屏还差一拍 —— 信号一到就挪回来,
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
// —— 那时 settleAtMs_ + kRevealSettleMs 溢出成一个极小的数,下面「信号后 1 ms 就来一个 tick」
// 那一格会当场放行 ⇒ 红。
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
    // 加法式在这里会算成「早就够了」,因为 settleAtMs_ + kRevealSettleMs 已经溢出回绕。
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

// [SL-558] Config::version 的默认值不带版本号。三个编辑器都显式传 JucePlugin_VersionString,
// 默认值只在漏传时生效 —— 此前它是 "0.1.0",漏传时首帧 seed 报一个像真的旧版本号,没有任何
// 东西会红。钉成空串:漏传时看到的是「没有版本」,不是一个错的版本。
TEST_CASE("[SL-558] WebViewHost::Config version default carries no hardcoded release number")
{
    const scvb::webview::WebViewHost::Config config{};
    CHECK(config.version.isEmpty());
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

#if JUCE_WINDOWS
    // 两个插件之间仍然分开(各自的会话/缓存互不干扰)。
    // [B 线 M07] 这一条挪进 Windows 分支:系统 WebKit 上不用 UDF,两者都是空 File(见下面的 #else)。
    CHECK_FALSE(in1 == out1);

    const auto o1 = PlatformWebView::makeWebViewOptions(WBC::Options{}, in1);
    CHECK(o1.getBackend() == WBC::Options::Backend::webview2); // 机制 1:显式选 WebView2
    CHECK(o1.getWinWebView2BackendOptions().getUserDataFolder() == in1);

    // [SL-253] WebView2 在所有 web 内容**之下**铺的那一层必须是**不透明**的占位底色。
    // [SL-370] 原话是「不透明暗色」——「暗」已经不对了(那正是用户看见的那段黑),
    // 现在这一层与成品外壳同明暗;[SL-402] 起占位是**渐变**(kShellBackdropStops),
    // 而 DefaultBackgroundColor 只收纯色 ⇒ 取占位渐变沿轴 50% 的插值色 shellBackdropMid()。
    // 本断言只钉「等于 shellBackdropMid() 且不透明」;「中点色确属 tokens 渐变」这一条
    // 由 web-preview/tests/smoke-embedded-resources.mjs 的 ⑥c 对色标数组判,这里不重复。
    // 不设的话它是默认构造的 juce::Colour = ARGB 0x00000000(全透明),JUCE 会把这个值
    // 原样 put 进 put_DefaultBackgroundColor —— 于是从控制器建好到 tokens.css/base.css
    // 解析完为止这一层什么都不挡,露的是窗口的白(用户实测「开窗一瞬全白」)。
    const auto bg = o1.getWinWebView2BackendOptions().getBackgroundColour();
    CHECK(bg == scvb::webview::shellBackdropMid());
    CHECK(bg.isOpaque()); // JUCE 只接受全不透明或全透明;半透明会在其内部断言
    CHECK_FALSE(bg == juce::Colour()); // ★ 反向哨兵:退回默认构造(全透明)即红
    // [SL-402] 已知值锚:色标数组(tokens 渐变的逐字拷贝)沿轴 50% 的插值色 = #d9cadb。
    // tokens 的 --page-gradient 改动时,kShellBackdropStops(⑥c 会红)与本格**同批**改 ——
    // 与旧版单色常量时期的维护方式一致:一处真源、两处判据。
    CHECK(scvb::webview::shellBackdropMid() == juce::Colour(0xffd9cadb));
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
    // [B 线 M07] 系统 WebKit 不用 UDF:两个插件拿到的都是空 File(细节判据见 [mac] 那几格)。
    CHECK(in1 == juce::File());
    CHECK(out1 == juce::File());
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

// =============================================================================
// [B 线 M07] WebView C++ 侧的 mac 适配 + 平台文件日志(PlatformLog)
//
// 分三层:
//   · [M07][platform] —— 按引擎分支的文案 / 诊断行 / 遮挡闸开关。每个平台都跑:Windows 上显式传
//     systemWebKit 把 mac 那一份逐条判过(本机没有 Mac),同时把 WebView2 那一份**逐字**钉住;
//   · [M07][source]   —— WebViewHost.cpp 与三个插件入口不进任何测试目标(真 WebView / 插件 wrapper),
//     接线只能读源码文本判(与 test_input_bridge.cpp 的 [SL-463] 同一手法);
//   · [M07][platformlog] / [mac] —— 轮转、去重、只在消息线程写;[mac] 只在 macOS 上编,核默认值确实落在
//     系统 WebKit、默认日志路径、真 IpcDiag sink 的去重与 1 MB 轮转。build-macos.yml 按 [mac] 标签单跑并数格数。
// =============================================================================
namespace
{
using scvb::webview::PlatformWebView;
using Engine = scvb::webview::PlatformWebView::Engine;

PlatformWebView::RuntimeInfo runtimeOf(PlatformWebView::RuntimeStatus status, const char* version)
{
    PlatformWebView::RuntimeInfo info;
    info.status = status;
    info.version = version;
    return info;
}

bool isAscii(const juce::String& s)
{
    for (auto p = s.getCharPointer(); !p.isEmpty(); ++p)
        if (static_cast<juce::uint32>(*p) > 0x7fu)
            return false;
    return true;
}

// mac 上用户看得见的字里不许出现的东西:WebView2、msedgewebview2、Microsoft Edge。
bool namesWebView2(const juce::String& s)
{
    return s.containsIgnoreCase("webview2") || s.containsIgnoreCase("msedge") || s.contains("Edge");
}

// 每格一个临时目录,格末整个删掉。
struct ScratchDir
{
    ScratchDir()
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile(
                      "scvb-m07-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()), "", false))
    {
        REQUIRE(dir.createDirectory().wasOk());
    }
    ~ScratchDir() { dir.deleteRecursively(); }
    juce::File dir;
};

juce::StringArray linesOf(const juce::File& f)
{
    juce::StringArray lines;
    if (f.existsAsFile())
        lines.addLines(f.loadFileAsString());
    lines.removeEmptyStrings();
    return lines;
}

// 全局日志的进出口:无论这一格从哪条路退出,都恢复「没装 + IpcDiag 空操作」,不把状态漏给下一格。
struct PlatformLogGuard
{
    PlatformLogGuard() { scvb::platformlog::uninstall(); }
    ~PlatformLogGuard() { scvb::platformlog::uninstall(); }
};

// 读源文件:剥 // 与 /* */ 注释、删掉字面量之外的全部空白(不钉排版),字面量原样保留;顺手收集
// **运行期**字符串字面量的内容(static_assert(...) 里的不算 —— 那是编译期消息,用户看不到)。
// 与 test_input_bridge.cpp 的 readStrippedSource 同一手法,多认了字面量:那边的版本会把 "https://"
// 里的 // 当注释吃掉,这里要扫字面量,不能那样。
struct LexedSource
{
    std::string code;
    std::vector<std::string> literals;
};

LexedSource lexSource(const char* relPath)
{
    const std::string path = std::string(SCVB_SOURCE_DIR) + "/" + relPath;
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    LexedSource out;
    const std::string kStaticAssert = "static_assert";
    int depth = 0;
    int staticAssertDepth = -1; // static_assert( 开在哪一层括号;-1 = 不在里面
    const auto n = raw.size();
    for (std::size_t i = 0; i < n;)
    {
        const char c = raw[i];
        if (c == '/' && i + 1 < n && raw[i + 1] == '/')
        {
            while (i < n && raw[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && raw[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < n && !(raw[i] == '*' && raw[i + 1] == '/'))
                ++i;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            std::string body;
            out.code.push_back(c);
            ++i;
            while (i < n && raw[i] != c)
            {
                if (raw[i] == '\\' && i + 1 < n)
                {
                    body.push_back(raw[i]);
                    out.code.push_back(raw[i]);
                    ++i;
                }
                body.push_back(raw[i]);
                out.code.push_back(raw[i]);
                ++i;
            }
            out.code.push_back(c);
            ++i;
            if (c == '"' && staticAssertDepth < 0)
                out.literals.push_back(body);
            continue;
        }
        if (c == '(')
        {
            ++depth;
            if (out.code.size() >= kStaticAssert.size() &&
                out.code.compare(out.code.size() - kStaticAssert.size(), kStaticAssert.size(), kStaticAssert) == 0)
                staticAssertDepth = depth;
        }
        else if (c == ')')
        {
            if (depth == staticAssertDepth)
                staticAssertDepth = -1;
            --depth;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            out.code.push_back(c);
        ++i;
    }
    return out;
}

// 取一个成员函数的函数体:从(去空白后的)签名起,到下一个 nextMarker 为止。改名 / 挪走 = 判负,不是跳过。
std::string methodBody(const std::string& code, const std::string& signature, const std::string& nextMarker)
{
    const auto begin = code.find(signature);
    REQUIRE(begin != std::string::npos);
    const auto end = code.find(nextMarker, begin + signature.size());
    return code.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::size_t countOf(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}
} // namespace

TEST_CASE("[M07] WebView2 copy and diagnostics lines stay byte-identical to the pre-M07 text", "[M07][platform]")
{
    // 期望串逐字取自 M07 之前 WebViewHost.cpp 里的字面量(missingRuntimeMessage / tooOldRuntimeMessage /
    // envNotStartedMessage / logBackgroundColourSupport / buildDiagnostics)。**这就是 Windows 用户看到的字**:
    // 搬家不许改一个字节。每一条都是独立落点,一律 CHECK。
    const auto wv2 = Engine::webView2;
    CHECK(PlatformWebView::missingRuntimeMessage(wv2) ==
          juce::String("Microsoft Edge WebView2 Runtime was not found, so the full UI cannot load.\n"
                       "Install the runtime once, then reopen this plugin window."));
    CHECK(PlatformWebView::tooOldRuntimeMessage(wv2) ==
          juce::String("The installed Microsoft Edge WebView2 Runtime is too old for this plugin.\n"
                       "Update to the Evergreen runtime, then reopen this plugin window."));
    CHECK(PlatformWebView::envNotStartedMessage(wv2) ==
          juce::String("The WebView2 environment did not start (no navigation ever began).\n"
                       "This is usually the user-data folder being unwritable or already in use by another\n"
                       "process, or the host blocking the msedgewebview2.exe child process."));

    using RS = PlatformWebView::RuntimeStatus;
    const auto current = runtimeOf(RS::ok, "137.0.3296.83");
    const auto old = runtimeOf(RS::ok, "86.0.622.38");
    const auto garbled = runtimeOf(RS::ok, "dev-build");
    const auto missing = runtimeOf(RS::missing, "");

    // 诊断行(兜底面板 + 日志)里运行时那一段。
    CHECK(PlatformWebView::runtimeDiagnosticsField(current, wv2) == "WebView2 137.0.3296.83");
    CHECK(PlatformWebView::runtimeDiagnosticsField(missing, wv2) == "WebView2 not found");

    // [SL-376] 背景色诊断行的四种形态(argb = shellBackdropMid() = #d9cadb,上面 [SL-402] 那格钉着)。
    CHECK(PlatformWebView::backgroundColourDiagnostics(current, wv2) ==
          "webview2 default background: available -- ICoreWebView2Controller2 inferred present (from runtime "
          "137.0.3296.83 >= major 87, not directly observed), JUCE puts argb ffd9cadb");
    CHECK(PlatformWebView::backgroundColourDiagnostics(old, wv2) ==
          "webview2 default background: UNAVAILABLE -- ICoreWebView2Controller2 inferred absent (from runtime "
          "86.0.622.38 < major 87, not directly observed), JUCE drops argb ffd9cadb silently");
    CHECK(PlatformWebView::backgroundColourDiagnostics(missing, wv2) ==
          "webview2 default background: unknown (no WebView2 runtime detected)");
    CHECK(PlatformWebView::backgroundColourDiagnostics(garbled, wv2) ==
          "webview2 default background: unknown (runtime version dev-build not parsable)");

    // 行为开关:遮挡闸与「下载运行时」按钮在 WebView2 上照旧开着。
    CHECK(PlatformWebView::revealGateEnabled(wv2));
    CHECK(PlatformWebView::offersRuntimeDownload(wv2));

    // UDF 那一段:非空时原样是完整路径。
    const auto udf = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SCVBInputWV2");
    CHECK(PlatformWebView::userDataFolderDisplay(udf) == udf.getFullPathName());

#if JUCE_WINDOWS
    // Windows 上默认值就是 WebView2 这一支 —— 上面钉的字就是插件实际打出来的字。
    CHECK(PlatformWebView::kEngine == Engine::webView2);
    CHECK(PlatformWebView::missingRuntimeMessage() == PlatformWebView::missingRuntimeMessage(wv2));
    CHECK(PlatformWebView::tooOldRuntimeMessage() == PlatformWebView::tooOldRuntimeMessage(wv2));
    CHECK(PlatformWebView::envNotStartedMessage() == PlatformWebView::envNotStartedMessage(wv2));
    CHECK(PlatformWebView::runtimeDiagnosticsField(current) == PlatformWebView::runtimeDiagnosticsField(current, wv2));
    CHECK(PlatformWebView::backgroundColourDiagnostics(current) ==
          PlatformWebView::backgroundColourDiagnostics(current, wv2));
    CHECK(PlatformWebView::revealGateEnabled());
    CHECK(PlatformWebView::offersRuntimeDownload());
#endif
}

TEST_CASE("[M07] system WebKit copy never names WebView2, msedgewebview2 or Edge", "[M07][platform]")
{
    // mac 那一份文案在**每个平台**上都判(显式传 systemWebKit);[mac] 格另核 mac 上默认值确实落在这里。
    // 反向注入「在 mac 分支放回 WebView2 文案」红在这一格(以及 [mac] 那一格)。
    const auto wk = Engine::systemWebKit;
    using RS = PlatformWebView::RuntimeStatus;
    const auto okNoVersion = runtimeOf(RS::ok, ""); // = mac 上 runtimeInfo() 的返回值
    const auto withVersion = runtimeOf(RS::ok, "137.0.3296.83");
    const auto missing = runtimeOf(RS::missing, "");

    const juce::String texts[] = {
        PlatformWebView::missingRuntimeMessage(wk),
        PlatformWebView::tooOldRuntimeMessage(wk),
        PlatformWebView::envNotStartedMessage(wk),
        PlatformWebView::runtimeDiagnosticsField(okNoVersion, wk),
        PlatformWebView::runtimeDiagnosticsField(missing, wk),
    };
    for (const auto& t : texts)
    {
        INFO(t);
        CHECK(t.isNotEmpty());
        CHECK_FALSE(namesWebView2(t));
        CHECK(isAscii(t)); // 运行期文案只写 ASCII(C4819 纪律)
    }

    // 诊断行写 `WebKit (system)`,不管 version 在不在(mac 上恒空)—— 不许再误报「not found」。
    CHECK(PlatformWebView::runtimeDiagnosticsField(okNoVersion, wk) == "WebKit (system)");
    CHECK(PlatformWebView::runtimeDiagnosticsField(missing, wk) == "WebKit (system)");

    // 背景色诊断只在 WebView2 上出:系统 WebKit 一律空串(调用方据此不打这一行)。
    CHECK(PlatformWebView::backgroundColourDiagnostics(okNoVersion, wk).isEmpty());
    CHECK(PlatformWebView::backgroundColourDiagnostics(withVersion, wk).isEmpty());
    CHECK(PlatformWebView::backgroundColourDiagnostics(missing, wk).isEmpty());

    CHECK_FALSE(PlatformWebView::revealGateEnabled(wk));
    CHECK_FALSE(PlatformWebView::offersRuntimeDownload(wk));

    // 两份不是同一句(防「两支返回同一个变量」这种改坏法让上面的 namesWebView2 两头都绿/都红)。
    CHECK(PlatformWebView::envNotStartedMessage(wk) != PlatformWebView::envNotStartedMessage(Engine::webView2));
    CHECK(PlatformWebView::missingRuntimeMessage(wk) != PlatformWebView::missingRuntimeMessage(Engine::webView2));

    // WebView2 那一份的确点名了 WebView2(否则上面的 namesWebView2 是个恒 false 的判据)。
    CHECK(namesWebView2(PlatformWebView::envNotStartedMessage(Engine::webView2)));
    CHECK(isAscii(PlatformWebView::envNotStartedMessage(Engine::webView2)));
}

TEST_CASE("[M07] reveal gate is armed only on the WebView2 engine", "[M07][platform]")
{
    // 这一格复刻 WebViewHost::onNavigationStarted 的分支形状(接线本身由下面的 [source] 格钉):
    // 闸门只在 revealGateEnabled() 时武装。系统 WebKit 上首帧信号一直不来时,WebView 从头到尾不挪、
    // 也就不用白等 kRevealFallbackMs —— 那正是 mac 上不启用它的理由。
    using scvb::webview::RevealGate;
    for (const auto engine : {Engine::webView2, Engine::systemWebKit})
    {
        const bool enabled = PlatformWebView::revealGateEnabled(engine);
        INFO("engine webView2=" << (engine == Engine::webView2));
        RevealGate gate;
        gate.beginLoadAttempt();
        const std::uint32_t navMs = 1000;
        if (enabled)
            gate.onNavigationStarted(navMs);
        CHECK(gate.parked() == enabled);
        gate.onNavigationFinished();
        gate.onTick(navMs + RevealGate::kRevealFallbackMs - 1);
        CHECK(gate.parked() == enabled);
        gate.onTick(navMs + RevealGate::kRevealFallbackMs);
        CHECK_FALSE(gate.parked());
        CHECK(juce::String(gate.lastRevealReason()) == juce::String(enabled ? "timeout" : ""));
    }
}

TEST_CASE("[M07] an empty user-data folder means not used: no probe, shown as (not used)", "[M07][platform]")
{
    // 系统 WebKit 上 makeUserDataFolder 返回空 File;探针对空 File 不建目录、不写探针、不报问题。
    // (Windows 上 UDF 恒非空,走不到这一支;这里直接喂空 File 判函数本身。)
    CHECK(PlatformWebView::probeUserDataFolder(juce::File()).isEmpty());
    CHECK(PlatformWebView::userDataFolderDisplay(juce::File()) == "(not used)");
    CHECK_FALSE(juce::File::getCurrentWorkingDirectory().getChildFile(".scvb-write-probe").exists());
}

TEST_CASE("[M07] WebViewHost.cpp routes platform text and the reveal gate through PlatformWebView", "[M07][source]")
{
    // WebViewHost.cpp 不进任何测试目标(真 WebView 实例化),上面几格证明 PlatformWebView 本身对,
    // 证明不了 WebViewHost 真的在用它 —— 把调用点改回字面量或去掉闸门的条件,那几格照样全绿。
    // 判据读的是源码文本(去注释、去空白后按片段匹配):红了先看是写法变了(改名、拆函数)还是接线真的断了。
    // 不变式是:带平台名字的文案 / 诊断行、遮挡闸的启用条件都经 PlatformWebView,日志同一行交给 platformlog::write。
    INFO("source-level check on WebViewHost.cpp: platform copy and the reveal-gate condition must go through "
         "PlatformWebView, logDiag must also feed platformlog::write; if only the wording of the code changed, "
         "update the expected snippet");
    const auto src = lexSource("src/plugin-common/WebViewHost.cpp");

    // ① 用户可见的、带平台名字的字都不再写在这个文件里(static_assert 的编译期消息不算)。
    CHECK(src.literals.size() > 20); // 判据自检:字面量真的收上来了(空集会让下面的循环恒绿)
    for (const auto& lit : src.literals)
    {
        INFO(lit);
        CHECK_FALSE(namesWebView2(juce::String(lit)));
    }

    // ② 遮挡闸:武装只在 revealGateEnabled() 之下,且全文件只有这一处武装。
    const auto nav = methodBody(src.code, "voidWebViewHost::onNavigationStarted(", "WebViewHost::");
    CHECK(nav.find("if(PlatformWebView::revealGateEnabled()){revealGate_.onNavigationStarted(juce::Time::"
                   "getMillisecondCounter());applyRevealGate();}") != std::string::npos);
    CHECK(countOf(src.code, "revealGate_.onNavigationStarted(") == 1);

    // ③ 兜底面板:三条提到运行时的文案 + install 按钮的平台条件。
    const auto fallback = methodBody(src.code, "voidWebViewHost::showFallback(", "WebViewHost::");
    CHECK(fallback.find("message=PlatformWebView::missingRuntimeMessage();") != std::string::npos);
    CHECK(fallback.find("message=PlatformWebView::tooOldRuntimeMessage();") != std::string::npos);
    CHECK(fallback.find("message=PlatformWebView::envNotStartedMessage();") != std::string::npos);
    CHECK(fallback.find("options.showInstall=(missing||tooOld)&&PlatformWebView::offersRuntimeDownload();") !=
          std::string::npos);

    // ④ 诊断行:运行时那一段 + UDF 那一段;背景色诊断行;冷/热起步那一行的 UDF。
    const auto diag = methodBody(src.code, "juce::StringWebViewHost::buildDiagnostics(", "WebViewHost::");
    CHECK(diag.find("PlatformWebView::runtimeDiagnosticsField(runtime_)") != std::string::npos);
    CHECK(diag.find("PlatformWebView::userDataFolderDisplay(userDataFolder_)") != std::string::npos);
    const auto bg = methodBody(src.code, "voidWebViewHost::logBackgroundColourSupport(", "WebViewHost::");
    CHECK(bg.find("PlatformWebView::backgroundColourDiagnostics(runtime_)") != std::string::npos);
    const auto begin = methodBody(src.code, "voidWebViewHost::beginLoadAttempt(", "WebViewHost::");
    CHECK(begin.find("PlatformWebView::userDataFolderDisplay(userDataFolder_)") != std::string::npos);

    // ⑤ 日志:OutputDebugString 那条照旧(juce::Logger),同一行再交给平台文件日志(Windows 上是空操作)。
    const auto log = methodBody(src.code, "voidWebViewHost::logDiag(", "WebViewHost::");
    CHECK(log.find("juce::Logger::writeToLog(message);") != std::string::npos);
    CHECK(log.find("platformlog::write(message);") != std::string::npos);
}

TEST_CASE("[M07] plugin entry points install the platform log before the first processor", "[M07][source]")
{
    // install 必须早于本二进制里任何一个 Processor 的构造(段后端从构造起就可能经 IpcDiag 报失败)。
    INFO("source-level check: createPluginFilter() must call scvb::platformlog::install(<role>, "
         "JucePlugin_VersionString) before constructing the processor");
    struct Entry
    {
        const char* file;
        const char* install;
        const char* construct;
    };
    const Entry entries[] = {
        {"src/input/InputPluginEntry.cpp", "scvb::platformlog::install(\"input\",JucePlugin_VersionString);",
         "returnnewScvbInputAudioProcessor();"},
        {"src/output/OutputPluginEntry.cpp", "scvb::platformlog::install(\"output\",JucePlugin_VersionString);",
         "returnnewScvbOutputAudioProcessor();"},
        {"src/monitor/MonitorProcessor.cpp", "scvb::platformlog::install(\"monitor\",JucePlugin_VersionString);",
         "returnnewScvbMonitorAudioProcessor();"},
    };
    for (const auto& e : entries)
    {
        INFO(e.file);
        const auto src = lexSource(e.file);
        const auto filter = src.code.find("JUCE_CALLTYPEcreatePluginFilter()");
        REQUIRE(filter != std::string::npos);
        const auto install = src.code.find(e.install, filter);
        const auto construct = src.code.find(e.construct, filter);
        CHECK(install != std::string::npos);
        CHECK(construct != std::string::npos);
        CHECK(install < construct);
    }
}

TEST_CASE("[M07] RotatingLogFile rotates right before a line would cross the limit", "[M07][platformlog]")
{
    ScratchDir scratch;
    // 父目录还不存在:append 自己建。
    const auto file = scratch.dir.getChildFile("sub").getChildFile("role.log");
    scvb::platformlog::RotatingLogFile log(file, 200);
    CHECK(log.rotatedFile() == file.getSiblingFile("role.1.log"));

    // 每行 49 字符 + '\n' = 50 字节:4 行正好 200,第 5 行之前轮转。
    const auto lineFor = [](int i) { return juce::String(i) + juce::String::repeatedString("a", 48); };
    for (int i = 0; i < 4; ++i)
        REQUIRE(log.append(lineFor(i)));
    CHECK(file.getSize() == 200);
    CHECK_FALSE(log.rotatedFile().exists());

    REQUIRE(log.append(lineFor(4)));
    CHECK(log.rotatedFile().getSize() == 200);
    CHECK(file.getSize() == 50);
    CHECK(linesOf(log.rotatedFile()) == juce::StringArray(lineFor(0), lineFor(1), lineFor(2), lineFor(3)));
    CHECK(linesOf(file) == juce::StringArray(lineFor(4)));

    // 再轮转一次:只留一份旧的(上一份被覆盖),一行不丢地落在两份里。
    for (int i = 5; i < 9; ++i)
        REQUIRE(log.append(lineFor(i)));
    CHECK(linesOf(log.rotatedFile()) == juce::StringArray(lineFor(4), lineFor(5), lineFor(6), lineFor(7)));
    CHECK(linesOf(file) == juce::StringArray(lineFor(8)));
    CHECK_FALSE(file.getSiblingFile("role.2.log").exists());
}

TEST_CASE("[M07] DiagDeduper keeps one line per (op, error, segment) per window", "[M07][platformlog]")
{
    using scvb::platformlog::DiagDeduper;
    DiagDeduper d;
    const juce::String seg("/SynchainSCVB.v1.g1.registry");
    const std::uint32_t t0 = 1000;

    auto v = d.onEvent(4, 13, seg, t0);
    CHECK(v.emit);
    CHECK(v.repeatsSuppressed == 0);
    CHECK(v.rateLimited == 0);

    // 25Hz 重试路径上的同一个失败:窗口内只计数。
    int emitted = 0;
    for (std::uint32_t i = 1; i <= 99; ++i)
        emitted += d.onEvent(4, 13, seg, t0 + i * 40).emit ? 1 : 0;
    CHECK(emitted == 0);

    // 键的三个分量各自独立:换任何一个都是新键,当场落行。
    CHECK(d.onEvent(4, 2, seg, t0 + 10).emit);
    CHECK(d.onEvent(5, 13, seg, t0 + 10).emit);
    CHECK(d.onEvent(4, 13, "/SynchainSCVB.v1.g8.audio.ch15", t0 + 10).emit);
    CHECK(d.keyCount() == 4);

    // 窗口边界:差 1 ms 仍略去,到点落行并带上期间略去的次数(99 + 1),计数随之清零。
    CHECK_FALSE(d.onEvent(4, 13, seg, t0 + DiagDeduper::kRepeatWindowMs - 1).emit);
    v = d.onEvent(4, 13, seg, t0 + DiagDeduper::kRepeatWindowMs);
    CHECK(v.emit);
    CHECK(v.repeatsSuppressed == 100);
    CHECK_FALSE(d.onEvent(4, 13, seg, t0 + DiagDeduper::kRepeatWindowMs + 1).emit);
}

TEST_CASE("[M07] DiagDeduper global rate limit and key table cap", "[M07][platformlog]")
{
    using scvb::platformlog::DiagDeduper;
    {
        // 同一窗口里大量**不同**键:前 kMaxLinesPerWindow 条落行,其余只计数,并进下一窗口的第一条。
        DiagDeduper d;
        for (std::uint32_t k = 0; k < DiagDeduper::kMaxLinesPerWindow; ++k)
            CHECK(d.onEvent(1, static_cast<int>(k), "/s", 5000).emit);
        CHECK_FALSE(d.onEvent(1, 9999, "/s", 5001).emit);
        CHECK_FALSE(d.onEvent(1, 9998, "/s", 5002).emit);
        const auto v = d.onEvent(1, 9999, "/s", 5000 + DiagDeduper::kRepeatWindowMs);
        CHECK(v.emit);
        CHECK(v.rateLimited == 2);
    }
    {
        // 记住的键有上限:错开时间(每窗口不超过全局上限)喂 kMaxKeys + 10 个不同键,表不涨过上限。
        DiagDeduper d;
        const std::uint32_t step = DiagDeduper::kRepeatWindowMs / DiagDeduper::kMaxLinesPerWindow + 1;
        std::uint32_t now = 0;
        int emitted = 0;
        const auto total = static_cast<int>(DiagDeduper::kMaxKeys) + 10;
        for (int k = 0; k < total; ++k, now += step)
            emitted += d.onEvent(2, k, "/s", now).emit ? 1 : 0;
        CHECK(emitted == total);
        CHECK(d.keyCount() == DiagDeduper::kMaxKeys);
        // 最近落过行的键还在表里(窗口内照样略去),淘汰的是最旧的那些。
        CHECK_FALSE(d.onEvent(2, total - 1, "/s", now).emit);
    }
}

TEST_CASE("[M07] DiagDeduper window survives the millisecond counter wrapping around", "[M07][platformlog]")
{
    using scvb::platformlog::DiagDeduper;
    DiagDeduper d;
    const std::uint32_t nearWrap = 0xffffffffu - 1000u;
    CHECK(d.onEvent(3, 1, "/w", nearWrap).emit);
    CHECK_FALSE(d.onEvent(3, 1, "/w", 500u).emit); // 回绕后 1501 ms:仍在窗口内
    const std::uint32_t later = nearWrap + DiagDeduper::kRepeatWindowMs; // 无符号回绕到 58999
    const auto v = d.onEvent(3, 1, "/w", later);
    CHECK(v.emit);
    CHECK(v.repeatsSuppressed == 1);
}

TEST_CASE("[M07] platform log writes only on the message thread", "[M07][platformlog]")
{
    juce::ScopedJuceInitialiser_GUI gui; // 本线程 = 消息线程
    PlatformLogGuard guard;
    ScratchDir scratch;
    const auto file = scratch.dir.getChildFile("input.log");

    // 没装:写什么都不落。
    scvb::platformlog::write("before install");
    CHECK_FALSE(file.exists());

    REQUIRE(scvb::platformlog::installAt(file));
    CHECK(scvb::platformlog::isInstalled());
    CHECK(scvb::platformlog::installedFile() == file);
    // 先装者为准:同一个二进制里第二个实例再装是空操作。
    CHECK_FALSE(scvb::platformlog::installAt(scratch.dir.getChildFile("other.log")));
    CHECK(scvb::platformlog::installedFile() == file);

    scvb::platformlog::write("on the message thread");
    auto lines = linesOf(file);
    CHECK(lines.size() == 1);
    if (!lines.isEmpty())
        CHECK(lines[0].endsWith(" on the message thread"));

    // 别的线程:不就地写(投递给消息线程;本进程不跑消息循环,所以它不会落下来)。
    std::thread worker([] { scvb::platformlog::write("from a worker thread"); });
    worker.join();
    CHECK(linesOf(file).size() == 1);

    scvb::platformlog::uninstall();
    CHECK_FALSE(scvb::platformlog::isInstalled());
    scvb::platformlog::write("after uninstall");
    CHECK(linesOf(file).size() == 1);
}

#if JUCE_WINDOWS
TEST_CASE("[M07] Windows keeps OutputDebugString only: no file log is installed", "[M07][platformlog]")
{
    // Windows 维持现状:不落文件日志。默认路径为空 ⇒ install() 空操作 ⇒ write() 什么都不做。
    PlatformLogGuard guard;
    for (const char* role : {"input", "output", "monitor"})
        CHECK(scvb::platformlog::defaultLogFile(role) == juce::File());
    CHECK_FALSE(scvb::platformlog::install("input", "0.0.0"));
    CHECK_FALSE(scvb::platformlog::isInstalled());
    CHECK(scvb::platformlog::installedFile() == juce::File());
}
#endif

#if JUCE_MAC
TEST_CASE("[M07][mac] defaults resolve to system WebKit: copy, diagnostics, reveal gate, pid", "[M07][mac]")
{
    CHECK(PlatformWebView::kEngine == Engine::systemWebKit);
    const auto okNoVersion = runtimeOf(PlatformWebView::RuntimeStatus::ok, ""); // = runtimeInfo() on macOS
    const juce::String texts[] = {
        PlatformWebView::missingRuntimeMessage(),
        PlatformWebView::tooOldRuntimeMessage(),
        PlatformWebView::envNotStartedMessage(),
        PlatformWebView::runtimeDiagnosticsField(okNoVersion),
    };
    for (const auto& t : texts)
    {
        INFO(t);
        CHECK_FALSE(namesWebView2(t));
    }
    CHECK(PlatformWebView::runtimeDiagnosticsField(okNoVersion) == "WebKit (system)");
    CHECK(PlatformWebView::backgroundColourDiagnostics(okNoVersion).isEmpty());
    CHECK_FALSE(PlatformWebView::revealGateEnabled());
    CHECK_FALSE(PlatformWebView::offersRuntimeDownload());
    // pid 进诊断行:mac 上与活动监视器对得上(M07 之前恒 0)。
    CHECK(PlatformWebView::processId() == static_cast<int>(::getpid()));
    CHECK(PlatformWebView::processId() > 0);
}

TEST_CASE("[M07][mac] no user-data folder: empty root, no directory, no probe file", "[M07][mac]")
{
    // M07 之前这里落在 tempDirectory/SCVB-WebView(= ~/Library/Caches/<宿主名>/SCVB-WebView)并建目录、写探针。
    const auto legacy = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SCVB-WebView");
    const bool legacyExisted = legacy.exists();

    CHECK(PlatformWebView::userDataFolderRoot() == juce::File());
    const auto in = PlatformWebView::makeUserDataFolder("SCVBInputWV2");
    CHECK(in == juce::File());
    CHECK(PlatformWebView::probeUserDataFolder(in).isEmpty());
    CHECK(PlatformWebView::userDataFolderDisplay(in) == "(not used)");

    CHECK(legacy.exists() == legacyExisted); // 不建旧目录
    CHECK_FALSE(juce::File("/SCVBInputWV2").exists()); // 空根没被拼成文件系统根下的路径
}

TEST_CASE("[M07][mac] default log file lives under ~/Library/Logs/Synchain/SCVB", "[M07][mac]")
{
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    REQUIRE(home != juce::File());
    for (const char* role : {"input", "output", "monitor"})
    {
        INFO(role);
        CHECK(scvb::platformlog::defaultLogFile(role) ==
              home.getChildFile("Library/Logs/Synchain/SCVB").getChildFile(juce::String(role) + ".log"));
    }
    // 角色名不在闭集里:不拼路径。
    CHECK(scvb::platformlog::defaultLogFile("") == juce::File());
    CHECK(scvb::platformlog::defaultLogFile("../input") == juce::File());
}

TEST_CASE("[M07][mac] the installed log writes and rotates at 1 MB", "[M07][mac]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PlatformLogGuard guard;
    ScratchDir scratch;
    const auto file = scratch.dir.getChildFile("Logs").getChildFile("input.log"); // 父目录由日志自己建
    REQUIRE(scvb::platformlog::installAt(file));

    // 每行约 2 KB(时间戳 + 序号 + 2000 字符):600 行约 1.2 MB ⇒ 走生产阈值 kMaxLogBytes 恰好轮转一次。
    const juce::String body = juce::String::repeatedString("x", 2000);
    constexpr int kLines = 600;
    for (int i = 0; i < kLines; ++i)
        scvb::platformlog::write(juce::String(i) + " " + body);

    const auto rotated = file.getSiblingFile("input.1.log");
    CHECK(rotated.existsAsFile());
    CHECK(rotated.getSize() <= scvb::platformlog::kMaxLogBytes);
    CHECK(rotated.getSize() > scvb::platformlog::kMaxLogBytes - 4096); // 轮转在「再写一行就超」那一刻,不更早
    CHECK(file.getSize() <= scvb::platformlog::kMaxLogBytes);
    const auto older = linesOf(rotated);
    const auto newer = linesOf(file);
    CHECK(older.size() + newer.size() == kLines); // 一次轮转,一行不丢
    if (!older.isEmpty() && !newer.isEmpty())
    {
        CHECK(older[0].contains(" 0 x"));
        CHECK(newer[newer.size() - 1].contains(" " + juce::String(kLines - 1) + " x"));
    }
}

TEST_CASE("[M07][mac] several writers on one log file never overwrite each other's lines", "[M07][mac]")
{
    // 同一份 <role>.log 的多个写者(同一宿主里同角色的 AU 与 VST3 是两个二进制,各有一把进程内锁):
    // 这里用 4 个线程、各自一个 RotatingLogFile 实例(= 各自的锁)同时往同一个文件追加。
    // 每行一次 O_APPEND write ⇒ 一行不丢、一行不残。阈值放大到不会轮转(轮转跨写者不协调,头注里写明了)。
    ScratchDir scratch;
    const auto file = scratch.dir.getChildFile("input.log");
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 2000;
    const juce::String pad = juce::String::repeatedString("x", 80);
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w)
        writers.emplace_back([&file, &pad, w] {
            scvb::platformlog::RotatingLogFile log(file, 64 * 1024 * 1024);
            for (int i = 0; i < kPerWriter; ++i)
                log.append("w" + juce::String(w) + " " + juce::String(i) + " " + pad);
        });
    for (auto& t : writers)
        t.join();

    const auto lines = linesOf(file);
    CHECK(lines.size() == kWriters * kPerWriter);
    int intact = 0;
    std::vector<int> perWriter(kWriters, 0);
    for (const auto& l : lines)
    {
        const auto tokens = juce::StringArray::fromTokens(l, " ", "");
        if (tokens.size() == 3 && tokens[0].length() == 2 && tokens[0][0] == 'w' && tokens[2] == pad)
        {
            const int w = tokens[0].getTrailingIntValue();
            if (w >= 0 && w < kWriters)
            {
                ++perWriter[static_cast<std::size_t>(w)];
                ++intact;
            }
        }
    }
    CHECK(intact == kWriters * kPerWriter);
    for (int w = 0; w < kWriters; ++w)
    {
        INFO("writer " << w);
        CHECK(perWriter[static_cast<std::size_t>(w)] == kPerWriter);
    }
}

TEST_CASE("[M07][mac] IpcDiag sink keeps one line per (op, error, segment)", "[M07][mac]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PlatformLogGuard guard;
    ScratchDir scratch;
    const auto file = scratch.dir.getChildFile("output.log");

    const auto hasSink = [] { return scvb::ipcDiagSink() != nullptr; };
    CHECK_FALSE(hasSink()); // M05 的默认:空操作
    REQUIRE(scvb::platformlog::installAt(file));
    CHECK(hasSink());

    scvb::IpcDiagEvent e;
    e.op = scvb::IpcDiagOp::kShmOpen;
    e.error = 13;
    e.segment = "/SynchainSCVB.v1.g1.registry";
    e.detail = "shm_open failed";
    // 25Hz 重试路径上的同一个失败报 50 次:只落一行。反向注入「去掉 sink 去重」红在这一格。
    for (int i = 0; i < 50; ++i)
        scvb::reportIpcDiag(e);
    auto lines = linesOf(file);
    CHECK(lines.size() == 1);
    if (!lines.isEmpty())
        CHECK(lines[0].contains("ipc shm-open errno 13 segment /SynchainSCVB.v1.g1.registry -- shm_open failed"));

    // 键的三个分量各自独立。
    e.error = 1;
    scvb::reportIpcDiag(e);
    e.segment = "/SynchainSCVB.v1.g8.audio.ch15";
    scvb::reportIpcDiag(e);
    e.op = scvb::IpcDiagOp::kSegmentLock;
    e.path = "/tmp/scvb-m07.lock";
    scvb::reportIpcDiag(e);
    lines = linesOf(file);
    CHECK(lines.size() == 4);
    if (lines.size() == 4)
        CHECK(lines[3].contains(
            "ipc segment-lock errno 1 segment /SynchainSCVB.v1.g8.audio.ch15 path /tmp/scvb-m07.lock"));

    // 卸下:IpcDiag 回到空操作,之后的报告不落盘。
    scvb::platformlog::uninstall();
    CHECK_FALSE(hasSink());
    scvb::reportIpcDiag(e);
    CHECK(linesOf(file).size() == 4);
}
#endif
