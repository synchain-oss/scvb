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
//
// ⚠ **判定面只有三行 Label,两个按钮不在里面**(#241 复审;这里写清边界,别把本用例读成
// 「兜底面板整体可读」的全称保证)。`fallback.install` / `fallback.retry` 都没有显式
// setColour,吃的是 LookAndFeel 默认;而全仓 `grep -rn "setLookAndFeel" src/` 零命中
// ⇒ 走 JUCE 默认的 LookAndFeel_V4(暗色配色),按钮底深、字浅,**今天**在这块浅底上看得见。
// 也就是说「按钮可读」现在挂在「JUCE 默认 LNF 恰好是暗色」这条**隐性依赖**上:
// 哪天给编辑器挂一套浅色 LookAndFeel,按钮会连着面板一起变浅,而本用例一格都不会红。
// 没有把按钮纳进循环,是因为那样等于把 JUCE 默认配色钉成判据(升 .juce-version 就可能红,
// 而红的原因与本卡无关);要收这条洞,该做的是给面板一套自己的显式配色,那是另一张卡。
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
// [SL-370] 开窗遮挡闸(RevealGate)。
//
// WebViewHost.cpp **不进任何测试目标**(它只在随插件 target 编译的 INTERFACE 库里),
// 所以「WebView 此刻该不该待在可视区外」这个判定被抽成了纯逻辑的 RevealGate,
// 才有下面这几格。接线那一半(谁调 onNavigationStarted / onFirstFrame / onTick)
// 编译期与运行期都够不着,只能靠 WebViewHost.cpp 里的调用点与真机验收 —— 这是
// 已登记的覆盖缺口(与 SL-282 同族),别把这几格读成「整条链都验过了」。
// -----------------------------------------------------------------------------
TEST_CASE("[SL-370] RevealGate parks on navigation start and reveals on the first-frame signal")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    CHECK_FALSE(gate.parked()); // 控制器建好之前必须留在原位:SL-271 的重试泵挂在 paint 上

    gate.onNavigationStarted(1000);
    CHECK(gate.parked());

    gate.onFirstFrame();
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "firstFrame");
}

TEST_CASE("[SL-370] RevealGate reveals on navigation finished when the first-frame signal never arrives")
{
    scvb::webview::RevealGate gate;
    gate.beginLoadAttempt();
    gate.onNavigationStarted(1000);
    REQUIRE(gate.parked());

    gate.onNavigationFinished();
    CHECK_FALSE(gate.parked());
    CHECK(juce::String(gate.lastRevealReason()) == "navFinished");
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
    gate.onFirstFrame();
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
    gate.onFirstFrame();
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
