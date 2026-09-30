// SPDX-License-Identifier: GPL-3.0-or-later
// test_web_assets_embedded —— 「页面真会去取的每一个文件都进了包」的第四层门禁。
//
// 【为什么要有这一层】前三层各自都对,却一起漏掉了同一类真机故障:
//   • 离线单测不实例化 WebView2(离线目标里连 WebViewHost.cpp 都不编);
//   • gate 8 的 GUI pluginval 只建/销毁编辑器,**看不见空白或被兜底面板盖住的 WebView**;
//   • gate 3e 的 web smoke 跑的是 web/ 源码与浏览器预览,那边文件系统里样样都在,
//     **与「有没有编进二进制」完全无关**。
// 于是「资源没进包」只能等到真机 DAW 里才暴露,而且症状(闪一下 → 兜底面板 → Retry 循环)
// 指不到成因。已经栽过两次:
//   ① v5.1 P0-C:Monitor 连 `scvb_add_web_assets` 都没接,index.html 都不在包里;
//   ② v5.2:①修好之后,`web/shared/trajectory-chart.js` import 的
//      `../output/canvas/{timeline,hidpi,layers,playhead}.js` 仍不在 Monitor 的 glob 范围内,
//      ES 模块图整体加载失败 → app.js 的 boot() 一行没跑 → requestInitialState() 没调 →
//      `bridgeReady_` 恒 false → 看门狗兜底。症状与 ① 几乎一样。
//
// 【断言的形状:闭包,不是清单】本文件**不维护一张手写的文件名表** —— ② 恰恰是被手写表放过的
// (上一版这里列的是 trajectory-chart.js 本身,没人去看它自己 import 了谁)。改为从
// `web/<role>/index.html` 出发,按 HTML/JS/CSS 三种引用形态做**传递闭包**,再把每个文件按
// WebView 真实会请求的 root-relative URL 喂给**真的** ResourceProvider,断言取得到、且非空。
// 新增一个 import、新加一个插件、glob 范围没跟上 —— 都在这里红,而不是在真机上。
//
// 【它证明什么、不证明什么】证明「WebView 请求这个 URL 时能拿到字节」。不证明 JS 跑得通
// (那是 gate 3e 与浏览器级 smoke 的事),也不证明桥握上了(那要真宿主)。

#include <catch2/catch_test_macros.hpp>

#include <ResourceProvider.h>

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <ScvbInputWebData.h>
#include <ScvbMonitorWebData.h>
#include <ScvbOutputWebData.h>

#ifndef SCVB_WEB_ROOT
#error "SCVB_WEB_ROOT 未定义 —— 见 tests/CMakeLists.txt"
#endif

namespace
{
using scvb::webview::ResourceProvider;

juce::File webRoot()
{
    return juce::File(juce::String(SCVB_WEB_ROOT));
}

// 一个文件里所有「会让浏览器再发一次请求」的引用。按扩展名选规则:
//   .html  <script src> / <link href> / <img src>
//   .css   url(...) / @import
//   其余(.js)静态 import / re-export / 动态 import()  —— 动态那条是 bridge.js 取
//          ../js/juce/index.js 的方式,漏掉它就等于漏掉整个 JUCE helper。
std::vector<std::string> referencesOf(const juce::File& file)
{
    const std::string text = file.loadFileAsString().toStdString();
    const juce::String ext = file.getFileExtension().toLowerCase();

    static const std::vector<std::regex> html{
        std::regex(R"(<script[^>]+src\s*=\s*["']([^"']+)["'])"),
        std::regex(R"(<link[^>]+href\s*=\s*["']([^"']+)["'])"),
        std::regex(R"(<img[^>]+src\s*=\s*["']([^"']+)["'])"),
    };
    static const std::vector<std::regex> css{
        std::regex(R"(url\(\s*["']?([^"')]+)["']?\s*\))"),
        std::regex(R"(@import\s+["']([^"']+)["'])"),
    };
    static const std::vector<std::regex> js{
        std::regex(R"(import\s+(?:[\w*{}\s,$]+\s+from\s+)?["']([^"']+)["'])"),
        std::regex(R"(import\s*\(\s*["']([^"']+)["']\s*\))"),
        std::regex(R"(export\s+(?:\*|\{[^}]*\})\s+from\s+["']([^"']+)["'])"),
    };

    const auto& rules = ext == ".html" ? html : (ext == ".css" ? css : js);

    std::vector<std::string> out;
    for (const auto& re : rules)
        for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
        {
            const std::string ref = (*it)[1].str();
            // 外链与内联数据不是包里的资源(tokens.css 有一个 data: 的 SVG chevron)。
            if (ref.rfind("http", 0) == 0 || ref.rfind("data:", 0) == 0 || ref.rfind("//", 0) == 0 ||
                ref.rfind('#', 0) == 0)
                continue;
            out.push_back(ref);
        }
    return out;
}

