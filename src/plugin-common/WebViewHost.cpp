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

juce::String tooOldRuntimeMessage()
{
    return "The installed Microsoft Edge WebView2 Runtime is too old for this plugin.\n"
           "Update to the Evergreen runtime, then reopen this plugin window.";
}

// 超时兜底按「最后一次导航走到哪一步」分三态给文案。这是本面板最重要的信息:
// 同一句「加载太慢」在三种完全不同的故障上都会出现,而这三种的下一步动作互不相同。
//   notStarted —— 导航事件一次都没来 ⇒ WebView2 **环境/控制器根本没建起来**。
//                 JUCE 把环境创建与控制器创建的 HRESULT 都吞了(回调里 HRESULT 形参无名,
//                 juce_WebBrowserComponent_windows.cpp),所以「没有事件」是我们唯一能拿到的
//                 信号 —— 也正因如此它很可靠:环境起来了就必然有 NavigationStarting。
//                 常见成因:user-data 目录不可写 / 被别的进程占着(宿主的插件扫描 sandbox
//                 进程与音频进程同时活着)/ 宿主策略挡掉 msedgewebview2.exe 子进程。
//   started    —— 导航开始但没走完 ⇒ 资源供给或渲染进程卡住。
//   finished   —— 页面加载完了桥仍没起来 ⇒ 前端脚本没跑到 requestInitialState。
juce::String envNotStartedMessage()
{
    return "The WebView2 environment did not start (no navigation ever began).\n"
           "This is usually the user-data folder being unwritable or already in use by another\n"
           "process, or the host blocking the msedgewebview2.exe child process.";
}

juce::String navStalledMessage()
{
    return "The page started loading but never finished.\n"
           "Click Retry, or close and reopen this plugin window.";
}

juce::String bridgeStalledMessage()
{
    return "The page loaded, but the plugin UI never finished starting up.\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

juce::String bootErrorMessage()
{
    return "The plugin UI failed to start (front-end script error).\n"
           "Click Retry; if it keeps failing, report the diagnostics line below.";
}

// **当前**桥已就绪的编辑器数量(进程内)。热预算的判据只能是「此刻确有一个活着的桥」,
// 不能是「历史上曾经起来过」:
//   • WebView2 的浏览器进程组按 user-data 目录共享(见 PlatformWebView::makeUserDataFolder),
//     只要还有一个实例活着,进程组就在,新开的编辑器只是往里加一个 WebView,5s 绰绰有余;
//   • 可一旦所有编辑器都关掉,浏览器进程组会退出,再开就是**完整冷启动** —— 此时若因
//     「曾经起来过」而只给 5s,等于亲手造出「关窗再开必超时」这个新故障。
// 故用计数而非 bool:>0 才算热。
std::atomic<int>& readyBridgeCount()
{
    static std::atomic<int> count{0};
    return count;
}
} // namespace

// -----------------------------------------------------------------------------
// HostWebView —— 只为拿到 JUCE 的三个页面回调而存在的薄子类(01 §6.1 机制 3)。
// 看门狗要「收到导航事件才起算」,而 pageAboutToLoad / pageFinishedLoading /
// pageLoadHadNetworkError 是 JUCE 唯一暴露导航时序的接口,只能经继承取得。
// -----------------------------------------------------------------------------
class WebViewHost::HostWebView final : public juce::WebBrowserComponent
{
public:
    HostWebView(WebViewHost& owner, Options options) : juce::WebBrowserComponent(std::move(options)), owner_(owner) {}

    bool pageAboutToLoad(const juce::String& url) override
    {
        owner_.onNavigationStarted(url);
        return true; // 本插件只导航到 resource provider 根,不拦
    }

    void pageFinishedLoading(const juce::String& url) override { owner_.onNavigationFinished(url); }

    bool pageLoadHadNetworkError(const juce::String& errorInfo) override
    {
        owner_.onNavigationError(errorInfo);
        return false; // 不显示 WebView2 内建错误页 —— 兜底面板给的是可操作信息,内建页不是
    }

private:
    WebViewHost& owner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostWebView)
};

