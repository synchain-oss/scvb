// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// scvb_pagezoom_probe —— 真 WebKit 上量 WKWebView.pageZoom 的坐标系(说明见同目录 README.md)。
//
// 为什么非要真 WebKit:web-preview 的冒烟跑在 Chromium 上,没有 pageZoom 这个东西,旧 WebKit 的
// rect 语义也只能模拟。mac 上缩放改走 pageZoom 的全部理由是「rect 和事件坐标落在同一个坐标系」,
// 这一条只有系统 WKWebView 自己答得了。
//
// 页面里跑的是**产品代码本身**(web/shared/hit.js、shell-fit.js、design-box.js):经一个自定义
// scheme 从仓库的 web/ 目录直接取,seed 的形状也与 JUCE 注入的一样(`nativeZoom: [true]`)。
//
// 退出码:0 = 全部断言成立;1 = 有断言不成立;2 = 环境问题(页面没起来),这**不是**通过。
// 真点击发出去却一个事件都没进页面,只记录、不判红(发不出来不等于坐标不对);
// 事件进了页面但落点不对,判红。
// =============================================================================

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

// 与 web/shared/design-box.js 的 Output 同值。页面里的 shell-fit 读的是真源;这里只用来定窗口尺寸,
// 真源改了,下面 shell 倍率那条断言会红。
constexpr double kDesignW = 1180.0;
constexpr double kDesignH = 780.0;
// Output 的档位表。0.8 排第一:它是 mac 上 Output 的出厂档,也是「开窗就不是 1 档」那条路。
const std::vector<double> kPresets = {0.8, 0.5, 0.65, 1.0, 1.25, 1.5, 2.0};

constexpr double kViewportTolerance = 1.5; // clientWidth 是整数,窗口尺寸也取过整
constexpr double kRectTolerance = 0.51; // 亚像素舍入
// 真点击落点的容差随档位走:WebKit 把事件位置取整到 view point,一个 view point = 1/pageZoom 个 CSS px,
// clientX/Y 自己再取整一次。所以合法偏差 < 1/zoom + 1(0.65 档实测 (1050, 695) → (1049, 693),正是
// 682.5 / 451.75 两个 view point 先取整再除 0.65)。比例算错的偏差随坐标增大,右下靶子会差几十 px。
double clickTolerance(double zoom)
{
    return 1.0 + 1.0 / zoom;
}

struct Target
{
    const char* id;
    double left, top, width, height;
    double cx() const { return left + width / 2.0; }
    double cy() const { return top + height / 2.0; }
};

// 三个靶子分散在设计盒的左上、中间、右下:缩放比例算错时偏差随坐标增大,右下那个一定露馅。
const std::vector<Target> kTargets = {
    {"a", 100, 60, 200, 120},
    {"b", 520, 340, 240, 160},
    {"c", 960, 640, 180, 110},
};

NSString* probePageHtml()
{
    return @"<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:#191820}"
            "#card{position:absolute;left:0;top:0;width:1180px;height:780px;background:#ddd}"
            ".t{position:absolute}"
            "#a{left:100px;top:60px;width:200px;height:120px;background:#c33}"
            "#b{left:520px;top:340px;width:240px;height:160px;background:#3c3}"
            "#c{left:960px;top:640px;width:180px;height:110px;background:#33c}"
            "</style></head><body><div id=\"card\"><div id=\"a\" class=\"t\"></div>"
            "<div id=\"b\" class=\"t\"></div><div id=\"c\" class=\"t\"></div></div>"
            "<script>window.__hits=[];"
            "['pointerdown','mousedown','mouseup','click'].forEach(function(t){"
            "document.addEventListener(t,function(e){window.__hits.push("
            "{type:t,x:e.clientX,y:e.clientY,id:(e.target&&e.target.id)||''});},true);});"
            "</script>"
            "<script type=\"module\">"
            "import * as hit from '/web/shared/hit.js';"
            "import * as sf from '/web/shared/shell-fit.js';"
            "import { DESIGN } from '/web/shared/design-box.js';"
            "const seed=Number(new URLSearchParams(location.search).get('seed'))||1;"
            "window.__JUCE__={initialisationData:{nativeZoom:[true],uiScale:[seed]}};"
            "const card=document.getElementById('card');"
            "const fit=sf.installShellFit({el:card,box:DESIGN.output});"
            "window.__m={hit,sf,DESIGN,fit,card};"
            "window.__ready=true;"
            "</script></body></html>";
}

} // namespace

