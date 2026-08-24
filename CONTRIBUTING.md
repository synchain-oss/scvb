# Contributing to SCVB

感谢你的关注。SCVB(Synchain Vocal Balancer)是一对协同工作的 VST3 插件:**SCVB Input** 装在每条人声轨、**SCVB Output** 装在人声总线,两者经共享内存完成多轨人声的自动声像与电平平衡。
issue 与 PR 都欢迎。开工之前请把本文读完 —— 尤其是 **§8 冻结契约**,那一节列的改动无论质量多高都会被直接关闭。

新贡献者从零搭环境到构建出两个 `.vst3`,请走 [docs/CONTRIBUTOR_ONBOARDING.md](./docs/CONTRIBUTOR_ONBOARDING.md)。

## 0. 语言政策

- issue / PR 可以用**中文或英文**;维护者用你使用的语言回复。维护者内部沟通用中文。
- **用户可见的硬约束(九条硬约束)以中文版为语义权威**:中英文各一份,两版出现歧义时以中文版为准。理由是 SCVB 的路由约束属于「读错就出静音或错音」的硬约束,而核心用户是中文 DAW 用户 —— 中文版必须是完整镜像,不是摘要。
- **标题文案统一写「九条」**(实际条目数就是 9 条)。早期草稿里出现过别的数量词,任何非「九条」的写法都是错的 —— **UI 侧**有禁词机检把关(`web-preview/tests/smoke-tab1-interactions.mjs` 扫 `web/shared/i18n.js` 的 zh/en/fr 全部词条值 + 三个 `web/output/` 源文件,数量词写错即红);**markdown 侧目前靠评审时的 grep**,不要指望 CI 替你兜住。
- **九条硬约束只有一个真源,禁止在任何位置手抄。** 真源 = `docs/USER_GUIDE.zh-CN.md` 的 `#硬约束` 小节;另外 6 处落地面(`docs/USER_GUIDE.md` 的 EN 块、两份 README 的 Quick start 块、`web/shared/i18n.js` 的 `guide.rule1..9` zh/en/fr 三语 key)全部由 `node scripts/gen-hard-rules.mjs` 生成。要改条目文本,**只能改真源再跑生成器**。提交前必须跑:

  ```powershell
  node scripts/gen-hard-rules.mjs --check   # 哈希比对 7 处落地面,不一致即非零退出
  node scripts/check-i18n.mjs               # zh/en/fr 三语 key 集合全等
  ```

- **插槽措辞**:统一写「**人声轨插件链最后一格**」。**不要用推子相关的插槽说法**(ADR-002 v1.2 / J45 已把旧措辞修宪掉:多数宿主根本没有 fader slot 这个概念,用户按字面找不到)。它和上一条共用同一份 UI 侧禁词机检(markdown 侧同样靠评审 grep)。逐宿主的具体位置说法归 `docs/DAW_COMPATIBILITY.md`。
- 法语(fr)只覆盖插件 UI 文案,不覆盖 README 与用户手册;**fr 的九条硬约束必须经人工审校后才能发布**,不接受未审校的机翻直接进产品。

## 1. 行为准则

参与本项目即视为同意 [CODE_OF_CONDUCT.md](./CODE_OF_CONDUCT.md)(Contributor Covenant v2.1;中文译本见 [CODE_OF_CONDUCT.zh-CN.md](./CODE_OF_CONDUCT.zh-CN.md),**英文版为准**)。举报渠道:**contact@synchain.ca**。

## 2. 开发者原创声明(DCO)

每个 commit 必须带 `Signed-off-by:` 尾行:

```
git commit -s
```

本项目采用 DCO,**不采用 CLA**(不需要额外基础设施,GPLv3 项目同样适用)。这条由 `branch-gate` 的 DCO step 机器强制(内联 `gh api` 断言,不引第三方 action —— org 的 allowed_actions 是白名单,会拦下第三方 DCO action)。忘签的补救:

```
git commit -s --amend     # 最后一个 commit
git rebase --signoff      # 一段区间
```

## 3. 分支模型

默认主干是 `dev`;**没有 stage / prod 分支**。SCVB 的主支线是 `feature/v1`(ADR-013 / J13)。

