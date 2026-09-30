// SPDX-License-Identifier: GPL-3.0-or-later
#include "ResourceProvider.h"

#include <vector>

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

ResourceProvider::ResourceProvider(Source source) : source_(source) {}

namespace
{
// 归一资源 URL 为 root-relative 资源路径(剥 origin/query/fragment;"/"或空 → "index.html")。
// JUCE 8.0.8 的 Windows/mac 后端在调 provider 前已剥 "https://juce.backend"(根请求给 "/"),
// 此处再防御性处理完整 URL 入参,保证跨后端/跨版本口径一致(01 §6.1 机制 4)。
juce::String resourcePath(const juce::String& url)
{
    juce::String path = url.upToFirstOccurrenceOf("?", false, false).upToFirstOccurrenceOf("#", false, false);

    // 完整 URL("scheme://host/path")→ 剥 origin;否则视为已 root-relative。
    if (const int schemeEnd = path.indexOf("://"); schemeEnd >= 0)
    {
        const int pathStart = path.indexOfChar(schemeEnd + 3, '/');
        path = (pathStart >= 0) ? path.substring(pathStart + 1) : juce::String("index.html");
    }

    if (path.startsWith("/"))
        path = path.substring(1);

    return path.isEmpty() ? juce::String("index.html") : path;
}
} // namespace

std::optional<juce::WebBrowserComponent::Resource> ResourceProvider::provide(const juce::String& url) const
{
    // 归一后为 root-relative 资源路径("index.html"、"js/juce/index.js")。
    const auto path = resourcePath(url);
    const auto baseName = path.fromLastOccurrenceOf("/", false, false);
    const auto fileName = baseName.isEmpty() ? path : baseName; // 顶层资源无 '/' → 整段

    if (source_.originalFilenames == nullptr || source_.resourceNames == nullptr || source_.getNamedResource == nullptr)
        return std::nullopt;

    // 用 BinaryData 的原始文件名匹配(避免手工复刻 JUCE 的符号名 mangling)。
    for (int i = 0; i < source_.resourceCount; ++i)
    {
        if (juce::String(source_.originalFilenames[i]) == fileName)
        {
            int size = 0;
            if (const char* data = source_.getNamedResource(source_.resourceNames[i], size))
            {
                std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                                             reinterpret_cast<const std::byte*>(data) + size);
                const auto ext = fileName.fromLastOccurrenceOf(".", false, false);
                return WBC::Resource{std::move(bytes), juce::String(mimeForExtension(ext))};
            }
        }
    }
    return std::nullopt;
}

const char* ResourceProvider::mimeForExtension(const juce::String& ext)
{
    const auto e = ext.toLowerCase();
    if (e == "html" || e == "htm")
        return "text/html";
    if (e == "css")
        return "text/css";
    if (e == "js" || e == "mjs")
        return "text/javascript"; // ES module 必须是 JS MIME
    if (e == "json")
        return "application/json";
    if (e == "svg")
        return "image/svg+xml";
    if (e == "woff2")
        return "font/woff2";
    if (e == "woff")
        return "font/woff";
    if (e == "ttf")
        return "font/ttf";
    if (e == "png")
        return "image/png";
    return "application/octet-stream";
}

} // namespace scvb::webview