@interface ProbeWindow : NSWindow
@end

@implementation ProbeWindow
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
- (BOOL)canBecomeMainWindow
{
    return YES;
}
// 2 档的窗口(2360x1560)比 runner 的屏幕大:不许系统把窗口压回屏幕内,否则窗口与档位对不上。
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen
{
    (void) screen;
    return frameRect;
}
@end

@interface ProbeSchemeHandler : NSObject <WKURLSchemeHandler>
@property(nonatomic, copy) NSString* webRoot;
@end

@implementation ProbeSchemeHandler
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void) webView;
    NSString* path = task.request.URL.path;
    NSData* body = nil;
    NSString* mime = @"text/plain";
    if ([path isEqualToString:@"/probe.html"])
    {
        body = [probePageHtml() dataUsingEncoding:NSUTF8StringEncoding];
        mime = @"text/html; charset=utf-8";
    }
    else if ([path hasPrefix:@"/web/"])
    {
        body = [NSData dataWithContentsOfFile:[self.webRoot stringByAppendingPathComponent:[path substringFromIndex:5]]];
        mime = @"text/javascript; charset=utf-8";
    }
    const NSInteger status = body != nil ? 200 : 404;
    if (body == nil)
        body = [NSData data];
    NSDictionary* headers = @{
        @"Content-Type" : mime,
        @"Content-Length" : [NSString stringWithFormat:@"%lu", static_cast<unsigned long>(body.length)]
    };
    NSHTTPURLResponse* response = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                                              statusCode:status
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:headers];
    [task didReceiveResponse:response];
    [task didReceiveData:body];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void) webView;
    (void) task;
}
@end

namespace
{

bool spinUntil(BOOL (^condition)(void), NSTimeInterval timeoutSeconds)
{
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
    while (!condition())
    {
        if ([deadline timeIntervalSinceNow] <= 0)
            return false;
        @autoreleasepool
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event != nil)
                [NSApp sendEvent:event];
        }
    }
    return true;
}

void spinFor(NSTimeInterval seconds)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    spinUntil(^BOOL {
      return [until timeIntervalSinceNow] <= 0;
    }, seconds + 1.0);
}

// 跑一段函数体(可以 await),结果是 Foundation 对象。失败时 *error 非空,返回 nil。
id runJs(WKWebView* view, NSString* body, std::string* error)
{
    __block id result = nil;
    __block NSError* failure = nil;
    __block BOOL done = NO;
    [view callAsyncJavaScript:body
                    arguments:@{}
                      inFrame:nil
               inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id value, NSError* err) {
              result = value;
              failure = err;
              done = YES;
            }];
    const bool finished = spinUntil(^BOOL {
      return done;
    }, 15.0);
    if (!finished)
    {
        if (error != nullptr)
            *error = "JS evaluation timed out";
        return nil;
    }
    if (failure != nil)
    {
        if (error != nullptr)
            *error = std::string("JS error: ") + failure.localizedDescription.UTF8String;
        return nil;
    }
    return result;
}

double numberAt(NSDictionary* dict, NSString* key)
{
    id v = dict[key];
    return [v isKindOfClass:[NSNumber class]] ? [v doubleValue] : NAN;
}

std::string stringAt(NSDictionary* dict, NSString* key)
{
    id v = dict[key];
    return [v isKindOfClass:[NSString class]] ? std::string([v UTF8String]) : std::string("(none)");
}

std::string bundleVersion(NSBundle* bundle, NSString* key)
{
    id v = [bundle objectForInfoDictionaryKey:key];
    return [v isKindOfClass:[NSString class]] ? std::string([v UTF8String]) : std::string("(unknown)");
}

std::string fmt(const char* format, double a)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, a);
    return buf;
}

struct Failures
{
    std::vector<std::string> lines;
    void add(const std::string& line)
    {
        lines.push_back(line);
        std::printf("  [FAIL] %s\n", line.c_str());
    }
};

// 真点击的结果:delivered = 事件进了页面;落点对不对单独算。
struct ClickResult
{
    bool delivered = false;
    std::string targetId;
    double clientX = NAN;
    double clientY = NAN;
    const char* path = "";
};