WebViewHost::WebViewHost(juce::AudioProcessor& processor, Config config)
    : juce::AudioProcessorEditor(&processor), config_(std::move(config)), provider_(config_.resourceSource),
      uiScale_(bridge::clampUiScale(config_.uiScale)), lang_(config_.lang)
{
    // UDF 必须在装 Options 之前定好,并当场探一次可写性:WebView2 自己碰这个目录时的失败被
    // JUCE 吞掉(环境创建回调的 HRESULT 形参无名),等到那时候就只剩「超时」两个字了。
    userDataFolder_ = PlatformWebView::makeUserDataFolder(config_.userDataFolderName);
    userDataFolderIssue_ = PlatformWebView::probeUserDataFolder(userDataFolder_);
    if (userDataFolderIssue_.isNotEmpty())
        logDiag("user-data folder problem: " + userDataFolderIssue_);

    webView_ = std::make_unique<HostWebView>(*this, makeOptions());
    addAndMakeVisible(*webView_);

    // 真 WebView2 实例化与 runtimeInfo 探测无法离线单测(需 WebView2 loader + 真机),
    // 属硬前置:T29/T30 接 InputEditor/OutputEditor 后经 gate 8 真机 GUI pluginval 验证。
    setResizable(false, false); // 仅经缩放档位下拉(setUiScale)编程改尺寸,不开自由拖角。
    resizeToDesignBox(uiScale_);

    beginLoadAttempt();
}

// 起算一次加载尝试(构造 / retry 共用):先探运行时三态,再决定加载还是直接兜底。
// 「没装」「太旧」两条都不进看门狗 —— 用户动作是装/升级 Runtime,干等一个预算窗口毫无意义。
void WebViewHost::beginLoadAttempt()
{
    releaseReadyBridge(); // 重试时本实例的旧桥不再算数(计数只统计活着的桥)
    bridgeReady_ = false;
    navState_ = NavState::notStarted;
    navDetail_ = {};
    bootError_ = {};
    navBudgetApplied_ = false;
    startMs_ = juce::Time::getMillisecondCounter(); // 先落起点:兜底面板的「已等待」也从这里算
    runtime_ = PlatformWebView::runtimeInfo();

    if (runtime_.status == PlatformWebView::RuntimeStatus::missing)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::MissingRuntime);
        return;
    }
    if (runtime_.status == PlatformWebView::RuntimeStatus::tooOld)
    {
        webView_->setVisible(false);
        showFallback(FallbackReason::RuntimeTooOld);
        return;
    }

    // 冷/热预算见 WebViewHost.h 的三个常量注释;导航事件到达后再按 kAfterNavBudgetMs 顺延。
    // 热的判据 = 此刻另有一个桥活着(⇒ 共享的浏览器进程组必然在跑),不是「曾经起来过」。
    const bool warm = readyBridgeCount().load() > 0;
    deadlineMs_ = startMs_ + static_cast<juce::uint32>(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs);
    logDiag(juce::String(warm ? "warm" : "cold") + " start, budget " +
            juce::String(warm ? kWarmLoadBudgetMs : kColdLoadBudgetMs) + " ms, udf " +
            userDataFolder_.getFullPathName());

    // 必须在任何 emit 之前完成首个 goToURL(前端脚本随后加载并注册监听)。
    webView_->setVisible(true);
    webView_->goToURL(WBC::getResourceProviderRoot());
    if (!isTimerRunning())
        startTimerHz(25);
}

WebViewHost::~WebViewHost()
{
    stopTimer();
    releaseReadyBridge(); // 编辑器关闭 -> 本实例不再为「热启动」判定背书
}

// 计数只统计**活着**的桥:重试与析构都要还回去,否则关掉全部编辑器后计数仍 >0,
// 下一次开窗会按热预算跑一次真冷启动 —— 那正是 bot 指出的「关窗再开必超时」。
void WebViewHost::releaseReadyBridge()
{
    if (countedAsReady_)
    {
        countedAsReady_ = false;
        readyBridgeCount().fetch_sub(1);
    }
}

juce::WebBrowserComponent& WebViewHost::webView()
{
    return *webView_;
}

void WebViewHost::resized()
{
    if (webView_ != nullptr)
        webView_->setBounds(getLocalBounds());
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
juce::String WebViewHost::buildDiagnostics() const
{
    const char* nav = "notStarted";
    switch (navState_)
    {
    case NavState::started:
        nav = "started";
        break;
    case NavState::finished:
        nav = "finished";
        break;
    case NavState::networkError:
        nav = "networkError";
        break;
    case NavState::notStarted:
    default:
        break;
    }

    juce::String d;
    d << "waited " << juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) << " ms"
      << "  |  nav " << nav;
    if (navDetail_.isNotEmpty())
        d << " (" << navDetail_ << ")";
    d << "  |  WebView2 " << (runtime_.version.isNotEmpty() ? runtime_.version : juce::String("not found"));
    d << "  |  host " << juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileName() << " pid "
      << juce::String(PlatformWebView::processId());
    // UDF 是「环境没起来」这一路的头号嫌疑,必须原样显示:用户把这一行发回来,就能直接看出
    // 目录在哪、写不写得进、以及(名字里的 PID)是不是被另一个宿主进程占着。
    d << "\n" << "udf " << userDataFolder_.getFullPathName();
    if (userDataFolderIssue_.isNotEmpty())
        d << "  [" << userDataFolderIssue_ << "]";
    if (bootError_.isNotEmpty())
        d << "\n" << bootError_;
    return d;
}

