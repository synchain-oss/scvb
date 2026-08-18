// SPDX-License-Identifier: GPL-3.0-or-later
#include "WebViewHost.h"

#include "BridgeBase.h"
#include "PlatformWebView.h"

#include <utility>

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

namespace
{
juce::String fallbackTitle(const juce::String& role)
{
    return role == "input" ? juce::String("SCVB Input") : juce::String("SCVB Output");
}

juce::String missingRuntimeMessage()
{
    return "Microsoft Edge WebView2 Runtime was not found, so the full UI cannot load.\n"
           "Install the runtime once, then reopen this plugin window.";
}

juce::String loadTimeoutMessage()
{
    return "The WebView is taking too long to load (possibly a first-time cold start).\n"
           "Click Retry, or close and reopen this plugin window.";
}
} // namespace

WebViewHost::WebViewHost(juce::AudioProcessor& processor, Config config)
    : juce::AudioProcessorEditor(&processor), config_(std::move(config)), provider_(config_.resourceSource),
      uiScale_(bridge::clampUiScale(config_.uiScale)), lang_(config_.lang), webView_(makeOptions())
{
    addAndMakeVisible(webView_);

    // 真 WebView2 实例化与 runtimeAvailable 探测无法离线单测(需 WebView2 loader + 真机),
    // 属硬前置:T29/T30 接 InputEditor/OutputEditor 后经 gate 8 真机 GUI pluginval 验证。
    setResizable(false, false); // 仅经缩放档位下拉(setUiScale)编程改尺寸,不开自由拖角。
    resizeToDesignBox(uiScale_);

    // 先探测 WebView2 运行时:有则正常加载(看门狗容忍冷启动);无则直接给可操作的兜底面板,
    // 并引导一次性安装,不做无意义等待。
    if (PlatformWebView::runtimeAvailable())
    {
        // 必须在任何 emit 之前完成首个 goToURL(前端脚本随后加载并注册监听)。
        webView_.goToURL(WBC::getResourceProviderRoot());
        startMs_ = juce::Time::getMillisecondCounter();
        startTimerHz(25);
    }
    else
    {
        webView_.setVisible(false);
        showFallback(FallbackReason::MissingRuntime);
    }
}

WebViewHost::~WebViewHost()
{
    stopTimer();
}

void WebViewHost::resized()
{
    webView_.setBounds(getLocalBounds());
    if (fallback_ != nullptr)
        fallback_->setBounds(getLocalBounds());
}

// -----------------------------------------------------------------------------
// 缩放(机制 9):固定设计盒 × CSS zoom + setSize 同步
// -----------------------------------------------------------------------------
void WebViewHost::resizeToDesignBox(float scale)
{
    const auto r = bridge::designBoxWindowSize(config_.role, scale);
    setSize(r.width, r.height);
}

void WebViewHost::setUiScale(float scale)
{
    uiScale_ = bridge::clampUiScale(scale);
    resizeToDesignBox(uiScale_);
}

void WebViewHost::commitUiScale()
{
    persistUiScaleAsDefault();
}

void WebViewHost::persistUiScaleAsDefault()
{
    // 默认空实现:宿主侧全局默认落盘由插件 Processor 承担(T29/T30 或后续)。
}

juce::var WebViewHost::buildSnapshot()
{
    bridge::UiSeed seed;
    seed.role = config_.role;
    seed.version = config_.version;
    seed.lang = lang_;
    seed.uiScale = uiScale_;
    seed.channelLimit = config_.channelLimit;
    return bridge::buildUiSnapshot(seed);
}

// -----------------------------------------------------------------------------
// 兜底面板(机制 3 后半)
// -----------------------------------------------------------------------------
void WebViewHost::showFallback(FallbackReason reason)
{
    if (fallback_ != nullptr)
        return;
    webView_.setVisible(false);

    const bool missing = (reason == FallbackReason::MissingRuntime);
    FallbackPanel::Options options;
    options.title = fallbackTitle(config_.role);
    options.message = missing ? missingRuntimeMessage() : loadTimeoutMessage();
    options.showInstall = missing;
    options.onInstall = [] { juce::URL("https://go.microsoft.com/fwlink/p/?LinkId=2124703").launchInDefaultBrowser(); };
    // 延后到消息线程执行:FallbackPanel 的 retry onClick 内同步 reset 会销毁正执行回调的按钮
    // (use-after-free)。SafePointer 兜底 WebViewHost 先于回调被销毁的情况。
    options.onRetry = [safeThis = juce::Component::SafePointer<WebViewHost>(this)] {
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->retryWebView();
        });
    };

    fallback_ = std::make_unique<FallbackPanel>(std::move(options));
    addAndMakeVisible(*fallback_);

    // 兜底面板固定像素布局(不随 uiScale 缩放)。小缩放档位下窗口过小会挤压/裁掉按钮,
    // 故切兜底时放大到至少设计盒尺寸;更大的缩放窗口保持不变。retryWebView 回到 WebView 时按 uiScale 恢复。
    const auto design = bridge::designBoxWindowSize(config_.role, 1.0f);
    if (getWidth() < design.width || getHeight() < design.height)
        setSize(juce::jmax(getWidth(), design.width), juce::jmax(getHeight(), design.height));
    resized();
}