- **内部贡献者**:从 `feature/v1` 开 `feat/<TASK-ID>-<slug>` 子支线,PR 回 `feature/v1`。一张卡一条子支线一个 PR。same-repo 提到 `dev` 的 PR 只接受 `feat/*` / `feature/*`(外加 `dependabot/*`)。
- **外部贡献者(J31 / J41)**:fork 本仓 → 用**任意分支名**(请不要用 `dev` / `stage` / `prod` / `feature/v1` / `feature/extraction`)→ PR 到 `dev`。

**fork PR 的门禁政策**(与 [CLAUDE.md](./CLAUDE.md) §1 同一份,不存在第二套):

| 项 | fork PR 的行为 |
| --- | --- |
| `branch-gate` 的**命名规则** | 对 fork **不 exit 1**。只断言 head 分支名不在上述长期分支名集合(防同名伪装晋升),**不强制 `feat/*` 命名** —— 命名规则仅约束 same-repo 分支 |
| `branch-gate` 的**另两条断言** | **对 fork 照常生效**:DCO(每个 commit 要有 `Signed-off-by:`)与冻结契约 path guard(见 §8)任一不过,`branch-gate` 同样红。豁免的只有命名规则那一条 |
| 构建 / 测试 | 只跑**无 secrets** 的 job(`build-vst3` / `format` / `branch-gate` / `compliance`),且需维护者批准 workflow run 后才开始跑 |
| AI review bot | `claude-review` / `deepseek-review` / `pr-agent` 都带 same-repo 条件,**fork PR 一律不自动跑** |
| `external` label | 由**维护者手工添加**(fork PR 的 `GITHUB_TOKEN` 只读,workflow 内加不了标签) |
| fork PR 的 AI 审查 | 只走两条合规路:**方案 D** = 维护者评论 `/review` 显式触发(本仓的默认实现,`.github/workflows/review-dispatch.yml`);**方案 C** = workflow_run 两阶段、全程不 checkout PR 代码(本仓未实现,保留为合规替代)。见 [CLAUDE.md](./CLAUDE.md) §0 铁律第 4 条 |

> 本项目**任何 workflow 都不使用 `pull_request_target`**,这是一条不接受个案豁免的安全禁令(J20)。请不要提交「加个 `pull_request_target` 就能给 fork PR 跑 bot 了」的 PR。

大改动请先开 issue 讨论,不要直接写好一大段再来对齐设计。

## 4. Commit 规范

`type(scope): 描述`,描述可中可英。type 取值:`fix` / `feat` / `docs` / `chore` / `refactor` / `test` / `ci` / `style` / `perf` / `revert` / `harden`。

## 5. 环境搭建

完整的从零安装步骤(含下载链接与版本)见 [docs/CONTRIBUTOR_ONBOARDING.md](./docs/CONTRIBUTOR_ONBOARDING.md);工具链清单的单一真源是 [CLAUDE.md](./CLAUDE.md) §6。要点:Windows 10/11 x64 + Visual Studio 2022(Desktop C++ 工作负载)、CMake ≥ 3.22、MSVC 静态 CRT `/MT`、WebView2 SDK(NuGet,配置时自动拉)+ Evergreen Runtime、JUCE(版本见 `.juce-version`)、pluginval(版本见 `.pluginval-version`)、clang-format 18.1.8。

## 6. 提 PR 前的本地 gates

命令清单的单一真源是 [CLAUDE.md](./CLAUDE.md) §2,本节只是转述:

```powershell
pwsh scripts/gates.ps1 -JucePath C:\path\to\JUCE                # 全量(含 gate 8 真机 GUI pluginval)
pwsh scripts/gates.ps1 -PluginOnly -JucePath C:\path\to\JUCE     # gate 1-7,与 CI 等价
pwsh scripts/gates.ps1 -Quick -BuildDir build-mine               # 跳过 pluginval,快速回环
```

覆盖面:依赖预检 / clang-format / prettier / gitleaks / `reuse lint` + SPDX 头 / 设计盒真源 / web smoke / CMake 配置 / 构建(零 warning)/ ctest / pluginval 非 GUI / pluginval 全量含 GUI。