void sendMouse(NSWindow* window, NSEventType type, NSPoint locationInWindow, NSInteger number)
{
    NSEvent* event = [NSEvent mouseEventWithType:type
                                        location:locationInWindow
                                   modifierFlags:0
                                       timestamp:[NSProcessInfo processInfo].systemUptime
                                    windowNumber:window.windowNumber
                                         context:nil
                                     eventNumber:number
                                      clickCount:type == NSEventTypeMouseMoved ? 0 : 1
                                        pressure:type == NSEventTypeLeftMouseDown ? 1.0f : 0.0f];
    [window sendEvent:event];
}

ClickResult clickAt(WKWebView* view, NSWindow* window, double viewX, double viewY)
{
    ClickResult out;
    const NSPoint inView = NSMakePoint(viewX, view.isFlipped ? viewY : view.bounds.size.height - viewY);
    const NSPoint inWindow = [view convertPoint:inView toView:nil];
    static NSInteger eventNumber = 0;

    // 先走真实入口 NSWindow sendEvent:;窗口没拿到 key 时第一下可能被窗口吃掉,再走一次,
    // 还不行就直接交给 WKWebView 的 mouseDown:(同一条 WebKit 输入路径,只是跳过窗口分发)。
    for (int attempt = 0; attempt < 3 && !out.delivered; ++attempt)
    {
        std::string err;
        runJs(view, @"window.__hits=[];return 1;", &err);
        if (attempt < 2)
        {
            sendMouse(window, NSEventTypeMouseMoved, inWindow, ++eventNumber);
            spinFor(0.03);
            sendMouse(window, NSEventTypeLeftMouseDown, inWindow, ++eventNumber);
            spinFor(0.03);
            sendMouse(window, NSEventTypeLeftMouseUp, inWindow, ++eventNumber);
            out.path = "NSWindow sendEvent:";
        }
        else
        {
            NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                               location:inWindow
                                          modifierFlags:0
                                              timestamp:[NSProcessInfo processInfo].systemUptime
                                           windowNumber:window.windowNumber
                                                context:nil
                                            eventNumber:++eventNumber
                                             clickCount:1
                                               pressure:1.0f];
            NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                             location:inWindow
                                        modifierFlags:0
                                            timestamp:[NSProcessInfo processInfo].systemUptime
                                         windowNumber:window.windowNumber
                                              context:nil
                                          eventNumber:++eventNumber
                                           clickCount:1
                                             pressure:0.0f];
            [view mouseDown:down];
            spinFor(0.03);
            [view mouseUp:up];
            out.path = "WKWebView mouseDown:";
        }

        __block NSDictionary* hit = nil;
        spinUntil(^BOOL {
          std::string e;
          id r = runJs(view, @"return (window.__hits.filter(h=>h.type==='mousedown')[0])||null;", &e);
          if ([r isKindOfClass:[NSDictionary class]])
          {
              hit = r;
              return YES;
          }
          return NO;
        }, 1.2);
        if (hit != nil)
        {
            out.delivered = true;
            out.targetId = stringAt(hit, @"id");
            out.clientX = numberAt(hit, @"x");
            out.clientY = numberAt(hit, @"y");
        }
    }
    return out;
}

WKWebView* makeWebView(ProbeSchemeHandler* handler, NSRect frame)
{
    WKWebViewConfiguration* config = [WKWebViewConfiguration new];
    [config setURLSchemeHandler:handler forURLScheme:@"scvb-probe"];
    WKWebView* view = [[WKWebView alloc] initWithFrame:frame configuration:config];
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    return view;
}

NSURLRequest* pageRequest(double seed)
{
    NSString* url = [NSString stringWithFormat:@"scvb-probe://app/probe.html?seed=%g", seed];
    return [NSURLRequest requestWithURL:[NSURL URLWithString:url]];
}

bool waitPageReady(WKWebView* view, std::string* error)
{
    const bool ok = spinUntil(^BOOL {
      std::string e;
      id r = runJs(view, @"return window.__ready===true;", &e);
      return [r isKindOfClass:[NSNumber class]] && [r boolValue];
    }, 30.0);
    if (!ok && error != nullptr)
        *error = "the probe page did not become ready within 30 s (module import failed or WebKit never started)";
    return ok;
}

