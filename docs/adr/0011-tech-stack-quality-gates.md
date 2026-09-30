# ADR-011 · 技术栈与质量门

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-011 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- JUCE 8.0.8 / CMake ≥3.22 / C++17 / MSVC 静态 CRT(对齐 Bridge)
- UI:JUCE WebView(WebView2 静态 loader),纯 HTML/CSS/JS,复用 Bridge 的资源嵌入+resource provider+native function 桥模式与缩放机制(web 固定设计盒 + CSS zoom + setSize 同步 + 10 秒确认防呆)
- 浏览器预览:复刻 web-contract 模式(mock 桥后端),axe-core 可跑无障碍检查
- 单测:Catch2 测 scvb_core(VAD/分段/响度/分配/曲线插值全部离线可测);pluginval strictness 5(--skip-gui-tests)进 CI
- clang-format(仓库带 .clang-format,风格对齐 Bridge 现状);/W4(MSVC)+ -Wall -Wextra -Wpedantic
- IPC 结构体:static_assert 布局与 lock-free;所有跨线程原子 std::atomic

## 修订历史(摘自 constitution/ADR.md)

- **[J14→ADR-011/014]** 增补:pluginval 双插件对偶场景「对端缺席时不阻塞不崩溃」为 S1 测项与常规 CI 关注点。
- **[J19→ADR-011]** 测试层增补 L1:双进程 IPC 契约测试(用例见 10-validation §2)。
- **[J20→ADR-011/新安全条款]** 禁止使用 `pull_request_target` 向 fork PR 暴露 secrets;此为三仓库 CI 安全禁令。
- **[J31/J41→ADR-011 安全条款扩展]** fork PR 统一门禁:review bot 仅 same-repo PR;fork PR 只跑无 secrets 构建/测试;branch-gate 仅约束 same-repo 分支命名。
- **[J38→ADR-011]** clang-format 版本钉 **18.1.8**,CI 与本地同版本。