void WebViewHost::logDiag(const juce::String& line) const
{
    // 既有日志通道:juce::Logger。DBG(= outputDebugString)在 Release 里被编掉,而兜底面板
    // 恰恰只在用户的 Release 包上出现 —— writeToLog 在无 logger 时也会落 outputDebugString
    // (DebugView 可见),宿主设了 logger 则进宿主日志。诊断不能只活在 Debug 构建里。
    juce::Logger::writeToLog("SCVB " + config_.role + ": " + line);
}

void WebViewHost::showFallback(FallbackReason reason)
{
    if (fallback_ != nullptr)
        return;
    webView_->setVisible(false);

    const bool missing = (reason == FallbackReason::MissingRuntime);
    const bool tooOld = (reason == FallbackReason::RuntimeTooOld);

    juce::String message;
    const char* tag = "loadTimeout";
    if (missing)
    {
        message = missingRuntimeMessage();
        tag = "missingRuntime";
    }
    else if (tooOld)
    {
        message = tooOldRuntimeMessage();
        tag = "runtimeTooOld";
    }
    else if (reason == FallbackReason::BootError)
    {
        message = bootErrorMessage();
        tag = "bootError";
    }
    else if (navState_ == NavState::notStarted)
    {
        // 导航一次都没开始 = WebView2 环境/控制器没建起来(见文件头三态注释)。
        message = envNotStartedMessage();
        tag = "envNotStarted";
    }
    else if (navState_ == NavState::finished)
    {
        message = bridgeStalledMessage();
        tag = "bridgeStalled";
    }
    else
    {
        message = navStalledMessage();
        tag = "navStalled";
    }

    const auto details = buildDiagnostics();
    logDiag(juce::String("fallback ") + tag + " — " + details.replaceCharacter('\n', ' '));

    FallbackPanel::Options options;
    options.title = fallbackTitle(config_.role);
    options.message = message;
    options.details = details;
    options.showInstall = missing || tooOld; // 两条的用户动作都是装/升级 Evergreen Runtime
    options.onInstall = [] { juce::URL(PlatformWebView::runtimeDownloadUrl()).launchInDefaultBrowser(); };
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
    resizeToDesignBox(uiScale_);
    beginLoadAttempt(); // 重探运行时三态 + 重置看门狗;若又是 missing/tooOld 会就地再切兜底
    resized();
}

// -----------------------------------------------------------------------------
// 加载时序回调(message 线程,来自 HostWebView)
// -----------------------------------------------------------------------------
void WebViewHost::onNavigationStarted(const juce::String& url)
{
    navState_ = NavState::started;
    navDetail_ = {};

    // 「收到首个导航事件才重新起算」:导航开始证明 WebView2 环境已就绪,页面加载该有自己的
    // 完整额度。只顺延一次(navBudgetApplied_),且只许延后不许提前 —— 否则冷启动刚用掉的
    // 时间会被这里抹掉,或者一个反复导航的页面能把看门狗无限推后。
    if (!navBudgetApplied_)
    {
        navBudgetApplied_ = true;
        const auto extended = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(kAfterNavBudgetMs);
        if (static_cast<juce::int32>(extended - deadlineMs_) > 0) // 回绕安全的「更晚吗」
            deadlineMs_ = extended;
    }

    logDiag("navigation started: " + url);
}

void WebViewHost::onNavigationFinished(const juce::String& url)
{
    // 页面已下载完 ≠ 桥已就绪:前端还要跑模块图并调 requestInitialState。看门狗继续跑,
    // 但状态记下来 —— 「finished 却超时」精确指向前端 boot 失败,与「压根没导航」判然不同。
    navState_ = NavState::finished;
    logDiag("navigation finished: " + url);
}