NSString* const kSnapshotJs =
    @"const de=document.documentElement;const {sf,fit,card,DESIGN,hit}=window.__m;fit.refresh();"
     "const rect=(id)=>{const r=document.getElementById(id).getBoundingClientRect();"
     "return {l:r.left,t:r.top,w:r.width,h:r.height};};"
     "const efp=(x,y)=>{const e=document.elementFromPoint(x,y);return e?e.id:'';};"
     "return {cw:de.clientWidth,ch:de.clientHeight,iw:window.innerWidth,ih:window.innerHeight,"
     "dpr:window.devicePixelRatio,availW:screen.availWidth,availH:screen.availHeight,"
     "vvw:window.visualViewport?window.visualViewport.width:-1,"
     "vvs:window.visualViewport?window.visualViewport.scale:-1,"
     "shell:sf.shellFitFactor(),cssZoom:String(card.style.zoom),locked:sf.zoomLocked(),"
     "presets:sf.scalePresets(DESIGN.output.presets).length,backing:sf.backingFitFactor(),"
     "mode:hit.zoomRectMode(),ua:navigator.userAgent,"
     "ra:rect('a'),rb:rect('b'),rc:rect('c'),"
     "ea:efp(200,120),eb:efp(640,420),ec:efp(1050,695)};";

struct PresetRow
{
    double zoom = 1.0;
    double viewW = 0.0;
    double viewH = 0.0;
    double clientW = NAN;
    double clientH = NAN;
    double dpr = NAN;
    double shell = NAN;
    std::string cssZoom;
    double backing = NAN;
    double availW = NAN;
    int rectFails = 0;
    int efpFails = 0;
    int clicksDelivered = 0;
    int clicksCorrect = 0;
    const char* clickPath = "";
};

