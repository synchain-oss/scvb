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

# web/monitor + web/shared + web/js/juce + web/fonts -> ScvbMonitorWebData(BinaryData)。
# 缺这一句时 Monitor 根本没有嵌入资源:ResourceProvider 对**每一个**请求都回 nullopt,
# WebView2 于是把 https://juce.backend/monitor/index.html 当成真网址去解析 —— 真机上就是
# 「找不到 juce.backend 的服务器 IP」(v5.1 实测 P0-C)。Input/Output 在 #82 就接了这一句,
# Monitor 立项时(#88)漏掉,而它不影响构建、也不影响 pluginval(那条路不开窗)。
#
# EXTRA_DIRS output/canvas:模块图是
#   web/monitor/app.js -> web/shared/trajectory-chart.js -> web/output/canvas/{timeline,hidpi,
#   layers,playhead}.js
# 四个文件都不在 monitor/shared/js/fonts 四个目录里,于是 v5.2(P0-C 修好、index.html 已进包)
# 上仍然是:HTML/CSS 先画出来 → `<script type="module" src="./app.js">` 的模块图在这四个 404 上
# 整体加载失败 → app.js 的 boot() 一行没跑 → requestInitialState() 没调 → `bridgeReady_` 恒 false
# → 冷预算 15000ms 到点看门狗盖上兜底面板,Retry 重来一遍还是这样。
# 真机三个症状(闪一下被盖 / 缩放没生效 / 没有播放头竖线)都出在这里:后两个是 boot 没跑的连带
# ——applyScale() 没执行、playhead.js 本身就是缺的那四个之一。
#
# 【这是 §9 分层的一处**有意例外**,不是疏忽】§9 禁的是「一个插件目录直接编译 / include 另一个
# 插件的源码」,说的是 C++ 那一层;这里跨过去的是 **web 资源**。方向上的真欠债在
# web/shared/trajectory-chart.js —— 一个 shared 模块反过来 import 了某个 role 目录。终局是把
# canvas/ 提到 web/shared/canvas/,那要动 Output 的路径面(眼下 #101 正在改那批文件),不在本次
# 范围。smoke-monitor.mjs §9 已把这条例外**逐字钉住**:web 资源只许经 EXTRA_DIRS 这一种写法跨,
# C++ 的 target_sources / include 面照旧一个字都不许出现另一个插件的目录。
scvb_add_web_assets(TARGET SCVBMonitorWebAssets NAMESPACE ScvbMonitorWebData ROLE_DIR monitor
                    EXTRA_DIRS output/canvas)

target_link_libraries(SCVBMonitor PRIVATE
  scvb_core
  scvb_plugin_common            # WebViewHost 装配层(INTERFACE,源编入本 target)
  SCVBMonitorWebAssets          # ScvbMonitorWebData::*(嵌入的 web UI 资源)
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