void WebViewHost::retryWebView()
{
    fallback_.reset();
    if (!PlatformWebView::runtimeAvailable())
    {
        webView_.setVisible(false);
        showFallback(FallbackReason::MissingRuntime);
        return;
    }
    bridgeReady_ = false;
    resizeToDesignBox(uiScale_);
    webView_.setVisible(true);
    webView_.goToURL(WBC::getResourceProviderRoot());
    startMs_ = juce::Time::getMillisecondCounter();
    if (!isTimerRunning())
        startTimerHz(25);
    resized();
}

// -----------------------------------------------------------------------------
// WebView 装配(机制 1/2/4/5/6)
// -----------------------------------------------------------------------------
juce::WebBrowserComponent::Options WebViewHost::makeOptions()
{
    auto options = PlatformWebView::makeWebViewOptions(WBC::Options{}, config_.userDataFolderName);

    options = options.withNativeIntegrationEnabled().withResourceProvider(
        [this](const juce::String& url) { return provider_.provide(url); },
        juce::URL(WBC::getResourceProviderRoot()).getOrigin());

    // 首帧同步 seed(机制 5):version / 角色 / channel 上限 / lang / uiScale(键值对与 buildSnapshot 同源)。
    for (const auto& seedPair :
         bridge::buildUiSeedPairs({config_.role, config_.version, lang_, uiScale_, config_.channelLimit}))
        options = options.withInitialisationData(seedPair.first, seedPair.second);

    // JS -> C++(机制 6):通用缩放/语言/首帧;插件专属函数由 augmentOptions 追加。
    options = options
                  .withNativeFunction(juce::Identifier(bridge::Fn::RequestInitialState),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRequestInitialState(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetLang),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetLang(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::SetUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetUiScale(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier(bridge::Fn::CommitUiScale),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleCommitUiScale(a, std::move(c));
                                      });

    if (config_.augmentOptions)
        config_.augmentOptions(options);

    return options;
}

// -----------------------------------------------------------------------------
// 原生函数处理(message 线程)
// -----------------------------------------------------------------------------
void WebViewHost::handleRequestInitialState(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    bridgeReady_ = true; // 前端确认就绪 -> 此后 timer 才允许 emit(机制 8)
    complete(buildSnapshot());
}

void WebViewHost::handleSetLang(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const juce::String code = args.size() > 0 ? args[0].toString() : juce::String("zh");
    lang_ = bridge::normalizeLang(code); // §1.30:仅 {zh,en,fr},未知回退 zh;实际值经 buildSnapshot 回推 scvb.state
    complete(bridge::okResponse());
}

void WebViewHost::handleSetUiScale(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    float scale = uiScale_;
    if (!bridge::parseUiScaleArg(args, config_.role, scale))
    {
        complete(bridge::badArgResponse()); // §1.28:非法参数(缺参/非数值/不在档位表)→ badArg
        return;
    }
    setUiScale(scale); // 只实时预览、不落盘(机制 9 前半)
    complete(bridge::okResponse()); // §1.28:{ok:true}
}

void WebViewHost::handleCommitUiScale(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    commitUiScale(); // 防呆确认「保持」后落盘(机制 9 后半)
    complete(bridge::okResponse()); // §1.29:{ok:true}
}

// -----------------------------------------------------------------------------
// 25Hz Timer(message 线程):看门狗 + 就绪门控 + diff-then-emit(机制 3/7/8)
// -----------------------------------------------------------------------------
void WebViewHost::timerCallback()
{
    // WebView2 看门狗:5s。后端选对后前端确实走 WebView2 加载,正常冷启动远快于此;
    // 超时判定加载失败切兜底(可重试/重开窗口),文案不误报「运行时缺失」。
    if (!bridgeReady_ && fallback_ == nullptr && juce::Time::getMillisecondCounter() - startMs_ > 5000)
    {
        showFallback(FallbackReason::LoadTimeout);
        return;
    }
    if (!bridgeReady_)
        return;

    emitTick();
}

void WebViewHost::emitTick()
{
    // 默认空实现;T29/T30 子类在此追加各事件类别的 diff-then-emit(机制 7)。
}

} // namespace scvb::webview
