# SCVBInput:Input 插件(VST3;macOS 上另出 AU,aufx/Scvi/Snch)。ADR-001:PLUGIN_CODE=Scvi,厂商码 Snch。
# T01 空壳:processBlock 直通的最小插件,无 Editor、无 WebView2(后续任务填充)。
juce_add_plugin(SCVBInput
  COMPANY_NAME             "Synchain"
  # 厂商元数据(Windows 文件属性「版权」+ VST3 factory 的 url/email)。版权行与 REUSE.toml 兜底块同口径;
  # 对外联系方式只用 contact@synchain.ca。只进二进制元数据,不影响插件身份(插件码 / bundle id 见 ADR-001)。
  COMPANY_COPYRIGHT        "Copyright 2026 Synchain"
  COMPANY_WEBSITE          "https://synchain.ca"
  COMPANY_EMAIL            "contact@synchain.ca"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvi
  FORMATS                  ${SCVB_PLUGIN_FORMATS}  # 根 CMakeLists.txt:VST3,Apple 上 + AU
  PRODUCT_NAME             "SCVB Input"

  # "Fx" 由 JUCE 的 VST3 wrapper 自动添加(05 文档定最终分类,ADR-001 冻结插件码/bundle id)
  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.input"

  # --- AU(只在 Apple 上随 SCVB_PLUGIN_FORMATS 生效)。两项都等于 JUCE 按本插件形态推导的默认值:
  #     Windows 上编译宏 JucePlugin_AUMainType 仍是 'aufx';AU_SANDBOX_SAFE 只进 AU 的 Info.plist ---
  # 类型写死 aufx:现在的默认推导也是 Effect,写死是防将来加 NEEDS_MIDI_INPUT 之类时 componentType
  # 从 aufx 漂成 aumf —— 那等于换了插件身份,用户工程里的 AU 实例全部丢失。AU 三元组
  # aufx/<PLUGIN_CODE>/Snch 发布后与四字码同样冻结。
  AU_MAIN_TYPE             kAudioUnitType_Effect
  # 显式 FALSE(= JUCE 默认),不要改成 TRUE:插件要建/附着跨进程共享内存段、起 WKWebView,
  # 沙箱里这两样都会被拒。M01 的 AUHostingService 进程外结论也是在 FALSE 下测得的。
  AU_SANDBOX_SAFE          FALSE

  # Windows:链接 WebView2 静态 loader(顶层 CMakeLists 已预置 NuGet 包)。
  NEEDS_WEBVIEW2           TRUE
)

target_sources(SCVBInput PRIVATE
  InputProcessor.h
  InputProcessor.cpp
  InputPluginEntry.cpp   # createEditor/createPluginFilter(抽出以便 host harness 免链 WebView2)
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
  # 参数 ID 两个宏(值与理由见根 CMakeLists.txt 的 SCVB_PLUGIN_PARAM_ID_DEFINITIONS);PUBLIC 才能
  # 传到 VST3 / AU 两个 wrapper 目标 —— 真正用它们算参数 ID 的是 wrapper 那几个编译单元。
  ${SCVB_PLUGIN_PARAM_ID_DEFINITIONS}
)

# Windows:WebView2 宏(JUCE_WEB_BROWSER 已由 scvb_plugin_common 提供;这三个与 Bridge 先例一致)。
if(WIN32)
  target_compile_definitions(SCVBInput PUBLIC
    JUCE_USE_WIN_WEBVIEW2=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  )
endif()
