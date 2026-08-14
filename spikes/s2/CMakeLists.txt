# S2 自动化写入 spike(J16:验证代码,不进生产路径,由 T15 的 PR 删除)。
# SCVBS2Output:123 参数完整布局 + AutomationPrinter 打固定阶梯包络(无 DSP、无分析)。

juce_add_plugin(SCVBS2Output
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scva
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB S2 Output"

  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.s2output"
)

target_sources(SCVBS2Output PRIVATE
  S2OutputProcessor.h
  S2OutputProcessor.cpp
  S2OutputEditor.h
  S2OutputEditor.cpp
)

target_compile_definitions(SCVBS2Output PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)

if(MSVC)
  target_compile_options(SCVBS2Output PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBS2Output PRIVATE -Wall -Wextra -Wpedantic)
endif()