**含 GUI 的全量校验只能在本地真机做** —— CI 的 runner 是无头的,跑不了 WebView2 编辑器,所以 PR 模板里那一项勾选有实际意义,请不要不跑就勾。并行开工时每个人各用独立的 git worktree 与 `-BuildDir`;GUI pluginval 全局串行。

只改文档时,请至少跑一次死链检查:

```powershell
npx --yes markdown-link-check -c .markdown-link-check.json -q CONTRIBUTING.md docs/*.md
```

配置必须用 `-c` 显式传 —— markdown-link-check 的 **CLI 不会自动发现** `.markdown-link-check.json`(自动读配置的是它的 GitHub Action wrapper);而且不给文件参数它会转去读 stdin,看起来就是卡住。

## 7. 评审流程与期望响应时间

- 维护者会在几个工作日内首次响应。issue 优先级:P0 24 小时 / P1 3 个工作日 / P2 7 天。
- **处理完所有 comment,不止 bot 的。** bot 的意见不必照单全收,但必须回应。
- 子 PR 的 merge 由维护者人工审核并亲自执行,不用 auto-merge。
- 安全问题**不要**开公开 issue,走 [SECURITY.md](./SECURITY.md)。

## 8. ★ 冻结契约:哪些 PR 一定不会被接受

这四条不是风格偏好,是「一旦发布就固化在用户工程文件里」的破坏性变更:

- **不接受新增 / 删除自动化参数的 PR。** 自动化参数永久冻结为 **123** 个(我方声明数;宿主可见 124 —— JUCE 的 VST3 wrapper 会自动合成一个 bypass)。原因是 Ableton Live 的 128 参数上限余量必须守住(只剩 4,预算封顶),且 Logic 按索引识别参数,增删会破坏所有既有工程。新需求一律走 state(非自动化)。
- **不接受修改 ParamID 字符串或参数 index 顺序的 PR。** VST3 参数 ID 由 JUCE 从 ParamID 做 hash,首个 release 之后修改 = 用户工程里的自动化全部错位。
- **不接受在不升 `abi` 的前提下修改 IPC 共享内存布局的 PR。** 布局改动必须 `abi+1` 且段名升到 `v2`,新旧不互认。段名前缀 `SynchainSCVB.v1.` 与各 header 前两字段(magic / abi)永久冻结。
- **不接受在 `processBlock` 中引入内存分配 / 加锁 / I-O / 日志 / 异常的 PR。** 见 [CLAUDE.md](./CLAUDE.md) §8 的完整禁止与允许清单。

四份冻结文档(`docs/PARAMETERS.md` / `docs/IPC_CONTRACT.md` / `docs/STATE_SCHEMA.md` / `docs/SCVB_CONTRACT.md`)与 `tests/golden/` 由 `branch-gate` 的「冻结契约 path guard」看守:碰了它们而同一个 PR 里没有对应的 `docs/contract-changes/<YYYYMMDD>-<slug>.md` 变更文档(模板见 `docs/contract-changes/TEMPLATE.md`),`branch-gate` 直接红 —— 这条对 same-repo 与 fork PR **一视同仁**,fork 豁免的只有分支命名规则,不豁免本条。

> **这道机器守卫只覆盖 base = `dev` 的 PR**(`branch-gate` 的触发面就是 `pull_request: branches: [dev]`)。base 为 `feature/v1` 的子 PR **不触发 branch-gate**,那一层靠评审与 CODEOWNERS 把关 —— 所以在子 PR 上碰冻结契约同样要自觉补变更文档,别等 CI 提醒你。

`docs/constitution/` 下是宪法原文的**只读副本**,只能随上游同步(`pwsh scripts/check-constitution-sync.ps1` 比对 sha256),不要就地编辑。

## 9. 发布流程(仅维护者)

版本号真源 = 顶层 `CMakeLists.txt` 的 `project(SCVB VERSION ...)`。打 `vX.Y.Z` tag 触发 `release.yml`:版本一致性门禁 → 构建 → pluginval → 两个插件打一个 zip + `.sha256` → 草稿 Release。完整 runbook 见 `docs/RELEASE.md`。
