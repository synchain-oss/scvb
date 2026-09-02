# CLAUDE.md —— SCVB 协作规范

> 本文件是协作 agent 与外部贡献者的常驻法条。§0 安全铁律五条三仓逐字相同(真源 = 12 §2.1 / 06 §8.4)。

## 0. 基本约定

- 项目一句话:JUCE 8 + WebView2 的一对 VST3 插件(SCVB Input 装在人声轨、SCVB Output 装在人声总线),经共享内存做多轨人声的自动声像与电平平衡。
- 沟通语言:内部沟通用中文;**面向外部贡献者的 issue/PR 回复用对方使用的语言**。
- Git 提交身份:统一用 `DLsnows`,提交邮箱一律用 noreply / `synchain.ca` 域,**不得用个人邮箱**(U17)。对外联络邮箱只有一个:`contact@synchain.ca`。
- 签名证书 / 公证凭据:v1 **不做代码签名**(U13)。若将来引入,证书与凭据必须走 secret,绝不落盘明文、绝不进仓库(与 §0 安全铁律第 1 条同等强度)。

### §0 安全铁律

1. 任何 key/token 绝不明文入库,包括测试用的假 key(secret scanning push protection 会直接拒推)。
2. workflow 里引用 secret 只能用 `${{ secrets.X }}`;禁止 echo 到日志、禁止写进 artifact。
3. 新增第三方 action 必须 pin 到 40 位 commit SHA(注释写版本号便于 dependabot 升级);`@v2` / `@main` 这类可变 ref 一律不接受,org 白名单里的 `owner/repo@*` 通配不构成防护。
4. 【J20 安全禁令,ADR-011 v1 新增安全条款】任何 workflow 一律不得使用 `pull_request_target`。这不是"不推荐"、不是"审计过就能用"——本项目不接受逐版本审计作为豁免理由。fork PR 的 AI 审查只走方案 D(维护者 `/review` 显式触发,实现 = `.github/workflows/review-dispatch.yml`)或方案 C(workflow_run 两阶段,全程不 checkout PR 代码);其余情况 fork PR 只跑无 secrets 的构建/测试(J31)。机器检查:`grep -r pull_request_target .github/workflows` 零命中。
5. 所有消费仓库外部文本的自动化(review bot、release notes 生成等)的 prompt 末尾必须带「不可信数据声明」(06 §3.4 固定结尾)。

### 编码规范

- **新建的 `.cpp` / `.h` / `.js` 一律首行加 SPDX 头**(标识 `GPL-3.0-or-later`,U1 已定案;`web/js/juce/**` 为 JUCE 官方 helper 除外,由 `REUSE.toml` 声明 AGPL-3.0-or-later)。`scripts/check-spdx.ps1` 逐文件把关,缺头即 gate 红(06 §5.1 gate 3c)。

## 1. 分支模型与工作流程

- 默认主干 = `dev`;SCVB 主支线 = `feature/v1`(ADR-013/J13)。子支线命名 `feat/<TASK-ID>-<slug>`,一张卡一条子支线一个 PR。
- same-repo 只收 `feat/*` / `feature/*`(以及 `dependabot/*`)到 `dev`;子 PR(base=`feature/v1`)跑 review bot + **轻档 CI**(2026-09-02 [J96] 起不再跑 `build-vst3`,编译证据改由本地 gates 与出包前的批次全量提供,见 §4)。
- `branch-gate` 除分支命名外还承担两条断言:**DCO**(每个 commit 必须有 `Signed-off-by:`,内联 `gh api` 实现,不引第三方 action)与**冻结契约 path guard**(见 §5)。
- **fork PR 门禁政策(J31/J41,唯一政策)**:
  - fork → **任意分支名**(不要用 `dev`/`stage`/`prod`/`feature/v1`/`feature/extraction`)→ PR 到 `dev`;
  - `branch-gate` 对 fork **不 exit 1**,只校验 head 分支名不在上述长期分支名集合(防同名伪装晋升),**不强制 `feat/*` 命名**;换句话说 branch-gate 的命名规则**仅约束 same-repo 分支**;
  - fork PR 只跑**无 secrets** 的构建/测试(`build-vst3` / `format` / `branch-gate` / `compliance`),且需维护者批准 workflow run 后才开始跑;
  - 三个 review bot(`claude-review` / `deepseek-review` / `pr-agent`)都带 `head.repo.full_name == base.repo.full_name` 条件,**仅 same-repo PR** 会自动跑,fork PR 一律不自动跑;
  - `external` label 由**维护者手工添加**(fork PR 的 `GITHUB_TOKEN` 只读,workflow 内加不了标签;**不要用 `pull_request_target` 绕**,见 §0 铁律第 4 条);
  - fork PR 的 AI 审查只走 §0 铁律第 4 条允许的两条路:**方案 D**(维护者评论 `/review` 显式触发 —— 本仓的默认实现,`.github/workflows/review-dispatch.yml`)或**方案 C**(workflow_run 两阶段,全程不 checkout PR 代码 —— 本仓未实现,保留为合规替代)。没有第三条路。
