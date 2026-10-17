# 发布前验证清单 —— v0.10.0-beta.1

> 状态:**进行中** —— 用户 2026-10-16 批准发布 `v0.10.0-beta.1`。**本版先只发 Windows**:CI 建出的草稿 Release 里会带 macOS 三件,发布前撤下留存;mac 三件在 LUNA 真机验收通过后补传到同一个 Release([luna-acceptance.md](luna-acceptance.md))。
> 模板:[release-checklist-v0.9.0-rc.1.md](release-checklist-v0.9.0-rc.1.md)。只保留本版适用的项;rc.1 那份的逐项长证据不照搬,本版证据只写在核对基线上实际看过的。
> 来源:`docs/RELEASE.md` 发版清单(J184 链)。
> 相关:上机矩阵 [daw-matrix.md](daw-matrix.md) · 支持等级 [daw-support-tiers.md](daw-support-tiers.md) · null test 记录 [audio/nulltest-log.md](audio/nulltest-log.md)

## 0. 怎么用

- **版本路线**:`CMakeLists.txt` 改为 `0.10.0`,打 tag `v0.10.0-beta.1`(beta 与正式版共用同一个 `X.Y.Z`)。
- 每项状态:**✅ 已核**(证据写在项里)/ **⏳ CI**(等 tag 那一跑)/ **⏳ 统筹** / **⏳ 用户** / **N/A + 理由**。只有 ✅ 与 N/A 勾 `[x]`,⏳ 一律 `[ ]`。**留空的项不得当作通过**。
- **mac 的口径**:本版只发 Windows。mac 相关项统一标「本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release」;mac 的 CI job(`build-macos.yml`)在 tag 那一跑照常出包,但包在发布前撤下,不进本版 Release 资产。
- 打 tag 前,统筹在 tag 所指提交上把全部 ✅ 项的「怎么查」重跑一遍,结果变了就改回 ⏳。

## 1. Windows 侧已有证据

| 证据 | 内容 |
| --- | --- |
| 用户上机 | 2026-10-16 在 Cubase 上测试包 `7ae5713`,走 W0–W8,**W5 未测** |
| 组合 CI | run 38708566079,Windows 与 macOS 两平台全绿 |
| 导出 null test | 重开工程前后离线导出相差 1 LSB;实时导出对离线导出的残差 RMS 约 −60 dBFS |

以上三条是用户报告 / CI 页面上的结果,本清单没有重跑;下面各项引用它们时只算「该项的参考证据」,按项另有「怎么查」。

## 2. 清单

### A. 代码与版本

- [ ] **A1 版本号与 tag 一致**
  - 怎么查:`git show v0.10.0-beta.1:CMakeLists.txt | grep -n "project(SCVB"` 应为 `VERSION 0.10.0`;`release.yml` 的 `verify-tag` job 绿(`scripts/check-release-tag.ps1` 已接受 `-beta.N`)。
  - 证据:(PR 分支)`CMakeLists.txt` 为 `project(SCVB VERSION 0.10.0)`。tag 那一跑:______
  - 状态:⏳ CI

- [ ] **A2 CHANGELOG 已更新**
  - 怎么查:`## [Unreleased]` 已归入 `## [0.10.0-beta.1] - <日期>`,底部对比链接补齐;`node scripts/check-changelog-drafts.mjs` 退出 0;契约变更条目各有 `docs/contract-changes/` 文档、每条带 PR 号。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **A3 带 `status/frozen-contract` 标签的 PR 均经用户显式批准**
  - 怎么查:`gh pr list -R synchain-oss/scvb --state all --label status/frozen-contract`,自 `v0.9.0-rc.1` 之后新合并的逐个对到用户裁定。(rc.1 已发布,本项不再适用 rc.1 的「首发无可破坏」例外。)
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **A4 git 历史无 secret**
  - 怎么查:`gh api repos/synchain-oss/scvb -q .security_and_analysis`;`gh api "repos/synchain-oss/scvb/secret-scanning/alerts?state=open" -q length`;每个 PR 的 `compliance` job 另跑 gitleaks。
  - 证据:______
  - 状态:⏳ 统筹

### B. 自动化门禁

