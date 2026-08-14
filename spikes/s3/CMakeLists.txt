# S3 WebView 承载力 spike(J16:验证代码,不进生产路径,由 T26 的 PR 删除)。
# SCVBS3Output:最小插件壳 —— 承载 WebView2 编辑器的 Output 插件(无真实 DSP,processBlock 直通)。
# WebView2 装配全套搬运 Bridge 先例(01 §6.1 的 9 条机制);spike 页经 juce_add_binary_data 嵌入。

juce_add_plugin(SCVBS3Output
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvs
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB S3 Output"

  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.s3output"

  # Windows:链接 WebView2 静态 loader(顶层 CMakeLists 已预置 NuGet 包)。
  NEEDS_WEBVIEW2           TRUE
)

# spike 页资源嵌入(零外部依赖;resource provider 按原始文件名服务)。
juce_add_binary_data(SCVBS3WebAssets SOURCES
  web/output/index.html
  web/output/styles.css
  web/output/bridge.js
  web/output/mock-data.js
  web/output/app.js
  web/output/canvas.js
  web/output/bench.js
)

target_sources(SCVBS3Output PRIVATE
  S3OutputProcessor.h
  S3OutputProcessor.cpp
  S3OutputEditor.h
  S3OutputEditor.cpp
)

target_link_libraries(SCVBS3Output PRIVATE
  juce::juce_audio_plugin_client
  juce::juce_gui_extra            # WebBrowserComponent 在 juce_gui_extra
  SCVBS3WebAssets                 # 生成的 BinaryData
)

target_compile_definitions(SCVBS3Output PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_WEB_BROWSER=1
)

if(WIN32)
  target_compile_definitions(SCVBS3Output PUBLIC
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  )
endif()

if(MSVC)
  target_compile_options(SCVBS3Output PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBS3Output PRIVATE -Wall -Wextra -Wpedantic)
endif()