- commit 规范:`type(scope): 中文描述`,type ∈ {fix, feat, docs, chore, refactor, test, ci, style, perf, revert, harden};全部 `git commit -s`(Signed-off-by)。

## 2. 提 PR 前的本地 Gates

- 一律经 `pwsh scripts/gates.ps1`(06 §5.1 的 gate 1–8,另含十个子档:`3b` gitleaks、`3c` reuse lint + `check-spdx.ps1`、`3d` 设计盒真源、`3e` web smoke、`3f` 文档真源、`3g` IPC 契约文档对拍、`3h` 字体子集覆盖、`3i` 桥面/曲线/设计盒对拍、`3j` 隐私、`3k` 字体保留名)。子档只增不减,以 `gates.ps1` 的 `Set-Gate` 为准。
- 三个档位:`gates.ps1` 全量(含 gate 8 真机 GUI pluginval)/ `-PluginOnly` 跑 gate 1–7 / `-Quick` 跳过 pluginval(gate 7/8)做快速回环。JUCE 路径经 `-JucePath` 传入(或环境变量 `JUCE_PATH`)。
- **本地 gates 与 CI 的生成器不同,不是等价关系**([J96] 起):CI 是 `Ninja Multi-Config` + sccache,本地 gate 4 默认用 CMake 在 Windows 上的默认生成器(Visual Studio)。两者会在不同的地方红 —— `add_custom_command` 漏声明的隐式依赖(MSBuild 靠工程内顺序兜住、Ninja 并行到炸)、生成物时序、PCH 行为。以前无所谓,因为 push→`feature/**` 每次都在 CI 上编一遍;那条触发撤掉之后,**Ninja 侧的错第一次被看见就是出包前那次 dispatch**。所以:改到 `CMakeLists.txt` / 构建脚本 / 依赖的 PR,要么打 `ci:full` 标签让 CI 编一遍,要么在 Developer Command Prompt 里跑 `pwsh scripts/gates.ps1 -Generator "Ninja Multi-Config"` 自己先对一遍(该参数默认空 = 保持旧行为;**不做自动探测**,`ninja` 在 PATH 上但 shell 里没有 vcvars 时会把所有人的 gate 4 一起变红)。
- 子 PR 至少 `-PluginOnly`;feature→dev 收口 PR 必须全量。**[J96] 之后这条从「建议」变成「唯一的编译门」**:子 PR 不再跑 `build-vst3`,所以本地 gates 没跑过的编译错误在合进 `feature/v1` 之前没有任何机器会看见。
- 并行 agent 必须各用独立 git worktree 与 `-BuildDir`;GUI pluginval 全局串行。
- **锁纪律([SL-277]/[J96] 拆锁,2026-09-02)**:`gates.ps1` 自己在 gate 6/7/8 外面套一把命名互斥体 `Local\SCVB-ipc-tests`,gate 1–5 **完全不持锁**。
  - 拆锁的理由:要互斥的是**跨进程共享内存段**(段名前缀 `SynchainSCVB.v1.` 全机唯一,ctest 的 ipc 套件与 pluginval 都会开同名段),而 configure/build 只动各自的 `-BuildDir`。旧做法把整条 gates 包进一把外部目录锁,连 20 分钟的编译一起串行,四个 agent 排队等一个人编译。
  - **调用方不要再在 `gates.ps1` 外面套目录锁** —— 那会把刚拆开的编译重新串起来。手跑 `ctest` / `pluginval`(不经 gates.ps1)时才需要自备互斥。
  - 互斥体是内核对象,进程被 kill 或崩溃时**必然**释放:没有 owner 文件、没有孤儿判定、也没有「等超时后覆写别人的锁」这条路径。
  - **只用 `Local\`,不设 `Global\` 降级**:`Global\` 创建失败的现实原因是已存在的同名对象 DACL 拒绝当前 token(提权终端先建、普通终端拿不到),一旦降级就变成两个进程各持一把不同的锁 —— 「以为有锁,其实没有」,正是本卡要根除的那类。同一用户登录会话下的多个 agent 终端用 `Local\` 就够。建不出来时**判负**(汇总表里多一行 FAIL),绝不静默继续。gate 8 的 GUI 互斥体同档。
  - 逃生口 `-NoIpcLock` 只在确认本机没有第二个 agent 时用;关掉它并行跑出来的红大概率是抢段,不是回归。

## 3. 评审规则

- 处理完所有 comment,不止 bot 的(D2);子 PR 的 merge 由用户人工审核并亲自执行。

## 4. CI 三档与各 Workflow 触发范围

**CI 三档([SL-277]/[J96],2026-09-02 用户裁定;推翻 2026-08-19「所有 PR 跑完整 CI」)**

| 档  | 触发                                                  | 跑什么                                                                                                      |
| --- | ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| 轻  | 子 PR(base=`feature/**`)                             | `format`(clang-format / web-smoke / docs-truth)+ `compliance` + 三个 review bot。**不跑 `build-vst3`。**      |
| 中  | 里程碑 PR(base=`dev`)、push→`dev`                    | 上面全部 + `build-vst3` + `branch-gate`                                                                       |
| 重  | `workflow_dispatch`(input `ref`,默认 `feature/v1`)  | `build-vst3` 全量 —— **出包前硬门**                                                                           |

- **出包流程硬规:出包(打 tag / 发 `release`)之前必须对目标 ref 手动 dispatch 一次 `build-vst3` 并全绿。** 命令:`gh workflow run build-vst3.yml --repo synchain-oss/scvb --ref feature/v1 -f ref=feature/v1`,然后 `gh run list --workflow build-vst3.yml -L 1` 看结果。没有这一次绿,不允许出包 —— push→`feature/**` 触发已撤,主支线上再没有别的机器编译证据。要取包也走这一次:preview artifact(`SCVB-VST3-win64-preview-<slug>-<sha>`)就由这次 run 产出。
- **逃生口**:任何 PR 打上 `ci:full` 标签即照跑 `build-vst3` 全量(job 的 `if:` 判标签,`on.pull_request.types` 含 `labeled`/`unlabeled`)。子 PR 改到 `CMakeLists.txt` / 依赖 / workflow 本身时用它。
- **编译缓存**:`build-vst3` 用 `Ninja Multi-Config` + sccache(磁盘缓存 + `actions/cache` 跨 run 搬运)。换生成器不是审美选择 —— `CMAKE_<LANG>_COMPILER_LAUNCHER` 对 Visual Studio(MSBuild)生成器**无效**,不换就没有编译缓存。sccache 二进制**手动钉版下载**(`.sccache-version` + `.sccache-sha256` 两个单一真源),不走 marketplace action:org 的 action 白名单本来就不放行它,而且与 gitleaks 手动下载同一条纪律。每次 run 末尾打印 `sccache --show-stats`:缓存失效的形态是静默零命中(CI 照样绿,只是慢回改造前),不打印没人会发现。
- **org action 白名单**:本 org 只放行 GitHub 自家 action、`synchain-oss/*`,以及 `anthropics/claude-code-action@*` / `oven-sh/setup-bun@*` / `qodo-ai/pr-agent@*` / `softprops/action-gh-release@*`。**加任何别的第三方 action 都会让整个 run 直接 `startup_failure`** —— 没有 job、没有 check、没有日志,只有一句「workflow file issue」,很容易被误判成语法错。先考虑用 `run:` 步骤自己实现;确实需要新 action 时,由用户在 org 设置里放行后再用。
- `build-vst3`(job `build-and-validate`):pull_request→`dev` + `feature/**`(feature 侧只为 `ci:full` 逃生口留触发面,job `if:` 决定跑不跑)、push→`dev`、`workflow_dispatch`。`format` / `compliance`:pull_request→dev + `feature/**`,push→dev + `feature/**`(轻档照跑)。`branch-gate`(命名 + DCO + 冻结契约 path guard)仍仅 pull_request→dev。
- `compliance`(gitleaks + reuse lint + check-privacy + 设计盒真源):无 secrets,fork PR 同样跑。
- `claude-review`:所有 base 分支、**仅 same-repo**(J31);`deepseek-review` / `pr-agent` 默认 disable,同样仅 same-repo。
- `review-dispatch`:维护者评论 `/review` 显式触发 —— 这是 §0 铁律第 4 条**方案 D** 的实现,也是本仓目前给 fork PR 做 AI 审查的**默认通道**(铁律允许的另一条是方案 C 的 workflow_run 两阶段,本仓未实现)。
- `release`:push tags `v*` 触发草稿 Release(tag ↔ CMake VERSION 一致性门禁 + 打包)。
- 成本纪律:runner 就低不就高;按量计费的 review bot 克制使用,不设为 required。

## 5. 冻结契约变更规范

**四份**冻结文档(`docs/PARAMETERS.md` / `docs/IPC_CONTRACT.md` / `docs/STATE_SCHEMA.md` / `docs/SCVB_CONTRACT.md`)与 `tests/golden/` 的任何改动都必须:① 先获用户明确批准;② 用 `docs/contract-changes/TEMPLATE.md` 写一份变更文档到 `docs/contract-changes/<YYYYMMDD>-<slug>.md`,**与改动放在同一个 PR 里**;③ 挂 `status/frozen-contract` 标签。机器强制 = `branch-gate` 的「冻结契约 path guard」——它逐字检这五个路径,缺变更文档即 exit 1(`.github/workflows/branch-gate.yml`)。

`docs/constitution/` 下三份宪法原文是**只读副本**,只能随 `masterPlan/constitution/` 同步,不得就地编辑;`pwsh scripts/check-constitution-sync.ps1` 比对 sha256。

## 6. 环境与依赖

- JUCE(版本见 `.juce-version`)、CMake ≥3.22、MSVC 2022(静态 CRT `/MT`)、WebView2 SDK(NuGet,版本常量在 `CMakeLists.txt` 的 `WEBVIEW2_VERSION`)+ WebView2 Evergreen Runtime、pluginval(版本见 `.pluginval-version`)、Catch2(仅测试目标,由 `tests/CMakeLists.txt` 的 FetchContent 钉版拉取)、clang-format 18.1.8(J38 钉死)、gitleaks(版本见 `.gitleaks-version`)、`reuse`(pipx)。
- **可选依赖:无头 Chrome / Edge**(T46 起)。`web-preview/tests/smoke-*-page.mjs` 这几套(经 CDP 驱动真页面:Monitor 投影面、Output 过期提示面、Output 分布图补间面)用得到,**没装不算失败** —— 该套回退出码 **2**,`scripts/gates.ps1` 的 Gate 3e 与 CI 的 web-smoke job 都把 2 记成 SKIP / `::warning::` 而不判红(理由写在那两处的注释里:web-smoke 是 required check,为一个可选依赖判红会卡住仓库里每一个 PR)。装了才跑得到「页面真的执行起来」那一层断言 —— node 侧的其余各套都不执行页面 JS,建议装。
- **版本单一真源纪律**:`.juce-version` / `.pluginval-version` / `.gitleaks-version` 三个文件是各自版本的唯一真源,workflow 与 `scripts/*.ps1` **一律从文件读**,不得写版本号字面量。
- 构建流水线不需要任何 secret(06 §3.1);review bot 用 org secrets(`CLAUDE_CODE_OAUTH_TOKEN` / `DEEPSEEK_KEY`)。

## 7. 冻结契约铁律

1. `docs/PARAMETERS.md` —— **123 个**自动化参数(我方声明数;宿主可见 **124**,JUCE VST3 wrapper 会自动合成一个 bypass —— 一切余量计算按 124 口径,Ableton Live 的 128 上限只剩 4,**预算封顶**)的 ParamID / index / 顺序 / skew 永久冻结。禁止:增删参数、改 ParamID 字符串、改 index 顺序、改 versionHint。允许:改显示名、改默认值(需变更文档 + CHANGELOG 醒目标注)。新需求一律走 state(非自动化)。
2. `docs/IPC_CONTRACT.md` —— 段名前缀 `SynchainSCVB.v1.` 与每个 header 前两字段(magic/abi)永久冻结。布局改动 → `abi+1` 且段名升 v2,新旧不互认。所有跨进程字段 `std::atomic` + `static_assert(is_always_lock_free)` + offsetof 布局校验;缓存行 64B 对齐。
3. `docs/STATE_SCHEMA.md` —— state chunk 带 `abi` 字段。读到高版本 → 拒载并提示升级(不得静默丢数据);读到低版本 → 走迁移函数升格。任何 schema 变更必须同时提供迁移函数与其单测。`setStateInformation` 处理不可信字节:长度/范围字段必须先校验再用于分配或索引。
4. `docs/SCVB_CONTRACT.md` —— JS↔C++ 桥契约(v1.0 已冻结)。函数名 / 签名形态 / 事件名 / 载荷字段只增不改:新增走 §9 的变更流程,**不得改名、不得改既有字段语义、不得删**。机器门禁 = `node scripts/check-bridge-parity.mjs`(比对 C++ 常量表 / mock 后端 / 契约文档三者的名字集合);桥函数不得由实施卡单方发明,必须先进契约。

## 8. 实时线程规则

`processBlock` / 音频回调内禁止:堆分配/释放、任何锁、文件/网络/日志 I-O、抛/捕获异常、调用 MessageManager / beginChangeGesture 系列、任何阻塞等待。允许:预分配缓冲上的定长运算、`juce::ScopedNoDenormals`、无锁 SPSC 环(acquire/release)、`std::atomic`(必须 `is_always_lock_free`)。gesture 三段式只在消息线程(25Hz Timer);音频线程只发布 playhead 快照(SPSC)。段间过渡走 ramp(默认 80ms,ADR-010);失准语义:缺口→该轨该块静音+警告计数,绝不静默出错。

## 9. 双 target 共享代码规范

- 任何被 Input 与 Output 同时使用的代码,必须落在 `src/core/`(scvb_core 静态库)。
- `src/input/` 与 `src/output/` 之间禁止互相 `#include`。
- scvb_core 不得链接 `juce_audio_plugin_client`,不得引用 `AudioProcessor`;必须能离线 Catch2 单测。
- 配置类数据的唯一真源在 Output state(ADR-004);Input 端不得自建副本作为真源。

## 10. UI / WebView 规范

web 资源经 `juce_add_binary_data` 嵌入;JS↔C++ 契约真源 = `docs/SCVB_CONTRACT.md` + `src/input/InputBridgeApi.h` / `src/output/OutputBridgeApi.h`(**不存在** `WEB_UI_CONTRACT.md` 或 `ScvbApi.h`,见到这两个名字一律按笔误处理,不要据此再造第二份桥契约);缩放走「web 固定设计盒 + CSS zoom + setSize 同步 + 10 秒确认防呆」;`web-preview/` 的 mock 桥必须与真桥同契约,契约改动要同时改两侧。

**用户可见硬约束文案的单一真源**:九条硬约束要落到 markdown ×4(`docs/USER_GUIDE.md` / `docs/USER_GUIDE.zh-CN.md` / 两份 README 的 Quick start)+ i18n ×3(`web/shared/i18n.js` 的 `guide.rule1..9`,zh/en/fr)共 **7 处**。唯一真源 = `docs/USER_GUIDE.zh-CN.md` 的 `#硬约束` 小节,另外 6 处由 `node scripts/gen-hard-rules.mjs` **生成**。**禁止在任何位置手抄**;提交前跑 `node scripts/gen-hard-rules.mjs --check`(哈希比对,不一致即非零退出)与 `node scripts/check-i18n.mjs`(三语 key 全等)。UI 引导页只负责展示与「不再显示」持久化,不得自己写条目文本。
