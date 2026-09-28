# 发布前验证清单 —— v0.9.0-rc.1

> 状态:进行中 —— **任何一项未勾不得发布**
> 最后更新:2026-09-28(核对基线:`feature/v1` @ `e3a7f5a1`)
> 来源:masterPlan 10 §6.1 的 A–K 逐项照搬;L 段是本版裁定追加的硬门。
> 相关:上机矩阵 [daw-matrix.md](daw-matrix.md) · 支持等级 [daw-support-tiers.md](daw-support-tiers.md) · null test 记录 [audio/nulltest-log.md](audio/nulltest-log.md)

## 0. 怎么用

- **版本路线(J123)**:`CMakeLists.txt` 改为 `0.9.0`,打 tag `v0.9.0-rc.1`;rc 测过后再发 `v1.0.0`,届时另建 `release-checklist-v1.0.0.md`,**不沿用本文件的勾**。
- **允许「N/A + 理由」**(10 §6.0),理由写在该项里。
- 每项分四行:**查什么 / 怎么查 / 证据 / 状态**。状态口径:
  - **✅ 已核**:写本文件时在 `e3a7f5a1` 上亲自看过证据,命令与结果写在「证据」里;
  - **⏳ CI**:由 tag 所指提交上的 CI 给出(tag 推上去后 `release.yml` 调用 `build-vst3.yml`,见 #297);此前的绿 run 只作参考,列在证据里;
  - **⏳ 统筹**:统筹要做的核对、派工,或要提请用户裁定;
  - **⏳ 用户上机 / ⏳ 用户**:只能用户在真机或仓库设置里做,步骤见 §3;
  - **N/A**:不适用,理由写在该项里。
- 打勾规则:只有 ✅ 与 N/A 勾 `[x]`,⏳ 一律 `[ ]`。
- **本文件的结论是阶段性的**(J129 / J139):打 tag 前,统筹要在 tag 所指提交上把全部 ✅ 项的「怎么查」重跑一遍,结果变了就改回 ⏳;再全量重扫一次漏项(L3)。

## 1. 汇总

截至 2026-09-28,共 54 项(10 §6.1 的 A–K 51 项 + 本版追加 L 3 项):

| 段 | 项数 | ✅ / N/A | ⏳ 用户 | ⏳ 统筹 | ⏳ CI |
| --- | --- | --- | --- | --- | --- |
| A 代码与版本 | 5 | 1 | 0 | 4 | 0 |
| B 自动化门禁 | 6 | 0 | 1 | 1 | 4 |
| C UI 与无障碍 | 4 | 0 | 0 | 2 | 2 |
| D 音质 | 5 | 0 | 3 | 2 | 0 |
| E 性能 | 3 | 0 | 3 | 0 | 0 |
| F DAW 矩阵 | 4 | 1 | 3 | 0 | 0 |
| G 兼容性与数据安全 | 4 | 3(均 N/A) | 0 | 1 | 0 |
| H 文档 | 8 | 2 | 0 | 6 | 0 |
| I 许可与合规 | 4 | 1 | 0 | 3 | 0 |
| J 打包与发布 | 6 | 0 | 3 | 1 | 2 |
| K 回滚预案 | 2 | 1(N/A) | 0 | 1 | 0 |
| L 本版追加 | 3 | 0 | 0 | 3 | 0 |
| **合计** | **54** | **9** | **13** | **24** | **8** |

「⏳ 用户」13 项:11 项要上机(B4、D1、D2、D5、E1–E3、F1、F2、F4、J3),2 项在 GitHub 上操作(J5 发布草稿、J6 tag 保护)。

## 2. 清单

### A. 代码与版本

- [ ] **A1 版本号与 tag 一致**
  - 查什么:顶层 `CMakeLists.txt` 的 `project(SCVB VERSION …)` 与 tag 一致。
  - 怎么查:`git show v0.9.0-rc.1:CMakeLists.txt | grep -n "project(SCVB"` 应为 `VERSION 0.9.0`;tag 推上去后 `release.yml` 的 `verify-tag` job 绿(#297 的 `scripts/check-release-tag.ps1`:rc tag 只比对 `X.Y.Z` 部分,rc 与正式版对应同一个 CMake 版本)。
  - 证据:当前 `CMakeLists.txt:8` 是 `project(SCVB VERSION 0.1.0)`;CI run 36364806176 的 pluginval 输出也是 `SCVB Input v0.1.0`。
  - 状态:⏳ 统筹(出 RC 时改;依赖 #297 合并)

- [ ] **A2 CHANGELOG 已更新**
  - 查什么:含破坏性变更、已知限制、DAW Tier 变化。
  - 怎么查:按 J123 在出 RC 时把 `## [Unreleased]` 改为 0.9.0 的版本节(节名与日期按 `docs/RELEASE.md` 发版清单第 2 步);`### ⚠️ 契约变更` 每条都带 `docs/contract-changes/` 文档;每条带 PR 号(CI `check-changelog-drafts.mjs` 判)。
  - 证据:`[Unreleased]` 已有 `### ⚠️ 契约变更` 小节;Tier 变化与三条已知限制已由 #298 写进 `### 变更`(`CHANGELOG.md:278` 起)。版本节尚未建。
  - 状态:⏳ 统筹

- [ ] **A3 带破坏性标签的 PR 均经用户显式批准**
  - 查什么:10 §6.1 写的是 `breaking:params` / `breaking:ipc-abi` / `breaking:state` 三个标签。**本仓没有这三个标签**(`gh label list` 无命中),实际用的是 `status/frozen-contract`。
  - 怎么查:`gh pr list -R synchain-oss/scvb --state all --label status/frozen-contract`,已合并的逐个对到用户裁定(J 号)或 PR 里的用户批准记录。
  - 证据(2026-09-28):带该标签的 PR 合并 41、关闭未合 9、在飞 7;`docs/contract-changes/` 下 41 份变更文档(另有 `TEMPLATE.md`)。
  - 状态:⏳ 统筹(逐个对 J 号;在飞 7 个合并时各自补)

- [ ] **A4 `spikes/` 为空、无 spike 残留**
  - 查什么:`spikes/` 目录为空(J16);无 spike 调试代码(`SpikeCurve`、`--flap-heartbeat`、FPS 角标、processBlock 计时)。`tests/tools/scvb_diag` 是常驻工具,不删。
  - 怎么查:`git ls-tree -r --name-only HEAD spikes/`;`git grep -n "SpikeCurve\|flap-heartbeat" -- src tests web`;`git grep -n -i "fps" -- src`。
  - 证据:`spikes/` 下只剩 `spikes/README.md` 一个文件(内容仍写「S2–S4 的验证代码放这里」,已过期);`SpikeCurve` 只在 `tests/tools/scvb_bench.cpp:154` 的一行注释里,`flap-heartbeat` 0 处;`src/` 里 `fps` 的命中全是特征指纹常量 `kFp*`,不是帧率角标。
  - 状态:⏳ 统筹(删 `spikes/README.md`,或写 N/A 理由)

- [x] **A5 git 历史无 secret**
  - 查什么:secret scanning 与 push protection 已开(06 §2.2);历史里没有密钥。
  - 怎么查:`gh api repos/synchain-oss/scvb -q .security_and_analysis`;`gh api "repos/synchain-oss/scvb/secret-scanning/alerts?state=open" -q length`。
  - 证据(2026-09-28):`secret_scanning` 与 `secret_scanning_push_protection` 均为 `enabled`;开放告警 0;全历史泄漏扫描 SL-268(2026-09-28)无密钥;每个 PR 的 `compliance` job 另跑 gitleaks(扫工作区)。
  - 状态:✅ 已核

### B. 自动化门禁(L0–L2)

- [ ] **B1 CI `build-and-validate` 绿**
  - 查什么:构建(/W4 零 warning)+ ctest 全绿 + pluginval strictness 5。10 §6.1 写「两个 bundle」,按 J137 现为**三个**(Input / Output / Monitor)。
  - 怎么查:tag 所指提交的 `release.yml` → `build` job(调用 `build-vst3.yml`)全绿;日志里 `100% tests passed` 且 pluginval 三行 `SUCCESS`。
  - 证据(参考,都不是 tag 提交):`feature/v1` 最近一次 dispatch 全量 run 36364806176(`72e4501`,即 v5.6.19 测试包):ctest 7/7 个套件通过,pluginval `--strictness-level 5 --skip-gui-tests` 三个 bundle 各 `SUCCESS`;最近一次 PR 全量 run 36403067073(#298 的 head `b220fc3`):同样 7/7 + 三个 `SUCCESS`。`e3a7f5a1` 本身没有构建 run。
  - 状态:⏳ CI

- [ ] **B2 CI `clang-format` 绿(含桥契约一致性)**
  - 查什么:10 §6.1 把桥契约一致性写在 clang-format 里;现行 workflow 里它在 `docs-truth` job(`check-bridge-parity.mjs`)。两个 required check 都要绿。
  - 怎么查:合入 tag 提交的那个 PR 的 Format workflow。
  - 证据(参考):#298 head 的 Format run 36403067004:`clang-format`、`docs-truth`、`web-smoke` 均绿。
  - 状态:⏳ CI

- [ ] **B3 三条宪法守卫绿**
  - 查什么:参数冻结(**123 参数**)/ IPC 布局冻结(**15 slot + `channels` 字段**)/ state abi 兼容。10 §6.1 的名字与仓内用例的对应:
    - `ParamLayoutFreeze` → `tests/core/test_params_golden.cpp` 的 `[params]` 用例(「params golden 逐行 diff(123 行)」等),对拍 `tests/golden/params_v0.tsv`(非注释行 123);
    - `IpcLayoutFreeze` → `tests/core/test_ipc_layout.cpp:541`,对拍 `tests/golden/ipc-layout.txt`(`max_channels 15`,含 `AudioRingHeader.channels` 等字段);
    - `StateAbiCompat` → `tests/core/test_state_codec.cpp:399`「abi1..abi5 迁移 + abi6.bin 格式锁」。
  - 怎么查:随 B1 的 ctest(`scvb_params_tests` / `scvb_tests`)。三组用例的标签都不带 `.`,在默认集里。
  - 证据:参考 run 同 B1。
  - 状态:⏳ CI

- [ ] **B4 `[reference]` libebur128 对拍在本地跑通**
  - 查什么:K 加权与 libebur128 逐点对拍(KW-3,10 §4.4.2)。
  - **CI 绿不算**:CI 的 cmake 行不开 `SCVB_TESTS_WITH_EBUR128`,此时 `tests/core/test_kweighting.cpp:197` 只是一条 `SUCCEED` 占位,**这条在 CI 里是空转的**。目前 `[reference]` 只有 KW-3 一条;10 §4.4.2 表里的 L0-b / L0-c(段响度对拍,`test_loudness_ref.cpp`)仓里没有。
  - 怎么查:§3 的 U-6。判据:`[reference]` 用例全过,且输出里**没有** `SCVB_TESTS_WITH_EBUR128=OFF` 那句占位文字。
  - 状态:⏳ 用户上机

- [ ] **B5 IPC 长跑用例(IPC-3 十万块)**
  - 查什么:IPC-3 双进程音频环 10 万块逐样本一致、gapCount==0。
  - 与 10 §6.1 不同:仓里**没有 `[.long]` 标签**(`git grep -n "\[\.long\]" -- tests` 0 处)。IPC-3 在 `tests/ipc/test_ipc_contract.cpp:505`,标签 `[ipc][contract]`,**每次 ctest 都跑**(`scvb_ipc_tests`),所以不用单独在本地补跑。
  - 怎么查:随 B1。CI 日志只打套件级 `Passed`,不列逐条用例;要逐条证据可在本地跑 `scvb_ipc_tests.exe "IPC-3*" --durations yes`(可选)。
  - 状态:⏳ CI

- [ ] **B6 本地全量 `pwsh scripts/gates.ps1` 全绿**
  - 查什么:全量(不加 `-Quick`),含 gate 8 的 GUI pluginval 与 gate 9 / 10。
  - 实况:维护者本机长期可用内存不足,J142 临时以 PR head 上的 CI 全量代替本地 gates;但 **GUI pluginval 只能在有桌面的本机跑**(CI 是 `--skip-gui-tests`),CI 代替不了这一半。
  - 状态:⏳ 统筹(内存恢复后在 tag 提交上跑一次;跑不了就提请用户批 N/A 并写理由)

### C. UI 与无障碍

- [ ] **C1 web 冒烟全绿**
  - 与 10 §6.1 不同:没有 `web-preview/smoke/run.mjs` 与「六个 fixture」;现行是 `web-preview/tests/smoke-*.mjs` 共 32 套,CI `web-smoke` job 逐套跑。
  - 怎么查:tag 提交的 `web-smoke` 绿,**且** job summary 里「跳过(缺浏览器)0、没跑成 0」—— SKIP / FLAKY 退出码不判红,只看绿会漏。
  - 证据(参考):#298 head 的 Format run 36403067004:32 套全部跑了,该步 0 条 warning。
  - 状态:⏳ CI

- [ ] **C2 axe-core 零 serious / 零 critical**
  - 实况:axe-core 是 dev-only,不在 CI(`web-preview/README.md` §5)。
  - 怎么查:按 `web-preview/README.md` §5 起本地服务,对 Output / Input / Monitor 三页各跑一次 `npx @axe-core/cli "<页面地址>"`。
  - 状态:⏳ 统筹

- [ ] **C3 缩放全档位无横向溢出;键盘可达**
  - 查什么:Output 7 档 / Input 10 档(05 §1.2)。
  - 线索:`smoke-shell-fit*.mjs`、`smoke-ui-layout-page.mjs`、`smoke-a11y-tabs.mjs` 相关;**还没逐条核它们是否覆盖全部档位**。
  - 状态:⏳ 统筹(核覆盖面;缺的档位补一次手测,或写 N/A 理由)

- [ ] **C4 i18n 新增文案都有 key,无硬编码字面量**
  - 怎么查:`node scripts/check-i18n.mjs`(与 CI `docs-truth` 同一条命令,查 zh / en / fr 键对等与红字九条);「无硬编码字面量」靠复审 prompt 第 6 节,没有机检。
  - 证据(参考):#298 head 的 `docs-truth` 绿。
  - 状态:⏳ CI

### D. 音质(10 §4)

- [ ] **D1 模式 A null test**
  - 查什么:透明性 null;判据 10 §1.1.3 S1-P1 / P2(样本偏移 0、残差峰值 < −120 dBFS)。10 §6.1 要求「至少在 REAPER 上重跑一遍」;首发只验 Cubase(J124),用 Cubase 代替要统筹认可。
  - ⚠ **补偿量要按成品重算**:10 §4.2 的补偿公式按 S1 spike 口径写(spike 版 Output 把 mono 原样复制到 L / R,0 dB);**成品 Output 对居中 mono 轨每侧给 0.7071(−3.01 dB)**(`tests/core/test_transition.cpp` 的 PAN-1)。所以宿主 pan law 为 −3 dB 等功率(Cubase 默认)时 mono 部分理论上不用补偿,宿主为 0 dB 时 test 要 +3.01 dB。`scripts/nulltest.ps1` 的 `-PanLawDb` 也是按 spike 口径写的(补偿 = 取反)。**这段是按源码推的,没有实跑过。**
  - 怎么查:§3 的 U-3。
  - 状态:⏳ 用户上机(跑之前统筹先定补偿口径)

- [ ] **D2 模式 B null test(engine vs follow)**
  - 判据:残差 RMS < −40 dBFS(S2-P5)。
  - 怎么查:§3 的 U-2。
  - 状态:⏳ 用户上机

- [ ] **D3 PANLAW-1..5 绿**
  - 实况:仓里没有以 PANLAW 命名的用例。PANLAW-1 由 `PAN-1 equal-power pan gains`(`tests/core/test_transition.cpp:25`)覆盖;`DUALPAN-1..3`、`WIDTH-1`、`MixMath stereo dual-pan + width` 覆盖 PANLAW-3 / 5 的公式层;**PANLAW-2 / 4 的渲染实测、PANLAW-5 的「width=50 与 width=100 总能量差 ≤0.05 dB」与「无极性反转」断言,没找到对应用例**(`git grep -n -i "PANLAW\|polarity" -- tests` 无相关用例)。
  - 状态:⏳ 统筹(补用例,或写 N/A 理由提请用户批)

- [ ] **D4 端到端响度对拍(L3)差 ≤ 0.2 LU**
  - 实况:`scvb_bench --render` 能出 wav,但「由特征预测的 10·log10(z_L+z_R)」与渲染结果的比对没有现成工具。
  - 状态:⏳ 统筹

- [ ] **D5 结果追加到 `audio/nulltest-log.md`**
  - 怎么查:D1 / D2 用 `scripts/nulltest.ps1` 跑,会自动把原始输出追加到该文件末尾;再在它的汇总表里补一行。
  - 状态:⏳ 用户上机(跑完把文件交统筹提交)

### E. 性能(10 §5)

- [ ] **E1 `scvb_bench --dsp` 对比上一 release 无 >10% 退化**
  - 首发没有上一 release ⇒ **本次建基线**:记下基线机器(10 §5.0 / U16)与两档结果,存 `docs/validation/perf/budget-log.md`(10 §0.4 的冻结路径,本 PR 未建)。
  - 怎么查:§3 的 U-4。
  - 状态:⏳ 用户上机

- [ ] **E2 PERF-1..17 全部在预算内,无红灯**
  - 怎么查:PERF-1..4 读 E1 的 `scvb_bench` 输出;PERF-5 / 6 / 7 读 Cubase 的 Audio Performance(10 §5.4 第 3 条,只在同一 DAW 内比);其余按 10 §5.3 各自口径。黄灯要记录并开 issue(10 §5.5)。步骤见 U-4、U-5。
  - 状态:⏳ 用户上机(统筹协助整理)

- [ ] **E3 MEM-1..7 在预算内;5 分钟稳态无泄漏**
  - 相关已知项:SL-445 拖 Q 滑杆时内存增长(J128;修复 PR #306 在飞)。
  - 怎么查:§3 的 U-5。
  - 状态:⏳ 用户上机

### F. DAW 矩阵(10 §3)

- [ ] **F1 矩阵跑完并填表**
  - 范围:按 J124,首发只验 Cubase(U14「每次 release 跑全矩阵」按此修订);REAPER 沿用 spike 证据、本版不复测;Ableton Live 12 / Studio One 6 标未验证(Tier 3);FL Studio 不在 v1 矩阵。
  - 怎么查:[daw-matrix.md](daw-matrix.md) 的 v0.9.0-rc.1 一节,Cubase 一行 13 格逐格填 P / F / N-A 与实测值;步骤见该节「Cubase 逐格怎么跑」与 §3 的 U-1。
  - 状态:⏳ 用户上机

- [ ] **F2 `daw-matrix.md` 新增本版本一节**
  - 证据:本 PR 已建该节,格子待填;填完才算。
  - 状态:⏳ 用户上机(随 F1)

- [x] **F3 `daw-support-tiers.md` 与 README 的 Tier 表同步**
  - 怎么查:对照 [daw-support-tiers.md](daw-support-tiers.md) §2、`docs/DAW_COMPATIBILITY.md` §4、`README.md` / `README.zh-CN.md` 的支持 DAW 表,DAW / 版本 / Tier 逐行一致。
  - 证据(`e3a7f5a1`):四处都是 Cubase 14 / 15 Tier 1、REAPER 7 Tier 2、Ableton Live 12 Tier 3、Studio One 6 Tier 3,FL Studio 不在 v1 矩阵。README 第 38 行写明它转贴自 DAW_COMPATIBILITY §4,那里是真源;本文件跟随。
  - 状态:✅ 已核

- [ ] **F4 MUTE / FRZ 行为与上一版逐字比对**
  - 首发没有上一版的逐字记录:S1 spike 的 C-10(solo / mute)与 C-11(Freeze / Render in Place)只记了「✅」,没记原话 ⇒ **本次在 RC 包上逐字记下,作为下一版的比对基线**。
  - 状态:⏳ 用户上机(随 F1 的 MUTE / FRZ 两格)

### G. 兼容性与数据安全

- [x] **G1 用上一 release 保存的工程在新版打开** —— N/A:首个公开版本,没有上一 release(2026-09-28 `gh api repos/synchain-oss/scvb/releases` 与 `/tags` 均为空)。参考:内部测试包之间「旧工程在新包打开」已在 v5.6.19 测试包 A-P8 验过(用户实测「过」),它不替代本项。
- [x] **G2 用新版保存的工程在上一 release 打开** —— N/A:理由同 G1。另:`docs/RELEASE.md` 发布说明模板的「升级须知」已写明新版工程在旧版(含内部测试包)里会被拒载。
- [ ] **G3 sidecar 缺失 / 篡改 / CoW 三条路径手测**
  - 实况:v1 出厂不再写 sidecar(SL-395,开关关),只保留读旧工程;自动化用例 `FEAT-SIDECAR-1..11`(`tests/core/test_state_features_roundtrip.cpp`)覆盖「仍能读 embedded=0」「删 sidecar 后特征缺失」「双开同 GUID copy-on-write」「路径穿越防护」等;**「篡改」没有同名用例**。
  - 建议:手测 N/A(新版不产生 sidecar,读路径有用例);认可前先核「篡改」由哪条用例兜着。
  - 状态:⏳ 统筹
- [x] **G4 golden 文件本版未变更** —— N/A:首个公开版本,没有「上一版」可比。首个公开 tag 起 ParamID 与 state 布局永久冻结(J123 / J21),打 tag 前的冻结评审见 L2。

### H. 文档

- [x] **H1 用户手册的红字前提齐全**
  - 查什么:人声轨路由指向总线、宿主 pan 居中、推子 0 dB;以及 J12 之后的措辞(连接健康时 Input 向下游输出静音是设计行为,没有健康 Output 时自动直通)。
  - 证据:`docs/USER_GUIDE.zh-CN.md` 九条规则里第 1 条(路由指向总线)、第 4 条(宿主 pan 居中)、第 3 条(J12 措辞)都在。「推子 0 dB」不在手册里(`grep -c -E "0 ?dB" docs/USER_GUIDE.zh-CN.md` = 0),**按 J45 它只是 null test 的可比性前提、不是产品要求**,所以不算缺。
  - 状态:✅ 已核

- [ ] **H2 ⚠ 单轨 Freeze / Render in Place 得静音文件的红字,三处一致**
  - 查什么:USER_GUIDE + KNOWN_ISSUES + UI 首次导出提示三处一致。
  - 证据:KNOWN_ISSUES `KI-4` 有(第 33 行起)。**USER_GUIDE 里没有**:`grep -c -i -E "freeze|render in place|静音文件|替换式|原素材"` 在 `USER_GUIDE.zh-CN.md` 为 0,在 `USER_GUIDE.md` 为 2,但那两行说的都是 SCVB 自己的冻结 P / V,不是这条。**UI 里没有首次导出提示**:同一模式在 `web/shared/i18n.js` 0 处。
  - 状态:⏳ 统筹(USER_GUIDE 补一处指向 KI-4;UI 提示做不做要用户裁)

- [ ] **H3 DAW 的 mute / solo / 推子对 SCVB 通路无效(J45)**
  - 证据:两份 USER_GUIDE、KNOWN_ISSUES、两份 README 都**没有**这条说明(`grep -i -E "\bmute\b|\bsolo\b|独奏"` 的命中只有 Input 自己「向下游输出静音」的描述)。S1 spike C-10 当时的判据就是「solo / mute 对 SCVB 通路失效」。
  - 状态:⏳ 统筹

- [x] **H4 同机同时只支持一个使用 SCVB 的工程**
  - 证据:`docs/KNOWN_ISSUES.md` KI-5(第 41 行起)。
  - 状态:✅ 已核

- [ ] **H5 Output 停摆直通兜底:三处一致 + L-5 / F-1 实测**
  - 证据:KNOWN_ISSUES KI-6 有;用户手册故障排查表「人声突然变成未平衡的原始声像」一行有(`docs/USER_GUIDE.zh-CN.md:203`,含 FL smart disable 规避);FL 作战卡在 masterPlan 03 §4.7,不在本仓。**载体实测 L-5(Live 设备停用)与 F-1(FL)没有跑** —— Live 未上机(J124),FL 不在 v1 矩阵;10 §6.1 写「任一红即不可勾」。
  - 状态:⏳ 统筹(按 J124 写 N/A 理由提请用户批,或补测)

- [ ] **H6 各 DAW 作战话术与截图与本版行为一致**
  - 实况:话术在 `docs/DAW_COMPATIBILITY.md` §2(每 DAW 一节);「03 §4 宿主专属界面提示」没做(发布盘点第二轮,待用户裁第 6 条);截图未核。
  - 状态:⏳ 统筹

- [ ] **H7 已知限制清单在 README 可见**
  - 查什么:FRZ / stem 静音产物、mute / solo 失效、单工程限制、上游 PDC、齐唱互相关偏差、sidecar 不随工程;「真立体声源延后 v2」一条已删([J57]);新增「Input 就地 gain 只做音量、不做声像」「stereo 轨默认不参与自动声像([J60])」。
  - 证据:两份 README 只在「文档」一节链到 `docs/KNOWN_ISSUES.md`(`README.zh-CN.md:96`),正文没有限制清单;「真立体声源延后 v2」0 处;「Input 就地 gain 只做音量」在 USER_GUIDE「已知限制」一节有。**[J60] 那条已被 J83 推翻**(现在所有轨默认参与,见红字第 7 条),要按 J83 改写后再判。
  - 状态:⏳ 统筹(先定「链接算不算可见」,再补缺的几条)

- [ ] **H8 `docs/SCVB_CONTRACT.md` 与代码一致,且是唯一一份桥契约**
  - 证据:`docs/` 下契约文件只有 `SCVB_CONTRACT.md` 与 `IPC_CONTRACT.md`(后者是 IPC 契约,不是桥契约),没有 `WEB_UI_CONTRACT.md`;机器一致性由 `check-bridge-parity.mjs`(CI `docs-truth`)守。版本行是 `1.0(已冻结)`;J105 只对 #281 那一次豁免了升版本号,**其余契约变更是否要求升号没核**。
  - 状态:⏳ 统筹(核版本号)

### I. 许可与合规(GPLv3)

- [x] **I1 `LICENSE` 为 GPLv3 全文**
  - 证据:`LICENSE` 674 行,开头是 `GNU GENERAL PUBLIC LICENSE` / `Version 3, 29 June 2007`。
  - 状态:✅ 已核

- [ ] **I2 第三方组件清单与许可在仓库根 `THIRD-PARTY-NOTICES.md`**
  - 证据:版本都对得上 —— JUCE 8.0.8 ↔ `.juce-version`;WebView2 SDK 1.0.2957.106 ↔ `CMakeLists.txt` 的 `WEBVIEW2_VERSION`;Catch2 v3.5.4、libebur128 v1.2.6 ↔ `tests/CMakeLists.txt` 的 `GIT_TAG`;pluginval v1.0.4 ↔ `.pluginval-version`。**许可证本身有缺口**(发布盘点第二轮):`LICENSES/` 缺 BSD-3-Clause 全文、VST3 SDK 许可证写成 MIT、JUCE 静态链入的 libjpeg / HarfBuzz / SheenBidi / libpng / zlib 未登记 —— 由 PR #314 补,在飞。
  - 状态:⏳ 统筹(#314 合并后复核)

- [ ] **I3 README 与 release notes 写明 GPLv3 源码获取途径**
  - 证据:README 已写(`README.zh-CN.md` 第 13 行许可证、第 51 行 Releases、第 83 行起从源码构建,含 `git clone https://github.com/synchain-oss/scvb.git`);release notes 模板有「自行从源码构建」链接;`INSTALL.txt` 精确到 tag 的源码地址由 #297 的 `package.ps1` 生成。
  - 状态:⏳ 统筹(release notes 定稿时核)

- [ ] **I4 二进制未内嵌不兼容许可的资源(字体尤其)**
  - 证据:字体都是 OFL-1.1(`THIRD-PARTY-NOTICES.md` 字体四行);OFL §3 保留名由 CI `compliance` 的 `check-font-names.py` 守;发布盘点第二轮指出「字体版权行指向不存在的表」,由 #314 修。
  - 状态:⏳ 统筹(#314 合并后复核)

### J. 打包与发布

- [ ] **J1 `SCVB-vX.Y.Z-win64.zip` 内含全部 `.vst3` bundle**
  - 10 §6.1 写「两个」,按 J137 为**三个**(Monitor 可选)。
  - 怎么查:tag 推上去后 `release.yml` 的 `release` job 由 `scripts/package.ps1` 打包并解包断言;再下载草稿 Release 的 zip 看一眼。
  - 状态:⏳ CI(依赖 #297)

- [ ] **J2 zip 内合规文件组断言通过**
  - 查什么:zip 根目录有 `LICENSE.txt`(GPLv3 全文)、`THIRD-PARTY-NOTICES.md`、`LICENSES/OFL-1.1.txt`、`INSTALL.txt`(含精确到 tag 的源码 URL);`LICENSE-EXCEPTION.md` 按 U2 **不附**。
  - 实况:#297 的 `package.ps1` 在 `verify-tag` 阶段做许可证全文覆盖检查,BSD-3-Clause 全文补上(#314)之前 rc tag 会停在 preflight。
  - 状态:⏳ CI(依赖 #297、#314)

- [ ] **J3 干净 Windows 11 上解压安装,DAW 扫得到并正常工作**
  - 怎么查:§3 的 U-7。
  - 状态:⏳ 用户上机

- [ ] **J4 SmartScreen 提示说明写进 release notes**
  - 证据:`docs/RELEASE.md` 发布说明模板里已有「本项目当前未做代码签名……解除锁定」一段(U13 不签名)。
  - 状态:⏳ 统筹(release notes 定稿时保留)

- [ ] **J5 GitHub Release 建为草稿,人工核对 notes 后再发布**
  - 实况:#297 的 `release.yml` 只建草稿(rc 自动勾 pre-release),发布手动;发布留用户(J121)。
  - 状态:⏳ 用户(步骤见 U-8)

- [ ] **J6 tag 保护生效(`v*`,06 §7.2)**
  - 证据:2026-09-28 `gh api repos/synchain-oss/scvb/rulesets` 返回 `[]`,旧式 tag protection 接口 404 ⇒ **目前没有 `v*` tag 保护**。
  - 状态:⏳ 用户(仓库设置,步骤见 U-8)

### K. 回滚预案

- [x] **K1 上一 release 的 zip 仍可下载** —— N/A:首个公开版本,没有上一 release。
- [ ] **K2 release notes 写明如何回退**
  - 实况:模板「升级须知」写了新版工程在旧版会被拒载,但**没有「如何回退」步骤**(卸载新版 `.vst3` 目录、装回旧版)。
  - 状态:⏳ 统筹(模板补回退段,或本版写 N/A 理由)

### L. 本版追加(不在 10 §6.1,来自发版流程与裁定)

- [ ] **L1 fr 红字审校完成**
  - 查什么:`docs/hard-rules.i18n.json` 的 `frReview.status` 为 `reviewed`(`docs/RELEASE.md` 发版清单第 5 步;J127 改为 AI 三语交叉核对,用户授权)。
  - 证据:当前 `frReview.status` = `pending`;置 `reviewed` 的 PR #299 在飞。
  - 状态:⏳ 统筹

- [ ] **L2 参数面冻结评审完成**
  - 查什么:J123 —— 第一个公开 tag 一打,ParamID 与 state 布局即永久冻结(J21),打 tag 前必须完成冻结评审。
  - 证据:发布盘点把它列为统筹派工项,还没有产出。
  - 状态:⏳ 统筹

- [ ] **L3 打 tag 前在 tag 提交上重做发布盘点与漏项全扫**
  - 查什么:J129 / J139 —— 本文件与发布盘点都是阶段性结论;打 tag 前在当时的 tip 上重跑本文件全部 ✅ 项的「怎么查」、重扫漏项。
  - 状态:⏳ 统筹

## 3. 用户上机步骤

所有上机项一律用 **RC tag 触发的草稿 Release 里那个 zip**(或同一提交的 dispatch 产物,artifact 名里的 40 位 sha 与 tag 提交逐字一致)。**测之前先把要用的工程另存一份副本**,FRZ 一格会覆盖素材。做到哪算哪,回报时说停在哪一步。

### U-0 准备工具(一次;也可以由统筹构建好把 exe 交给你)

在仓库根目录、检出到 RC tag,构建验证工具与测试(`--parallel 1` 省内存;libebur128 要联网拉取):

```powershell
git fetch --tags
git checkout v0.9.0-rc.1
cmake -S . -B build-val -DJUCE_PATH=<JUCE 8.0.8 所在目录> -DSCVB_TESTS_WITH_EBUR128=ON
cmake --build build-val --config Release --parallel 1 --target scvb_tests scvb_ipc_tests scvb_nulltest scvb_bench scvb_diag
```

产物在 `build-val\tests\Release\`(`scvb_tests.exe`)、`build-val\tests\ipc\Release\`(`scvb_ipc_tests.exe`)与 `build-val\tests\tools\Release\`(另外三个)。

### U-1 Cubase 矩阵(F1 / F2 / F4)

按 [daw-matrix.md](daw-matrix.md) v0.9.0-rc.1 一节的「Cubase 逐格怎么跑」逐格做,结果填进该节的矩阵。想看时间线缺口的计数,播放时另开一个 PowerShell 跑:

```powershell
build-val\tests\tools\Release\scvb_diag.exe --out diag-rc1.csv --group 1
```

(`--group` 填测试工程用的组号,A=1 … H=8;按 Ctrl+C 停。)

### U-2 模式 B null test(D2 / D5)

1. 打开一份已采集、已分析的测试工程副本(自有多轨人声素材,不入库);人声轨与总线 pan 居中,主输出无限制器、无 dither,总线上除 SCVB Output 外没有别的处理。
2. 用左右定位器圈一段 30–60 秒的区间(含独唱与重叠段),记下起止。
3. Output 输出开关 **ON**,Cubase 自动化保持 Read —— File → Export → Audio Mixdown,32-bit float,导出 `engine_C.wav`。
4. 打开 Output 窗口,轨道自动化设 **Write**(或 Latch),从区间起点播到终点,停,切回 **Read**(打印的车道在 Ins 隐藏车道下,见 `docs/DAW_COMPATIBILITY.md` §2.1;若出现加载守卫横幅,先点「继续写入自动化」)。
5. Output 输出开关 **OFF**,同一区间、同一导出设置导出 `follow_D.wav`。
6. 比对并自动写日志:

   ```powershell
   pwsh scripts/nulltest.ps1 engine_C.wav follow_D.wav -Align -BuildDir build-val
   ```

7. 判据:逐声道残差 RMS < −40 dBFS。不过就分开打印(只打印 pan、只打印 vol)各比一次,定位是哪条链(10 §4.2)。
8. 把 `docs/validation/audio/nulltest-log.md` 新追加的那段交给统筹,并在它的汇总表补一行。

### U-3 模式 A null test(D1 / D5,等统筹定补偿口径后再做)

1. 新建 48 kHz 工程,导入同一组人声(有 stereo 轨的话建议 13 mono + 2 stereo;先只放 mono 跑一遍,再加 stereo 跑完整格),全部 pan 居中、推子 0 dB,送同一条 stereo Group(VOX BUS),总线同样居中、0 dB。记下 Project → Project Setup 里的 Stereo Pan Law(Cubase 默认 −3 dB Equal Power)。
2. **不装 SCVB**,导出区间 → `ref_A.wav`(32-bit float)。
3. 每条人声轨插件链最后一格插 SCVB Input,总线第一格插 SCVB Output。**不分析、输出开关保持 OFF、不写任何自动化**(此时 pan 0 / vol 0 dB / width 100 / MS Balance 0 / Lead Select 0 都是参数默认值,见 `tests/golden/params_v0.tsv`)。确认各 Input 显示已连接、Output 没有「时间线缺口」横幅,导出同一区间 → `test_B.wav`。
4. 比对(补偿值以统筹定的为准;下面是按源码推的,没实跑过):
   - 宿主 −3 dB Equal Power:`pwsh scripts/nulltest.ps1 ref_A.wav test_B.wav -PanLawDb 0 -Align -BuildDir build-val`
   - 宿主 0 dB:`pwsh scripts/nulltest.ps1 ref_A.wav test_B.wav -PanLawDb -3.01 -Align -BuildDir build-val`
5. 判据:样本偏移 0,残差峰值 < −120 dBFS(理想为按位相等)。**不过就把工具输出的逐声道残差与偏移原样回报,别凭听感调增益去凑**(10 §4.2 明令禁止)。汇总表的「宿主 pan law」一列写 Cubase 里的真实设置。

### U-4 性能基线 `scvb_bench`(E1,E2 的 PERF-1..4)

在性能参考机上跑(U16:275HX / 32GB 笔记本),插电源、关掉 DAW 与其他重负载程序:

```powershell
build-val\tests\tools\Release\scvb_bench.exe --dsp --fs 48000 --block 512 --tracks 15 --stereo-tracks 2 --blocks 100000 --json bench-48k-512.json
build-val\tests\tools\Release\scvb_bench.exe --dsp --fs 96000 --block 128 --tracks 15 --stereo-tracks 2 --blocks 100000 --json bench-96k-128.json
```

一并记下:CPU 型号 / 核数 / 基频、内存、Windows 版本号、音频接口与驱动。两个 json 与这些信息交给统筹,由统筹建 `docs/validation/perf/budget-log.md` 并对照 10 §5.1 判 PERF-1..4。

### U-5 宿主性能与内存(E2 的 PERF-5..7,E3)

用一份 15 条 Input + 1 个 Output 的工程:

1. **MEM-7**:插件都加载好、所有编辑器关着,在任务管理器记下 Cubase 进程内存;对比不插 SCVB 的同一工程,差值 ≤ 280 MB。
2. **MEM-5**:打开 Output 编辑器,任务管理器里 Cubase 下面的 WebView2 进程内存,刚打开时 ≤ 250 MB,放着 5 分钟后增量 ≤ 20 MB。
3. **PERF-5 / 6**:Studio → Audio Performance,播放全曲,记平均与峰值;再把 SCVB 全部停用播一遍作对照。
4. **PERF-7**:同 3,但播放时开着 Output 编辑器(UI 全负载),记增量。

### U-6 `[reference]` 对拍(B4)

U-0 构建时已带 `-DSCVB_TESTS_WITH_EBUR128=ON`:

```powershell
build-val\tests\Release\scvb_tests.exe "[reference]" --durations yes
```

判据:全过,且输出里**没有** `SCVB_TESTS_WITH_EBUR128=OFF` 字样(有这句 = 构建时没开,跑的是占位)。

### U-7 干净 Windows 11 安装(J3)

1. 准备一台**没装** Visual Studio / Windows SDK / 本仓构建环境的 Windows 11(虚拟机也行),装好 Cubase 15;记下 Windows 版本号。
2. 从草稿 Release 下载 zip 与 `.sha256`(文件名以草稿里的为准),`Get-FileHash <zip> -Algorithm SHA256` 与 `.sha256` 内容逐字一致。
3. **解压前**右键 zip → 属性 → 勾「解除锁定」→ 确定(用户手册「安装」一节)。
4. 把三个 `.vst3` 文件夹整个复制到 `C:\Program Files\Common Files\VST3\`。
5. 打开 Cubase,插件管理器里 SCVB Input / Output / Monitor 三个都在;按用户手册「5 分钟上手」装一对跑通:Input 显示已连接,Output 编辑器正常打开(不是空白窗口或兜底面板),能采集、分析,输出开关 ON 能听到平衡后的结果。
6. 记下:有没有 SmartScreen / 「未知发布者」提示,提示的原文。

### U-8 仓库设置与发布(J5 / J6)

1. **tag 保护**(J6):仓库 Settings → Rules → Rulesets → New tag ruleset,目标 `v*`,只允许维护者创建,禁止删除与强推(06 §7.2:禁止非 owner 创建 / 删除)。
2. **发布**(J5):本清单全部勾上之后,在 Releases 页打开 `v0.9.0-rc.1` 草稿,按 `docs/RELEASE.md` 的模板核对正文(SHA-256 从 CI 的 `package-summary.md` 复制),确认勾着 pre-release,再发布。