// 从 web/<role>/index.html 出发的引用闭包,键是相对 web/ 的 posix 路径(= WebView 请求的
// root-relative URL 去掉开头的 "/")。
std::set<std::string> importClosure(const juce::String& role)
{
    const juce::File root = webRoot();
    std::set<std::string> seen;
    std::vector<juce::File> stack{root.getChildFile(role).getChildFile("index.html")};

    while (!stack.empty())
    {
        const juce::File f = stack.back();
        stack.pop_back();

        const std::string rel = f.getRelativePathFrom(root).replaceCharacter('\\', '/').toStdString();
        if (!seen.insert(rel).second)
            continue;

        // 引用了一个磁盘上不存在的文件 —— 那是 web/ 自己的 bug,先在这里报出来。
        INFO("closure file: " << rel);
        REQUIRE(f.existsAsFile());

        for (const auto& ref : referencesOf(f))
            stack.push_back(f.getParentDirectory().getChildFile(juce::String(ref)));
    }
    return seen;
}

void checkPluginAssets(const char* label, const juce::String& role, ResourceProvider::Source src)
{
    INFO("plugin: " << label);
    REQUIRE(src.resourceCount > 0); // 空 Source ⇒ provide() 恒 nullopt ⇒ 真机空白窗口 + 看门狗超时
    REQUIRE(src.originalFilenames != nullptr);
    REQUIRE(src.getNamedResource != nullptr);

    const ResourceProvider provider(src);

    // WebView 的根请求:JUCE 后端已剥掉 origin,provider 收到的是 "/"。
    const auto root = provider.provide("/");
    REQUIRE(root.has_value());
    REQUIRE_FALSE(root->data.empty());
    REQUIRE(juce::String(root->mimeType) == "text/html");

    // 闭包里的每一个文件,按 WebView 真实请求的 URL 形态取。
    // (URL 相对 https://juce.backend/<role>/index.html 解析,`../shared/x.js` 就是 `/shared/x.js`;
    //  provider 按 basename 扁平匹配,见 cmake/ScvbWebAssets.cmake 头注。)
    const auto closure = importClosure(role);
    REQUIRE(closure.size() > 10); // 闭包走塌了(正则全不命中)也要红,而不是空集恒真

    for (const auto& rel : closure)
    {
        INFO("resource: /" << rel);
        const auto r = provider.provide(juce::String("/") + juce::String(rel));
        REQUIRE(r.has_value());
        REQUIRE_FALSE(r->data.empty());
    }

    // index.html 本身按显式路径也要取得到,且与根请求是同一份。
    const auto idx = provider.provide("/" + role + "/index.html");
    REQUIRE(idx.has_value());
    REQUIRE(idx->data == root->data);
}
} // namespace

// JUCE 生成的 ScvbXxxWebData 是 namespace 不是 type,没法用模板收敛,逐个显式展开。
TEST_CASE("三个插件:页面 import 闭包里的每个文件都编进了二进制", "[webview][assets]")
{
    checkPluginAssets("SCVB Input", "input",
                      {ScvbInputWebData::namedResourceListSize, ScvbInputWebData::originalFilenames,
                       ScvbInputWebData::namedResourceList, &ScvbInputWebData::getNamedResource});

    checkPluginAssets("SCVB Output", "output",
                      {ScvbOutputWebData::namedResourceListSize, ScvbOutputWebData::originalFilenames,
                       ScvbOutputWebData::namedResourceList, &ScvbOutputWebData::getNamedResource});

    checkPluginAssets("SCVB Monitor", "monitor",
                      {ScvbMonitorWebData::namedResourceListSize, ScvbMonitorWebData::originalFilenames,
                       ScvbMonitorWebData::namedResourceList, &ScvbMonitorWebData::getNamedResource});
}

// 闭包必须真的穿过 shared 模块自己的 import —— v5.2 那个 bug 就藏在第二跳:
// app.js 直接 import 的 trajectory-chart.js 在包里,它 import 的 output/canvas/* 不在。
// 上一版本文件按手写清单断言,恰好只列到第一跳,于是全绿。
TEST_CASE("闭包穿透二跳:Monitor 经 trajectory-chart 达到 output/canvas", "[webview][assets]")
{
    const auto closure = importClosure("monitor");

    REQUIRE(closure.count("shared/trajectory-chart.js") == 1);
    for (const char* deep : {"output/canvas/timeline.js", "output/canvas/hidpi.js", "output/canvas/layers.js",
                             "output/canvas/playhead.js"})
    {
        INFO("second-hop: " << deep);
        REQUIRE(closure.count(deep) == 1);
    }

    // 动态 import() 那条也要在闭包里(bridge 取 JUCE helper 走的是 import("../js/juce/index.js"))。
    REQUIRE(closure.count("js/juce/index.js") == 1);
}
