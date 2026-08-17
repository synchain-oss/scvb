// SPDX-License-Identifier: GPL-3.0-or-later
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include <BridgeBase.h>
#include <FallbackPanel.h>
#include <PlatformWebView.h>
#include <ResourceProvider.h>

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

TEST_CASE("ResourceProvider serves by original filename")
{
    scvb::webview::ResourceProvider::Source src;
    src.resourceCount = 1;
    src.originalFilenames = kOriginalFilenames;
    src.resourceNames = kResourceNames;
    src.getNamedResource = &fakeGetResource;

    scvb::webview::ResourceProvider provider(src);
    const auto res = provider.provide("/index.html");
    REQUIRE(res.has_value());
    CHECK(res->mimeType == juce::String("text/html"));
    CHECK(res->data.size() == 5);
}

TEST_CASE("clampUiScale matches Bridge bounds")
{
    using scvb::bridge::clampUiScale;
    CHECK(clampUiScale(1.0f) == 1.0f);
    CHECK(clampUiScale(0.1f) == scvb::bridge::plugin::MinUiScale);
    CHECK(clampUiScale(9.0f) == scvb::bridge::plugin::MaxUiScale);
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
    CHECK(panel.getNumChildComponents() == 4); // title + message + install + retry

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
    CHECK(panel.getNumChildComponents() == 3); // title + message + retry
    CHECK(panel.findChildWithID("fallback.install") == nullptr);
    CHECK(panel.findChildWithID("fallback.retry") != nullptr);
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

TEST_CASE("parseUiScaleArg rejects non-numeric and clamps numeric (§1.28)")
{
    using scvb::bridge::parseUiScaleArg;
    float out = 0.0f;

    juce::Array<juce::var> nonNumeric;
    nonNumeric.add(juce::var("abc"));
    CHECK_FALSE(parseUiScaleArg(nonNumeric, out)); // 非数字 → badArg

    juce::Array<juce::var> empty;
    CHECK_FALSE(parseUiScaleArg(empty, out)); // 缺参 → badArg

    juce::Array<juce::var> small;
    small.add(juce::var(0.1));
    REQUIRE(parseUiScaleArg(small, out));
    CHECK(out == scvb::bridge::plugin::MinUiScale); // 数值 → clamp 下界

    juce::Array<juce::var> big;
    big.add(juce::var(9.0));
    REQUIRE(parseUiScaleArg(big, out));
    CHECK(out == scvb::bridge::plugin::MaxUiScale); // clamp 上界

    juce::Array<juce::var> ok;
    ok.add(juce::var(1.5));
    REQUIRE(parseUiScaleArg(ok, out));
    CHECK(out == 1.5f);
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

TEST_CASE("makeWebViewOptions selects WebView2 backend + unique userDataFolder (§9/#5)")
{
    using WBC = juce::WebBrowserComponent;
#if JUCE_WINDOWS
    const auto o1 = scvb::webview::PlatformWebView::makeWebViewOptions(WBC::Options{}, "SCVBInputWV2");
    CHECK(o1.getBackend() == WBC::Options::Backend::webview2); // 机制 1:显式选 WebView2
    const auto f1 = o1.getWinWebView2BackendOptions().getUserDataFolder();
    CHECK(f1.getFileName().startsWith("SCVBInputWV2_")); // 机制 2:临时目录 + 每实例后缀

    const auto o2 = scvb::webview::PlatformWebView::makeWebViewOptions(WBC::Options{}, "SCVBInputWV2");
    const auto f2 = o2.getWinWebView2BackendOptions().getUserDataFolder();
    CHECK_FALSE(f1.getFileName() == f2.getFileName()); // 多实例同名 folder 抢占防护
#else
    const auto o1 = scvb::webview::PlatformWebView::makeWebViewOptions(WBC::Options{}, "SCVBInputWV2");
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
