# SCVBMonitor:Monitor 插件(VST3),第三主目标([J75];ADR-001 由 J75 授权扩为三主目标,
# 修宪见 docs/contract-changes/20260825-monitor-target.md)。
# PLUGIN_CODE=Scvm(Scvi/Scvo/Snb1 已占),厂商码 Snch。0 自动化参数、音频直通、只读 attach。
juce_add_plugin(SCVBMonitor
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvm
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB Monitor"

  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.monitor"

  # Windows:链接 WebView2 静态 loader(顶层 CMakeLists 已预置 NuGet 包)。
  NEEDS_WEBVIEW2           TRUE
)

target_sources(SCVBMonitor PRIVATE
  MonitorProcessor.h
  MonitorProcessor.cpp
  MonitorBridgeApi.h
  MonitorEditor.h
  MonitorEditor.cpp
)

target_include_directories(SCVBMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(SCVBMonitor PRIVATE
  scvb_core
  scvb_plugin_common            # WebViewHost 装配层(INTERFACE,源编入本 target)
)

if(MSVC)
  target_compile_options(SCVBMonitor PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBMonitor PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBMonitor PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)

# Windows:WebView2 宏(JUCE_WEB_BROWSER 已由 scvb_plugin_common 提供;与 Input/Output 先例一致)。
if(WIN32)
  target_compile_definitions(SCVBMonitor PUBLIC
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  )
endif()
