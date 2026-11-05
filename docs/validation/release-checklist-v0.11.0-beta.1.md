# 发布前验证清单 —— v0.11.0-beta.1

> 状态:**准备中**。本版只发 Windows(用户 2026-11-01 裁定)。
> 模板:[release-checklist-v0.10.0-beta.1.md](release-checklist-v0.10.0-beta.1.md)。只保留本版适用的项,证据只写实际看过的。
> 来源:`docs/RELEASE.md` 发版清单(J184 链)。
> 最后更新:2026-11-05
> 相关:上机矩阵 [daw-matrix.md](daw-matrix.md) · 支持等级 [daw-support-tiers.md](daw-support-tiers.md) · null test 记录 [audio/nulltest-log.md](audio/nulltest-log.md) · 性能 [perf/budget-log.md](perf/budget-log.md)

## 0. 怎么用

- **版本路线**:`CMakeLists.txt` 改为 `0.11.0`,打 tag `v0.11.0-beta.1`。
- 每项状态:**✅ 已核**(证据写在项里)/ **⏳ CI**(等 tag 那一跑)/ **⏳ 统筹** / **⏳ 用户** / **N/A + 理由**。只有 ✅ 与 N/A 勾 `[x]`。**✅ 已核(有豁免)** = 有一步经所有者明确裁定省去,偏差与裁定写在该项里。
- **mac 的口径**:本版只发 Windows。tag 那一跑里 mac 那一路照常跑,它的三件如果挂上了草稿,发布前撤下(见 J2)。
- 打 tag 前,统筹在 tag 所指提交上把全部 ✅ 项的「怎么查」重跑一遍,结果变了就改回 ⏳。

## 1. 本版的宿主证据

| 证据 | 内容 |
| --- | --- |
| REAPER 7.82 成品矩阵 | 2026-11-03,`tests/reaper/` 的两层矩阵,148 行全部通过(3 行第一次被授权提醒等挡住,重跑通过)。结果写在 [DAW_COMPATIBILITY.md](../DAW_COMPATIBILITY.md) 脚注 9 与「两层 SCVB(嵌套)」小节 |
| Cubase | **本版不复测**:用户 2026-11-04 裁定(「没事不用看了cubase」)。上一次 Cubase 实测是 v0.10.0-beta.1 的测试包 `7ae5713` |

