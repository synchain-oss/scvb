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
)

target_sources(SCVBInput PRIVATE
  PluginProcessor.h
  PluginProcessor.cpp
)

target_include_directories(SCVBInput PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(SCVBInput PRIVATE scvb_core)

if(MSVC)
  target_compile_options(SCVBInput PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBInput PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBInput PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)
