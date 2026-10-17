# 发布前验证清单 —— v0.10.0-beta.1

> 状态:**进行中** —— 用户 2026-10-16 批准发布 `v0.10.0-beta.1`。**本版先只发 Windows**:CI 建出的草稿 Release 里会带 macOS 三件,发布前撤下留存;mac 三件在 LUNA 真机验收通过后补传到同一个 Release([luna-acceptance.md](luna-acceptance.md))。
> 模板:[release-checklist-v0.9.0-rc.1.md](release-checklist-v0.9.0-rc.1.md)。只保留本版适用的项;rc.1 那份的逐项长证据不照搬,本版证据只写在核对基线上实际看过的。
> 来源:`docs/RELEASE.md` 发版清单(J184 链)。
> 最后更新:v0.10.0-beta.1 已于 2026-10-17 发布;本轮回填依据是发布后回读的事实——tag 与 prod / staging 的提交关系、`release.yml` run 39008918499 的三个 job 与草稿资产、本机 GUI pluginval、发布与匿名下载回读、tag 保护规则集、#410 主线记回。表里没有对上事实的项保持原样。
> 相关:上机矩阵 [daw-matrix.md](daw-matrix.md) · 支持等级 [daw-support-tiers.md](daw-support-tiers.md) · null test 记录 [audio/nulltest-log.md](audio/nulltest-log.md)

## 0. 怎么用

- **版本路线**:`CMakeLists.txt` 改为 `0.10.0`,打 tag `v0.10.0-beta.1`(beta 与正式版共用同一个 `X.Y.Z`)。
- 每项状态:**✅ 已核**(证据写在项里)/ **⏳ CI**(等 tag 那一跑)/ **⏳ 统筹** / **⏳ 用户** / **N/A + 理由**。只有 ✅ 与 N/A 勾 `[x]`,⏳ 一律 `[ ]`。**留空的项不得当作通过**。
- **mac 的口径**:本版只发 Windows。mac 相关项统一标「本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release」;mac 的 CI job(`build-macos.yml`)在 tag 那一跑照常出包,但包在发布前撤下,不进本版 Release 资产。
- 打 tag 前,统筹在 tag 所指提交上把全部 ✅ 项的「怎么查」重跑一遍,结果变了就改回 ⏳。

## 1. Windows 侧已有证据

| 证据 | 内容 |
| --- | --- |
| 用户上机 | 2026-10-16 在 Cubase 上测试包 `7ae5713`,W0–W4、W6–W8 通过,**W5 未测** |
| 组合 CI | run 38708566079,Windows 与 macOS 两平台全绿 |
| 导出 null test | 重开工程前后离线导出相差 1 LSB;实时导出对离线导出的残差 RMS 约 −60 dBFS |

以上三条是用户报告 / CI 页面上的结果,本清单没有重跑;下面各项引用它们时只算「该项的参考证据」,按项另有「怎么查」。

## 2. 清单

### A. 代码与版本

