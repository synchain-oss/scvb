// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>

namespace scvb::webview
{
// ResourceProvider —— resource provider + MIME 映射(01 §6.1 机制 4;平台无关)。
// 按 BinaryData::originalFilenames 反查原始文件名(避免手工复刻 JUCE 的符号名 mangling)。
// Input / Output 各有一个 juce_add_binary_data 生成的命名空间,经 Source 注入本类:
// 插件侧把自己的 BinaryData::* 静态成员填进 Source,ResourceProvider 不感知具体命名空间。
class ResourceProvider
{
public:
    // juce_add_binary_data 生成命名空间的最小描述(填插件自己的 BinaryData::* 静态成员)。
    // T27/T28 交付 web 资源前,插件可传空 Source(provide 恒 nullopt,不造假资源)。
    struct Source
    {
        int resourceCount = 0;
        const char* const* originalFilenames = nullptr;
        const char* const* resourceNames = nullptr;
        const char* (*getNamedResource)(const char* resourceName, int& dataSizeInBytes) = nullptr;
    };

    explicit ResourceProvider(Source source);

    // url 形如 "/" 或 "/index.html"(root-relative 路径);命中则返回资源 + MIME,否则 nullopt。
    std::optional<juce::WebBrowserComponent::Resource> provide(const juce::String& url) const;

    // 扩展名 -> MIME:html/css/js/mjs/json/svg/woff2/woff/ttf/png;mjs 与 js 都给 text/javascript
    // (ES module 必须是 JS MIME,否则浏览器拒载)。
    static const char* mimeForExtension(const juce::String& ext);

    const Source& source() const { return source_; }

private:
    Source source_;
};

} // namespace scvb::webview
