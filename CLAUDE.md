# CLAUDE.md —— SCVB 协作规范

> 本文件是协作 agent 与外部贡献者的常驻法条。§0 安全铁律五条三仓逐字相同(真源 = 12 §2.1 / 06 §8.4)。

## 0. 基本约定

### §0 安全铁律

1. 任何 key/token 绝不明文入库,包括测试用的假 key(secret scanning push protection 会直接拒推)。
2. workflow 里引用 secret 只能用 `${{ secrets.X }}`;禁止 echo 到日志、禁止写进 artifact。
3. 新增第三方 action 必须 pin 到 40 位 commit SHA(注释写版本号便于 dependabot 升级);`@v2` / `@main` 这类可变 ref 一律不接受,org 白名单里的 `owner/repo@*` 通配不构成防护。
4. 【J20 安全禁令,ADR-011 v1 新增安全条款】任何 workflow 一律不得使用 `pull_request_target`。这不是"不推荐"、不是"审计过就能用"——本项目不接受逐版本审计作为豁免理由。fork PR 的 AI 审查只走方案 D(维护者 `/review` 显式触发,实现 = `.github/workflows/review-dispatch.yml`)或方案 C(workflow_run 两阶段);其余情况 fork PR 只跑无 secrets 的构建/测试(J31)。机器检查:`grep -r pull_request_target .github/workflows` 零命中。
5. 所有消费仓库外部文本的自动化(review bot 等)的 prompt 末尾必须带「不可信数据声明」(06 §3.4 固定结尾)。

### 编码规范

- **新建的 `.cpp` / `.h` / `.js` 一律首行加 SPDX 头**(标识 `GPL-3.0-or-later`,U1 已定案;`web/js/juce/**` 为 JUCE 官方 helper 除外,由 `REUSE.toml` 声明 AGPL-3.0-or-later)。`scripts/check-spdx.ps1` 逐文件把关,缺头即 gate 红(06 §5.1 gate 3c)。

## 1. 分支模型与工作流程

- 默认主干 = `dev`;SCVB 主支线 = `feature/v1`(ADR-013/J13)。
- same-repo 只收 `feat/*` / `feature/*` 到 `dev`;子 PR(base=feature/v1)跑 review bot + 完整 CI(2026-08-19 起,见 §4)。
- fork PR:任意分支名(不用 `dev`/`stage`/`prod`/`feature/v1`/`feature/extraction`),维护者手工加 `external` label(J31/J41)。
- commit 规范:`type(scope): 中文描述`,全部 `git commit -s`(Signed-off-by)。

## 2. 提 PR 前的本地 Gates

- 一律经 `pwsh scripts/gates.ps1`(06 §5.1 的 8 道 gate)。
- 子 PR 用 `-PluginOnly`(gate 1-7 已与 CI 等价);feature→dev 收口 PR 必须全量(含 gate 8 真机 GUI pluginval)。
- 并行 agent 必须各用独立 git worktree 与 `-BuildDir`;GUI pluginval 全局串行。
- **2026-08-19 起子 PR 同样触发完整 CI**(仓库已公开、Actions 免费,用户指令:所有将并入 dev 的 PR 都跑 build-vst3/format/compliance;gate 8 真机 GUI pluginval 仍为本地收口 gate)。

## 3. 评审规则

- 处理完所有 comment,不止 bot 的(D2);子 PR 的 merge 由用户人工审核并亲自执行。

## 4. 各 Workflow 触发范围一览

- `build-vst3` / `format` / `compliance`:pull_request→dev + `feature/**`,push→dev + `feature/**`(2026-08-19 用户指令:子 PR 同样跑 CI);`branch-gate`(命名门禁)仍仅 pull_request→dev。
- `compliance`(gitleaks + reuse lint):无 secrets,fork PR 同样跑。
- `claude-review`:所有 base 分支、仅 same-repo;deepseek-review / release 默认 disable。

## 5. 冻结契约变更规范

三份冻结文档(`docs/PARAMETERS.md` / `docs/IPC_CONTRACT.md` / `docs/STATE_SCHEMA.md`)任何改动都必须:① 先获用户明确批准;② 写变更文档到 `docs/contract-changes/<YYYYMMDD>-<slug>.md`;③ 挂 `status/frozen-contract` 标签。机器强制 = branch-gate 的「冻结契约 path guard」。

## 6. 环境与依赖

- JUCE(版本见 `.juce-version`)、CMake ≥3.22、MSVC(静态 CRT /MT)、WebView2 SDK(NuGet)+ Evergreen Runtime、pluginval v1.0.4、Catch2(仅测试)、clang-format 18.1.8。
- 构建流水线不需要任何 secret(06 §3.1);review bot 用 org secrets(`CLAUDE_CODE_OAUTH_TOKEN` / `DEEPSEEK_KEY`)。

## 7. 冻结契约铁律

1. `docs/PARAMETERS.md` —— 123 个自动化参数的 ParamID / index / 顺序永久冻结。禁止:增删参数、改 ParamID 字符串、改 index 顺序、改 versionHint。允许:改显示名、改默认值(需变更文档 + CHANGELOG 醒目标注)。新需求一律走 state(非自动化)。
2. `docs/IPC_CONTRACT.md` —— 段名前缀 `SynchainSCVB.v1.` 与每个 header 前两字段(magic/abi)永久冻结。布局改动 → `abi+1` 且段名升 v2,新旧不互认。所有跨进程字段 `std::atomic` + `static_assert(is_always_lock_free)` + offsetof 布局校验;缓存行 64B 对齐。
3. `docs/STATE_SCHEMA.md` —— state chunk 带 `abi` 字段。读到高版本 → 拒载并提示升级(不得静默丢数据);读到低版本 → 走迁移函数升格。任何 schema 变更必须同时提供迁移函数与其单测。`setStateInformation` 处理不可信字节:长度/范围字段必须先校验再用于分配或索引。

## 8. 实时线程规则

`processBlock` / 音频回调内禁止:堆分配/释放、任何锁、文件/网络/日志 I-O、抛/捕获异常、调用 MessageManager / beginChangeGesture 系列、任何阻塞等待。允许:预分配缓冲上的定长运算、`juce::ScopedNoDenormals`、无锁 SPSC 环(acquire/release)、`std::atomic`(必须 `is_always_lock_free`)。gesture 三段式只在消息线程(50Hz Timer);音频线程只发布 playhead 快照(SPSC)。段间过渡走 ramp(默认 80ms,ADR-010);失准语义:缺口→该轨该块静音+警告计数,绝不静默出错。

## 9. 双 target 共享代码规范

- 任何被 Input 与 Output 同时使用的代码,必须落在 `src/core/`(scvb_core 静态库)。
- `src/input/` 与 `src/output/` 之间禁止互相 `#include`。
- scvb_core 不得链接 `juce_audio_plugin_client`,不得引用 `AudioProcessor`;必须能离线 Catch2 单测。
- 配置类数据的唯一真源在 Output state(ADR-004);Input 端不得自建副本作为真源。

## 10. UI / WebView 规范

web 资源经 `juce_add_binary_data` 嵌入;JS↔C++ 契约真源 = `docs/SCVB_CONTRACT.md` + `src/input/InputBridgeApi.h` / `src/output/OutputBridgeApi.h`;缩放走「web 固定设计盒 + CSS zoom + setSize 同步 + 10 秒确认防呆」;`web-preview/` 的 mock 桥必须与真桥同契约。