- [x] **A1 版本号与 tag 一致**
  - 怎么查:`git show v0.10.0-beta.1:CMakeLists.txt | grep -n "project(SCVB"` 应为 `VERSION 0.10.0`;`release.yml` 的 `verify-tag` job 绿(`scripts/check-release-tag.ps1` 已接受 `-beta.N`)。
  - 证据:(PR 分支)`CMakeLists.txt` 为 `project(SCVB VERSION 0.10.0)`。tag 那一跑:`release.yml` run 39008918499 的 `verify-tag` job success。tag `v0.10.0-beta.1` → `bc04686a3634e0b58169ec85a6ef2144b7c1d564`(prod 经 #409 的合并提交 P),CMake 版本 0.10.0。
  - 状态:✅ 已核

- [ ] **A2 CHANGELOG 已更新**
  - 怎么查:`## [Unreleased]` 已归入 `## [0.10.0-beta.1] - <日期>`,底部对比链接补齐;`node scripts/check-changelog-drafts.mjs` 退出 0;契约变更条目各有 `docs/contract-changes/` 文档、每条带 PR 号。
  - 证据:里程碑合并前的五项核对都过(diff-exit=0、1 个提交、带签名、Landed-PRs 尾注一致、drafts-exit=0),合入 dev 后回读同样通过。契约变更文档与 PR 号逐条核对未在本轮做。
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

- [x] **B1 CI `build-and-validate`(Windows)绿**
  - 怎么查:tag 所指提交的 `release.yml` → `build` job 全绿:`100% tests passed`、pluginval strictness 5 三个 bundle 各 `SUCCESS`、`warning C` 零命中。
  - 参考证据:组合 CI run 38708566079 Windows 侧绿(不是 tag 提交)。tag 那一跑:run 39008918499 的 `build`(`detect-native`、`build-and-validate`)success。这里只读了 job 结论,没有逐条重读 ctest / pluginval / 编译警告的日志行。
  - 状态:✅ 已核(只核 job 结论)

- [ ] **B2 CI 格式 / 文档 / 合规门禁绿**
  - 怎么查:tag 所指提交上 `format`(clang-format、docs-truth、prettier)、`compliance`(gitleaks、reuse)、`web-smoke`、三条宪法守卫全绿。
  - 证据:______
  - 状态:⏳ CI

- [ ] **B3 本地全量 `pwsh scripts/gates.ps1`(含真机 GUI pluginval)全绿**
  - 怎么查:发版清单第 4 步;并按 `CLAUDE.md` 的出包硬规对 `staging` dispatch 一次 `build-vst3`,Windows 的 `detect-native` 与 `build-and-validate` 两个 job 为 `success` 即绿(mac job 不挡)。
  - 证据:本版没有对 staging dispatch `build-vst3`(所有者 2026-10-17 裁定,见 L1);本机全量 `gates.ps1` 的结果本轮没有事实可填。
  - 状态:⏳ 统筹

- [ ] **B4 mac job(`build-macos.yml`)在 tag 那一跑的结果**
  - 本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release;**mac job 红不挡本版 Windows 发布**,但结果要如实记下。
  - 参考证据:组合 CI run 38708566079 macOS 侧绿。tag 那一跑:发布时 `build-mac` 还在跑,结果未回读。
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

- [x] **F1 Windows 上机矩阵**
  - 怎么查:`daw-matrix.md` 新增本版一节。
  - 证据:用户 2026-10-16 在 Cubase 上测了测试包 `7ae5713`:W0–W4、W6–W8 通过;**W5(单轨旁路回原声)未测**,用户判断影响不大,不补测。W5 的行为由 CI 用例覆盖:`tests/host/test_host_harness.cpp` 的 `HOST A-8 U3(b)-ii: bypassing the only Input brings the bus back to raw through an 80 ms crossfade, un-bypassing rejoins the mix`(单独旁路唯一一条 Input,总线经 80 ms 交叉切回原声,解除旁路后回到 SCVB 混音),与同文件的 `HOST A-8 U3(b)-ii boundary: …`;`tests/core/test_output_session.cpp` 的 `[A-8] transport running: a lane whose writer freezes keeps its connected_mask and inject bits` 等 `[A-8]` 四条。这些是模拟宿主上的 CI 证据,不是 Cubase 上的实测。W3 中 Cubase 开了监听的那条轨无声并亮「路由失准」,符合判据(读不到数据就必须告警);这是否误报,列入后续排查,不挡本版。其他 Windows 宿主本版未测。
  - 状态:✅ 已核(W5 未测,见上)

- [ ] **F2 `daw-support-tiers.md` 与 README 的 Tier 表同步**
  - 证据:______
  - 状态:⏳ 统筹

- [x] **F3 mac 宿主(LUNA 验收)**
  - N/A(本版):本版先只发 Windows,mac 三件在 LUNA 真机验收后补传同一 Release;mac 两行 Tier 3 保持「待验证」,验收结果由统筹脱敏回填后再补传。
  - 状态:N/A

### G. 文档

- [ ] **G1 文档对等与红字真源**
  - 怎么查:`pwsh scripts/check-readme-parity.ps1`、`node scripts/gen-hard-rules.mjs --check` 退出 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 为 `reviewed`(fr 红字未经审校不得发版)。
  - 证据:______
  - 状态:⏳ 统筹

- [x] **G2 README / USER_GUIDE 写明 macOS 包的发布节奏**
  - 怎么查:四份文档(`README.md`、`README.zh-CN.md`、`docs/USER_GUIDE.md`、`docs/USER_GUIDE.zh-CN.md`)各有一句同义的话:「macOS beta 包在 LUNA 真机验收通过后才补传到 Release;在那之前 Releases 页上只有 Windows 包。」
  - 证据:M20 在 PR 分支上逐份 grep 过,四份各有一句(英文两份 `only the Windows package`,中文两份 `只有 Windows 包`)。
  - 状态:✅ 已核

- [ ] **G3 已知限制清单在 README 可见,`KNOWN_ISSUES.md` 与本版行为一致**
  - 证据:______
  - 状态:⏳ 统筹

### H. 许可与合规(GPLv3)

- [x] **H1 zip 内合规文件组断言通过**
  - 怎么查:`release.yml` 的 `release` job 里 `scripts/package.ps1` 的解包断言(`LICENSE`、`THIRD-PARTY-NOTICES.md`、`third_party/notices/` 等,见 `docs/RELEASE.md`「zip 内必须携带的合规文件组」)。
  - 证据:run 39008918499 的 `release` job success(打包与解包断言在该 job 内);`package-summary.md` 记 30 项、三个 `.vst3`。
  - 状态:✅ 已核

- [ ] **H2 第三方组件清单、GPLv3 源码获取途径、无不兼容许可资源**
  - 证据:______
  - 状态:⏳ 统筹

### J. 打包与发布

- [x] **J1 `SCVB-v0.10.0-beta.1-win64.zip` 内含三个 `.vst3` bundle**
  - 怎么查:`package.ps1` 打包后重开 zip 的断言;再下载草稿 Release 的 zip 看一眼;核对 `package-summary.md` 的 SHA-256 三处一致。
  - 证据:草稿资产 `SCVB-v0.10.0-beta.1-win64.zip`(8 322 166 字节,sha256 `b9c33ca21c8d35b4e7ecf4e5d7788a3592eb5a07d8ba9d34bfcad262f8d7fcee`,`sha256sum -c` 通过)、`.sha256`、`package-summary.md`(`sourceCommit` = `bc04686`,releaseDate 2026-10-17,30 项,三个 `.vst3`)。发布后匿名下载回读,SHA-256 一致。本机 GUI pluginval 对该 zip 解出的三个 bundle(strictness 5、timeout 60 s、含 GUI)均 SUCCESS。
  - 状态:✅ 已核

- [x] **J2 草稿 Release 里的 macOS 三件已撤下并留存**
  - 怎么查:草稿 Release 的资产列表里没有 `SCVB-v0.10.0-beta.1-macos-arm64.*`(zip、`.sha256`、`package-summary-macos.md`);三件已下载另存,SHA-256 记在 [luna-acceptance.md](luna-acceptance.md) 的验收记录里,待 LUNA 验收后补传到同一 Release。
  - 标题与重跑:mac 那一路绿时,`release-macos` 会把草稿标题改成「(Windows x64 · macOS arm64 beta)」。撤下三件后**把标题手改回** `Synchain Vocal Balancer v0.10.0-beta.1 (Windows x64)`,再用 `gh release view v0.10.0-beta.1 --json name,assets` 回读,确认标题与 3 个资产名都对。手改回去的标题正好等于流水线原样标题,所以撤下之后**不要再以任何方式重跑这次 run**(Re-run all jobs,或单独重跑 `build-mac` / `release-macos`),否则 mac 三件会被重新传上来、标题也会再被改掉,Re-run all 还会把正文重置(`docs/RELEASE.md` 同一 tag 重跑一段)。
  - 补传(LUNA 验收后,Release 已发布,流水线不碰,只能手工):`gh release upload` 三件 → 读回并 `shasum -a 256 -c` 核对 → 标题改成合并标题 → 正文补上 mac 的 SHA-256(取自 `package-summary-macos.md`)。
  - 证据:**未挂上,无需撤下。** 发布时 `build-mac` 还在跑,mac 三件从没挂上草稿;`release-macos` 对已发布的 Release 拒绝上传,所以发布后也不会挂上。已发布 Release 标题为 `Synchain Vocal Balancer v0.10.0-beta.1 (Windows x64)`。mac 包要从 `build-mac` 的 artifact 另存,等 LUNA 验收后手工补传;另存与 SHA-256 登记不在本次核对范围。
  - 状态:✅ 已核(未挂上,无需撤下)

- [ ] **J3 发布说明按 `docs/RELEASE.md` 模板填写,写明本版只有 Windows 包**
  - 怎么查:正文含升级须知、回退步骤、SmartScreen 说明、SHA-256(从 `package-summary.md` 复制)、「macOS beta 包在 LUNA 真机验收通过后补传」一句。
  - 证据:______
  - 状态:⏳ 统筹

- [x] **J4 GitHub Release 建为草稿,人工核对后发布为 pre-release**
  - 证据:草稿由 `release.yml` run 39008918499 建出,核对后 2026-10-17 发布为 pre-release。发布前回读:tag = prod = `bc04686`,staging = `b956dd5`。
  - 状态:✅ 已核

- [x] **J5 tag 保护生效(`v*`)**
  - 证据:tag 保护规则集 `protect-release-tags-v`(`refs/tags/v*`,creation / update / deletion / non_fast_forward)状态 active。
  - 状态:✅ 已核

- [x] **J6 干净 Windows 11 上解压安装,DAW 扫得到并正常工作**
  - N/A(本版):本版未在干净 Windows 11 上验证。用户是在已装 rc.1 的机器上换装测试包 `7ae5713` 测的,安装方式与 rc.1 相同;是否要求干净机,v1.0.0 前再议。
  - 状态:N/A

### K. 回滚预案

- [ ] **K1 上一 release(`v0.9.0-rc.1`)的 zip 仍可下载,发布说明写明如何回退**
  - 证据:______
  - 状态:⏳ 统筹

### L. 本版追加

- [x] **L1 `staging` / `prod` 按 `docs/RELEASE.md` 现行的 J184 / J184a 前移**
  - 怎么查:链路是 feature → `dev`(里程碑 squash)→ PR(merge commit)进 `staging` → 对 `staging` dispatch `build-vst3`,Windows 的 `detect-native` 与 `build-and-validate` 两个 job 为 `success` 即绿(mac job 不挡)→ PR(merge commit)进 `prod` → tag 打在 `prod` 的合并提交 P 上。两次晋升都走 PR、用 merge commit,不快进、不临时关分支保护;`prod` = 最新已发布版本(含预发布)。回读见 `docs/RELEASE.md`「每次发布的顺序」第 5、6 步:`prod` 等于 tag 指向的提交,`staging` 是它的祖先且树相同;`package-summary.md` 的 `sourceCommit` 是 P。
  - 证据:#408(dev → staging)与 #409(staging → prod)都是 PR、merge commit,没有快进,没有临时关保护。tag `v0.10.0-beta.1` = prod = P = `bc04686`;P^2 = staging 的合并提交 S = `b956dd5`,S 的树与 dev 上里程碑提交 `a4a76a3`(#407,feature/macos-au @ `e8a9569` squash 进 dev)逐字相同;`package-summary.md` 的 `sourceCommit` = `bc04686`。**与「怎么查」的偏差**:本次没有对 staging dispatch `build-vst3`——所有者 2026-10-17 要求只在最后构建一轮,以 tag 触发的 `release.yml` 作为本版的完整构建;#407 的 PR 头上 Windows `build-and-validate` 绿。
  - 状态:✅ 已核(dispatch 一步按所有者裁定省去)

- [x] **L2 `.markdown-link-check.json` 里 CHANGELOG 两条对比链接的临时放行已删**
  - 怎么查:tag 推上去之后,删掉为 `[Unreleased]` / `[0.10.0-beta.1]` 两个对比地址加的临时放行,再跑 `npx -y markdown-link-check -c .markdown-link-check.json CHANGELOG.md`。
  - 证据:本 PR 删掉 `.markdown-link-check.json` 里的临时放行后,`npx -y markdown-link-check -c .markdown-link-check.json CHANGELOG.md` 两条对比链接(`v0.9.0-rc.1...v0.10.0-beta.1`、`v0.10.0-beta.1...HEAD`)均 200。
  - 状态:✅ 已核

- [ ] **L3 打 tag 前在 tag 提交上重做发布盘点与漏项全扫**
  - 证据:______
  - 状态:⏳ 统筹

## 3. 汇总

截至 v0.10.0-beta.1 发布后收尾:共 31 项,已勾 11 项 ✅(F1〔W5 未测〕、G2、A1、B1〔只核 job 结论〕、H1、J1、J2、J4、J5、L1〔dispatch 一步按所有者裁定省去〕、L2)与 F3、J6(N/A),其余待 CI 或统筹。「证据」留空的格子表示还没核,不是通过。

| 状态 | 项数 |
| --- | --- |
| ✅ 已核 | 11(A1、B1、F1、G2、H1、J1、J2、J4、J5、L1、L2) |
| N/A | 2(F3、J6) |
| ⏳ CI | 4(B2、B4、C1、D2) |
| ⏳ 统筹 | 14(A2、A3、A4、B3、D1、D3、E1、F2、G1、G3、H2、J3、K1、L3) |
| ⏳ 用户 | 0 |

(合计 31:11 + 2 + 4 + 14 + 0。)