void applyZoomAndSize(ProbeWindow* window, WKWebView* view, double zoom, bool applyPageZoom)
{
    const double w = std::round(kDesignW * zoom);
    const double h = std::round(kDesignH * zoom);
    [window setFrame:NSMakeRect(20, 20, w, h) display:YES];
    view.frame = window.contentView.bounds;
    if (applyPageZoom)
        view.pageZoom = zoom;
}

} // namespace

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        NSString* webRoot = nil;
        NSString* summaryPath = nil;
        bool negativeControl = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--web-root" && i + 1 < argc)
                webRoot = [NSString stringWithUTF8String:argv[++i]];
            else if (arg == "--summary" && i + 1 < argc)
                summaryPath = [NSString stringWithUTF8String:argv[++i]];
            else if (arg == "--negative-control")
                negativeControl = true;
            else
            {
                std::fprintf(stderr, "usage: scvb_pagezoom_probe --web-root <repo>/web [--summary <file>] "
                                     "[--negative-control]\n");
                return 2;
            }
        }
        if (webRoot == nil || ![[NSFileManager defaultManager] fileExistsAtPath:[webRoot stringByAppendingPathComponent:@"shared/shell-fit.js"]])
        {
            std::fprintf(stderr, "--web-root must point at the repository's web/ directory\n");
            return 2;
        }
        webRoot = [[NSURL fileURLWithPath:webRoot] URLByStandardizingPath].path;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];

        const double firstZoom = kPresets.front();
        const NSRect firstFrame = NSMakeRect(0, 0, std::round(kDesignW * firstZoom), std::round(kDesignH * firstZoom));

        ProbeSchemeHandler* handler = [ProbeSchemeHandler new];
        handler.webRoot = webRoot;
        WKWebView* view = makeWebView(handler, firstFrame);

        // 与插件同序:WebView 建好就设 pageZoom、发导航,之后才进窗口(编辑器构造期还没有 peer)。
        if (!negativeControl)
            view.pageZoom = firstZoom;
        [view loadRequest:pageRequest(firstZoom)];

        ProbeWindow* window = [[ProbeWindow alloc] initWithContentRect:NSMakeRect(20, 20, firstFrame.size.width, firstFrame.size.height)
                                                             styleMask:NSWindowStyleMaskBorderless
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO];
        window.releasedWhenClosed = NO;
        [window.contentView addSubview:view];
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];

        std::string error;
        if (!waitPageReady(view, &error))
        {
            std::fprintf(stderr, "ENV: %s\n", error.c_str());
            return 2;
        }

        // ---- 环境留痕
        NSBundle* webKit = [NSBundle bundleForClass:[WKWebView class]];
        NSBundle* safari = [NSBundle bundleWithPath:@"/Applications/Safari.app"];
        const std::string macos = [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String;
        const std::string safariVersion = safari != nil ? bundleVersion(safari, @"CFBundleShortVersionString") : "(no Safari.app)";
        const std::string webKitVersion = bundleVersion(webKit, @"CFBundleVersion");
        NSScreen* screen = window.screen != nil ? window.screen : NSScreen.mainScreen;
        const double screenScale = screen != nil ? screen.backingScaleFactor : NAN;
        const NSSize screenSize = screen != nil ? screen.visibleFrame.size : NSZeroSize;

        Failures failures;
        std::string zoomRectMode = "(unknown)";
        std::string userAgent = "(unknown)";
        std::vector<PresetRow> rows;
        int totalClicks = 0;
        int totalDelivered = 0;
        int totalCorrect = 0;

        for (const double zoom : kPresets)
        {
            std::printf("=== pageZoom %.2f ===\n", zoom);
            applyZoomAndSize(window, view, zoom, !negativeControl);
            const double viewW = view.bounds.size.width;
            const double viewH = view.bounds.size.height;
            const double wantW = viewW / zoom;
            const double wantH = viewH / zoom;

            NSString* settle = [NSString stringWithFormat:@"const de=document.documentElement;"
                                                           "return Math.abs(de.clientWidth-%f)<=%f&&Math.abs(de.clientHeight-%f)<=%f;",
                                                          wantW, kViewportTolerance, wantH, kViewportTolerance];
            spinUntil(^BOOL {
              std::string e;
              id r = runJs(view, settle, &e);
              return [r isKindOfClass:[NSNumber class]] && [r boolValue];
            }, 3.0);
            spinFor(0.1);

            // 档位经 state 回推到页面的那一步(真页面在 renderScale / applyScale 里调)。
            std::string err;
            runJs(view, [NSString stringWithFormat:@"window.__m.sf.noteNativeScale(%g);return 1;", zoom], &err);

            NSDictionary* snap = runJs(view, kSnapshotJs, &err);
            if (![snap isKindOfClass:[NSDictionary class]])
            {
                failures.add("zoom " + fmt("%.2f", zoom) + ": snapshot failed: " + err);
                continue;
            }

            PresetRow row;
            row.zoom = zoom;
            row.viewW = viewW;
            row.viewH = viewH;
            row.clientW = numberAt(snap, @"cw");
            row.clientH = numberAt(snap, @"ch");
            row.dpr = numberAt(snap, @"dpr");
            row.shell = numberAt(snap, @"shell");
            row.cssZoom = stringAt(snap, @"cssZoom");
            row.backing = numberAt(snap, @"backing");
            row.availW = numberAt(snap, @"availW");
            zoomRectMode = stringAt(snap, @"mode");
            userAgent = stringAt(snap, @"ua");

            const std::string z = "zoom " + fmt("%.2f", zoom) + ": ";
            if (!(std::abs(row.clientW - wantW) <= kViewportTolerance))
                failures.add(z + "documentElement.clientWidth " + fmt("%.1f", row.clientW) + ", want " + fmt("%.1f", wantW) + " (window / pageZoom)");
            if (!(std::abs(row.clientH - wantH) <= kViewportTolerance))
                failures.add(z + "documentElement.clientHeight " + fmt("%.1f", row.clientH) + ", want " + fmt("%.1f", wantH) + " (window / pageZoom)");

            // 产品的 shell-fit:档位都开着,CSS zoom 恒 1。
            if (row.shell != 1.0 || row.cssZoom != "1")
                failures.add(z + "shell-fit factor " + fmt("%.4f", row.shell) + " / style.zoom '" + row.cssZoom + "', want 1 / '1'");
            if ([snap[@"locked"] boolValue])
                failures.add(z + "zoomLocked() is true under nativeZoom");
            if (numberAt(snap, @"presets") != 7.0)
                failures.add(z + "scalePresets() lost presets under nativeZoom");

            // rect 与设计坐标一致;elementFromPoint 与 rect 一致。
            NSDictionary* rects[] = {snap[@"ra"], snap[@"rb"], snap[@"rc"]};
            NSString* efpKeys[] = {@"ea", @"eb", @"ec"};
            for (size_t i = 0; i < kTargets.size(); ++i)
            {
                const Target& t = kTargets[i];
                const double got[4] = {numberAt(rects[i], @"l"), numberAt(rects[i], @"t"), numberAt(rects[i], @"w"), numberAt(rects[i], @"h")};
                const double want[4] = {t.left, t.top, t.width, t.height};
                for (int k = 0; k < 4; ++k)
                {
                    if (!(std::abs(got[k] - want[k]) <= kRectTolerance))
                    {
                        ++row.rectFails;
                        failures.add(z + "getBoundingClientRect #" + t.id + " field " + std::to_string(k) + " = " + fmt("%.2f", got[k]) + ", want " + fmt("%.2f", want[k]));
                    }
                }
                const std::string efp = stringAt(snap, efpKeys[i]);
                if (efp != t.id)
                {
                    ++row.efpFails;
                    failures.add(z + "elementFromPoint at the centre of #" + t.id + " returned '" + efp + "'");
                }
            }

            // 真点击:窗口坐标按「CSS 坐标 x pageZoom」算,事件的 clientX/Y 和落点元素都要回到设计坐标。
            for (const Target& t : kTargets)
            {
                ++totalClicks;
                const ClickResult click = clickAt(view, window, t.cx() * zoom, t.cy() * zoom);
                if (!click.delivered)
                    continue;
                ++totalDelivered;
                ++row.clicksDelivered;
                row.clickPath = click.path;
                const bool onTarget = click.targetId == t.id;
                const bool closeX = std::abs(click.clientX - t.cx()) < clickTolerance(zoom);
                const bool closeY = std::abs(click.clientY - t.cy()) < clickTolerance(zoom);
                if (onTarget && closeX && closeY)
                {
                    ++totalCorrect;
                    ++row.clicksCorrect;
                }
                else
                {
                    failures.add(z + "real click at #" + t.id + " landed on '" + click.targetId + "' at client (" + fmt("%.1f", click.clientX) + ", " + fmt("%.1f", click.clientY) + "), want (" + fmt("%.1f", t.cx()) + ", " + fmt("%.1f", t.cy()) + ")");
                }
            }

            std::printf("  view %.0fx%.0f client %.0fx%.0f (window/zoom %.1fx%.1f) dpr %.2f shell %.4f style.zoom %s "
                        "backing %.2f screen.availWidth %.0f clicks %d/%d correct %d\n",
                        viewW, viewH, row.clientW, row.clientH, wantW, wantH, row.dpr, row.shell, row.cssZoom.c_str(),
                        row.backing, row.availW, row.clicksDelivered, static_cast<int>(kTargets.size()), row.clicksCorrect);
            rows.push_back(row);
        }

        // pageZoom 活过一次重新导航(retryWebView 走 goToURL 那条路):这里不再设 pageZoom。
        std::string reloadNote = "not run";
        if (!negativeControl)
        {
            const double zoom = kPresets.front();
            applyZoomAndSize(window, view, zoom, true);
            spinFor(0.2);
            [view loadRequest:pageRequest(zoom)];
            spinFor(0.2);
            std::string err;
            if (!waitPageReady(view, &err))
            {
                failures.add("reload: " + err);
            }
            else
            {
                NSDictionary* snap = runJs(view, kSnapshotJs, &err);
                const double cw = [snap isKindOfClass:[NSDictionary class]] ? numberAt(snap, @"cw") : NAN;
                const double want = view.bounds.size.width / zoom;
                reloadNote = "clientWidth " + fmt("%.0f", cw) + " after reload at pageZoom " + fmt("%.2f", view.pageZoom);
                if (!(std::abs(cw - want) <= kViewportTolerance))
                    failures.add("reload: pageZoom did not survive navigation: " + reloadNote);
            }
        }

        // 对照臂(只记录):pageZoom = 1、CSS zoom 0.8 时 rect 与真实点击的落点差多少。旧语义下这就是 KI-16 那个偏移。
        std::string cssArm = "not run";
        if (!negativeControl)
        {
            applyZoomAndSize(window, view, 1.0, true);
            spinFor(0.2);
            std::string err;
            NSDictionary* before = runJs(view, @"const c=window.__m.card;c.style.zoom='0.8';"
                                                 "const r=document.getElementById('c').getBoundingClientRect();"
                                                 "return {l:r.left,t:r.top,w:r.width};",
                                         &err);
            if ([before isKindOfClass:[NSDictionary class]])
            {
                // 点的是 #c 中心的**真实**位置(设计坐标 x 0.8):事件的 clientX 与 rect 换算的结果差多少,就是偏移。
                const Target& t = kTargets[2];
                const ClickResult click = clickAt(view, window, t.cx() * 0.8, t.cy() * 0.8);
                cssArm = "rect.left " + fmt("%.1f", numberAt(before, @"l")) + " width " + fmt("%.1f", numberAt(before, @"w")) + "; real click " + (click.delivered ? "client (" + fmt("%.1f", click.clientX) + ", " + fmt("%.1f", click.clientY) + ") on '" + click.targetId + "'" : std::string("not delivered"));
            }
            runJs(view, @"window.__m.card.style.zoom='1';return 1;", &err);
        }

        // ---- 结论
        const bool clicksRecorded = totalDelivered == 0;
        std::printf("\nPAGEZOOM-PROBE macos=\"%s\"\n", macos.c_str());
        std::printf("PAGEZOOM-PROBE safari=%s webkit-bundle=%s\n", safariVersion.c_str(), webKitVersion.c_str());
        std::printf("PAGEZOOM-PROBE user-agent=\"%s\"\n", userAgent.c_str());
        std::printf("PAGEZOOM-PROBE zoomRectMode=%s\n", zoomRectMode.c_str());
        std::printf("PAGEZOOM-PROBE screen-backing-scale=%.2f visible=%.0fx%.0f\n", screenScale, screenSize.width, screenSize.height);
        std::printf("PAGEZOOM-PROBE window-key=%s\n", window.isKeyWindow ? "yes" : "no");
        std::printf("PAGEZOOM-PROBE real-clicks delivered=%d/%d correct=%d%s\n", totalDelivered, totalClicks, totalCorrect, clicksRecorded ? " (not delivered: recorded, not judged)" : "");
        std::printf("PAGEZOOM-PROBE reload=\"%s\"\n", reloadNote.c_str());
        std::printf("PAGEZOOM-PROBE css-zoom-arm=\"%s\"\n", cssArm.c_str());
        std::printf("PAGEZOOM-PROBE negative-control=%s\n", negativeControl ? "yes" : "no");
        std::printf("PAGEZOOM-PROBE RESULT=%s failures=%d\n", failures.lines.empty() ? "PASS" : "FAIL", static_cast<int>(failures.lines.size()));

        if (summaryPath != nil)
        {
            std::string md;
            md += "### pageZoom probe (real WebKit)\n\n";
            md += "- macOS: " + macos + "; Safari " + safariVersion + "; WebKit bundle " + webKitVersion + "\n";
            md += "- `zoomRectMode` (product `hit.js`): **" + zoomRectMode + "**; screen backing scale " + fmt("%.2f", screenScale) + "\n";
            md += "- real clicks delivered " + std::to_string(totalDelivered) + "/" + std::to_string(totalClicks) + ", correct " + std::to_string(totalCorrect) + "\n";
            md += "- reload: " + reloadNote + "\n";
            md += "- CSS zoom arm (record only): " + cssArm + "\n\n";
            md += "| pageZoom | view | client | dpr | shell | style.zoom | backing | rect fails | efp fails | clicks |\n";
            md += "|---|---|---|---|---|---|---|---|---|---|\n";
            for (const PresetRow& r : rows)
            {
                md += "| " + fmt("%.2f", r.zoom) + " | " + fmt("%.0f", r.viewW) + "x" + fmt("%.0f", r.viewH) + " | " + fmt("%.0f", r.clientW) + "x" + fmt("%.0f", r.clientH) + " | " + fmt("%.2f", r.dpr) + " | " + fmt("%.4f", r.shell) + " | " + r.cssZoom + " | " + fmt("%.2f", r.backing) + " | " + std::to_string(r.rectFails) + " | " + std::to_string(r.efpFails) + " | " + std::to_string(r.clicksCorrect) + "/" + std::to_string(r.clicksDelivered) + " |\n";
            }
            md += "\nResult: **" + std::string(failures.lines.empty() ? "PASS" : "FAIL") + "**\n";
            [[NSString stringWithUTF8String:md.c_str()] writeToFile:summaryPath atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }

        return failures.lines.empty() ? 0 : 1;
    }
}
