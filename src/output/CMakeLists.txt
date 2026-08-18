# SCVBOutput:Output 插件(VST3)。ADR-001:PLUGIN_CODE=Scvo,厂商码 Snch。
# T01 空壳:processBlock 直通的最小插件,无 Editor、无 WebView2(后续任务填充)。
juce_add_plugin(SCVBOutput
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvo
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB Output"

  # "Fx" 由 JUCE 的 VST3 wrapper 自动添加(05 文档定最终分类,ADR-001 冻结插件码/bundle id)
  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.output"
)

target_sources(SCVBOutput PRIVATE
  PluginProcessor.h
  PluginProcessor.cpp
  OutputParams.h
  OutputParams.cpp
  OutputAuthority.h
  OutputAuthority.cpp
)

target_include_directories(SCVBOutput PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(SCVBOutput PRIVATE scvb_core)

if(MSVC)
  target_compile_options(SCVBOutput PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBOutput PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBOutput PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)
