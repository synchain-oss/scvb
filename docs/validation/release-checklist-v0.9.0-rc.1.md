# 发布前验证清单 —— v0.9.0-rc.1

> 状态:进行中 —— **任何一项未勾不得发布**
> 最后更新:2026-09-30(SL-581 打 tag 前收口:CHANGELOG 的 `[Unreleased]` 归入 `[0.9.0-rc.1]` 版本节之后按 A2 的「怎么查」重跑,A2 改回 ⏳ 统筹;D1 / D2 / D5 按第三包的导出由统筹比对并记录后勾上;E2 按 PERF-2 安静重跑勾上。这一轮的证据标「(SL-581)」,在 `feature/v1` @ `518ab466`(#345 合并后)+ 本 PR 的改动上核对;下面是此前的记录)2026-09-29(SL-575 第二轮,按 J178 落实统筹口径:C3 / D4 / E3 / H8 勾上、E2 改回未勾;H2 随 #344(SL-574)合入勾上;I2 按 #340 更新许可证份数。这一轮的证据标「(J178…)」,在 `feature/v1` @ `17037be4`(#344 合并后)+ #345 的改动上核对)2026-09-29(SL-575 按 J175 把「⏳ 统筹」各项逐格收口、把 J175 裁定的用户项改了状态,这一轮的证据标「(SL-575,`61b7575d`)」,在 `feature/v1` @ `61b7575d`(#338 合并后)+ #345 的改动上核对;下面是此前的记录)2026-09-28(核对基线:`feature/v1` @ `e3a7f5a1`;切 rc.1 版本节的那个 PR 在 `5b1c908e`(#332 合并后)+ 该 PR 自身的改动上刷新了一轮,刷新过的项在证据里标「(`5b1c908e`)」;A2 又在 `19267527`(#333 合并后)+ #334 上刷新过一次,证据里标「(`19267527` + #334)」)
> 来源:masterPlan 10 §6.1 的 A–K 逐项照搬;L 段是本版裁定追加的硬门。
> 相关:上机矩阵 [daw-matrix.md](daw-matrix.md) · 支持等级 [daw-support-tiers.md](daw-support-tiers.md) · null test 记录 [audio/nulltest-log.md](audio/nulltest-log.md)

## 0. 怎么用

- **版本路线(J123)**:`CMakeLists.txt` 改为 `0.9.0`,打 tag `v0.9.0-rc.1`;rc 测过后再发 `v1.0.0`,届时另建 `release-checklist-v1.0.0.md`,**不沿用本文件的勾**。
- **允许「N/A + 理由」**(10 §6.0),理由写在该项里。
- 每项分四行:**查什么 / 怎么查 / 证据 / 状态**。状态口径:
  - **✅ 已核**:在核对基线上亲自看过证据(`e3a7f5a1`;证据里标了「(`5b1c908e`)」的是在 `5b1c908e` 上),命令与结果写在「证据」里;
  - **⏳ CI**:由 tag 所指提交上的 CI 给出(tag 推上去后 `release.yml` 调用 `build-vst3.yml`,见 #297);此前的绿 run 只作参考,列在证据里;
  - **⏳ 统筹**:统筹要做的核对、派工,或要提请用户裁定;
  - **⏳ 用户上机 / ⏳ 用户**:只能用户在真机或仓库设置里做,步骤见 §3;**⏳ 用户(第三包)** = J175 裁定随第三个测试包由用户上机做的那几项(D1 / D2 / D5);
  - **N/A**:不适用,理由写在该项里。
- 打勾规则:只有 ✅ 与 N/A 勾 `[x]`,⏳ 一律 `[ ]`。**N/A 也算「勾了」**:头部「任何一项未勾不得发布」里的「勾」包括 N/A,前提是该项写明了理由与依据(裁定号,或本文件里的推导);没写理由的 N/A 不算。按 J175 标的 N/A 只对 rc.1 成立,v1.0.0 另建清单时逐项重新判定(见上一条)。
- 文中的「文件:行号」除标了 `5b1c908e` 与「(SL-575,`61b7575d`)」的以外都是 `e3a7f5a1` 上的位置,别的 PR 合入后会漂;以同处写出的用例名 / 小节标题为准。
- **本文件的结论是阶段性的**(J129 / J139):打 tag 前,统筹要在 tag 所指提交上把全部 ✅ 项的「怎么查」重跑一遍,结果变了就改回 ⏳;再全量重扫一次漏项(L3)。

## 1. 汇总

截至 2026-09-30(SL-581 打 tag 前收口之后),共 54 项(10 §6.1 的 A–K 51 项 + 本版追加 L 3 项):

| 段 | 项数 | ✅ / N/A | ⏳ 用户 | ⏳ 统筹 | ⏳ CI |
| --- | --- | --- | --- | --- | --- |
| A 代码与版本 | 5 | 3(A3 为 N/A) | 0 | 1 | 1 |
| B 自动化门禁 | 6 | 2(B6 为 N/A) | 0 | 0 | 4 |
| C UI 与无障碍 | 4 | 2 | 0 | 0 | 2 |
| D 音质 | 5 | 5(D4 为 N/A) | 0 | 0 | 0 |
| E 性能 | 3 | 3 | 0 | 0 | 0 |
| F DAW 矩阵 | 4 | 4(F1 / F2 / F4 为 N/A) | 0 | 0 | 0 |
| G 兼容性与数据安全 | 4 | 4(均 N/A) | 0 | 0 | 0 |
| H 文档 | 8 | 8(H5 为 N/A) | 0 | 0 | 0 |
| I 许可与合规 | 4 | 4 | 0 | 0 | 0 |
| J 打包与发布 | 6 | 2(J3 为 N/A) | 2 | 0 | 2 |
| K 回滚预案 | 2 | 2(K1 为 N/A) | 0 | 0 | 0 |
| L 本版追加 | 3 | 1 | 0 | 2 | 0 |
| **合计** | **54** | **40** | **2** | **3** | **9** |

括号里只列整格为 N/A 的项;C3、E2、E3、H6 是一格里一半 ✅、一半 N/A,计在 ✅ / N/A 一列,各自的状态行写明哪一半是 N/A。

「⏳ 用户」2 项:J5 发布草稿、J6 tag 保护,都在 GitHub 上操作(U-8)。D1、D2、D5 已随第三包做完:用户导出 wav,统筹比对并写进 nulltest-log(J175;D1 的补偿口径按 J178①)。原先的 11 项上机项,按 J175:B4、E1 由统筹在本机做掉(✅);F1、F2、F4、J3 写 N/A(rc.1 不跑,v1.0.0 前再议);E2 的上机部分(PERF-5..17)与 E3 的上机部分(MEM-5 / MEM-6 / MEM-7 与 5 分钟稳态)同样 N/A。E3 的定容项 MEM-1 / MEM-3 按 J178③ 修订预算后勾上;E2 按 PERF-2 安静重跑(J178④)勾上。

「⏳ 统筹」3 项:
- **A2**:CHANGELOG 归节之后重跑,⚠️ 契约变更里 #343 那条没有 `docs/contract-changes/` 文档,补写还是写明豁免待统筹定。
- **L2**:在 tag 提交上重跑冻结评审,并请用户确认。
- **L3**:在 tag 提交上重做发布盘点与漏项全扫。

上一轮计在这里的 E2 已勾上。更早计在这里的 C3、D4、E3、H8 分别按 J178⑤ / ⑥ / ③ / ⑦ 收口,H2 随 #344(SL-574)合入勾上。

## 2. 清单

### A. 代码与版本

- [ ] **A1 版本号与 tag 一致**
  - 查什么:顶层 `CMakeLists.txt` 的 `project(SCVB VERSION …)` 与 tag 一致。
  - 怎么查:`git show v0.9.0-rc.1:CMakeLists.txt | grep -n "project(SCVB"` 应为 `VERSION 0.9.0`;tag 推上去后 `release.yml` 的 `verify-tag` job 绿(#297 的 `scripts/check-release-tag.ps1`:rc tag 只比对 `X.Y.Z` 部分,rc 与正式版对应同一个 CMake 版本)。
  - 证据:`e3a7f5a1` 上 `CMakeLists.txt:8` 是 `project(SCVB VERSION 0.1.0)`;CI run 36364806176 的 pluginval 输出也是 `SCVB Input v0.1.0`。#331 已改为 `project(SCVB VERSION 0.9.0)`,在它的 head 上本地跑 `scripts/check-release-tag.ps1 -Tag v0.9.0-rc.1` 退出 0(kind=rc),`-Tag v1.0.0` 退出 1。
    (`5b1c908e`)`CMakeLists.txt:8` 是 `project(SCVB VERSION 0.9.0)`;切版 PR #333 的 head 上 `check-release-tag.ps1 -SelfTest` 19 格全过、`-Tag v0.9.0-rc.1` 退出 0(kind=rc);#332 的 CI run 36489830961 里 pluginval 输出 `SCVB Input v0.9.0` / `SCVB Monitor v0.9.0` / `SCVB Output v0.9.0`。另:核心库自报的版本串此前写死 `0.1.0`,#333 改为从 CMake 注入。
  - 状态:⏳ CI(版本号与判据都已就位,剩 tag 推上去后 `verify-tag` 那一跑)

- [ ] **A2 CHANGELOG 已更新**
  - 查什么:含破坏性变更、已知限制、DAW Tier 变化。
  - 怎么查:按 J123 在出 RC 时把 `## [Unreleased]` 改为 0.9.0 的版本节(节名与日期按 `docs/RELEASE.md` 发版清单第 2 步);`### ⚠️ 契约变更` 每条都带 `docs/contract-changes/` 文档;每条带 PR 号(CI `check-changelog-drafts.mjs` 判)。
  - 证据:`[Unreleased]` 已有 `### ⚠️ 契约变更` 小节;Tier 变化与三条已知限制已由 #298 写进 `### 变更`(以「README 与用户手册:安装改为从本仓库 Releases 页下载」开头的那条)。版本节尚未建。
    (`5b1c908e` + #333)#333 已把 `[Unreleased]` 切成 `## [0.9.0-rc.1] - 2026-09-29`(条目逐字搬,文末对比链接补齐,留空的 `[Unreleased]`)。按条目逐条扫(一条 = 行首 `- ` 起到下一个空行 / 标题):⚠️ 契约变更 28、新增 53、变更 45、修复 110。**两处没过**:
    ① 两条没有任何 PR 号 —— ⚠️ 契约变更「VAD 五项与过渡时间随工程保存」(SL-416)与修复「快速起停之后,『宿主自动化在写』徽标仍会在播放中途消失」;
    ② ⚠️ 契约变更里五条没有 `docs/contract-changes/` 文档链接 ——「插件自有撤销栈的深度与内存改为显式定参」(#152)、「你画的 pan 增益曲线现在真的会响」(#270)、「范围设置……写明『不随工程保存』」与「Input 的 `idle` 态定义补全」(#281,两条都自述为契约文本追上既有行为)、「轨道页的设置现在会随工程保存」(#290;`docs/contract-changes/20260927-sl472-channel-config-persist.md` 在仓里,条目没链)。其余 23 条引用的 22 份文档都在仓里。`check-changelog-drafts.mjs` 绿(它只判括号里的号落没落地,不判「有没有号」)。
    (`19267527` + #334)两处都补了,条目正文其余一字未改(只加号、加文档路径,#281 两条为放路径各多断一行)。
    ① 补号:「VAD 五项……」是 #263 —— `git log -S "VAD 五项与过渡时间随工程保存" -- CHANGELOG.md` 只命中 `930e7b87`,标题尾部 `(#263)`;「快速起停之后……」是 #236 —— 同法以该条正文「连续停满 0.5 秒」只命中 `f36cc4ab`,标题尾部 `(#236)`。两个 PR 在 `gh pr view` 里都是 MERGED,标题卡号(SL-416 / SL-356)与条目一致。
    ② 补链:#152 那条 → `20260827-sl209-analyze-undoable.md`(该文「撤销预算的定参与推导」一节,64 MiB / 16 步 / ≈174 步的推导都在那里);#281 两条 → `20260926-j101-j103-contract-withdrawals.md`(J102 `global.range` 移出落盘树、J103 `idle` 定义句两节;该文由 #281 新增);#290 那条 → `20260927-sl472-channel-config-persist.md`(由 #290 新增)。**#270 合并时没有写变更文档**:`git show --stat 2885539e` 里没有 `docs/contract-changes/` 下的文件,该目录 grep `SL-442` / `442` / `只画不响` 全 0 命中;它没碰冻结契约文件,branch-gate 当时不要求。#334 补写 `docs/contract-changes/20260919-sl442-pan-curve-realtime.md`,只登记 #270 合并时的事实(取自合并提交与 PR 描述),条目链到它。
    复扫(同上口径,另把「下一个 `- ` 行」也算作条目边界;判据:条目里出现 `#数字` 即算有 PR 号,⚠️ 契约变更条目里出现 `docs/contract-changes/<8 位日期>-….md` 即算有文档,并逐个检查文件存在):四节条数 28 / 53 / 45 / 110 不变;没有 PR 号的 0 条(改前 2 条);⚠️ 契约变更里没有文档路径的 0 条(改前 5 条);引用 24 份不同的文档,全部在仓里。`node scripts/check-changelog-drafts.mjs` 退出 0(`#334` 大于当前最大落地号,按本 PR 自己的号打 WARN 放过)。这次复扫不验「号与条目内容是否对得上」之外的东西,除上面两条补号外没有逐条回核其余 234 条的号。
    (SL-581)按发版清单第 2 步把 `[Unreleased]` 正文里 #334–#346 的条目归入 `## [0.9.0-rc.1]`,版本节日期改为 2026-09-30,`[Unreleased]` 留新增 / 变更 / 修复三个空小节(同 #333)。
    - **搬了什么**:⚠️ 契约变更 4 条、变更 7 条、修复 5 条,分别接在版本节同名小节末尾;新增 1 条(#344,Input 导览多一步)接在「新增」下「界面」组的末尾。条目正文逐字未改。
    - **没搬的一条**:变更里 #334 的「`0.9.0-rc.1` 版本节补齐出处」那条没有带进版本节。它记的是对这个版本节本身的订正;搬进去之后,它说明的是自己所在的这一节,而这一节在 tag 之前从没发布过,不存在「订正之前」的版本可比。#333 切版时对条目的订正(#331 那条的 `stage` 改 `staging`)也是原地改、没有另记条目。发布说明的「本次更新」从版本节原样复制,带进去的话,这条记账会出现在给用户看的说明里。它补写的变更文档仍由 #270 那条链着,#334 的号仍在修复一节(Output 页脚版本号)里。
    - **原地改的两处**:#320 与 #331 两条里的「说明文档」改成界面原文「文档」,与 #345 那条对 KI-7 / RELEASE.md 的订正同口径。
    - **复扫**(口径同上一段):四节条数 32 / 54 / 52 / 115(改前 28 / 53 / 45 / 110,多出的正是搬进来的 4 / 1 / 7 / 5 条);没有 PR 号的 0 条;引用 27 份不同的变更文档,全部在仓里。
    - **⚠️ 契约变更里没有变更文档路径的 1 条**:#343「新插入的 SCVB Output,输出开关默认改为『跟随宿主』(关)」。该 PR 的描述写明冻结契约文档零改动,它们都没写这一项的默认值,所以不写变更文档、不挂 `status/frozen-contract`。本项「怎么查」与 `docs/RELEASE.md` 发版清单第 1 步都要求契约变更条目各自有 `docs/contract-changes/` 文档,两者字面不符。
    - `node scripts/check-changelog-drafts.mjs` 与 `--self-test` 均退出 0。
  - 状态:⏳ 统筹(#343 那条缺变更文档:照 #334 给 #270 补文档的做法补写一份,或在本项写明这类条目的豁免,二选一;本项此前在 `19267527` + #334 上为 ✅)

- [x] **A3 带破坏性标签的 PR 均经用户显式批准**
  - 查什么:10 §6.1 写的是 `breaking:params` / `breaking:ipc-abi` / `breaking:state` 三个标签。**本仓没有这三个标签**(`gh label list` 无命中),实际用的是 `status/frozen-contract`。
  - 怎么查:`gh pr list -R synchain-oss/scvb --state all --label status/frozen-contract`,已合并的逐个对到用户裁定(J 号)或 PR 里的用户批准记录。
  - 证据(2026-09-28):带该标签的 PR 合并 41、关闭未合 9、在飞 7;`docs/contract-changes/` 下 41 份变更文档(另有 `TEMPLATE.md`)。
    (`5b1c908e`,2026-09-28 晚)合并 61、关闭未合 9、在飞 0(`gh pr list --state merged|closed|open --label status/frozen-contract`,closed 70 含合并的 61);`docs/contract-changes/` 下 60 份(不含 `TEMPLATE.md`)。
    (SL-575,`61b7575d`)合并 63(多了 #337 / #338)、关闭未合 9、在飞 0。**本项对 rc.1 写 N/A**:10 §6.1 这一条防的是「破坏已发布的参数面 / IPC / state 的改动没经用户点头就发出去」,前提是有一个已发布的版本;首发之前没有已发布的东西可以被破坏 —— J21 定的冻结点就是首个公开 rc,G1 / G2 / G4 按同一理由写了 N/A。rc.1 之后,碰冻结契约的 PR 照本项原义逐个对到用户批准。
    顺带按上面的「怎么查」把 63 个 PR 的标题与描述扫了一遍,**只看有没有引用裁定号,没有逐个核引用的那条裁定是不是就批准了该 PR 的契约改动**:50 个引用了至少一条「来源」列以「用户」开头的裁定(masterPlan `plan/adjudications.md`);13 个没有引用任何裁定号,其中 #231 / #259 / #262 / #264 的描述写了用户的裁定或批准,#232 / #240 / #261 / #263 写的是统筹裁定,#41 / #81 / #87 / #238 / #258 没写批准来源。
  - 状态:N/A(J21:首个公开 rc 才是冻结点,此前没有可被破坏的已发布版本;统筹认可,J178⑧)

- [x] **A4 `spikes/` 为空、无 spike 残留**
  - 查什么:`spikes/` 目录为空(J16);无 spike 调试代码(`SpikeCurve`、`--flap-heartbeat`、FPS 角标、processBlock 计时)。`tests/tools/scvb_diag` 是常驻工具,不删。
  - 怎么查:`git ls-tree -r --name-only HEAD spikes/`;`git grep -n "SpikeCurve\|flap-heartbeat" -- src tests web`;`git grep -n -i "fps" -- src`。
  - 证据:`spikes/` 下只剩 `spikes/README.md` 一个文件(内容仍写「S2–S4 的验证代码放这里」,已过期);`SpikeCurve` 只在 `tests/tools/scvb_bench.cpp:154` 的一行注释里,`flap-heartbeat` 0 处;`src/` 里 `fps` 的命中全是特征指纹常量 `kFp*`,不是帧率角标。
    (SL-575,`61b7575d`)#345 删掉 `spikes/README.md`,`spikes/` 随之从仓库消失;`scripts/check-native-paths.mjs` 顶层白名单里的 `spikes/` 一并删掉(否则它按「清单里列着、仓库里已经没有」报僵尸条目),改后 `node scripts/check-native-paths.mjs` 通过。上面三条 grep 重跑:`git ls-tree -r --name-only HEAD spikes/` 为空;`SpikeCurve` 仍只有 `scvb_bench.cpp` 那一行注释(说它造的测试曲线与 S2 的 SpikeCurve 同构;`scvb_bench` 是常驻工具);`flap-heartbeat` 0 处;`src/` 里不分大小写的 `fps` 命中仍全是特征指纹的 `kFp*` / `fp*` 标识符;processBlock 计时的形态 `QueryPerformanceCounter` 在 `src tests web` 下 0 处。
  - 状态:✅ 已核

- [x] **A5 git 历史无 secret**
  - 查什么:secret scanning 与 push protection 已开(06 §2.2);历史里没有密钥。
  - 怎么查:`gh api repos/synchain-oss/scvb -q .security_and_analysis`;`gh api "repos/synchain-oss/scvb/secret-scanning/alerts?state=open" -q length`。
  - 证据(2026-09-28):`secret_scanning` 与 `secret_scanning_push_protection` 均为 `enabled`;开放告警 0;全历史泄漏扫描 SL-268(2026-09-28)无密钥;每个 PR 的 `compliance` job 另跑 gitleaks(扫工作区)。(`5b1c908e`)同两条命令重跑,结果相同。
  - 状态:✅ 已核

### B. 自动化门禁(L0–L2)

- [ ] **B1 CI `build-and-validate` 绿**
  - 查什么:构建(/W4 零 warning)+ ctest 全绿 + pluginval strictness 5。10 §6.1 写「两个 bundle」,按 J137 现为**三个**(Input / Output / Monitor)。
  - 怎么查:tag 所指提交的 `release.yml` → `build` job(调用 `build-vst3.yml`)全绿;日志里 `100% tests passed` 且 pluginval 三行 `SUCCESS`。
  - 证据(参考,都不是 tag 提交):`feature/v1` 最近一次 dispatch 全量 run 36364806176(`72e4501`,即 v5.6.19 测试包):ctest 7/7 个套件通过,pluginval `--strictness-level 5 --skip-gui-tests` 三个 bundle 各 `SUCCESS`;最近一次 PR 全量 run 36403067073(#298 的 head `b220fc3`):同样 7/7 + 三个 `SUCCESS`。`e3a7f5a1` 本身没有构建 run。
    (`5b1c908e`)#332 的 PR 全量 run 36489830961:`100% tests passed, 0 tests failed out of 7`,三个 bundle `v0.9.0` 各 `SUCCESS`,日志里 `warning C` 0 条。
  - 状态:⏳ CI

- [ ] **B2 CI `clang-format` 绿(含桥契约一致性)**
  - 查什么:10 §6.1 把桥契约一致性写在 clang-format 里;现行 workflow 里它在 `docs-truth` job(`check-bridge-parity.mjs`)。两个 required check 都要绿。
  - 怎么查:合入 tag 提交的那个 PR 的 Format workflow。
  - 证据(参考):#298 head 的 Format run 36403067004:`clang-format`、`docs-truth`、`web-smoke` 均绿。(`5b1c908e`)#332 的 Format run 36489830978 同样三个均绿。
  - 状态:⏳ CI

- [ ] **B3 三条宪法守卫绿**
  - 查什么:参数冻结(**123 参数**)/ IPC 布局冻结(**15 slot + `channels` 字段**)/ state abi 兼容。10 §6.1 的名字与仓内用例的对应:
    - `ParamLayoutFreeze` → `tests/core/test_params_golden.cpp` 的 `[params]` 用例(「params golden 逐行 diff(123 行)」等),对拍 `tests/golden/params_v0.tsv`(非注释行 123);
    - `IpcLayoutFreeze` → `tests/core/test_ipc_layout.cpp:541`,对拍 `tests/golden/ipc-layout.txt`(`max_channels 15`,含 `AudioRingHeader.channels` 等字段);
    - `StateAbiCompat` → `tests/core/test_state_codec.cpp:399`「abi1..abi5 迁移 + abi6.bin 格式锁」。
  - 怎么查:随 B1 的 ctest(`scvb_params_tests` / `scvb_tests`)。三组用例的标签都不带 `.`,在默认集里。
  - 证据:参考 run 同 B1。
  - 状态:⏳ CI

- [x] **B4 `[reference]` libebur128 对拍在本地跑通**
  - 查什么:K 加权与 libebur128 逐点对拍(KW-3,10 §4.4.2)。
  - **CI 绿不算**:CI 的 cmake 行不开 `SCVB_TESTS_WITH_EBUR128`,此时编进去的是 `#else` 分支里那个 KW-3 用例(`tests/core/test_kweighting.cpp:197`),它只执行一条 `SUCCEED` 占位,**这条在 CI 里是空转的**。目前 `[reference]` 只有 KW-3 一条;10 §4.4.2 表里的 L0-b / L0-c(段响度对拍,`test_loudness_ref.cpp`)仓里没有。
  - 怎么查:§3 的 U-6。判据:`[reference]` 用例全过,**且**跑到的是开关打开时编进去的那个用例(`KW-3: 粉噪 30s 与 libebur128 M(400ms) 对拍`,`:141`),认用例名。占位用例通过时不打印 `SUCCEED` 里那句文字,所以「输出里没有占位文字」分不出开关开没开,不能当判据。
  - 证据(SL-575,`61b7575d`):这一项不需要 DAW,按 J175 由统筹在本机做。按 U-6 构建:`-DSCVB_TESTS_WITH_EBUR128=ON`,libebur128 由 `FetchContent` 按 `tests/CMakeLists.txt` 的 `GIT_TAG v1.2.6` 从 GitHub 拉取(拉到的提交 `67b33abe`,即 v1.2.6 tag;Catch2 用本机已有的 v3.5.4 源码目录,与 `GIT_TAG` 同版本),`--config Release --target scvb_tests scvb_bench --parallel 1`,0 条 `warning C`。`scvb_tests.exe "[reference]" --durations yes` 输出:
    `0.376 s: KW-3: 粉噪 30s 与 libebur128 M(400ms) 对拍` / `All tests passed (35848 assertions in 1 test case)` —— 用例名带 `M(400ms)`,是开关打开时编进去的那一条,不是占位(占位那条是 `1 assertion`)。该用例在 44.1 / 48 / 88.2 / 96 kHz 四个采样率上逐 10 ms 比对 M(400ms),容差 0.05 LU。10 §4.4.2 L0-a 另列的 997 Hz 正弦与真实人声片段两种素材、以及 L0-b / L0-c,仓里仍然没有(见上)。
  - 状态:✅ 已核

- [ ] **B5 IPC 长跑用例(IPC-3 十万块)**
  - 查什么:IPC-3 双进程音频环 10 万块逐样本一致、gapCount==0。
  - 与 10 §6.1 不同:仓里**没有 `[.long]` 标签**(`git grep -n "\[\.long\]" -- tests` 0 处)。IPC-3 在 `tests/ipc/test_ipc_contract.cpp:505`,标签 `[ipc][contract]`,**每次 ctest 都跑**(`scvb_ipc_tests`),所以不用单独在本地补跑。
  - 怎么查:随 B1。CI 日志只打套件级 `Passed`,不列逐条用例;要逐条证据可在本地跑 `scvb_ipc_tests.exe "IPC-3*" --durations yes`(可选)。
  - 状态:⏳ CI

- [x] **B6 本地全量 `pwsh scripts/gates.ps1` 全绿**
  - 查什么:全量(不加 `-Quick`),含 gate 8 的 GUI pluginval 与 gate 9 / 10。
  - 实况:维护者本机长期可用内存不足,J142 临时以 PR head 上的 CI 全量代替本地 gates;但 **GUI pluginval 只能在有桌面的本机跑**(CI 是 `--skip-gui-tests`),CI 代替不了这一半。
  - 证据(SL-575):依据 J142(统筹 2026-09-28,在 J121 授权内):本机长期可用内存不足 2 GB,PR 合并门禁改为「PR head 上 CI 的 `build-and-validate`(构建 + ctest + pluginval)全绿」,本地整套 gates 不再强制,本地只做经排队器的定向构建。此后合入 `feature/v1` 的每个 PR 都按这条过的门;#345 改了 `tests/`,落在 `build-vst3.yml` 的 `NATIVE_RE` 里,CI 自动跑全量,run 号记在 #345 的描述里。CI 代替不了的 GUI pluginval:按「出包前本机自测」的惯例,第二包出包前跑过 pluginval 含 GUI 6/6(masterPlan 发布盘点 2026-09-29 M8 所记);第三包出包前同样要跑一次,那一次在第三包的提交上。
    (J178⑧)统筹认可本项 N/A,并要求在 `docs/RELEASE.md` 发版清单第 4 步(「跑全量门禁」)注明 J142:#345 已在该步末尾加了一句 —— v1 这一次本地全量门禁由 PR head 上 CI 的 `build-and-validate` 代替,GUI pluginval 由出包前本机自测补。
  - 状态:N/A(J142:本地全量 gates 由 PR head 上的 CI 全量代替;GUI pluginval 由出包前本机自测补;统筹认可,J178⑧)

### C. UI 与无障碍

- [ ] **C1 web 冒烟全绿**
  - 与 10 §6.1 不同:没有 `web-preview/smoke/run.mjs` 与「六个 fixture」;现行是 `web-preview/tests/smoke-*.mjs`(`e3a7f5a1` 上 32 套;`5b1c908e` 上 44 套,#333 再加 `smoke-tour-version-page.mjs` 共 45 套),CI `web-smoke` job 逐套跑。
  - 怎么查:tag 提交的 `web-smoke` 绿,**且** job summary 里「跳过(缺浏览器)0、没跑成 0」—— SKIP / FLAKY 退出码不判红,只看绿会漏。
  - 证据(参考):#298 head 的 Format run 36403067004:32 套全部跑了,该步 0 条 warning。(`5b1c908e`)#332 的 run 36489830978:44 套全部跑了,「Run web smokes」一步 0 条 `::warning::`。
  - 状态:⏳ CI

- [x] **C2 axe-core 零 serious / 零 critical**
  - 实况:axe-core 是 dev-only,不在 CI(`web-preview/README.md` §5)。
  - 怎么查:按 `web-preview/README.md` §5 起本地服务,对 Output / Input / Monitor 三页各跑一次 `npx @axe-core/cli "<页面地址>"`。
  - 证据(SL-575,`61b7575d`):没有走 `@axe-core/cli`(它要一份与本机 Chrome 版本对得上的 chromedriver),改用同一个引擎直接注入:照 web-preview 页面级冒烟的做法起静态服务与无头 Chrome(CDP),把 axe-core 4.13.0 的 `axe.min.js` 注入壳页 iframe 里的真源页面,跑默认规则集 `axe.run(document)`。扫了 7 个面:Output(`?fixture=fifteen-tracks`)的总览 / 轨道 / 波形 / 设置四个 tab,Input(`?fixture=channel-conflict` 与默认 fixture 各一次),Monitor。**serious 0、critical 0**。moderate 三类,记在这里备查:`region`(有内容不在地标区域里 —— Output 首启引导的询问段、Input 组选择区的几行、Monitor 的图例)、`landmark-one-main`(Input / Monitor 没有 `main` 地标)、`page-has-heading-one`(Input 没有一级标题)。脚本是一次性的,没有入库;复跑按上面的「怎么查」即可(同一套规则)。
  - 状态:✅ 已核

- [x] **C3 缩放全档位无横向溢出;键盘可达**
  - 查什么:Output 7 档 / Input 10 档(05 §1.2)。
  - 线索:`smoke-shell-fit*.mjs`、`smoke-ui-layout-page.mjs`、`smoke-a11y-tabs.mjs` 相关;**还没逐条核它们是否覆盖全部档位**。
  - 证据(SL-575,`61b7575d`):
    **覆盖面**:`smoke-shell-fit-page.mjs` 在三侧各量设计盒 / 缩到 60% / 放到 150% / 宽高比不匹配四种视口,再经真 UI 选**一个**档位(Output 0.5、Input 0.75、Monitor 0.8),没有逐档走;档位表在 `web/shared/design-box.js`(Output 7 档、Input 10 档、Monitor 7 档,与 05 §1.2 一致)。
    **补测全档位**(一次性脚本,起法同 C2):把宿主窗口(壳页的 iframe)依次设成「设计盒 × 档位」四舍五入后的尺寸 —— 与宿主按档位 `setSize` 同一口径 —— 等两帧后量文档与 body 的 scroll 尺寸和外壳包围盒。Output 7 档 × 4 个 tab、Input 10 档 × 2 个 fixture、Monitor 7 档,共 55 格,**全部零溢出**(横向、纵向都没有,外壳都落在窗口内)。
    **键盘**:同一脚本在 C2 的 7 个面上列出看得见的交互元素(按钮、链接、表单控件、带交互 `role` 的自定义控件、带 `data-gb` 的 canvas),共 395 个;除波形页的 16 个 canvas 外都能用 Tab 到(`tabIndex ≥ 0`,或属于有一个成员可 Tab 到的 roving 组)。那 16 个是 15 条轨的静态波形层与 1 个交互层(`wave-overlay`):**在波形上框选区间、点选或拖动段边界没有键盘做法** —— 波形页的页级键盘只有 Esc 取消选择、选中相邻两段后 Delete 合并,而「选中」本身要用鼠标。Output tab 条的 ←/→ / Home / End 由 `smoke-a11y-tabs.mjs` 钉着。
    **键盘缺口的处置**(J178⑤):rc.1 记为已知限制,v1.0.0 前补。#345 在 `docs/KNOWN_ISSUES.md` 新增 KI-10「波形页上建选区、选段与改分段只能用鼠标」,两份用户手册的「已知限制」各加一条指向它。写 KI-10 前回读了 `web/output/tab-wave.js` 与 `web/output/index.html`,比上面那段多核出两点,都写进了 KI-10:选区是在时间标尺上拖出来的(标尺 `aria-hidden`、不可聚焦);选区一旦存在,两端手柄是 `role="slider"`、`tabindex="0"`,←/→ 按视口跨度的 1% 微调。只能用鼠标的是四件:标尺上拖出选区、泳道上点选段、拖段边界、泳道上双击分割 / 删边界。两份 README 的「已知限制」没有加:那一节列的是 v1 已裁定接受的限制,这一条按 J178⑤ 要在 v1.0.0 前补掉(同 KI-7,README 也没列)。
  - 状态:✅ 已核(全档位零溢出);键盘缺口 N/A(J178⑤:rc.1 记为已知限制 KI-10,v1.0.0 前补)

- [ ] **C4 i18n 新增文案都有 key,无硬编码字面量**
  - 怎么查:`node scripts/check-i18n.mjs`(与 CI `docs-truth` 同一条命令,查 zh / en / fr 键对等与红字九条);「无硬编码字面量」靠复审 prompt 第 6 节,没有机检。
  - 证据(参考):#298 head 的 `docs-truth` 绿。(`5b1c908e`)#332 的 `docs-truth` 绿;#333 head 上本地 `node scripts/check-i18n.mjs` 退出 0。
  - 状态:⏳ CI

### D. 音质(10 §4)

- [x] **D1 模式 A null test**
  - 查什么:透明性 null;判据 10 §1.1.3 S1-P1 / P2(样本偏移 0、残差峰值 < −120 dBFS)。10 §6.1 要求「至少在 REAPER 上重跑一遍」;首发只验 Cubase(J124),统筹已认可用 Cubase 代替(J178②)。
  - ⚠ **补偿量要按成品重算**:10 §4.2 的补偿公式按 S1 spike 口径写(spike 版 Output 把 mono 原样复制到 L / R,0 dB);**成品 Output 对居中 mono 轨每侧给 0.7071(−3.01 dB)**(`tests/core/test_transition.cpp` 的 PAN-1)。所以宿主 pan law 选等功率(Equal Power)时 mono 部分理论上不用补偿,宿主为 0 dB 时 mono 部分要 +3.01 dB 而 stereo 部分不用 —— **0 dB 档下 mono + stereo 的完整格用单一增益调不平**,完整格只能在等功率档下跑。`scripts/nulltest.ps1` 的 `-PanLawDb` 也是按 spike 口径写的(补偿 = 取反),成品口径下传进去的是「补偿量取反」,不是宿主设置。**这段是按源码推的,没有实跑过。**
  - 怎么查:§3 的 U-3。
  - 补偿口径(SL-575 推导,**已定(J178①)**:c = 宿主居中增益(dB)+ 3.0103,`-PanLawDb` = −c;Equal Power 档填 0,mono 与 stereo 都不补,完整格只用这一档;0 dB 档填 `-3.0103`,只限纯 mono;先纯 mono、再完整格各跑一遍,用来分辨 Cubase 对居中的 stereo 轨是否也施加 pan law):在上面这段的基础上补两点,都已写进 U-3。① 宿主要选**等功率**那一档(Cubase 列表里的「Equal Power」):它在居中时每侧正好 1/√2 = −3.0103 dB,与成品 Output 相同,mono 与 stereo 都不用补偿;若 Cubase 的「−3 dB」档按字面是 −3.000 dB,它与成品差 0.0103 dB,残差只会比信号低约 58 dB,远到不了 −120 dBFS。② 0 dB 档的纯 mono 定口径跑,补偿量要写到 `-PanLawDb -3.0103`,不能写 `-3.01`:少掉的 0.0003 dB 会让残差停在信号以下约 89 dB,比判据的 −120 dBFS 高得多。
  - 证据(SL-581):第三包(`518ab466`,CI run 36653086798),Cubase 15,用户在 TestProject T37 里导出、统筹用 `scripts/nulltest.ps1 … -PanLawDb 0 -Align` 比对,原始记录与汇总行在 [audio/nulltest-log.md](audio/nulltest-log.md)(2026-09-30)。
    - **结果**:ref = 不装 SCVB 的完整混音 `D1-full-ref_A`;test = 装着 SCVB 的导出(13 条 mono 轨各插 SCVB Input,VOX BUS 第一格插 SCVB Output,无自动化)。两份都是 32-bit float、48 kHz、1,401,962 帧。样本偏移 0,残差峰值 L / R 都是 −168.6 dBFS,RMS −192.1 dBFS。判据「偏移 0、峰值 < −120 dBFS」过;是浮点舍入量级,不是逐位相同。
    - **口径**:这次的中性状态是「输出开(写入自动化档)、无自动化」。U-3 与 nulltest-log §1.2 写的「输出开关 OFF(跟随宿主)」没有单独导出,不在本条证据里。test 与不装 SCVB 的 ref 一致到浮点舍入量级,说明导出时没有分析结果或自动化在起作用。
    - 宿主 Stereo Pan Law 为 Equal Power,`-PanLawDb 0`(J178①)。Cubase 代替 REAPER(J178②)。
    - **没覆盖的**:两条立体声轨没插 SCVB Input,U-3 第 9 步「完整 · 装 SCVB」没有导出,所以立体声轨经 SCVB Input 的透明性这次没有验到。它们在这段里内容也很少:完整 ref 与纯 mono ref 只差 RMS −81.5 dBFS。
    - **前两轮不成立**:用的是同一份第一轮导出的 test,它在做过 D2 的工程里导出,带着 D2 写入的逐段音量(残差峰值 −38.5 dBFS,从第 0 帧起)。用户删掉全部自动化后重导,即上面这一份。
  - 状态:✅ 已核(Cubase 15;「输出开、无自动化」这一中性状态,mono 轨经 SCVB;Cubase 代替 REAPER,J178②)

- [x] **D2 模式 B null test(engine vs follow)**
  - 判据:残差 RMS < −40 dBFS(S2-P5)。
  - 怎么查:§3 的 U-2。
  - 证据(SL-581):第三包、Cubase 15、同一工程,统筹用 `scripts/nulltest.ps1 … -Align` 比对 `D2-engine_C`(写入自动化档导出)与 follow_D(写进自动化后关回跟随宿主导出),记录同上。
    - **结果**:两份都是 24-bit PCM(不是 32-bit float;24-bit 量化步长约 −138 dBFS,不影响 −40 dBFS 的判据)、48 kHz、1,401,962 帧。样本偏移 0,残差 RMS L −99.0 / R −102.7 dBFS,峰值 −72.5 dBFS。判据 RMS < −40 dBFS 过,余量约 59 dB。
    - **前提**:第二份文件用户命名为 `D2-engine_D`,按清单第 5–6 步的角色记为 follow_D。要用来核这条前提的两张截图没有附。旁证:两份不是逐位相同,而两份若都在写入自动化档下离线导出,预期逐位相同。
  - 状态:✅ 已核(第三包;J175)

- [x] **D3 PANLAW-1..5 绿**
  - 实况:仓里没有以 PANLAW 命名的用例。PANLAW-1 由 `PAN-1 equal-power pan gains`(`tests/core/test_transition.cpp:25`)覆盖;`DUALPAN-1..3`、`WIDTH-1`、`MixMath stereo dual-pan + width` 覆盖 PANLAW-3 / 5 的公式层;**PANLAW-2 / 4 的渲染实测、PANLAW-5 的「width=50 与 width=100 总能量差 ≤0.05 dB」与「无极性反转」断言,没找到对应用例**(`git grep -n -i "PANLAW\|polarity" -- tests` 无相关用例)。
  - 证据(SL-575,`61b7575d`):#345 新增 `tests/core/test_panlaw.cpp`(标签 `[panlaw]`,在 `scvb_tests` 里,默认集,CI 每次跑),7 个用例对 10 §4.4.1 逐格:PANLAW-2(9 个 pan 值,L/R 电平比与 `20·log10(cot θ)` 差 ≤ 0.05 dB;±100 两端改判「另一侧低 120 dB 以上」)、PANLAW-3(全局 width 0 / 50 / 100 / 150 把 P=60 缩成 0 / 30 / 60 / 90,由两声道能量反解)、PANLAW-4(单轨 −100 → +100 每 5 取一点,总能量起伏 ≤ 0.05 dB 且等于输入)、PANLAW-5a–5d(每轨 width=100 原样还原源的 L/R;width=0 左右逐样本相等;width=50 相关介于两端且总能量与 width=100 差 ≤ 0.05 dB;width 0 / 50 / 100 下都没有极性反转)。「渲染」走的是 Output 实时混音循环逐样本调用的同一对原语 `output/MixMath.h` 的 `mixMonoSample` / `mixStereoSample`,不含读环、平滑、`busXfade` 与宿主 —— 即 10 §4.5 说的数学层端到端,不替代 D1 / D2。本地 `scvb_tests.exe "[panlaw]"`:`All tests passed (26 assertions in 7 test cases)`,7 个用例合计约 0.25 s;`test_panlaw.cpp` 编译 0 条 `warning C`。
    删除式(每次只改 `MixMath.h` 一处、重编、只跑 `[panlaw]`,跑完还原):mono 路径左右增益对调 ⇒ PANLAW-2 红(PANLAW-3 也红,其余绿);mono 路径忽略全局 width ⇒ 只有 PANLAW-3 红;mono 增益乘一个随 pan 变的系数(左右比不变、总能量起伏)⇒ 只有 PANLAW-4 红;stereo 两个子声像左右对调 ⇒ 只有 5a 红;右子声像多偏 1 ⇒ 只有 5b 红;音量随每轨 width 缩放(「简单混合」而非等功率)⇒ 只有 5c 红;stereo 输出做 M/S 拉宽(side × 3)⇒ 5d 红(5a、5c 也红)。还原后 7 个全绿。
  - 状态:✅ 已核

- [x] **D4 端到端响度对拍(L3)差 ≤ 0.2 LU**
  - 实况:`scvb_bench --render` 能出 wav,但「由特征预测的 10·log10(z_L+z_R)」与渲染结果的比对没有现成工具。
  - 证据(SL-575,`61b7575d`):仍然没有这件工具,本卡没有做。各段零件有用例:`test_loudness.cpp` 的 LOUD-1..5(含 LOUD-4 997 Hz 正弦走 K 加权到段响度的全链)、`test_balance.cpp` 的 BAL-1..10(能量相加模型下的平衡解)、D3 新增的 PANLAW-4(混音原语等功率、总能量不随 pan 变)。缺的是把三段串起来、在渲染结果上量整体响度那一步。这一格的意义在真实人声素材上(10 §4.4.2 的 0.2 LU 容差就是给齐唱 / double 的互相关偏差留的,见 KNOWN_ISSUES KI-9);用合成的互不相关素材做,只会复证上面几条已有的结论。
  - 状态:N/A(rc.1,J178⑥:与 J175 对上机项的处理同向,v1.0.0 前再议;要做的话需新写一个端到端工具,并要用户的真实多轨人声素材)

- [x] **D5 结果追加到 `audio/nulltest-log.md`**
  - 怎么查:D1 / D2 用 `scripts/nulltest.ps1` 跑,会自动把原始输出追加到该文件末尾;再在它的汇总表里补一行。
  - 证据(SL-581):第三包起比对改由统筹做,用户只导出 wav。统筹用 `scripts/nulltest.ps1` 比对 D2、D1 各一次,两节原始记录由脚本追加到 [audio/nulltest-log.md](audio/nulltest-log.md) §3,时间戳 2026-09-30 00:57:04 与 00:57:29。§2 汇总表补了两行,表后写明两条的前提与口径。素材是用户自有人声,wav 不入库;脚本记下四份 wav 的 sha256,调用时 wav 按角色名传入,记录里没有本机路径。
  - 状态:✅ 已核

### E. 性能(10 §5)

- [x] **E1 `scvb_bench --dsp` 对比上一 release 无 >10% 退化**
  - 首发没有上一 release ⇒ **本次建基线**:记下基线机器(10 §5.0 / U16)与两档结果,存 `docs/validation/perf/budget-log.md`(10 §0.4 的冻结路径;建清单的那个 PR 没建,由 #345 新建)。
  - 怎么查:§3 的 U-4。
  - 证据(SL-575,`61b7575d`):按 J175 由统筹在本机做。本机就是 U16 那台(Intel Core Ultra 9 275HX / 32 GB 笔记本,Windows 11 build 26200,接交流电)。按 U-4 的两条命令各跑 4 次(构建同 B4,经排队器、`--parallel 1`),结果与机器信息落在 [perf/budget-log.md](perf/budget-log.md)(#345 新建)。**非安静条件**:同机开着另外 4 个开发会话与浏览器,整机 CPU 占用 19–38%、可用内存 2.2–3.5 GB;跑分期间排队器在本卡手里,没有别的构建并行。同档 4 次的 `checksum` 逐字相同。`scvb_bench --dsp` 量的是 Input / Output 的替身(见 budget-log「这把尺子量的是什么」),下一版对比时按同一条命令、同一台机器、同一套工具链(这次是本机的 VS 2019,不是出包用的 VS 2022,见 budget-log)。
  - 状态:✅ 已核(基线已建;非安静条件)

- [x] **E2 PERF-1..17 全部在预算内,无红灯**
  - 怎么查:PERF-1..4 读 E1 的 `scvb_bench` 输出;PERF-5 / 6 / 7 读 Cubase 的 Audio Performance(10 §5.4 第 3 条,只在同一 DAW 内比);其余按 10 §5.3 各自口径。黄灯要记录并开 issue(10 §5.5)。步骤见 U-4、U-5。
  - 证据(SL-575,`61b7575d`):PERF-1..4 已按 E1 的跑分判过,写在 budget-log:PERF-1 / 3 / 4 两档都在预算内;**PERF-2(Input 最坏单块)在 96k/128 那一档 4 次里 3 次超预算**(4.35–13.37% 对 ≤ 5.0%),48k/512 四次都在预算内(最坏 1.96% 对 ≤ 2.0%)。那一档 mean 与 p99 四次几乎不变而 max 在 58–178 µs 之间变了 3 倍,像是非安静条件下计时线程被调度打断,**没有做能证实这一点的测量**,所以没有定档,要在安静条件下重跑一次才能判。其余 PERF-5..17 要上机。
    (J178④)PERF-2 按字面超预算,**本项在它有结论之前不勾**:打 tag 前在同一台机器、没有并发负载时重跑 96k/128 那一档;仍超就按 10 §5.5 定档。PERF-5..17 的上机部分仍按 J175 为 N/A(rc.1 不跑,v1.0.0 前再议);PERF-1 / 3 / 4 两档在预算内,理由不变。
    (SL-581)按 J178④ 在安静条件下重跑了 96k/128 一档,记录在 [perf/budget-log.md](perf/budget-log.md)「PERF-2 安静重跑」。
    - **条件**:2026-09-29 晚,同一台 U16 参考机、同一次构建的 `scvb_bench`,命令同 U-4,10 万块,经排队器串行、没有别的构建。跑之前用户关掉了浏览器,统筹记下可用内存 3971 MB、整机 CPU 占用 14%。
    - **结果**:三次 Input 最坏单块 13.5 / 52.8 / 11.1 µs,即 1.01% / 3.96% / 0.83%(期限 1.333 ms,预算 ≤ 5.0%),**三次都在预算内**。mean 476.5–477.4 ns、p99 600 ns,与非安静时相同,`checksum` 也相同。
    - **归因**:先前非安静条件下的 4.35–13.37%,以及同晚浏览器开着时(单个 Chrome 进程约 1.4 GB)的一次 5.87%,归因于同机负载下计时线程被调度打断。
    - Output 最坏单块 41.7 / 27.9 / 64.8 µs(3.13% / 2.09% / 4.86%,PERF-4 预算 ≤ 30.0%),只作记录。
    - 原始 json 与日志由统筹留存,不入库。
  - 状态:✅ 已核(PERF-1..4;PERF-2 按 J178④ 的安静重跑);PERF-5..17 上机部分 N/A(rc.1,J175)

- [x] **E3 MEM-1..7 在预算内;5 分钟稳态无泄漏**
  - 相关已知项:SL-445 拖 Q 滑杆时内存增长(J128;修复 PR #306 已于 2026-09-28 合并,上机时顺带看它)。
  - 怎么查:MEM-5 / MEM-7 与 5 分钟稳态上机量(§3 的 U-5);MEM-1..4 是按设计定容的项,由统筹对照代码常量核;MEM-6 只记录。
  - 证据(SL-575,`61b7575d`,逐条推导见 [perf/budget-log.md](perf/budget-log.md)「E3 的定容项」):**MEM-2 与 MEM-4 在预算内**(特征环 15 条共 15.0 MiB,与预算公式一致;曲线 / state 按 2 版 × 15 轨 × 300 段约 0.86 MB)。**MEM-1 与 MEM-3 超出预算公式**:① 音频环每条都按 stereo 容量建(`1<<19` 帧 × 2 个 float,IPC 几何纪律,防宿主冻结 / 就地渲染时重建段越界),15 条共 60.0 MiB,是预算「15 × `1<<19` × 4 B = 30 MB」的 2 倍 —— 10 §5.2 MEM-1 的备注自己写着「若上调……60 MB,需在此重新裁定」;② FrameStore 20 分钟 × 15 轨按页算约 8.8 MiB,预算 ≤ 3 MB 出自 04 §3.1 一处算术(「8 页 ≈ 40KB/轨」,按同句的 5 B/hop 应是 160 KiB),实现与 04 描述的数据结构一致。两格都要统筹 / 用户定:认可现值并按 10 §5.5 走一次显式的预算修订,或改实现。MEM-5 / MEM-7 与 5 分钟稳态:rc.1 按 J175 不跑(N/A,v1.0.0 前再议);MEM-6 只记录。
    (J178③)**MEM-1 / MEM-3 按 10 §5.5 显式修订预算,实现不改**:MEM-1 改为 60 MiB(15 × 4 MiB,段恒按 stereo 容量建),MEM-3 改为约 8.8 MiB(20 分钟 × 15 轨,04 §3.1 原算错);修订前后的数据记在 budget-log 新增的「预算修订」一节。masterPlan 10 §5.2、04 §3.1、01 §5.3 的正文由统筹按 J178③ 改。于是 MEM-1..4 四个定容项都在(修订后的)预算内;MEM-5 / MEM-6 / MEM-7 与 5 分钟稳态要上机,rc.1 按 J175 为 N/A(MEM-6 本来也只记录)。SL-445(拖 Q 滑杆时内存增长)随上机部分留到 v1.0.0 前看。
  - 状态:✅ 已核(MEM-1..4 定容项;MEM-1 / MEM-3 按修订后的预算,J178③);上机部分 N/A(rc.1,J175)

### F. DAW 矩阵(10 §3)

- [x] **F1 矩阵跑完并填表**
  - 范围:按 J124,首发只验 Cubase(U14「每次 release 跑全矩阵」按此修订);REAPER 沿用 spike 证据、本版不复测;Ableton Live 12 / Studio One 6 标未验证(Tier 3);FL Studio 不在 v1 矩阵。
  - 怎么查:[daw-matrix.md](daw-matrix.md) 的 v0.9.0-rc.1 一节,Cubase 一行 13 格逐格填 P / F / N-A 与实测值;步骤见该节「Cubase 逐格怎么跑」与 §3 的 U-1。
  - 状态:N/A(rc.1,J175:用户裁 rc.1 不跑,v1.0.0 前再议)。rc.1 在 Cubase 上的现有证据见 daw-matrix.md 该节的「证据分两层」(S1 路由 spike 与内部测试包 v5.6 – v5.6.19)

- [x] **F2 `daw-matrix.md` 新增本版本一节**
  - 证据:本 PR 已建该节,格子待填;填完才算。
  - 状态:N/A(rc.1,J175:用户裁 rc.1 不跑,v1.0.0 前再议)。该节已建,格子留空,随 F1 到 v1.0.0 再填

- [x] **F3 `daw-support-tiers.md` 与 README 的 Tier 表同步**
  - 怎么查:对照 [daw-support-tiers.md](daw-support-tiers.md) §2、`docs/DAW_COMPATIBILITY.md` §4、`README.md` / `README.zh-CN.md` 的支持 DAW 表,DAW / 版本 / Tier 逐行一致。
  - 证据(`e3a7f5a1`):四处都是 Cubase 14 / 15 Tier 1、REAPER 7 Tier 2、Ableton Live 12 Tier 3、Studio One 6 Tier 3,FL Studio 不在 v1 矩阵。README 第 38 行写明它转贴自 DAW_COMPATIBILITY §4,那里是真源;本文件跟随。(`5b1c908e`)四处逐行重对,仍一致。
  - 状态:✅ 已核

- [x] **F4 MUTE / FRZ 行为与上一版逐字比对**
  - 首发没有上一版的逐字记录:S1 spike 的 C-10(solo / mute)与 C-11(Freeze / Render in Place)只记了「✅」,没记原话 ⇒ **本次在 RC 包上逐字记下,作为下一版的比对基线**。
  - 状态:N/A(rc.1,J175:用户裁 rc.1 不跑,v1.0.0 前再议)。逐字基线随 F1 在 v1.0.0 前记;MUTE 与 FRZ 的行为本版已写进用户可见文档(KNOWN_ISSUES KI-8 / KI-4,见 H3 / H2)

### G. 兼容性与数据安全

- [x] **G1 用上一 release 保存的工程在新版打开** —— N/A:首个公开版本,没有上一 release(2026-09-28 `gh api repos/synchain-oss/scvb/releases` 与 `/tags` 均为空;`5b1c908e` 时两者仍为 0 条)。参考:内部测试包之间「旧工程在新包打开」已在 v5.6.19 测试包 A-P8 验过(用户实测「过」),它不替代本项。
- [x] **G2 用新版保存的工程在上一 release 打开** —— N/A:理由同 G1。另:`docs/RELEASE.md` 发布说明模板的「升级须知」已写明新版工程在旧版(含内部测试包)里会被拒载。
- [x] **G3 sidecar 缺失 / 篡改 / CoW 三条路径手测**
  - 实况:v1 出厂不再写 sidecar(SL-395,开关关),只保留读旧工程;自动化用例 `FEAT-SIDECAR-1..11`(`tests/core/test_state_features_roundtrip.cpp`)覆盖「仍能读 embedded=0」「删 sidecar 后特征缺失」「双开同 GUID copy-on-write」「路径穿越防护」等;**「篡改」没有同名用例**。
  - 建议:手测 N/A(新版不产生 sidecar,读路径有用例);认可前先核「篡改」由哪条用例兜着。
  - 证据(SL-575,`61b7575d`):核了「篡改」—— **没有用例兜着**。生产代码里有这道闸:`src/output/OutputProcessor.cpp` 读引用节时把外部文件的 sha256 与工程里记的比,不符就不认这份特征、原样保留引用节(打 `sidecar sha256 mismatch; refusing to load` 的那一支)。`test_state_features_roundtrip.cpp` 里的 `loadFeatures()` 是一份**照着** Output 加载语义写的测试替身,也做同样的比对,但没有任何用例改过 sidecar 的字节再去加载;host 用例里带 sidecar 的两条(`HOST SL-233` 与 `HOST SL395`,`tests/host/test_host_harness.cpp`)用的都是能过 sha256 的文件。缺失与 CoW 各有用例(FEAT-SIDECAR-2 / -3,同样走那份替身)。手测对 rc.1 不适用:v1 出厂不写 sidecar(SL-395;用户 2026-09-14 裁定「sidecar 不上」,SL-415 随之收起全部 sidecar UI),能带着 sidecar 打开的只有内部测试包时期存过、且当时开着 sidecar 的老工程,首发没有这类公开用户。
  - 状态:N/A(v1 不产生 sidecar;「篡改」只有生产代码里的 sha256 闸、没有用例,记为 v1.0.0 前可补的缺口;统筹认可,J178⑧)
- [x] **G4 golden 文件本版未变更** —— N/A:首个公开版本,没有「上一版」可比。首个公开 tag 起 ParamID 与 state 布局永久冻结(J123 / J21),打 tag 前的冻结评审见 L2。

### H. 文档

- [x] **H1 用户手册的红字前提齐全**
  - 查什么:人声轨路由指向总线、宿主 pan 居中、推子 0 dB;以及 J12 之后的措辞(连接健康时 Input 向下游输出静音是设计行为,没有健康 Output 时自动直通)。
  - 证据:`docs/USER_GUIDE.zh-CN.md` 九条规则里第 1 条(路由指向总线)、第 4 条(宿主 pan 居中)、第 3 条(J12 措辞)都在。「推子 0 dB」不在手册里(`grep -c -E "0 ?dB" docs/USER_GUIDE.zh-CN.md` = 0),**按 J45 它只是 null test 的可比性前提、不是产品要求**,所以不算缺。
  - 状态:✅ 已核

- [x] **H2 ⚠ 单轨 Freeze / Render in Place 得静音文件的红字,三处一致**
  - 查什么:USER_GUIDE + KNOWN_ISSUES + UI 首次导出提示三处一致。
  - 证据:KNOWN_ISSUES `KI-4` 有(第 33 行起)。**USER_GUIDE 里没有**:`grep -c -i -E "freeze|render in place|静音文件|替换式|原素材"` 在 `USER_GUIDE.zh-CN.md` 为 0,在 `USER_GUIDE.md` 为 2,但那两行说的都是 SCVB 自己的冻结 P / V,不是这条。**UI 里没有首次导出提示**:同一模式在 `web/shared/i18n.js` 0 处。(`5b1c908e`)同一条 grep 在 `USER_GUIDE.zh-CN.md` 仍为 0、`USER_GUIDE.md` 仍为 2;在 `i18n.js` 命中 12 处,逐条看过全是 SCVB 自己的冻结 P / V(`tracks.colFreeze*`、`tour.step26.*`、`workflow.tweak` 等),**不是**宿主 Freeze / Render in Place 的提示 —— 结论不变。
    (SL-574 / J176,#344)用户裁定 J176:两份用户手册与两份 README 现在就写;UI 那一处**落在 Input 导览里**,不另做「首次导出提示」。三处现状:
    ① `docs/KNOWN_ISSUES.md` 的「KI-4」小节(未改);
    ② 两份用户手册新增「导出与渲染」一节(`docs/USER_GUIDE.zh-CN.md` 的 `## 导出与渲染` / `docs/USER_GUIDE.md` 的 `## Exporting and rendering`),「5 分钟上手」写自动化一步的末尾、故障排查表、已知限制各有一处指向它;两份 README 的快速上手在九条规则之后加了一段简短警告,链到该节;
    ③ Input 导览第 5 步(`web/input/tour-in.js` 的 `TOUR_IN_STEPS` 第 5 条,居中卡;词条 `tour-in.step5.*` zh / en / fr),原末步「完整控制在 Output」顺延为第 6 步。
    一致性按四件事逐处核:单轨就地渲染 / 冻结 / 单轨导出得到静音文件;选「替换原音频」会把原素材换成静音;对人声总线整体导出;要单轨素材先旁路(Bypass)或移除该轨的 SCVB Input 再渲染 —— 三处都有,说法不冲突。「不确定时先备份」只在 KI-4 与手册里,导览为求短没写。
    原来那条 grep 现在:`USER_GUIDE.zh-CN.md` 5 处(原为 0,5 处全是本卡新增:写自动化一步末尾的指引、新增一节里警告的两行、故障排查行、已知限制行)、`USER_GUIDE.md` 3 处(英文这条模式只认得出 `Freeze` / `Render in Place` 两个词,多出的 1 处是新增一节的警告段)。`i18n.js` 里 `tour-in.step5.body` 三语各 1 处是本条,其余仍是 SCVB 自己的冻结 P / V。
    机检:`web-preview/tests/smoke-input-tour.mjs` ①(步数 6、第 5 步是居中卡且夹在连接状态与末步「?」之间)与 ④(三语词条与步骤表一一对应、第 5 步正文含上面四件事的关键词);`web-preview/tests/smoke-ui-layout-page.mjs` E4(真页面上三语各走到第 5 步:计数 `5/6`、标题与正文逐字等于词条、说明框整体在卡内)。
  - 状态:✅ 已核

- [x] **H3 DAW 的 mute / solo / 推子对 SCVB 通路无效(J45)**
  - 证据:两份 USER_GUIDE、KNOWN_ISSUES、两份 README 都**没有**这条说明(`grep -i -E "\bmute\b|\bsolo\b|独奏"` 的命中只有 Input 自己「向下游输出静音」的描述)。S1 spike C-10 当时的判据就是「solo / mute 对 SCVB 通路失效」。
    (SL-575,`61b7575d`)#345 补了四处:`docs/KNOWN_ISSUES.md` 新增 KI-8(现象 / 原因 / 影响 / 缓解 / 彻底修复方向,缓解 = 用 Output 轨道页每轨的「ON」开关);两份用户手册的故障排查表各加一行、「已知限制」各加一条,指向 KI-8;两份 README 新增的「已知限制」一节(见 H7)各有一条。10 §6.1 要的「红字」没有照做:九条红字的条数被三道机检锁在 9(同 H5 那段说明),所以这条落在已知限制与故障排查里,不进红字。KI-8 写的「推子在插件链之后」只对 Input 放在推子之前成立,那正是红字第 2 条与 DAW_COMPATIBILITY §2.1(Cubase 要放在 pre-fader 区)的要求。
  - 状态:✅ 已核

- [x] **H4 同机同时只支持一个使用 SCVB 的工程**
  - 证据:`docs/KNOWN_ISSUES.md` KI-5(第 41 行起;`5b1c908e` 上仍在第 41 行)。
  - 状态:✅ 已核

- [x] **H5 Output 停摆直通兜底:三处一致 + L-5 / F-1 实测**
  - 证据:KNOWN_ISSUES KI-6 有;用户手册故障排查表「人声突然变成未平衡的原始声像」一行有(`docs/USER_GUIDE.zh-CN.md:203`,含 FL smart disable 规避);FL 作战卡在 masterPlan 03 §4.7,不在本仓。**载体实测 L-5(Live 设备停用)与 F-1(FL)没有跑** —— Live 未上机(J124),FL 不在 v1 矩阵;10 §6.1 写「任一红即不可勾」。
    (SL-575,`61b7575d`)仓内两处的说法对得上:KI-6 与故障排查表那一行都写了约 5.5 秒后人声变成未平衡的原始声像,以及 FL Studio 要对 SCVB Output 所在的总线关掉 smart disable;本卡新加的 README「已知限制」一条同样口径。看门狗的判定逻辑由 core 用例 `停摆看门狗四格(R3/J52)`(`tests/core/test_ipc_lifecycle.cpp`)钉着。L-5 / F-1 两个载体实测都要 Live / FL 上机,本版不做:首发只验 Cubase(J124),Live 为 Tier 3 未验证,FL 不在 v1 矩阵;J175 对上机项的裁定同一方向(rc.1 不跑,v1.0.0 前再议)。
  - 状态:N/A(J124:Live 未上机、FL 不在 v1 矩阵;仓内说法一致已核;统筹认可,J178⑧)

- [x] **H6 各 DAW 作战话术与截图与本版行为一致**
  - 实况:话术在 `docs/DAW_COMPATIBILITY.md` §2(每 DAW 一节);「03 §4 宿主专属界面提示」没做(发布盘点第二轮,待用户裁第 6 条);截图未核。
  - 证据(SL-575,`61b7575d`):「宿主专属界面提示」已由 J150 ① 做出来(#324):REAPER 的「写入期间保持窗口打开」横幅与首次写入时的首选项提示、Live 写入结束后的 Re-Enable Automation 提示,词条 `banner.reaperKeepOpen` / `banner.reaperPrintNote` / `banner.liveReEnable`;DAW_COMPATIBILITY §2.2 / §2.3 的「录自动化」已写明「插件界面会提示这件事」。**截图**:`DAW_COMPATIBILITY.md` 里没有截图(`grep` 图片语法与 `.png` / `.jpg` 均 0 处),这一半不适用。**话术**:§2.1 Cubase 一节逐段对过本版行为 —— 建总线、Input 放 Insert 最后一格且在 pre-fader 区、Output 放总线第一格、宿主 pan 居中、Write / Latch 与 Ins 隐藏车道、Export → Audio Mixdown、Render in Place / Freeze 得静音(KI-4)、已知坑四条 —— 没有与本版行为冲突的句子。§2.2 REAPER、§2.3 Live、§2.4 Studio One 三节在本版都没有上机(J124,Tier 2 / 3),话术对不对只能等上机;这一点文件开头的「口径说明」已写明(成品层只覆盖 Cubase 15,REAPER 只有 spike 层证据,Live 与 Studio One 两层都没有),§1 矩阵里这三个宿主的未验证格也都标了原因。
  - 状态:✅ 已核(Cubase 一节);其余三节 N/A(J124,未上机)

- [x] **H7 已知限制清单在 README 可见**
  - 查什么:FRZ / stem 静音产物、mute / solo 失效、单工程限制、上游 PDC、齐唱互相关偏差、sidecar 不随工程;「真立体声源延后 v2」一条已删([J57]);新增「Input 就地 gain 只做音量、不做声像」「stereo 轨默认不参与自动声像([J60])」。
  - 证据:两份 README 只在「文档」一节链到 `docs/KNOWN_ISSUES.md`(`README.zh-CN.md:96`),正文没有限制清单;「真立体声源延后 v2」0 处;「Input 就地 gain 只做音量」在 USER_GUIDE「已知限制」一节有。**[J60] 那条已被 J83 推翻**(现在所有轨默认参与,见红字第 7 条),要按 J83 改写后再判。
    (SL-575,`61b7575d`)不靠「链接算不算可见」:两份 README 在「从源码构建」与「文档」之间新增「已知限制」一节(`README.zh-CN.md` 的 `## 已知限制` / `README.md` 的 `## Known limitations`,两边标题序列对等),正文直接列 8 条,每条指向 KNOWN_ISSUES 的条目或红字。对照上面的清单:FRZ / stem 静音产物 → KI-4;mute / solo 失效 → KI-8(本卡新增,见 H3);单工程限制 → KI-5;上游 PDC → 列为「Output 不向 DAW 报告延迟,别用 PDC 去修正」(红字第 8 条;上游插件带延迟时的对齐,S1 在 Cubase 上实测零错位,见 DAW_COMPATIBILITY §1 脚注 1,没有另外的限制可写);齐唱互相关偏差 → KI-9(本卡新增,口径取 masterPlan 02 §6.5「能量模型忽略轨间互相关」);sidecar 不随工程 → **不列**,v1 已不写 sidecar(SL-395 / SL-415,见 G3);「Input 就地 gain 只做音量、不做声像」→ 列了;J60 那条按 J83 改写为「所有轨默认参与自动声像,立体声轨也一样,要保留原声像就在轨道页关掉参与」(红字第 7 条)。另列了 KI-6(宿主停调 Output)。
  - 状态:✅ 已核

- [x] **H8 `docs/SCVB_CONTRACT.md` 与代码一致,且是唯一一份桥契约**
  - 证据:`docs/` 下契约文件只有 `SCVB_CONTRACT.md` 与 `IPC_CONTRACT.md`(后者是 IPC 契约,不是桥契约),没有 `WEB_UI_CONTRACT.md`;机器一致性由 `check-bridge-parity.mjs`(CI `docs-truth`)守。版本行是 `1.0(已冻结)`;J105 只对 #281 那一次豁免了升版本号,**其余契约变更是否要求升号没核**。
    (SL-575,`61b7575d`)「唯一一份」:`docs/` 下名字带 CONTRACT 的仍只有 `SCVB_CONTRACT.md` 与 `IPC_CONTRACT.md`(外加 `contract-changes/` 目录),成立。「版本号」:规则在 `SCVB_CONTRACT.md` §0.1 第 3 条与 §9.0 第 3 条 —— 只增(新增函数 / 事件、在既有载荷里加可选字段)不升;改名、改参数顺序、**改既有字段语义**、删除、收窄取值域才升主版本。逐份读了 `docs/contract-changes/` 下勾了 `SCVB_CONTRACT.md` 的变更文档里关于 `contractVersion` 的那句:多数写明「只增,保持 1.0」,与 §0.1 一致;两份改了既有语义、都有用户豁免 —— #275(`channel_id=0` 的语义,SL-464;用户经 SL-468 裁定豁免)与 #281(J105);**一份改了既有语义、没有专门的豁免**:#302(J131 / SL-180),它的变更文档 `20260928-j131-sl180-manual-one-dim.md` 自己写着「变的是一条行为语义……按 J105 的做法保持 1.0;若统筹认为『改既有语义』须升主版本,请另行裁定」,而 J105 是只对 #281 的一次性豁免。另有 8 份早于 SL-468(2026-09-21)的变更文档勾了 `SCVB_CONTRACT.md`,却没写 `contractVersion` 怎么处置(当时还没有这道核对),其中 `20260822-pan-curve-cut-slope.md`(`points[].q` 的语义按 shape 分化)与 `20260826-j83-participate-default.md`(`participate_in_auto_pan` 的默认档,用户 J83)从内容看碰了既有语义或默认值;这 8 份没有逐份展开核。
    (J178⑦)**版本号按总口径定**:首个公开 tag 之前的桥面变更一律并入 `contractVersion` 1.0(1.0 = rc.1 tag 上的桥面),rc.1 起严格按 §0.1。#302 与上面 8 份都在首个公开 tag 之前,按这条都并入 1.0,不逐份补豁免,也不升 2.0;所以这 8 份不再需要展开核。这条口径已写进 CHANGELOG `[Unreleased]` 的「变更」(#345)。「与代码一致」:`node scripts/check-bridge-parity.mjs` 在本分支上通过(`contractVersion = 1.0`;output 函数 37 / 事件 10、input 函数 8 / 事件 5,manifest 与 `web/shared/bridge.js`、C++ 常量表、三个编辑器已注册的 handler 逐一零差异,事件载荷字段对拍全过);同一条命令也在 CI `docs-truth` 里跑,打 tag 前 L3 按 tag 提交再看一次。
  - 状态:✅ 已核(唯一性与机器一致性;版本号按 J178⑦ 总口径,首个公开 tag 之前的桥面变更并入 1.0)

### I. 许可与合规(GPLv3)

- [x] **I1 `LICENSE` 为 GPLv3 全文**
  - 证据:`LICENSE` 674 行,开头是 `GNU GENERAL PUBLIC LICENSE` / `Version 3, 29 June 2007`(`5b1c908e` 上相同)。
  - 状态:✅ 已核

- [x] **I2 第三方组件清单与许可在仓库根 `THIRD-PARTY-NOTICES.md`**
  - 证据:版本都对得上 —— JUCE 8.0.8 ↔ `.juce-version`;WebView2 SDK 1.0.2957.106 ↔ `CMakeLists.txt` 的 `WEBVIEW2_VERSION`;Catch2 v3.5.4、libebur128 v1.2.6 ↔ `tests/CMakeLists.txt` 的 `GIT_TAG`;pluginval v1.0.4 ↔ `.pluginval-version`。**许可证本身有缺口**(发布盘点第二轮):`LICENSES/` 缺 BSD-3-Clause 全文、VST3 SDK 许可证写成 MIT、JUCE 静态链入的 libjpeg / HarfBuzz / SheenBidi / libpng / zlib 未登记 —— 由 PR #314 补,在飞。
    (`5b1c908e`,#314 已合)三处缺口都补上了:`LICENSES/BSD-3-Clause.txt` 在;VST3 SDK 一行改为 JUCE 8.0.8 内置的 3.7.12、「Steinberg VST3 License 或 GPLv3」二选一(原文副本 `third_party/notices/vst3sdk.LICENSE.txt`);zlib 1.3.1 / libpng 1.6.37 / libjpeg 6b / HarfBuzz 10.1.0 / SheenBidi 各有一行,许可证全文(`Zlib.txt` / `libpng-2.0.txt` / `IJG.txt` / `MIT-Modern-Variant.txt` / `Apache-2.0.txt`)与上游声明(`third_party/notices/` 七份)都在。上面五个版本号与各自真源再对一遍仍一致。`scripts/package.ps1 -Preflight -Version 0.9.0-rc.1 -Tag v0.9.0-rc.1` 退出 0(「随二进制分发」表点名的许可证 11 个在 `LICENSES/` 里都有全文,NOTICES 点名的声明文件 3 个都在)。
    (J178,#340 合入后)#340(SL-571,J173)补登 Unicode 数据许可 Unicode-3.0,上面的份数随之变了:`LICENSES/` 现为 **12** 份(新增 `Unicode-3.0.txt`),`third_party/notices/` 现为 **8** 份(新增 `unicode.license.txt`;上面「七份」是 #340 之前的数)。`git ls-tree` 按 `feature/v1` @ `17037be4` 数过。本分支合并 `17037be4` 之后重跑 `scripts/package.ps1 -Preflight -Version 0.9.0-rc.1 -Tag v0.9.0-rc.1`:退出 0,输出「许可证 12 个已核,LICENSES/ 全文 12 份,NOTICES 点名的声明文件 5 个已核」—— #340 另加了反向断言,`LICENSES/` 下每个文件都要被「随二进制分发」表点名。
  - 状态:✅ 已核

- [x] **I3 README 与 release notes 写明 GPLv3 源码获取途径**
  - 证据:README 已写(`README.zh-CN.md` 第 13 行许可证、第 51 行 Releases、第 83 行起从源码构建,含 `git clone https://github.com/synchain-oss/scvb.git`);release notes 模板有「自行从源码构建」链接;`INSTALL.txt` 精确到 tag 的源码地址由 #297 的 `package.ps1` 生成。
    (SL-575,`61b7575d`)模板原来只有「自行从源码构建校验(见 CONTRIBUTOR_ONBOARDING.md)」一句,没有直接写出源码在哪。#345 在 `docs/RELEASE.md` 发布说明模板「下载与安装」一节加了一行:「源码(GPL-3.0-or-later):本版对应的完整源码在 `https://github.com/synchain-oss/scvb/tree/v{X.Y.Z}`(Release 页下方 GitHub 自动附带的 Source code 压缩包是同一份),构建方法见 README『从源码构建』」,两个链接都 pin 在 tag 上。README 一侧上面已核。定稿的发布说明从这个模板改写,J5 发布前按 U-8 第 2 步对模板核一遍。
  - 状态:✅ 已核(README 与模板;定稿随 J5)

- [x] **I4 二进制未内嵌不兼容许可的资源(字体尤其)**
  - 证据:字体都是 OFL-1.1(`THIRD-PARTY-NOTICES.md` 字体四行);OFL §3 保留名由 CI `compliance` 的 `check-font-names.py` 守;发布盘点第二轮指出「字体版权行指向不存在的表」,由 #314 修。
    (`5b1c908e`,#314 已合)`THIRD-PARTY-NOTICES.md` 现有「字体版权行」表,四行与用 fontTools 读出的四个 `.woff2` 的 nameID 0(Windows / 0x409)逐字一致;`python scripts/check-font-names.py` 通过(OFL-1.1 §3:呈现名与 49 份 css/js/html 的字体栈都不含保留名)。编进插件的其余资源是 `web/` 下的自研源码(GPL-3.0-or-later)与 JUCE JS helper(AGPL-3.0-or-later,`REUSE.toml` 声明),`compliance` 的 `reuse lint` 在 #332 上绿。
  - 状态:✅ 已核

### J. 打包与发布

- [ ] **J1 `SCVB-vX.Y.Z-win64.zip` 内含全部 `.vst3` bundle**
  - 10 §6.1 写「两个」,按 J137 为**三个**(Monitor 可选)。
  - 怎么查:tag 推上去后 `release.yml` 的 `release` job 由 `scripts/package.ps1` 打包并解包断言;再下载草稿 Release 的 zip 看一眼。
  - 证据:(`5b1c908e`)#297 已合;#333 head 上 `package.ps1 -Preflight` 退出 0。
  - 状态:⏳ CI(tag 推上去后看 `release` job)

- [ ] **J2 zip 内合规文件组断言通过**
  - 查什么:zip 根目录有 `LICENSE.txt`(GPLv3 全文)、`THIRD-PARTY-NOTICES.md`、`LICENSES/OFL-1.1.txt`、`third_party/notices/`(上游声明原文,HarfBuzz 的逐行版权只在其中的 `harfbuzz.COPYING`)、`INSTALL.txt`(含精确到 tag 的源码 URL);`LICENSE-EXCEPTION.md` 按 U2 **不附**。
  - 实况:#297 的 `package.ps1` 在 `verify-tag` 阶段做许可证全文覆盖检查,BSD-3-Clause 全文补上(#314)之前 rc tag 会停在 preflight。
    (`5b1c908e`)#297 / #314 / #330(zip 带上 `third_party/notices/`)都已合;#333 head 上 `package.ps1 -Preflight -Version 0.9.0-rc.1 -Tag v0.9.0-rc.1` 退出 0。zip 内断言只在打包时跑,要等 tag。
  - 状态:⏳ CI

- [x] **J3 干净 Windows 11 上解压安装,DAW 扫得到并正常工作**
  - 怎么查:§3 的 U-7。
  - 状态:N/A(rc.1,J175:用户裁 rc.1 不跑,v1.0.0 前再议)

- [x] **J4 SmartScreen 提示说明写进 release notes**
  - 证据:`docs/RELEASE.md` 发布说明模板里已有「本项目当前未做代码签名……解除锁定」一段(U13 不签名)。
    (SL-575,`61b7575d`)重读模板:「下载与安装」一节标着 `<!-- 未签名时必填 -->` 的那段写了未做代码签名(U13)、先核 SHA-256、解压前「解除锁定」(含 `Unblock-File` 命令)、SmartScreen 拦下时点「更多信息 → 仍要运行」,并指向用户手册「安装」一节与 zip 里的 `INSTALL.txt`。定稿时保留这段,J5 发布前按 U-8 第 2 步对模板核一遍。
  - 状态:✅ 已核(模板;定稿随 J5)

- [ ] **J5 GitHub Release 建为草稿,人工核对 notes 后再发布**
  - 实况:#297 的 `release.yml` 只建草稿(rc 自动勾 pre-release),发布手动;发布留用户(J121)。
  - 状态:⏳ 用户(步骤见 U-8)

- [ ] **J6 tag 保护生效(`v*`,06 §7.2)**
  - 证据:2026-09-28 `gh api repos/synchain-oss/scvb/rulesets` 返回 `[]`,旧式 tag protection 接口 404 ⇒ **目前没有 `v*` tag 保护**。(`5b1c908e` 同日晚)rulesets 仍为空数组。
  - 状态:⏳ 用户(仓库设置,步骤见 U-8)

### K. 回滚预案

- [x] **K1 上一 release 的 zip 仍可下载** —— N/A:首个公开版本,没有上一 release。
- [x] **K2 release notes 写明如何回退**
  - 实况:模板「升级须知」写了新版工程在旧版会被拒载,但**没有「如何回退」步骤**(卸载新版 `.vst3` 目录、装回旧版)。
  - 证据(SL-575,`61b7575d`):#345 在模板「升级须知」之后加了「如遇问题如何回退」一节,三步:关掉 DAW、删掉 `C:\Program Files\Common Files\VST3\` 下三个 SCVB 的 `.vst3` 文件夹;从 Releases 页下载上一版 zip、核 SHA-256、三个一起装回;工程兼容性提醒(本版保存过的工程在 state abi 更低的旧版里会被拒载,回退前先确认手上有旧版存的副本)。节头注释写明首个公开版本之前没有公开版本可回时怎么改写(第 2 步改成装回之前在用的版本,或只留第 1、3 步)—— rc.1 正是这种情况。
  - 状态:✅ 已核(模板;定稿随 J5)

### L. 本版追加(不在 10 §6.1,来自发版流程与裁定)

- [x] **L1 fr 红字审校完成**
  - 查什么:`docs/hard-rules.i18n.json` 的 `frReview.status` 为 `reviewed`(`docs/RELEASE.md` 发版清单第 5 步;J127 改为 AI 三语交叉核对,用户授权)。
  - 证据:当前 `frReview.status` = `pending`;置 `reviewed` 的 PR #299 在飞。
    (`5b1c908e`)`frReview.status` = `reviewed`(#299 置位,date 2026-09-28);其后两次改动红字的 PR 都按 J127 同一方式复核并记在 `frReview.note` 里(J149 轮:rule2 / rule9 的 prod 地址;J153 轮:#319 改的 zh 标题)。`node scripts/gen-hard-rules.mjs --check` 退出 0 —— 它逐条比对 `zhSha256` 与 zh 真源,zh 在复核之后再被改过就会红。
  - 状态:✅ 已核

- [ ] **L2 参数面冻结评审完成**
  - 查什么:J123 —— 第一个公开 tag 一打,ParamID 与 state 布局即永久冻结(J21),打 tag 前必须完成冻结评审。
  - 证据:发布盘点把它列为统筹派工项,还没有产出。
    (SL-575,`61b7575d`)评审已在 masterPlan `ops/param-freeze-review.md` 做过一次,评审对象 `1926752`(#333),四条判据全过,结论「冻结通过(待用户确认)」。从 `1926752` 到 `61b7575d`,`tests/golden`、`src/output/OutputParams.h`、`src/core/state`、`docs/constitution` 零改动;`docs/PARAMETERS.md` 与 `docs/STATE_SCHEMA.md` 只有 #338(J167)改了两处说明文字(`lead_select` 说明列、`LEAD` 块说明),参数面、载荷与 abi 都没变。J177 还会从 `STATE_SCHEMA` 撤回 Input `uiGuideSeen` 的声明(SL-238),所以要按发布盘点 M4 在最终 tag 提交上重跑四条判据,再请用户确认。
    (SL-581)仍待做。按 `docs/RELEASE.md` 第 6 步,tag 打在 `dev` 上里程碑压成的那个提交上。那个提交的树与压进去的 `feature/v1` 提交逐字相同,由里程碑第 2 步与第 5 步的 `diff-exit=0` 核。所以 L2 由统筹在里程碑合进 `dev` 之后、推 tag 之前,在那个提交上重跑四条判据,再请用户确认。本 PR 只改 CHANGELOG 与 `docs/validation/` 下三份记录,不碰参数面、state 与冻结契约文件。
  - 状态:⏳ 统筹(在 tag 提交上重跑并请用户确认)

- [ ] **L3 打 tag 前在 tag 提交上重做发布盘点与漏项全扫**
  - 查什么:J129 / J139 —— 本文件与发布盘点都是阶段性结论;打 tag 前在当时的 tip 上重跑本文件全部 ✅ 项的「怎么查」、重扫漏项。
  - 证据(SL-581):仍待做。由统筹在 L2 之后、推 tag 之前做,提交与 L2 相同(`dev` 上里程碑压成的那个提交)。本轮勾上的 D1 / D2 / D5 / E2 与其余 ✅ 项的「怎么查」都在那时重跑,A2 的去留也在那时再看一次。
  - 状态:⏳ 统筹(待 tag 提交;由统筹最后做)

## 3. 用户上机步骤

**rc.1 要跑的只有 U-2、U-3(D1 / D2 / D5,随第三包,J175)与 U-8(J5 / J6)。** U-2、U-3 已随第三包做完:用户只导出 wav,比对与记录由统筹做,见 D1 / D2 / D5。U-4、U-6 已由统筹在本机做掉(E1 / B4);U-1、U-5、U-7 按 J175 在 rc.1 不跑(F1 / F2 / F4、E2 / E3 上机部分、J3 写 N/A),步骤留给 v1.0.0。

所有上机项一律用 **RC tag 触发的草稿 Release 里那个 zip**(或同一提交的 dispatch 产物,artifact 名里的 40 位 sha 与 tag 提交逐字一致)。**测之前先把要用的工程另存一份副本**,FRZ 一格会覆盖素材。做到哪算哪,回报时说停在哪一步。

### U-0 准备工具(一次;也可以由统筹构建好把 exe 交给你)

rc.1 只用得到 `scvb_nulltest`(U-2 / U-3 经 `scripts/nulltest.ps1` 调用),最省事是由统筹构建好交给你;要自己构建,把下面第二条命令的 `--target` 换成 `scvb_nulltest` 即可。下面是完整的一份(v1.0.0 跑全套时用)。在仓库根目录、检出到 RC tag,构建验证工具与测试(`--parallel 1` 省内存;libebur128 要联网拉取):

```powershell
git fetch --tags
git checkout v0.9.0-rc.1
cmake -S . -B build-val -DJUCE_PATH=<JUCE 8.0.8 所在目录> -DSCVB_TESTS_WITH_EBUR128=ON
cmake --build build-val --config Release --parallel 1 --target scvb_tests scvb_ipc_tests scvb_nulltest scvb_bench scvb_diag
```

产物在 `build-val\tests\Release\`(`scvb_tests.exe`)、`build-val\tests\ipc\Release\`(`scvb_ipc_tests.exe`)与 `build-val\tests\tools\Release\`(另外三个)。

### U-1 Cubase 矩阵(F1 / F2 / F4;rc.1 不跑,J175)

按 [daw-matrix.md](daw-matrix.md) v0.9.0-rc.1 一节的「Cubase 逐格怎么跑」逐格做,结果填进该节的矩阵。想看时间线缺口的计数,播放时另开一个 PowerShell 跑:

```powershell
build-val\tests\tools\Release\scvb_diag.exe --out diag-rc1.csv --group 1
```

(`--group` 填测试工程用的组号,A=1 … H=8;按 Ctrl+C 停。)

### U-2 模式 B null test(D2 / D5)

1. 打开一份已采集、已分析的测试工程副本(自有多轨人声素材,不入库);人声轨与总线 pan 居中,主输出无限制器、无 dither,总线上除 SCVB Output 外没有别的处理。这份副本里 SCVB Output 的参数上如果已经有以前写进去的自动化,先删掉(或换一份从没写过自动化的副本):Cubase 在 Read 下会按旧自动化改参数,第 3 步导出的就不是纯引擎结果。
2. 用左右定位器圈一段 30–60 秒的区间(含独唱与重叠段),记下起止。
3. Output 输出开关 **ON**,Cubase 自动化保持 Read —— File → Export → Audio Mixdown,32-bit float,导出 `engine_C.wav`。
4. 打开 Output 窗口,轨道自动化设 **Write**(或 Latch),从区间起点播到终点,停,切回 **Read**(打印的车道在 Ins 隐藏车道下,见 `docs/DAW_COMPATIBILITY.md` §2.1;若出现加载守卫横幅,先点「继续写入自动化」;若第 3 步开输出时出了写入确认条,先点「知道了,开始」—— 点之前只试听、不写,[J166])。
5. Output 输出开关 **OFF**,同一区间、同一导出设置导出 `follow_D.wav`。
6. 比对并自动写日志(在仓库根目录跑;两份 wav 写完整路径 —— 脚本一开始就切到仓库根目录,只写文件名时只会在仓库根目录里找,找不到就报「无法打开文件」):

   ```powershell
   pwsh scripts/nulltest.ps1 "<导出目录>\engine_C.wav" "<导出目录>\follow_D.wav" -Align -BuildDir build-val
   ```

7. 判据:逐声道残差 RMS < −40 dBFS。不过就分开打印(只打印 pan、只打印 vol)各比一次,定位是哪条链(10 §4.2)。
8. 把 `docs/validation/audio/nulltest-log.md` 新追加的那段交给统筹,并在它的汇总表补一行。

### U-3 模式 A null test(D1 / D5;补偿口径已定,J178①)

> 补偿口径(SL-575 按源码推导,统筹已定为 J178①;没有实跑过):成品 Output 对居中的 mono 轨每侧给 cos 45° = 1/√2(−3.0103 dB),对居中、width 100 的 stereo 轨 L→L、R→R(0 dB)。所以要施加给 test 的补偿 = 宿主居中增益(dB)− (−3.0103);`-PanLawDb` 填它的相反数。只有宿主居中增益恰好是 −3.0103 dB 时,mono 与 stereo 两部分都不用补偿,完整格才能用一个增益调平。

1. 新建 48 kHz 工程,导入同一组人声(有 stereo 轨的话建议 13 mono + 2 stereo;先只放 mono 跑一遍,再加 stereo 跑完整格),全部 pan 居中、推子 0 dB,送同一条 stereo Group(VOX BUS),总线同样居中、0 dB。Project → Project Setup 里的 Stereo Pan Law **选「Equal Power」(等功率)**,并把列表里选中那一项的原文记下来:按推导只有等功率这一档 mono 与 stereo 两部分都不用补偿。「−3 dB」那一档若按字面是 −3.000 dB,与成品差 0.0103 dB,残差只会比信号低约 58 dB,过不了 −120 dBFS;0 dB 档下 mono 要补 +3.0103 dB、stereo 要 0 dB,单一增益调不平,**0 dB 档只许用在纯 mono 的定口径跑**。另记下 stereo 轨用的是哪种 panner(Stereo Balance Panner / Stereo Combined Panner)—— Cubase 对居中 stereo 轨是否也施加 pan law 没实测,先纯 mono、再完整格这两遍就是用来分辨这一点的(J178①)。
2. **不装 SCVB**,导出区间 → `ref_A.wav`(32-bit float)。
3. 每条人声轨插件链最后一格插 SCVB Input,总线第一格插 SCVB Output。**不分析、输出开关保持 OFF、不写任何自动化**(此时 pan 0 / vol 0 dB / width 100 / MS Balance 0 / Lead Select 0 都是参数默认值,见 `tests/golden/params_v0.tsv`)。确认各 Input 显示已连接、Output 没有「时间线缺口」横幅,导出同一区间 → `test_B.wav`。
4. 比对(补偿值按 J178①;它是按源码推的,没实跑过。wav 同 U-2 写完整路径):
   - 宿主 Equal Power(完整格与纯 mono 都用这档):`pwsh scripts/nulltest.ps1 "<导出目录>\ref_A.wav" "<导出目录>\test_B.wav" -PanLawDb 0 -Align -BuildDir build-val`
   - 宿主 0 dB(**只限纯 mono 的定口径跑**):`pwsh scripts/nulltest.ps1 "<导出目录>\ref_A.wav" "<导出目录>\test_B.wav" -PanLawDb -3.0103 -Align -BuildDir build-val` —— **要写到小数点后 4 位**:写成 `-3.01` 少补 0.0003 dB,残差会停在信号以下约 89 dB,整格判红而原因不在插件
   - ⚠ 成品口径下 `-PanLawDb` 传的是「要施加给 test 的补偿量取反」,**不是宿主设置**;脚本会把这个数原样写进原始记录的「宿主 pan law」那一行(同一行括号里的 `--gain-db` 才是实际施加的补偿)。原始记录不手改,**宿主的真实设置以汇总表「宿主 pan law」一列为准**。
5. 判据:样本偏移 0,残差峰值 < −120 dBFS(理想为按位相等)。**不过就把工具输出的逐声道残差与偏移原样回报,别凭听感调增益去凑**(10 §4.2 明令禁止)。汇总表的「宿主 pan law」一列写 Cubase 里的真实设置,备注写 stereo 轨的 panner 类型。

### U-4 性能基线 `scvb_bench`(E1,E2 的 PERF-1..4;rc.1 已由统筹在本机跑过,见 E1 与 perf/budget-log.md;PERF-2 的 96k/128 一档打 tag 前由统筹在无并发负载时重跑,J178④)

在性能参考机上跑(U16:275HX / 32GB 笔记本),插电源、关掉 DAW 与其他重负载程序:

```powershell
build-val\tests\tools\Release\scvb_bench.exe --dsp --fs 48000 --block 512 --tracks 15 --stereo-tracks 2 --blocks 100000 --json bench-48k-512.json
build-val\tests\tools\Release\scvb_bench.exe --dsp --fs 96000 --block 128 --tracks 15 --stereo-tracks 2 --blocks 100000 --json bench-96k-128.json
```

一并记下:CPU 型号 / 核数 / 基频、内存、Windows 版本号、音频接口与驱动。两个 json 与这些信息交给统筹,由统筹建 `docs/validation/perf/budget-log.md` 并对照 10 §5.1 判 PERF-1..4。

### U-5 宿主性能与内存(E2 的 PERF-5..7,E3;rc.1 不跑,J175)

用一份 15 条 Input + 1 个 Output 的工程:

1. **MEM-7**:插件都加载好、所有编辑器关着,在任务管理器记下 Cubase 进程内存;对比不插 SCVB 的同一工程,差值 ≤ 280 MB。
2. **MEM-5**:打开 Output 编辑器,任务管理器里 Cubase 下面的 WebView2 进程内存,刚打开时 ≤ 250 MB,放着 5 分钟后增量 ≤ 20 MB。
3. **PERF-5 / 6**:Studio → Audio Performance,播放全曲,记平均与峰值;再把 SCVB 全部停用播一遍作对照。
4. **PERF-7**:同 3,但播放时开着 Output 编辑器(UI 全负载),记增量。

### U-6 `[reference]` 对拍(B4;rc.1 已由统筹在本机跑过,见 B4)

U-0 构建时已带 `-DSCVB_TESTS_WITH_EBUR128=ON`:

```powershell
build-val\tests\Release\scvb_tests.exe "[reference]" --durations yes
```

判据:全过,**且**输出里那行用例名带 `M(400ms)`(完整是 `KW-3: 粉噪 30s 与 libebur128 M(400ms) 对拍`;中文在控制台可能乱码,认这段英文就行)。如果用例名里带 `SCVB_TESTS_WITH_EBUR128`,或者末行是 `All tests passed (1 assertion in 1 test case)`,说明构建时没开开关、跑的是占位,**不算**。别拿「输出里没有 `=OFF` 字样」判:占位用例通过时那句文字根本不打印(2026-09-28 在一份开关关闭的构建上按上面这条命令跑过,输出里 0 处 `=OFF`)。

### U-7 干净 Windows 11 安装(J3;rc.1 不跑,J175)

1. 准备一台**没装** Visual Studio / Windows SDK / 本仓构建环境的 Windows 11(虚拟机也行),装好 Cubase 15;记下 Windows 版本号。
2. 从草稿 Release 下载 zip 与 `.sha256`(文件名以草稿里的为准),`Get-FileHash <zip> -Algorithm SHA256` 与 `.sha256` 内容逐字一致。
3. **解压前**右键 zip → 属性 → 勾「解除锁定」→ 确定(用户手册「安装」一节)。
4. 把三个 `.vst3` 文件夹整个复制到 `C:\Program Files\Common Files\VST3\`。
5. 打开 Cubase,插件管理器里 SCVB Input / Output / Monitor 三个都在;按用户手册「5 分钟上手」装一对跑通:Input 显示已连接,Output 编辑器正常打开(不是空白窗口或兜底面板),能采集、分析,输出开关 ON 能听到平衡后的结果。
6. 记下:有没有 SmartScreen / 「未知发布者」提示,提示的原文。

### U-8 仓库设置与发布(J5 / J6)

1. **tag 保护**(J6):仓库 Settings → Rules → Rulesets → New tag ruleset,目标 `v*`,只允许维护者创建,禁止删除与强推(06 §7.2:禁止非 owner 创建 / 删除)。
2. **发布**(J5):本清单全部勾上之后,在 Releases 页打开 `v0.9.0-rc.1` 草稿,按 `docs/RELEASE.md` 的模板核对正文(SHA-256 从 CI 的 `package-summary.md` 复制),确认勾着 pre-release,再发布。
