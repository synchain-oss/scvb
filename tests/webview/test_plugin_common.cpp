// SPDX-License-Identifier: GPL-3.0-or-later
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include <BridgeBase.h>
#include <FallbackPanel.h>
#include <PlatformWebView.h>
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