void WebViewHost::onNavigationError(const juce::String& errorInfo)
{
    navState_ = NavState::networkError;
    navDetail_ = errorInfo.substring(0, 200);
    logDiag("navigation error: " + navDetail_);

    // **这里刻意不切兜底面板**,由看门狗统一裁决。
    //
    // 原因是「重试自锁」:点 Retry -> retryWebView 先 reset 掉面板、再 goToURL,而这个
    // goToURL 会**中止**上一次仍在飞的导航;被中止的那次随后异步回调过来报错,此时
    // fallback_ 恰好是 nullptr、bridgeReady_ 也还是 false —— 于是上一次的错误把刚开始的
    // 这一次就地判死,面板瞬间又贴回来。用户看到的就是「点重试没反应」。
    //
    // 不用「按错误码放行」来修:JUCE 只帮忙过滤了 OPERATION_CANCELED
    // (juce_WebBrowserComponent_windows.cpp 里把它当成功、走 pageFinishedLoading),
    // CONNECTION_ABORTED(错误码 9)照样漏下来 —— JUCE 自己的注释都写着「code 9 往往可以
    // 安全忽略」。与其维护一张「哪些码不算数」的名单,不如根本不让任何单次导航错误拥有
    // 直接判死的权力:真失败会在预算耗尽时照常进兜底面板,诊断行里带着这里记下的错误码;
    // 而中止、瞬时错误则被后续的 navigationStarted / finished 自然覆盖掉。
    // 代价只是「必然失败的情形要多等一个预算窗口」,换来的是重试真的能用。
}

// 前端 boot 失败上报(机制 3 补强)。
//
// 【为什么不是契约桥函数】契约 §7 manifest 已冻结(Output 34 函数 / 9 事件,Input 7 / 5),
// 且那些名字全部要求桥已就绪;而这里要报的恰恰是「桥还没起来」。故走 JUCE **内建**的上行
// 通道:window.__JUCE__.postMessage({eventId, payload}) ←→ C++ Options::withEventListener。
// 该通道由 JUCE 在文档创建前注入,不经 bridge.js、不占用 §7 名表、不参与 check-bridge-parity,
// 名字加 __scvb__ 前缀(照 JUCE 自己的 __juce__ 惯例)标明它是诊断面而非契约面。
void WebViewHost::handleBootError(const juce::var& payload)
{
    const auto stage = payload.getProperty("stage", juce::var("unknown")).toString();
    const auto detail = payload.getProperty("detail", juce::var("")).toString();

    bootError_ = "boot " + stage + ": " + detail.substring(0, 300);
    logDiag("front-end " + bootError_);

    if (bridgeReady_ || fallback_ != nullptr)
        return; // 桥已起来后的运行期错误归 UI 自己处理,不砸掉一个能用的界面
    showFallback(FallbackReason::BootError);
}

// -----------------------------------------------------------------------------
// WebView 装配(机制 1/2/4/5/6)
// -----------------------------------------------------------------------------
juce::WebBrowserComponent::Options WebViewHost::makeOptions()
{
    auto options = PlatformWebView::makeWebViewOptions(WBC::Options{}, userDataFolder_);

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

    // 诊断面(非契约):前端 boot 失败上行。理由与通道选择见 handleBootError 的注释。
    options = options.withEventListener(juce::Identifier(kBootErrorEventId),
                                        [this](const juce::var& payload) { handleBootError(payload); });

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
    // 本实例的桥活着 -> 共享的 WebView2 进程组必在跑 -> 其它编辑器可按热预算起看门狗。
    // requestInitialState 可能被同一页面多次调用(页面重载后要重来一次),故用标志防重复计数。
    if (!countedAsReady_)
    {
        countedAsReady_ = true;
        readyBridgeCount().fetch_add(1);
    }
    logDiag("bridge ready after " + juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startMs_)) +
            " ms");
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
    // WebView2 看门狗:预算由 beginLoadAttempt 定(冷 15s / 热 5s),首个导航事件到达后按
    // kAfterNavBudgetMs 顺延。超时判定加载失败切兜底(可重试/重开窗口),文案不误报「运行时缺失」。
    // 比较走 uint32 差值再转 int32:getMillisecondCounter 每 ~49 天回绕一次,直接比大小会在
    // 回绕点把「还没到点」算成「早就超时」,把好好的窗口砸成兜底面板。
    if (!bridgeReady_ && fallback_ == nullptr &&
        static_cast<juce::int32>(juce::Time::getMillisecondCounter() - deadlineMs_) > 0)
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