- [ ] **B1 CI `build-and-validate`(Windows)绿**
  - 怎么查:tag 所指提交的 `release.yml` → `build` job 全绿:`100% tests passed`、pluginval strictness 5 三个 bundle 各 `SUCCESS`、`warning C` 零命中。
  - 参考证据:组合 CI run 38708566079 Windows 侧绿(不是 tag 提交)。tag 那一跑:______
  - 状态:⏳ CI

- [ ] **B2 CI 格式 / 文档 / 合规门禁绿**
  - 怎么查:tag 所指提交上 `format`(clang-format、docs-truth、prettier)、`compliance`(gitleaks、reuse)、`web-smoke`、三条宪法守卫全绿。
  - 证据:______
  - 状态:⏳ CI

- [ ] **B3 本地全量 `pwsh scripts/gates.ps1`(含真机 GUI pluginval)全绿**
  - 怎么查:发版清单第 4 步;并按 `CLAUDE.md` 的出包硬规对目标 ref dispatch 一次 `build-vst3` 并全绿。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **B4 mac job(`build-macos.yml`)在 tag 那一跑的结果**
  - 本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release;**mac job 红不挡本版 Windows 发布**,但结果要如实记下。
  - 参考证据:组合 CI run 38708566079 macOS 侧绿。tag 那一跑:______
  - 状态:⏳ CI(不挡)

### C. UI 与无障碍

- [ ] **C1 web 冒烟全绿、i18n 无硬编码字面量**
  - 怎么查:tag 提交上 `web-smoke` job 与 `node scripts/check-i18n.mjs`。
  - 证据:______
  - 状态:⏳ CI

### D. 音质

- [ ] **D1 导出 null test**
  - 怎么查:`audio/nulltest-log.md` 追加本版一条(D5)。
  - 证据:用户 2026-10-16 在包 `7ae5713` 上的结果:重开工程前后离线导出相差 1 LSB;实时导出对离线导出的残差 RMS 约 −60 dBFS。**这是用户口头 / 测试记录给出的数字,逐字比对与入库由统筹做**;若发布包与 `7ae5713` 之间有影响音频路径的改动,要重跑。
  - 状态:⏳ 统筹(数字已有,记录入库待做)

- [ ] **D2 PANLAW-1..5 绿**
  - 怎么查:`ctest` 里对应用例(含在 B1 的 `100% tests passed` 内)。
  - 证据:______
  - 状态:⏳ CI

- [ ] **D3 结果追加到 `audio/nulltest-log.md`**
  - 证据:______
  - 状态:⏳ 统筹

### E. 性能

- [ ] **E1 `scvb_bench --dsp` 对比 v0.9.0-rc.1 无 >10% 退化**
  - 怎么查:`perf/budget-log.md` 的 PERF-1..4;宿主性能 PERF-5..7 与内存 MEM-1..7 本版不新跑,沿用 rc.1 的口径,**若自 rc.1 之后动过 DSP / 读写环 / 调度路径,要重跑**。
  - 证据:______
  - 状态:⏳ 统筹

### F. DAW 矩阵

- [ ] **F1 Windows 上机矩阵**
  - 怎么查:`daw-matrix.md` 新增本版一节。
  - 证据:用户 2026-10-16 在 Cubase 上测试包 `7ae5713`,W0–W8 跑完,**W5 未测**(未测的不算过;W5 的处置由统筹向用户确认:补测,或写 N/A 与理由)。其他 Windows 宿主本版未测。
  - 状态:⏳ 用户(W5)

- [ ] **F2 `daw-support-tiers.md` 与 README 的 Tier 表同步**
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **F3 mac 宿主(LUNA 验收)**
  - N/A(本版):本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release;mac 两行 Tier 3 保持「待验证」,验收结果由统筹脱敏回填后再补传。
  - 状态:N/A

### G. 文档

- [ ] **G1 文档对等与红字真源**
  - 怎么查:`pwsh scripts/check-readme-parity.ps1`、`node scripts/gen-hard-rules.mjs --check` 退出 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 为 `reviewed`(fr 红字未经审校不得发版)。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **G2 README / USER_GUIDE 写明 macOS 包的发布节奏**
  - 怎么查:四份文档(`README.md`、`README.zh-CN.md`、`docs/USER_GUIDE.md`、`docs/USER_GUIDE.zh-CN.md`)各有一句同义的话:「macOS beta 包在 LUNA 真机验收通过后才补传到 Release;在那之前 Releases 页上只有 Windows 包。」
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **G3 已知限制清单在 README 可见,`KNOWN_ISSUES.md` 与本版行为一致**
  - 证据:______
  - 状态:⏳ 统筹

