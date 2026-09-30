# scvb_plugin_common:两插件共用的 WebView2 装配层(01 §6.1 / §9)。
# INTERFACE 库:源文件编译进每个链接它的插件(避免 STATIC 库把 JUCE 模块源码重复编入多个 target)。
# 这里只声明 JUCE_WEB_BROWSER(WebBrowserComponent 声明所需);Windows 的 WebView2 宏与
# NEEDS_WEBVIEW2 由各插件在 juce_add_plugin 上声明(与 Bridge 先例一致,同时避免测试 target
# 被迫链接 WebView2 loader)。

add_library(scvb_plugin_common INTERFACE)

target_sources(scvb_plugin_common INTERFACE
  WebViewHost.h
  WebViewHost.cpp
  PlatformWebView.h
  PlatformWebView.cpp
  PlatformWebViewRuntime.cpp
  ResourceProvider.h
  ResourceProvider.cpp
  FallbackPanel.h
  FallbackPanel.cpp
  BridgeBase.h
  BridgeBase.cpp
  # 系统级 UI 全局默认(%APPDATA% 小文件:引导页/导览已读位、语言、缩放档)。
  # 归在本层而不是任一插件目录:Input 与 Output **都**要读写同一份,而它唯一的依赖
  # (BridgeBase.h 的缩放档位边界)就在本层。放 src/core/ 会让核心库反过来依赖
  # plugin-common —— §9 的分层是 core ← plugin-common ← 插件,不能倒。
  UiDefaultsStore.h
  UiDefaultsStore.cpp
)

target_include_directories(scvb_plugin_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/core   # DesignBox.h(由 scripts/gen-design-box.py 生成)
)

target_link_libraries(scvb_plugin_common INTERFACE
  juce::juce_audio_processors      # AudioProcessorEditor
  juce::juce_gui_extra             # WebBrowserComponent
)

target_compile_definitions(scvb_plugin_common INTERFACE
  JUCE_WEB_BROWSER=1               # 启用 WebBrowserComponent(juce_gui_extra)
)

if(MSVC)
  target_compile_options(scvb_plugin_common INTERFACE /W4 /utf-8)
else()
  target_compile_options(scvb_plugin_common INTERFACE -Wall -Wextra -Wpedantic)
endif()
