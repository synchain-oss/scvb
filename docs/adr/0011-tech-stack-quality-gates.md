# ADR-011 · 技术栈与质量门

> 状态: 冻结
> 最后更新: 2026-10-16(对应 constitution/ADR.md v2.4)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-011 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- JUCE 8.0.8 / CMake ≥3.22 / C++17 / **Windows:MSVC 静态 CRT(对齐 Bridge);macOS:Apple clang,arm64,`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`(v2.4/J186)**
- UI:JUCE WebView(**Windows:WebView2 静态 loader;macOS:系统 WKWebView**),纯 HTML/CSS/JS,复用 Bridge 的资源嵌入+resource provider+native function 桥模式与缩放机制(web 固定设计盒 + CSS zoom + setSize 同步 + 10 秒确认防呆)
- 浏览器预览:复刻 web-contract 模式(mock 桥后端),axe-core 可跑无障碍检查
- 单测:Catch2 测 scvb_core(VAD/分段/响度/分配/曲线插值全部离线可测);pluginval strictness 5(--skip-gui-tests)进 CI;macOS 同强度:pluginval strictness 5 对三插件 × VST3 / AU 两格式共 6 次(v2.4/J186;具体清单以 `build-macos.yml` 为准)
- clang-format(仓库带 .clang-format,风格对齐 Bridge 现状);/W4(MSVC)+ -Wall -Wextra -Wpedantic;**macOS 同用 -Wall -Wextra -Wpedantic,Apple clang 一方源码零警告**(v2.4/J186)
- IPC 结构体:static_assert 布局与 lock-free;所有跨线程原子 std::atomic
- **macOS 质量门(v2.4/J186)**:mac CI job 跑 clang 一方零警告构建、ctest(12 条)、`auval -v aufx <Scvi|Scvo|Scvm> Snch` ×3(判负)、`auval -strict -v` ×3(单列,**只记录、不判负**;注:pluginval strictness 5 对 `.component` 自带的 auval 子项跑的正是 `-strict`,所以 `-strict` 实际已随 pluginval 入门)、pluginval strictness 5 共 6 次(3 插件 × VST3 / AU,判负)、Output GUI pluginval 一次(**只记录、不判负**)、`scvb-auhost` 验证(进程内 / 进程外两种)、打包冒烟;门禁清单以 `build-macos.yml` 为准。**mac job 失败只提示、不阻塞 Windows 出包**(发布流程中 macOS 产物为 beta 附件,Windows 产物按原门禁出)。Windows 现有门禁(gate 1-8)口径不变。

## 修订历史(摘自 constitution/ADR.md)

- **[J14→ADR-011/014]** 增补:pluginval 双插件对偶场景「对端缺席时不阻塞不崩溃」为 S1 测项与常规 CI 关注点。
- **[J19→ADR-011]** 测试层增补 L1:双进程 IPC 契约测试(用例见 10-validation §2)。
- **[J20→ADR-011/新安全条款]** 禁止使用 `pull_request_target` 向 fork PR 暴露 secrets;此为三仓库 CI 安全禁令。
- **[J31/J41→ADR-011 安全条款扩展]** fork PR 统一门禁:review bot 仅 same-repo PR;fork PR 只跑无 secrets 构建/测试;branch-gate 仅约束 same-repo 分支命名。
- **[J38→ADR-011]** clang-format 版本钉 **18.1.8**,CI 与本地同版本。
- **[J81 连带]** ADR-011 的质量门:pluginval strictness 5 与 CI 构建计数由 2 目标扩到 3 目标;`build.ps1 -Target` 增 `Monitor`。
- **[J186→ADR-011 增补]** 工具链:macOS = Apple clang / arm64 / `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`,UI = 系统 WKWebView;质量门:macOS 同强度的 pluginval 与零警告口径,
  并新增「macOS 质量门」一条(clang 一方零警告构建、ctest、auval ×3 判负、`auval -strict` ×3 单列只记录(其效力已随 pluginval 的 auval 子项入门)、pluginval ×6 判负、Output GUI pluginval 一次只记录、`scvb-auhost` 进程内 / 进程外、打包冒烟;
  **mac job 失败只提示、不挡 Windows 出包**)。数字取自 mac CI 实况(run 38708566079 日志),清单以 `build-macos.yml` 为准,免得每改一个 CI 步骤就再修一次宪。
