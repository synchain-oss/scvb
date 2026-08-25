# scvb_add_web_assets —— 把 web/ 的 UI 资源经 juce_add_binary_data 编进插件二进制。
#
# 【为什么必须有这一步】WebViewHost 的 resource provider(01 §6.1 机制 4)只从
# BinaryData 取资源:ResourceProvider::Source 为空时 provide() 恒 nullopt,WebView 拿不到
# index.html,页面永远空白,前端也就永远调不到 requestInitialState —— 表现为「看门狗超时」
# 兜底面板,而真因是资源根本没进包。web/README.md 早写明「资源经 juce_add_binary_data 嵌入」,
# 这一步此前一直缺位(T27b/T28 只交付了 web/ 与浏览器预览)。
#
# 【口径:按文件名扁平匹配】ResourceProvider 按 BinaryData::originalFilenames 的**原始文件名**
# (只有 basename,没有目录)反查,所以:
#   • 目录结构不进包,页面里的相对路径(../shared/x.js、./canvas/y.js)照样能命中;
#   • 但同一插件的资源集内**文件名必须全局唯一**,否则先命中者赢、后者永远取不到。
# 故本函数在配置期就断言唯一性,重名即 FATAL_ERROR —— 让它在 cmake 阶段炸,而不是等到
# 真机上表现为一个查不出来的空白面板。
#
# 用法:
#   scvb_add_web_assets(TARGET SCVBOutputWebAssets NAMESPACE ScvbOutputWebData ROLE_DIR output)

function(scvb_add_web_assets)
  set(one_value_args TARGET NAMESPACE ROLE_DIR)
  cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_NAMESPACE OR NOT ARG_ROLE_DIR)
    message(FATAL_ERROR "scvb_add_web_assets: TARGET / NAMESPACE / ROLE_DIR 三者都必填")
  endif()

  set(web_root "${CMAKE_SOURCE_DIR}/web")

  # CONFIGURE_DEPENDS:新增 web 文件会触发重新配置。显式列表会在有人加一个新 tab 模块时
  # 静默漏掉它(症状同样是运行期空白),glob + 唯一性断言比手工清单可靠。
  file(GLOB_RECURSE role_files CONFIGURE_DEPENDS
    "${web_root}/${ARG_ROLE_DIR}/*.html"
    "${web_root}/${ARG_ROLE_DIR}/*.js"
    "${web_root}/${ARG_ROLE_DIR}/*.css")

  file(GLOB shared_files CONFIGURE_DEPENDS
    "${web_root}/shared/*.js"
    "${web_root}/shared/*.css"
    "${web_root}/shared/*.png")

  # JUCE 官方前端 helper(bridge.js 惰性 import 的 ../js/juce/index.js)。
  file(GLOB juce_helper_files CONFIGURE_DEPENDS "${web_root}/js/juce/*.js")

  # 字体(base.css 的 @font-face 经 ../fonts/*.woff2 引用)。
  file(GLOB font_files CONFIGURE_DEPENDS "${web_root}/fonts/*.woff2")

  set(all_files ${role_files} ${shared_files} ${juce_helper_files} ${font_files})

  if(NOT all_files)
    message(FATAL_ERROR "scvb_add_web_assets: web/${ARG_ROLE_DIR} 下没扫到任何资源")
  endif()

  # 文件名唯一性断言(见文件头「按文件名扁平匹配」)。
  set(seen_names)
  foreach(f IN LISTS all_files)
    get_filename_component(base "${f}" NAME)
    if(base IN_LIST seen_names)
      message(FATAL_ERROR
        "scvb_add_web_assets: 资源文件名重复 '${base}'(${ARG_ROLE_DIR} 侧)。"
        " ResourceProvider 按 basename 匹配,重名会让其中一个永远取不到 —— 请改名。")
    endif()
    list(APPEND seen_names "${base}")
  endforeach()

  juce_add_binary_data(${ARG_TARGET}
    NAMESPACE   ${ARG_NAMESPACE}
    HEADER_NAME ${ARG_NAMESPACE}.h
    SOURCES     ${all_files})
endfunction()
