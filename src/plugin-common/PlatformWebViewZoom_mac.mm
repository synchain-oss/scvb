// SPDX-License-Identifier: GPL-3.0-or-later
// [Z1] PlatformWebView::applyNativePageZoom 的 macOS 实现。为什么走 pageZoom 见 PlatformWebView.h。
//
// JUCE 不暴露 WKWebView。它的后端 WKWebViewImpl 是 NSViewComponent,挂在 WebBrowserComponent 底下
// (juce_WebBrowserComponent_mac.mm),所以在这棵子树里找:每个编辑器只碰自己的 WebView,不用遍历窗口。
// 用 isKindOfClass: 核一下,JUCE 还有一个老的 WebViewImpl(旧 WebView 类,同样是 NSViewComponent)。

#include "PlatformWebView.h"

#import <WebKit/WebKit.h>

namespace scvb::webview
{

namespace
{
WKWebView* findWkWebView(juce::Component& root)
{
    if (auto* embedded = dynamic_cast<juce::NSViewComponent*>(&root))
    {
        id view = (__bridge id) embedded->getView();
        if ([view isKindOfClass:[WKWebView class]])
            return (WKWebView*) view;
    }

    for (auto* child : root.getChildren())
        if (auto* found = findWkWebView(*child))
            return found;

    return nil;
}
} // namespace

bool PlatformWebView::applyNativePageZoom(juce::WebBrowserComponent& browser, float scale)
{
    WKWebView* view = findWkWebView(browser);
    if (view == nil)
        return false;
    return true;
}

} // namespace scvb::webview