测的构建是 `feature/v1` @ `647a725`(#435 合并前的候选)。之后合进来的 #436、#437 只改开窗,#438、#439 只改测试与文档,都不碰音频路径。

## 2. 清单

### A. 代码与版本

- [ ] **A1 版本号与 tag 一致**
  - 怎么查:`git show v0.11.0-beta.1:CMakeLists.txt | grep -n "project(SCVB"` 应为 `VERSION 0.11.0`;`release.yml` 的 `verify-tag` job 绿。
  - 证据:(PR 分支)`CMakeLists.txt` 为 `project(SCVB VERSION 0.11.0)`。tag 那一跑:______
  - 状态:⏳ CI

- [x] **A2 CHANGELOG 已更新**
  - 怎么查:`## [Unreleased]` 已归入 `## [0.11.0-beta.1] - <日期>`,底部对比链接补齐;`node scripts/check-changelog-drafts.mjs` 退出 0;契约变更条目各有 `docs/contract-changes/` 文档、每条带 PR 号。
  - 证据:(PR 分支)`## [Unreleased]` 已归入 `## [0.11.0-beta.1] - 2026-11-04`,上面留一个空的 `[Unreleased]`;底部补了两条对比链接(tag 推上去之前 404,临时放行见 L2)。`node scripts/check-changelog-drafts.mjs` 退出 0。本节 17 条每条带 PR 号;三条契约变更各有变更文档(见 A3)。发版日期如果不是 11-04,打 tag 前改标题。
  - 状态:✅ 已核

- [x] **A3 带 `status/frozen-contract` 标签的 PR 均经用户显式批准**
  - 怎么查:`gh pr list -R synchain-oss/scvb --state merged --label status/frozen-contract`,`v0.10.0-beta.1` 之后合并的逐个对到用户裁定。
  - 证据:三个:#427(pan 曲线只在播放时叠加,用户 2026-11-01 批准,`docs/contract-changes/20261101-pan-curve-playback-only.md`)、#432(离线交接措辞,用户 2026-11-02 批准,`20261102-offline-first-block-handoff.md`)、#435(实时起播,措辞用户 2026-11-03 批准,`20261103-realtime-play-start.md`)。三份变更文档的状态行都写明批准日期。
  - 待同步(不挡发版):#432、#435 在 `docs/adr/0002` 加的补注还没进宪法原件(masterPlan 仓)和 `docs/constitution/` 副本,两份变更文档都写明了。
  - 状态:✅ 已核

- [x] **A4 git 历史无 secret**
  - 怎么查:`gh api repos/synchain-oss/scvb -q .security_and_analysis`;`gh api "repos/synchain-oss/scvb/secret-scanning/alerts?state=open" -q length`;每个 PR 的 `compliance` job 另跑 gitleaks。
  - 证据:2026-11-04 回读:secret scanning 与 push protection 都是 enabled,未关闭的告警 0 条。本版各子 PR 的 `compliance` 都绿。
  - 状态:✅ 已核

### B. 自动化门禁

- [ ] **B1 CI `build-and-validate`(Windows)绿**
  - 怎么查:tag 所指提交的 `release.yml` → `build` job 全绿。
  - 证据:______
  - 状态:⏳ CI

- [ ] **B2 CI 格式 / 文档 / 合规门禁绿**
  - 怎么查:里程碑 PR 上 `format`(clang-format、docs-truth、prettier)、`compliance`、`web-smoke`、`branch-gate` 全绿。
  - 证据:______
  - 状态:⏳ CI

- [x] **B3 本地全量 `pwsh scripts/gates.ps1`(含真机 GUI pluginval)全绿**
  - 怎么查:发版清单第 4 步。对 `staging` dispatch `build-vst3` 这一步本版省去(所有者 2026-11-01 裁定)。
  - 证据:2026-11-04 在 PR 分支 `00f7f09` 上跑 `pwsh scripts/gates.ps1`(全量,含 gate 8),本机 MSVC 14.44、JUCE 8.0.8、pluginval 1.0.4:gate 1–8 与各子档全部 PASS,没有 SKIP / FLAKY-SKIP;gate 5 构建 0 条 `warning C`;gate 6 的 9 个 ctest 目标全部通过;gate 7 / 8 三个插件在 strictness 5 下都过(gate 8 含 GUI)。同一 PR 上之后只改了文档。
  - 状态:✅ 已核

- [ ] **B4 mac job 在 tag 那一跑的结果**
  - 本版只发 Windows,mac job 红不挡发布,但结果要如实记下。macOS 的宿主用例有几条计时敏感,偶发红过(`modeAt(2.0) == Armed`、`blockDiff(bus, level)`、`writerJumpedAtRerender`);P0-2 / P1-5 两条已由 #439 修掉。
  - 证据:______
  - 状态:⏳ CI(不挡)

### C. UI 与无障碍

- [ ] **C1 web 冒烟全绿、i18n 无硬编码字面量**
  - 怎么查:里程碑 PR 上 `web-smoke` job 与 `node scripts/check-i18n.mjs`。
  - 证据:______
  - 状态:⏳ CI

### D. 音质

- [x] **D1 导出 null test**
  - 怎么查:`audio/nulltest-log.md` 追加本版一条。
  - 证据:`scripts/nulltest.ps1` 分声道比对(2026-11-04,模式 A,`-PanLawDb 0 -Align`):REAPER 7.82 上一层工程的 Full-speed Offline 与 1× 导出各对不插插件的同一工程,样本偏移 0,残差峰值 L / R 都是 −150.5 dBFS,RMS −168.5 / −168.6 dBFS,判据是峰值 < −120 dBFS;文件是 2026-11-03 矩阵 `n0-ref` 那一行导出的。同一份矩阵里还有:两层工程的离线导出(窗口 / 命令行、Full-speed Offline / 1×、块大小 32 / 2048、44.1 kHz、选区、多 region)和同样位置的一层工程逐位相同;一层工程的离线导出和不插插件的混音差在 −150 dB;改渲染采样率时差别在 -132 dB 以下;存盘重开三次 FX state 不变;实时渲染开头约 0.5–0.6 秒和离线不同、之后逐位相同(KI-21)。这部分比较用的是 reaper-harness 自带的对比和 `tests/reaper/nested/session/wav.mjs`。记录见 D3。
  - 状态:✅ 已核

- [ ] **D2 PANLAW-1..5 绿**
  - 怎么查:`ctest` 里对应用例(含在 B1 / B3 的 `100% tests passed` 内)。
  - 证据:PANLAW 用例(`tests/core/test_panlaw.cpp`)在 `scvb_tests` 里,含在 B3 那次 gate 6 的 ctest 全部通过之内;本 PR 的 CI `build-and-validate` 也绿。tag 那一跑再回读一次。
  - 状态:⏳ CI(本地与 PR 上已绿)

- [x] **D3 结果追加到 `audio/nulltest-log.md`**
  - 证据:本 PR 在 §2 汇总表追加两行(REAPER 7.82,模式 A,Full-speed Offline 与 1×)和一段说明,§3 追加 `scripts/nulltest.ps1` 的两节原始记录(2026-11-04 03:25:26 / 03:25:28)。
  - 状态:✅ 已核

### E. 性能

- [x] **E1 `scvb_bench --dsp` 对比上一版无 >10% 退化**
  - 怎么查:`perf/budget-log.md`。本版动了离线交接、实时起播与分析,按模板要重跑:在同一台机器、同一套工具链上编 `v0.10.0-beta.1` 和本版,各跑两档。
  - 证据:2026-11-04 在同一台机器、同一套工具链上编 `v0.10.0-beta.1` 和本版的 `scvb_bench`,两档交替各跑 6 次。两份 exe 逐字节比只差构建时间戳,`checksum` 两版相同;基准编进去、调用到的代码 `git diff` 为空。Output 48k/512 的中位数本版高约 15%,但同一份旧版自己 6 次之间就差 17%,两份又是同一份机器码,判为计时噪声;其余三格中位数差约 2% 以内。本版改的离线交接、实时起播代码(OutputSession、ShmRingMixSource、InputSession 等)`--dsp` 不量,没有基准覆盖。数与条件见 `perf/budget-log.md`「v0.11.0-beta.1(发版清单 E1)」。这台不是 U16 参考机,数不和基线比。
  - 状态:✅ 已核

### F. DAW 矩阵

- [x] **F1 Windows 上机矩阵**
  - 怎么查:`daw-matrix.md` 新增本版一节。
  - 证据:REAPER 7.82 成品矩阵 148 行全部通过(见 §1)。Cubase 本版不复测(用户 2026-11-04 裁定)。
  - 状态:✅ 已核(有豁免:Cubase 不复测)

- [x] **F2 `daw-support-tiers.md` 与 README 的 Tier 表同步**
  - 证据:REAPER 维持 Tier 2,本 PR 只把依据改成成品矩阵的结果;README 两份与 DAW_COMPATIBILITY §4 的 REAPER 行在 #438 已改,等级没变,三处一致。
  - 状态:✅ 已核

- [x] **F3 mac 宿主(LUNA 验收)**
  - N/A(本版):只发 Windows。
  - 状态:N/A

### G. 文档

- [x] **G1 文档对等与红字真源**
  - 怎么查:`pwsh scripts/check-readme-parity.ps1`、`node scripts/gen-hard-rules.mjs --check` 退出 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 为 `reviewed`。
  - 证据:2026-11-04 在 PR 分支上:`check-readme-parity.ps1` 两组双语文档结构一致;`gen-hard-rules.mjs --check` 6 个落地面与真源逐字节一致;`frReview.status` 为 `reviewed`。
  - 状态:✅ 已核

- [x] **G2 README / USER_GUIDE 写明 macOS 包的发布节奏**
  - 证据:四份文档里「macOS beta 包在 LUNA 真机验收通过后才补传」那句没有改动,本版仍然成立。
  - 状态:✅ 已核

- [x] **G3 已知限制清单在 README 可见,`KNOWN_ISSUES.md` 与本版行为一致**
  - 证据:本版新增 KI-20 到 KI-27;KI-8 按宿主改写,README 与手册的摘要同步(#438)。KI-27(从 REAPER 窗口离线导出 68 次里有 1 次整个文件无声,原因没查清)用户 2026-11-05 裁定接受,登记与手册提示见 #440。
  - 状态:✅ 已核

### H. 许可与合规(GPLv3)

- [ ] **H1 zip 内合规文件组断言通过**
  - 怎么查:`release.yml` 的 `release` job 里 `scripts/package.ps1` 的解包断言。
  - 证据:______
  - 状态:⏳ CI

- [ ] **H2 第三方组件清单、GPLv3 源码获取途径、无不兼容许可资源**
  - 怎么查:本版有没有新增第三方组件;`INSTALL.txt` 的源码链接指向本 tag。
  - 证据:`v0.10.0-beta.1` 到本版,`third_party/`、`REUSE.toml`、`THIRD-PARTY-NOTICES.md`、`LICENSES/`、`web/js/juce/`、`web/fonts/`、四个钉版文件、`tests/CMakeLists.txt` 的 `git diff` 都为空(2026-11-04 核),没有新增第三方组件。`INSTALL.txt` 的源码链接由 `package.ps1` 钉在 tag 上,tag 那一跑回读。
  - 状态:⏳ CI(第三方部分已核)

### J. 打包与发布

- [ ] **J1 `SCVB-v0.11.0-beta.1-win64.zip` 内含三个 `.vst3` bundle**
  - 怎么查:`package.ps1` 的断言;下载草稿的 zip 看一眼;`package-summary.md` 的 SHA-256 三处一致。
  - 证据:______
  - 状态:⏳ CI

- [ ] **J2 草稿里没有 macOS 三件,标题是「(Windows x64)」**
  - 怎么查:`gh release view v0.11.0-beta.1 --json name,assets` 只有 Windows 三件。mac 那一路绿时 `release-macos` 会把三件挂上并把标题改成合并标题,这时撤下三件(另存)、标题改回 `Synchain Vocal Balancer v0.11.0-beta.1 (Windows x64)`,再回读。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J3 发布说明按 `docs/RELEASE.md` 模板填写,写明本版只有 Windows 包**
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **J4 GitHub Release 建为草稿,人工核对后发布为 pre-release**
  - 证据:______
  - 状态:⏳ 用户

- [x] **J5 tag 保护生效(`v*`)**
  - 证据:2026-11-04 回读:tag 规则集 `protect-release-tags-v`(`refs/tags/v*`,creation / update / deletion / non_fast_forward)状态 active。
  - 状态:✅ 已核

- [x] **J6 干净 Windows 11 上解压安装**
  - N/A(本版):同 v0.10.0-beta.1,v1.0.0 前再议。
  - 状态:N/A

### K. 回滚预案

- [ ] **K1 上一 release(`v0.10.0-beta.1`)的 zip 仍可下载,发布说明写明如何回退**
  - 证据:2026-11-04 回读 `v0.10.0-beta.1` 仍是 pre-release,资产是 Windows 三件。回退说明待写进发布说明。
  - 状态:⏳ 统筹

### L. 本版追加

- [ ] **L1 `staging` / `prod` 按 J184 前移**
  - 怎么查:`feature/v1` →(里程碑 squash)`dev` → PR(merge commit)进 `staging` → PR(merge commit)进 `prod` → tag 打在 `prod` 的合并提交上。对 `staging` dispatch `build-vst3` 本版省去(所有者 2026-11-01 裁定),演练 tag 也不推(同一裁定)。
  - 证据:______
  - 状态:⏳ 用户

- [ ] **L2 `.markdown-link-check.json` 里 CHANGELOG 两条对比链接的临时放行已删**
  - 怎么查:tag 推上去之后删掉为 `[Unreleased]` / `[0.11.0-beta.1]` 两个对比地址加的临时放行,再跑 `npx -y markdown-link-check -c .markdown-link-check.json CHANGELOG.md`。
  - 证据:______
  - 状态:⏳ 统筹

- [ ] **L3 官网**
  - 怎么查:Release 发布之后按 release-kit 的 `release-scvb` 手册更新下载页的版本号与 SHA-256,加曲线语义的 FAQ,REAPER 的说法按成品矩阵改。只更新到官网的 `dev`,不推 `prod`:官网的 `prod` 由用户定期集中推(用户 2026-11-04 裁定)。
  - 证据:______
  - 状态:⏳ 统筹

## 3. 汇总

截至 2026-11-05(PR 分支,打 tag 前):共 31 项。「证据」留空的格子表示还没核,不是通过。

| 状态 | 项数 |
| --- | --- |
| ✅ 已核 | 13(A2、A3、A4、B3、D1、D3、E1、F1、F2、G1、G2、G3、J5) |
| N/A | 2(F3、J6) |
| ⏳ CI | 9(A1、B1、B2、B4、C1、D2、H1、H2、J1) |
| ⏳ 统筹 | 5(J2、J3、K1、L2、L3) |
| ⏳ 用户 | 2(J4、L1) |

(合计 31:13 + 2 + 9 + 5 + 2。)
