// SPDX-License-Identifier: GPL-3.0-or-later
#include "ResourceProvider.h"

#include <vector>

namespace scvb::webview
{

using WBC = juce::WebBrowserComponent;

ResourceProvider::ResourceProvider(Source source) : source_(source) {}

std::optional<juce::WebBrowserComponent::Resource> ResourceProvider::provide(const juce::String& url) const
{
    // url 形如 "/" 或 "/index.html" 或 "/js/juce/index.js"(root-relative 路径)。
    const auto path =
        (url == "/" || url.isEmpty()) ? juce::String("index.html") : url.fromFirstOccurrenceOf("/", false, false);
    const auto baseName = path.fromLastOccurrenceOf("/", false, false);

    if (source_.originalFilenames == nullptr || source_.resourceNames == nullptr || source_.getNamedResource == nullptr)
        return std::nullopt;

    // 用 BinaryData 的原始文件名匹配(避免手工复刻 JUCE 的符号名 mangling)。
    for (int i = 0; i < source_.resourceCount; ++i)
    {
        if (juce::String(source_.originalFilenames[i]) == baseName)
        {
            int size = 0;
            if (const char* data = source_.getNamedResource(source_.resourceNames[i], size))
            {
                std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                                             reinterpret_cast<const std::byte*>(data) + size);
                const auto ext = baseName.fromLastOccurrenceOf(".", false, false);
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