### H. 许可与合规(GPLv3)

- [ ] **H1 zip 内合规文件组断言通过**
  - 怎么查:`release.yml` 的 `release` job 里 `scripts/package.ps1` 的解包断言(`LICENSE`、`THIRD-PARTY-NOTICES.md`、`third_party/notices/` 等,见 `docs/RELEASE.md`「zip 内必须携带的合规文件组」)。
  - 证据:______
  - 状态:⏳ CI

- [ ] **H2 第三方组件清单、GPLv3 源码获取途径、无不兼容许可资源**
  - 证据:______
  - 状态:⏳ 统筹

### J. 打包与发布

- [ ] **J1 `SCVB-v0.10.0-beta.1-win64.zip` 内含三个 `.vst3` bundle**
  - 怎么查:`package.ps1` 打包后重开 zip 的断言;再下载草稿 Release 的 zip 看一眼;核对 `package-summary.md` 的 SHA-256 三处一致。
  - 证据:tag 那一跑:______
  - 状态:⏳ CI

- [ ] **J2 草稿 Release 里的 macOS 三件已撤下并留存**
  - 怎么查:草稿 Release 的资产列表里没有 `SCVB-v0.10.0-beta.1-macos-arm64.*`(zip、`.sha256`、`package-summary-macos.md`);三件已下载另存,SHA-256 记在 [luna-acceptance.md](luna-acceptance.md) 的验收记录里,待 LUNA 验收后补传到同一 Release。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J3 发布说明按 `docs/RELEASE.md` 模板填写,写明本版只有 Windows 包**
  - 怎么查:正文含升级须知、回退步骤、SmartScreen 说明、SHA-256(从 `package-summary.md` 复制)、「macOS beta 包在 LUNA 真机验收通过后补传」一句。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J4 GitHub Release 建为草稿,人工核对后发布为 pre-release**
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J5 tag 保护生效(`v*`)**
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J6 干净 Windows 11 上解压安装,DAW 扫得到并正常工作**
  - 参考证据:用户 2026-10-16 已在 Cubase 上装并测过测试包 `7ae5713`(不是发布包,也不是干净机)。
  - 状态:⏳ 用户(是否要求干净机,由统筹向用户确认;不确认则写 N/A 与理由)

### K. 回滚预案

- [ ] **K1 上一 release(`v0.9.0-rc.1`)的 zip 仍可下载,发布说明写明如何回退**
  - 证据:______
  - 状态:⏳ 统筹

### L. 本版追加

- [ ] **L1 `staging` / `prod` 分支按 `docs/RELEASE.md` 前移**
  - 怎么查:发版清单第 9 步:beta 同 rc 一样只前移 `staging`,不前移 `prod`(以 `docs/RELEASE.md` 现行文字为准)。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **L2 `.markdown-link-check.json` 里 CHANGELOG 两条对比链接的临时放行已删**
  - 怎么查:tag 推上去之后,删掉为 `[Unreleased]` / `[0.10.0-beta.1]` 两个对比地址加的临时放行,再跑 `npx -y markdown-link-check -c .markdown-link-check.json CHANGELOG.md`。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **L3 打 tag 前在 tag 提交上重做发布盘点与漏项全扫**
  - 证据:______
  - 状态:⏳ 统筹

## 3. 汇总

截至本文件初稿(发版准备卡 M20):共 31 项,**全部未勾**。除 F3 为 N/A 外,其余均待 CI、统筹或用户;「证据」留空的格子表示还没核,不是通过。

| 状态 | 项数 |
| --- | --- |
| ✅ 已核 | 0 |
| N/A | 1(F3) |
| ⏳ CI | 8(A1、B1、B2、B4、C1、D2、H1、J1) |
| ⏳ 统筹 | 20 |
| ⏳ 用户 | 2(F1、J6) |

(合计 31:1 + 8 + 20 + 2。F3 的 N/A 在本版也留着未勾框,以免被当成已通过;统筹在 tag 前决定是否勾上。)
