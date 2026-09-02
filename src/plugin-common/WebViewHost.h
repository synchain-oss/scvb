// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <memory>

#include "FallbackPanel.h"
#include "PlatformWebView.h"
#include "ResourceProvider.h"

namespace scvb::webview
{
// WebViewHost —— 两插件共用的 WebView2 编辑器装配基类(01 §6.1 机制 3/5/6/7/8/9 的中枢)。
// T29/T30 的 InputEditor / OutputEditor 继承本类,经 Config::augmentOptions 追加各自的
// 原生函数 / 首帧 seed,并覆写 buildSnapshot() / emitTick() 补充 diff-then-emit。
class WebViewHost : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    struct Config
    {
        juce::String role; // Init::Role:"input" | "output" | "monitor"([J75])
        juce::String userDataFolderName; // WebView2 user-data 目录名(Input/Output 各一)
        juce::String version = "0.1.0"; // 首帧 version seed(插件侧传 JucePlugin_VersionString)
        juce::String lang = "zh";
        float uiScale = 1.0f;
        int channelLimit = 15; // [J59]
        ResourceProvider::Source resourceSource; // 插件自己的 BinaryData::*;暂无可嵌资源则传空 Source
        // T29/T30 在此追加插件专属的原生函数/首帧 seed(在通用装配之后调用)。空 = 只用通用装配。
        std::function<void(juce::WebBrowserComponent::Options&)> augmentOptions;
    };

    WebViewHost(juce::AudioProcessor& processor, Config config);
    ~WebViewHost() override;

    void resized() override;
    // 铺满 shellBackdrop()。[SL-271] 更正 SL-253 时的说法:本函数**挡不住**开窗白闪。
    // webView_ 可见时它铺满本组件且 setOpaque(true),JUCE 会把它的矩形从父组件的裁剪区里
    // 剔掉,本 paint 净效果为 0(真正管那一段的是 HostWebView::paint,见 .cpp)。
    // 它仍然有用,且只有一个用途:兜底面板路径 —— 那时 webView_ 被 setVisible(false),
    // 本组件自己露出来,这一层就是面板四周那块底。
    void paint(juce::Graphics& g) override;

    // 缩放(机制 9):uiScale 实时预览(不落盘);commitUiScale 防呆确认后落盘全局默认。
    float uiScale() const { return uiScale_; }
    const juce::String& lang() const { return lang_; } // T30:scvb.state ui.language / 落 state 用
    void setUiScale(float scale); // clamp + setSize(DESIGN×scale) 预览
    void commitUiScale(); // 落盘(子类覆写 persistUiScaleAsDefault)

    // 定义在 .cpp:HostWebView 在本头文件里只前置声明,派生→基类转换需要完整类型。
    juce::WebBrowserComponent& webView();

    // -------------------------------------------------------------------------
    // 看门狗预算(01 §6.1 机制 3)。原实现从 goToURL 那一刻起算固定 5s,把「WebView2 环境
    // 冷启动」和「页面加载」压在同一个预算里 —— 这正是冷启动被误判成加载失败的根源。
    // 现改为两段:导航事件到达前用冷/热启动预算,导航一开始就从那一刻重新起算(见
    // extendDeadlineAfterNav)。三个常量都对外可见,便于单测与文档引用。
    // -------------------------------------------------------------------------

    // 冷启动预算:进程内第一个 WebView2 实例要先拉起 msedgewebview2.exe 进程组(浏览器 +
    // 渲染 + GPU 三类进程)、建 user-data 目录、编译首屏 JS。机械盘 / 杀软实时扫描 / 首次
    // 建目录的测试机上这一段常态就超过 5s,原预算把「慢」判成「坏」。
    static constexpr int kColdLoadBudgetMs = 15000;
    // 复用预算:本进程已成功起过一次桥 ⇒ WebView2 浏览器进程常驻,再开编辑器只是新建一个
    // WebView,不再有进程组冷启动成本。继续用 15s 只会让真失败晚 10s 才给兜底面板。
    static constexpr int kWarmLoadBudgetMs = 5000;
    // 导航开始后的预算:首个导航事件到达即证明 WebView2 环境已就绪,剩下只是资源加载 +
    // 前端 boot。从该时刻重新起算,避免环境启动慢吃掉页面加载的额度。
    static constexpr int kAfterNavBudgetMs = 5000;

