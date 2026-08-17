// SPDX-License-Identifier: GPL-3.0-or-later
#include <juce_gui_extra/juce_gui_extra.h>

#include <BridgeBase.h>
#include <FallbackPanel.h>
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
