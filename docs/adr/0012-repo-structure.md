# ADR-012 · SCVB 仓库结构(骨架)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-012 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

```
scvb/
├── CMakeLists.txt          # 顶层:core + input + output 三 target
├── src/core/               # scvb_core:dsp/ analysis/ ipc/ state/ engine/
├── src/input/              # Input 插件(Processor/Editor)
├── src/output/             # Output 插件(Processor/Editor)
├── web/                    # WebView UI(input/ output/ shared/ fonts/)
├── web-preview/            # 浏览器 mock 预览(复刻 web-contract 模式)
├── tests/                  # Catch2
├── scripts/                # build.ps1 一键构建等
├── docs/                   # 架构/契约/DAW 指南/发布
└── .github/workflows/      # build-vst3 / review bots / branch-gate
```

## 修订历史(摘自 constitution/ADR.md)

- **[J37→ADR-012]** 仓库骨架补开源必备文件:LICENSE、.clang-format、.gitignore、.gitattributes、CLAUDE.md、CONTRIBUTING.md、CODE_OF_CONDUCT.md、SECURITY.md、.github/ISSUE_TEMPLATE(12 §1.1 八件套为准)。