    // 前端 boot 失败上行的事件名(**诊断面,不属契约 §7 manifest**)。走 JUCE 内建的
    // window.__JUCE__.postMessage ←→ Options::withEventListener 通道,不经 bridge.js、
    // 不参与 check-bridge-parity;__scvb__ 前缀照 JUCE 自己的 __juce__ 惯例标明非契约面。
    // 真源在此,web 侧 index.html 的 boot 守卫逐字引用同一个名字。
    static constexpr const char* kBootErrorEventId = "__scvb__bootError";

protected:
    // 子类覆写以落盘全局默认(宿主侧持久化由插件 Processor 实现;默认空实现)。
    virtual void persistUiScaleAsDefault();

    // 子类覆写以提供首帧全量快照(requestInitialState 回执)。
    virtual juce::var buildSnapshot();

    // 25Hz diff-then-emit(机制 7):先跑看门狗/就绪门控,再调用子类的 emitTick()。
    void timerCallback() override;

    // 子类在 timer 里追加自己的 diff-then-emit(仅在 mBridgeReady 且非兜底时被调用)。
    virtual void emitTick();

    // 通用原生函数 handler(两插件共用;子类可复用/覆写)。
    void handleRequestInitialState(const juce::Array<juce::var>& args,
                                   juce::WebBrowserComponent::NativeFunctionCompletion complete);
    virtual void handleSetLang(const juce::Array<juce::var>& args,
                               juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleSetUiScale(const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion complete);
    void handleCommitUiScale(const juce::Array<juce::var>& args,
                             juce::WebBrowserComponent::NativeFunctionCompletion complete);

private:
    enum class FallbackReason
    {
        MissingRuntime,
        RuntimeTooOld,
        LoadTimeout,
        BootError
    };

    // 最后一次导航的状态(诊断口径:区分「WebView2 根本没动」和「导航起来了但页面/脚本没起来」)。
    enum class NavState
    {
        notStarted,
        started,
        finished,
        networkError
    };

    // 装 juce::WebBrowserComponent 的薄子类:只把三个页面回调转给宿主。看门狗要靠导航事件
    // 起算,而这三个回调是 JUCE 唯一暴露导航时序的地方,只能经继承拿到。
    class HostWebView;

    juce::WebBrowserComponent::Options makeOptions(); // 装配通用 Options + 子类 augmentOptions
    void showFallback(FallbackReason reason);
    void retryWebView();
    void resizeToDesignBox(float scale);

    // 加载时序(message 线程):由 HostWebView 的页面回调驱动。
    void onNavigationStarted(const juce::String& url);
    void onNavigationFinished(const juce::String& url);
    void onNavigationError(const juce::String& errorInfo);
    void handleBootError(const juce::var& payload); // 前端 boot 失败上报(非契约面,见 .cpp)

    // 首页 URL = <provider root>/<role>/index.html。让服务 URL 空间与 web/ 的磁盘布局
    // 逐段对齐,从而保证 ES module 身份唯一(同一文件不会被两个 URL 各实例化一份)。
    // 完整理由见 .cpp 实现处 —— 这条是 Tab1/Tab3 播放头状态分裂那个 bug 的根子。
    juce::String entryUrl() const;

    void beginLoadAttempt(); // 起算看门狗(构造 / retry 共用)
    void releaseReadyBridge(); // 把本实例从「活着的桥」计数里摘掉(retry / 析构)
    juce::String buildDiagnostics() const; // 兜底面板诊断行 + 日志行的同一份文本
    void logDiag(const juce::String& line) const; // 既有日志通道(juce::Logger)

    Config config_;
    ResourceProvider provider_;
    float uiScale_;
    juce::String lang_;

    std::atomic<bool> bridgeReady_{false};
    juce::uint32 startMs_ = 0;
    juce::uint32 deadlineMs_ = 0; // startMs_ + 冷/热预算;首个导航事件到达时按 kAfterNavBudgetMs 顺延
    bool navBudgetApplied_ = false; // 只顺延一次:页面若反复导航,不允许无限推迟看门狗
    NavState navState_ = NavState::notStarted;
    juce::String navDetail_; // 最后一次导航错误信息(networkError 时非空)
    juce::String bootError_; // 前端上报的 boot 失败摘要(BootError 分支用)
    PlatformWebView::RuntimeInfo runtime_; // 本次加载尝试开始时的运行时探测结果(诊断用)
    juce::File userDataFolder_; // 本插件的 WebView2 user-data 目录(per-plugin 固定,进程组据此复用)
    juce::String userDataFolderIssue_; // 构造期可写性探针结果;空 = 没问题
    bool countedAsReady_ = false; // 本实例是否已计入 readyBridgeCount(防重复加减)

    std::unique_ptr<HostWebView> webView_;
    std::unique_ptr<FallbackPanel> fallback_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebViewHost)
};

} // namespace scvb::webview
