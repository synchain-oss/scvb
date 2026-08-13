# Contributing to SCVB

0. **语言政策** —— issue/PR 接受中文或英文;维护者内部沟通用中文;红字约束条目以中文版为语义权威。
1. **行为准则** —— 见 [CODE_OF_CONDUCT.md](./CODE_OF_CONDUCT.md)。
2. **开发者原创声明(DCO)** —— 每个 commit 必须带 `Signed-off-by:`(`git commit -s`);不采用 CLA。由 branch-gate 的 DCO step 机器强制。补救:`git commit -s --amend` 或 `git rebase --signoff`。
3. **分支模型** —— 内部按 ADR-013(主支线 `feature/v1`)。外部贡献者:fork → 任意分支名(不用 `dev`/`stage`/`prod`/`feature/v1`/`feature/extraction`)→ PR 到 `dev` → 维护者手工加 `external` label(J31)。
4. **Commit 规范** —— `type(scope): 描述`;type ∈ {fix, feat, docs, chore, refactor, test, ci, style, perf, revert, harden}。
5. **环境搭建** —— 见 [CLAUDE.md](./CLAUDE.md) §6。
6. **提 PR 前的本地 gates** —— 与 CLAUDE.md §2 同一份命令清单:`pwsh scripts/gates.ps1`。
7. **评审流程与期望响应时间** —— 见 [SECURITY.md](./SECURITY.md) 与 issue 优先级标签(P0 24h / P1 3 工作日 / P2 7 天)。
8. **★ 冻结契约:哪些 PR 一定不会被接受**
    - 不接受新增/删除自动化参数的 PR。自动化参数永久冻结为 **123** 个(params-v0 §一、ADR-004),Ableton Live 128 参数上限余量必须守住。新需求一律走 state(非自动化)。
    - 不接受修改 ParamID 字符串或参数 index 顺序的 PR。VST3 参数 ID 由 JUCE 从 ParamID 做 hash,首个 release 后修改 = 用户工程里的自动化全部错位。
    - 不接受在不升 `abi` 的前提下修改 IPC 共享内存布局的 PR。布局改动必须 `abi+1` 且段名升到 `v2`(ipc-contract-v0 §5)。
    - 不接受在 `processBlock` 中引入内存分配/加锁/IO/日志/异常的 PR(见 CLAUDE.md §8)。
9. **发布流程(仅维护者)** —— 见 docs/RELEASE.md(随 release.yml 由 T40 补齐)。
