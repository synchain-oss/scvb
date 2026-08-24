# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与[语义化版本](https://semver.org/lang/zh-CN/)。

版本号唯一真源 = 顶层 `CMakeLists.txt` 的 `project(SCVB VERSION ...)`;发版流程与 tag 规则见 [docs/RELEASE.md](docs/RELEASE.md)。

固定小节 `### ⚠️ 契约变更` 置于其他小节之前,记录 state abi / IPC abi 升级、参数默认值变更、DSP 可闻变化与协议不变量变更;**没有内容时整节省略**,所以它一出现就是醒目的。每个 PR 自己改本文件,不留到发版时补写。

## [Unreleased]

### 新增

- 仓库骨架与构建脚手架(CMake 5 目标 / CI / 本地 gates,#T01)
- 用户文档组:双语 README、[用户手册](docs/USER_GUIDE.zh-CN.md)(中英)、[发布流程与发布说明模板](docs/RELEASE.md)(#T39b)
- 九条硬约束红字的唯一生成器 `scripts/gen-hard-rules.mjs`:真源 = `docs/USER_GUIDE.zh-CN.md` 的 `## 硬约束` 小节,译文 = `docs/hard-rules.i18n.json`,机器生成其余 6 处落地面(USER_GUIDE EN、两份 README 快速上手、`web/shared/i18n.js` 的 zh/en/fr);`--check` 逐字节比对,任何位置手抄即红(#T39b)
- 双语文档结构对等检查 `scripts/check-doc-parity.ps1`,覆盖 README 与 USER_GUIDE 两对(#T39b)

### 变更

- `web/shared/i18n.js` 的 `guide.title` 与 `guide.rule1..9` 改为生成区,不再手写(#T39b)

## [0.1.0] - 2026-08-13

### 新增

- 首 commit:SCVB 仓库骨架(ADR-012 目录 + 开源必备八件套 + 空壳可构建双插件)。

[Unreleased]: https://github.com/synchain-oss/scvb/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/synchain-oss/scvb/releases/tag/v0.1.0
