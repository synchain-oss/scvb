# SCVBInput:Input 插件(VST3)。ADR-001:PLUGIN_CODE=Scvi,厂商码 Snch。
# T01 空壳:processBlock 直通的最小插件,无 Editor、无 WebView2(后续任务填充)。
juce_add_plugin(SCVBInput
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvi
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB Input"

  # "Fx" 由 JUCE 的 VST3 wrapper 自动添加(05 文档定最终分类,ADR-001 冻结插件码/bundle id)
  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.input"

  # Windows:链接 WebView2 静态 loader(顶层 CMakeLists 已预置 NuGet 包)。
  NEEDS_WEBVIEW2           TRUE
)

target_sources(SCVBInput PRIVATE
  InputProcessor.h
  InputProcessor.cpp
  InputBridgeApi.h       # T30:桥名常量表(parity C 侧真源)
  InputBridgeLogic.h     # T30:纯载荷/判定逻辑(离线可测)
  InputBridgeLogic.cpp
  InputEditor.h          # T30:WebView 编辑器 + diff-then-emit
  InputEditor.cpp
)

target_include_directories(SCVBInput PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# web/input + web/shared + web/js/juce + web/fonts -> ScvbInputWebData(BinaryData)。
# 这是 resource provider 的唯一供给面;没有它 WebView 取不到 index.html,窗口空白到看门狗
# 超时(详见 cmake/ScvbWebAssets.cmake 头注)。
scvb_add_web_assets(TARGET SCVBInputWebAssets NAMESPACE ScvbInputWebData ROLE_DIR input)

target_link_libraries(SCVBInput PRIVATE
  scvb_core
  scvb_plugin_common            # WebViewHost 装配层(INTERFACE,源编入本 target)
  SCVBInputWebAssets            # ScvbInputWebData::*(嵌入的 web UI 资源)
)

if(MSVC)
  target_compile_options(SCVBInput PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBInput PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBInput PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)

# Windows:WebView2 宏(JUCE_WEB_BROWSER 已由 scvb_plugin_common 提供;这三个与 Bridge 先例一致)。
if(WIN32)
  target_compile_definitions(SCVBInput PUBLIC
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  )
endif()
