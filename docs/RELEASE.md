# RELEASE —— SCVB 发布流程与发布说明模板

> 状态:演进中
> 最后更新:2026-09-29
> 真源:12 §4.1–§4.5(版本号 / tag / CHANGELOG / release note / 分发渠道)

本文件是**维护者**发版时照着走的清单,以及发布说明的模板。用户侧的安装说明在 [README](../README.zh-CN.md),使用说明在[用户手册](USER_GUIDE.zh-CN.md)。

**发版门禁**:每个版本在 `docs/validation/` 下建一份 `release-checklist-vX.Y.Z.md`(10 §6),逐项打勾,**任何一项未勾不得发布**;当前一份是 [v0.9.0-rc.1](validation/release-checklist-v0.9.0-rc.1.md)。

## 版本号的唯一真源

SCVB 的版本号真源是顶层 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。运行时经 JUCE 的 `ProjectInfo::versionString` / `JucePlugin_VersionString` 读出。

**铁律:除真源外,任何地方都不得硬编码版本号** —— README、docs、UI HTML、脚本、workflow 一律不写死。README 里的版本靠 badge 动态显示(shields.io 读 GitHub Release,带 `include_prereleases`,所以 rc 发布后显示的是 rc 的版本号;徽章链到 Releases 列表页而不是 `releases/latest`,后者只认正式版)。

下游镜像(发版时必须同步):`CHANGELOG.md`、Release tag;官网下载页上线后再加上它的常量。

**三个插件(Input / Output / Monitor)共用同一个版本号、同一次发布、同一个 zip。** Input 与 Output 必须配对,分开编号的直接后果是用户装出不匹配的组合:IPC 协议版本不同的两侧会拒绝互连;协议相同时虽然连得上,混装也不受支持 —— 新 Output 配旧 Input 做离线渲染,人声可能被双路叠加(SL-260,发布说明模板「升级须知」第一条就是它)。Monitor 是可选的只读观察窗,可以不装,但版本号与另两个相同、在同一个 zip 里发布。

## Tag 规则

- 格式 **`vX.Y.Z`**,纯 semver 无前缀;预发布 `vX.Y.Z-rc.N`(流水线建的草稿自动勾 pre-release)。**rc 不改 `CMakeLists.txt`**:`v1.2.3-rc.1` 与 `v1.2.3` 对应的都是 `project(SCVB VERSION 1.2.3)`。首个公开版本按 J123 走的是另一种形态:`v0.9.0-rc.N`(CMake `0.9.0`)测过之后发的是 `v1.0.0` 而不是 `v0.9.0`,所以发 `v1.0.0` 之前还要再改一次 CMake 版本(`0.9.0` → `1.0.0`)。另外 rc 构建在插件设置页里显示的版本号不带 `-rc.N`(它来自 CMake 版本),区分 rc 几要看 zip 文件名或 Release 页。
- 演练专用 **`v0.0.0-test`**(可加 `.N`):只用来走通「构建 → 打包 → 草稿 Release」全程,不比对 CMake 版本;限死 `0.0.0` 是为了让它不可能冒充真版本。用完删掉 tag 与草稿(见下方发版清单第 0 步)。
- 其他形态(`v1.2.3-beta.1`、`v1.2` 等)一律被拒。
- tag 只由维护者在 `dev`(或将来的 release 分支)上打,**不在 feature 分支打 tag**(唯一例外是演练 tag,见发版清单第 0 步)。v1 的改动都在主支线 `feature/v1` 上,所以发版前先按下方「里程碑合并:`feature/v1` → `dev`」把它压成一个提交合进 `dev`,tag 打在 `dev` 上的这个提交(J170)。
- tag push 时,`.github/workflows/release.yml` 的 `verify-tag` job 先比对 tag 与 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`(判据 `scripts/check-release-tag.ps1`,带自测),不一致即 fail,不会进入 20 分钟的构建。

semver 语义(音频插件特化):

| 变更 | 版本位 |
|---|---|
| state abi 升级(需迁移函数) | **MINOR**,且 CHANGELOG 顶部醒目标注 |
| IPC abi 升级(段名升 v2,新旧不互认) | **MAJOR**,用户必须同时升级两个插件 |
| ParamID / index / skew 变更 | **MAJOR**(理论上永不发生 —— 冻结生效点 = 首个公开 rc 起,params-v0 [J21]) |
| 参数默认值变更 | MINOR |
| 新增 state 字段(带默认值,旧 state 可读) | MINOR |
| DSP 结果可闻变化(同一工程渲染结果不同) | MINOR,且 CHANGELOG 醒目标注 |
| 纯修复 / 文档 / 构建 | PATCH |

## CHANGELOG 约定

标准 = Keep a Changelog 1.1 + SemVer。**SCVB 用中文小节标题**(`### 新增` / `### 变更` / `### 修复` / `### 弃用` / `### 移除` / `### 安全`)—— 与仓库的中文文档策略一致。CLI 仓用英文小节,那是按受众分档的例外,不要照搬过来。

- 固定小节 **`### ⚠️ 契约变更`** 置于所有小节之前,记录 state abi 升级、IPC abi 升级、参数默认值变更、DSP 可闻变化、协议不变量变更。**没有内容时整节省略**,所以它一出现就是醒目的。
- 每条末尾带 PR 链接 `(#123)`;涉及冻结契约的条目额外带变更文档链接 `(见 docs/contract-changes/20260901-state-abi-2.md)`。
- `## [Unreleased]` 常驻顶部;发版时把 Unreleased 的内容整体下移为新版本节并填日期。
- 底部维护 tag 对比链接。
- **写入时机:每个 PR 自己改 CHANGELOG**,不留到发版时补写。

## 发版流水线现状

推一个 `v*` tag,`.github/workflows/release.yml` 依次跑三个 job:

| job | 做什么 | 失败时 |
|---|---|---|
| `verify-tag` | 先跑判据自测,再比对 tag 与 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`(`scripts/check-release-tag.ps1`,规则见上方「Tag 规则」);再跑 `package.ps1 -Preflight`(许可证全文覆盖、`THIRD-PARTY-NOTICES.md` 点名的声明文件都在、INSTALL.txt 的规则提取) | 立刻红,不进构建 |
| `build` | **调用 `build-vst3.yml`**(同一份配方):构建(/W4 零 warning)→ ctest → 三个 bundle 的 pluginval(CI 无桌面,`--skip-gui-tests`)→ 按 tag 名上传 `.vst3` artifact | 红,不打包 |
| `release` | 取回 artifact → `scripts/package.ps1` 打 zip / `.sha256` / `package-summary.md` 并解包断言 → `gh release create --draft` 建**草稿** Release(rc 与演练 tag 自动勾 pre-release);summary 同时写进 job summary | 红,不建 Release |

- 权限:workflow 级只读;只有 `release` job 拿 `contents: write`。所有 action 都 pin 到 40 位 SHA。
- 同一个 tag 重跑:已有草稿就覆盖资产,并把正文重置为新的 `package-summary.md`(手改过的正文会丢,改正文放在最后一次重跑之后);**已发布的 Release 流水线一律不碰**。
- 许可证全文:`THIRD-PARTY-NOTICES.md`「随二进制分发」表点名的每个许可证(加本项目的 GPL-3.0-or-later)都必须在 `LICENSES/` 里有全文,缺一个 `package.ps1` 就红;只有演练 tag 降为警告并在 `package-summary.md` 的 `missingLicenseTexts` 行写明。这项检查在 `verify-tag` 的 preflight 里先跑一次(构建之前),打包时再判一次。
- 声明原文:`THIRD-PARTY-NOTICES.md` 全文里点名的每个 `third_party/notices/<文件>` / `LICENSES/<文件>` 路径都必须在仓库里存在(preflight 与打包各判一次,**演练 tag 也不放行**),打包后再逐个核对它在 zip 里。`third_party/notices/` 整个目录按原相对路径进 zip,所以 NOTICES 里的这些路径在解压目录里原样可查。
- 草稿 Release 的正文是 `package-summary.md`(版本 / 文件名 / 大小 / SHA-256 / 发布日期 / 源码提交 / 逐条目哈希),发布前按下方模板改写。
- **「tag 触发 → 调用构建 → 建草稿」这一段只有推 tag 才会执行**:PR 上的 CI 只跑 `build-vst3`,不跑 `release.yml`;`scripts/package.ps1` 可以拿 `build-vst3` 的产物在本地试打包(`-BuildDir <artifact 目录> -Version 0.0.0-dryrun`;`LICENSES/` 缺许可证全文时会红在许可证检查上,加 `-AllowMissingLicenseTexts` 可降为警告,这个开关只用于本地试打包与演练 tag),但覆盖不到 workflow 本身。所以首次发版、以及改过这三处文件之后,先做下面第 0 步。

## 发版清单

0. **(首次,或改过 `release.yml` / `build-vst3.yml` / `scripts/package.ps1` 之后)演练一次**:`git tag v0.0.0-test <要验的提交> && git push origin v0.0.0-test`。tag push 跑的是**被打 tag 那个提交里**的 `release.yml`,所以要验的提交必须已含 T40(`feature/v1` 上 T40 合并之后的任一提交即可;演练 tag 是「不在 feature 分支打 tag」的唯一例外,用完即删)。等 Release workflow 全绿,在 Releases 页打开标着 `[pipeline test - delete me]` 的草稿,下载 zip 与 `.sha256`,核对第 7 步那几项。演练完**删草稿再删 tag**:`gh release delete v0.0.0-test --yes` 然后 `git push origin :refs/tags/v0.0.0-test && git tag -d v0.0.0-test`。要再演练一次就用 `v0.0.0-test.2` 之类的新名字,或先删干净再推。
1. **确认 CHANGELOG**:`## [Unreleased]` 的内容完整(每条带 PR 号),契约变更条目齐全且各自有 `docs/contract-changes/` 文档。
2. **下移版本节**:把 Unreleased 内容改写成 `## [X.Y.Z] - YYYY-MM-DD`,补底部对比链接,留一个空的 Unreleased。新的对比链接指向第 6 步才推的 tag,推之前 GitHub 回 404,CI 的死链检查会红 —— 同一个 PR 在 `.markdown-link-check.json` 里给这两个确切地址加一条临时放行,tag 推上去之后删掉。
3. **改版本号**:改 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。这是唯一一处(rc 与正式版用同一个 X.Y.Z)。
4. **跑全量门禁**:`pwsh scripts/gates.ps1`(含真机 GUI pluginval),必须全绿;并按 `CLAUDE.md` 的出包硬规对目标 ref dispatch 一次 `build-vst3` 并全绿。v1 这一次按 J142(2026-09-28):维护者本机长期可用内存不够跑整套 `gates.ps1`,本地全量门禁由 PR head 上 CI 的 `build-and-validate`(构建 + ctest + pluginval)全绿代替;CI 的 pluginval 带 `--skip-gui-tests`,GUI pluginval 那一半由出包前在本机的自测补(见发版验证清单 B6)。
5. **红字真源自检**:`node scripts/gen-hard-rules.mjs --check` 退出码 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 必须是 `reviewed` —— **fr 红字未经审校不得发版**(05 §5:未经审校的机翻安全警告发到公开产品是明确禁止项)。审校可以是人工,也可以是经用户授权的 AI 三语交叉核对(以中文为准核 en 与 fr 的意思):v1 这一次按 J127(2026-09-28)由后者代替人工抽检。zh 真源或 en/fr 译文此后再改,`frReview.status` 要改回 `pending` 并重新审校。
6. **合进 `dev`,在 `dev` 上打 tag 并推送**:先按下方「里程碑合并:`feature/v1` → `dev`」把第 1–5 步所在的主支线提交压成一个提交合进 `dev`(J170 / J171),等 push→`dev` 触发的那次 `build-vst3` 全绿,再在 `dev` 上的这个提交打 tag:`git fetch origin && git tag vX.Y.Z origin/dev && git push origin vX.Y.Z`(预发布用 `vX.Y.Z-rc.N`)。打之前确认 `origin/dev` 仍是那个里程碑提交(该节第 5 步的回读)。`release.yml` 随之触发,`verify-tag` 先卡版本号。
7. **核对产物**(草稿 Release 的资产):zip 里两个必装 bundle `SCVB Input.vst3` / `SCVB Output.vst3` 加可选的 `SCVB Monitor.vst3`,三个完整 bundle 都要在 —— Monitor 对用户是可选安装,但 zip 里少了它同样不能发(`package.ps1` 断言恰好三个);合规文件组齐全(见下),`INSTALL.txt` 里的源码链接指向本 tag;`.sha256` 与 zip 实际哈希一致(`sha256sum -c` 或 `Get-FileHash`)。
8. **填发布说明**:用下面的模板改写草稿正文,SHA-256 **直接从 `package-summary.md`(草稿正文 / 资产 / job summary 三处同一份)复制,不要手抄**。核对无误后在网页上点发布。
9. **发布后**:先按下方「`staging` 与 `prod` 两个分支」前移分支 —— **每次发布(含 rc)都把 `staging` 前移到本次 tag**,**只有正式版再把 `prod` 前移到本次 tag**(两个分支都开着保护,前移时由仓库管理员临时允许绕过、推完恢复,J172;命令见该节;都不加 `--force`)。插件里的文档链接都指向 `prod`(见下「文档链接」),正式版漏了这一步,用户在插件里点开的就还是上一个正式版的手册;rc 不前移 `prod`(J163),所以**首个正式版之前,rc 构建里的这些链接是 404**(已接受,rc 的发布说明必须写明,见下)。**首个正式版这次**前移 `prod` 之后,这件事就不成立了:同一次把 [KNOWN_ISSUES](KNOWN_ISSUES.md) 的 KI-7 删掉,并删掉下方发布说明模板里「本版是预发布(rc)……404」那一句和注释里对它的说明。然后:若本次含契约变更,确认 KNOWN_ISSUES 与 DAW_COMPATIBILITY 的相关条目已同步。官网下载页是否上线、何时上线**待定**(见下「分发渠道」);上线后它必须发布**同一份** zip 与 `.sha256`,并与 Release 正文里的 SHA-256 逐字一致,同时同步官网下载页常量。

## 里程碑合并:`feature/v1` → `dev`(J170 / J171)

v1 的改动先逐张合进主支线 `feature/v1`,发版前再整体合进 `dev`,tag 打在 `dev` 上(J170)。做法是:**把 `feature/v1` 压成一个带 `Signed-off-by` 的提交合进 `dev`,完整历史留在 `feature/v1`**;不改写历史,不 force-push(J171)。

为什么不把 `feature/v1` 直接开 PR 合进 `dev`:主支线累积了几百个提交,`branch-gate` 的 DCO 步读 PR 提交列表的接口最多返回 250 条,所以到 250 条就直接判红(见 `branch-gate.yml` 里的注释);历史里也有少数提交缺 `Signed-off-by`,不改写历史补不上。压成一个提交之后,DCO 只看这一个提交。

**前置:`dev` 必须是 `feature/v1` 的祖先。** `dev` 上可能有主支线没有的提交(dependabot 的依赖升级、直接修在 `dev` 上的 CI 配置)。不是祖先时,下面第 1 步的 `git merge --squash` 就得当场解冲突,而那一步没有 PR 复审。所以先查:

```bash
git fetch origin
git merge-base --is-ancestor origin/dev origin/feature/v1; echo "ancestor-exit=$?"   # 必须是 0
```

不是 0 就先开一个 PR 把 `dev` 合进 `feature/v1`:从 `feature/v1` 拉分支,`git merge --signoff origin/dev`,逐个文件解冲突并在提交说明里写清取舍(原则:功能与发版流水线以 `feature/v1` 为准,`dev` 上的钉版与依赖升级保留),**用「Create a merge commit」合并(`gh pr merge --merge`),不要 squash** —— squash 不会把 `dev` 记成祖先,上面的检查仍然过不了。

前置满足后,由维护者执行:

1. 记下这次要压的 `feature/v1` 提交,从 `dev` 拉一条临时分支,把它压进来(前置满足时是快进形态,没有冲突):

   ```bash
   git fetch origin
   FV1=$(git rev-parse origin/feature/v1)
   git switch -c feat/v1-milestone-<版本> origin/dev
   git merge --squash "$FV1"
   git commit -s -F <提交说明文件>
   ```

   提交说明写:本次版本号、压的是 `feature/v1` 的哪个提交(完整 sha)、完整历史见 `feature/v1`、本版内容见 CHANGELOG 对应版本节;末行是 `Signed-off-by`。
2. 推之前核三件事 —— 树与 `$FV1` 逐字相同、恰好一个提交、带签名:

   ```bash
   git diff --quiet "$FV1" HEAD; echo "diff-exit=$?"          # 必须是 0
   git rev-list --count origin/dev..HEAD                      # 必须是 1
   git log -1 --format=%B | grep -c '^Signed-off-by:'         # 必须 >= 1
   ```
3. 推这条临时分支,开 PR 到 `dev`(`dev` 要求走 PR,不能直推;分支名要符合 `branch-gate` 的 `feat/*` / `feature/*`)。这个 PR 只有一个提交,DCO 与冻结契约守卫照常判。它会碰到冻结契约文件(`feature/v1` 期间对它们的改动),按 `CLAUDE.md` §5 挂 `status/frozen-contract` 标签;对应的变更文档已随 `feature/v1` 一起进来,不另写。
4. CI 绿了、PR 上的讨论全部解决(`dev` 开着 `required_conversation_resolution`)之后,用 squash 合并,**标题与正文显式给出,正文末行带 `Signed-off-by`**(不显式给时,正文由 GitHub 按仓库设置生成,不保证带签名):

   ```bash
   gh pr merge <PR 号> --squash -t "<标题> (#<PR 号>)" -b "<正文>"
   ```

   ⚠ 已知例外:docs-truth 里的「Changelog drafts not stranded」一步按 **base 分支的提交标题**判 CHANGELOG 正文里的每个 `(#N)` 有没有落地,而 `dev` 的历史里没有 `feature/v1` 上那些合并提交的标题 —— 所以它在这个 PR 上会成片判红(压进 `dev` 之后,push→`dev` 与之后 base=`dev` 的 PR 也一样),红的原因不是 CHANGELOG 写错。这一步怎么处理(让它改读 `feature/v1` 的落地记录,或确认后带着这条红合并)要在里程碑合并之前定。
5. 合后回读:`dev` 上的新提交与 `$FV1` 的树逐字相同、带签名:

   ```bash
   git fetch origin
   git diff --quiet "$FV1" origin/dev; echo "diff-exit=$?"     # 必须是 0
   git log -1 --format=%B origin/dev | grep -c '^Signed-off-by:'   # 必须 >= 1
   ```

   push→`dev` 会自动跑一次全量 `build-vst3`;它绿了才进发版清单第 6 步打 tag。

合完之后,`dev` 上的这个提交不在 `feature/v1` 的历史里,下次里程碑的前置检查会不过。所以**里程碑 PR 一合完就**把它以 `-s ours` 合回 `feature/v1` —— 它的内容 `feature/v1` 全有,这一步只记一笔「已合并」,不改任何文件:

```bash
git fetch origin
git rev-list --count <里程碑提交>..origin/dev                   # 必须是 0:dev 上还没有别的新提交
git switch -c feat/v1-milestone-<版本>-backmerge origin/feature/v1
git merge -s ours --signoff -m "<说明>" origin/dev
git diff --quiet origin/feature/v1 HEAD; echo "diff-exit=$?"   # 必须是 0:一个文件都不改
```

然后开 PR 到 `feature/v1`,它的 Files changed 应当是 0(不是 0 就停下),用「Create a merge commit」合并(`gh pr merge --merge`)。第二条命令不是 0(`dev` 上已经有了别的新提交)就**不要**用 `-s ours` —— 它会把那些提交的改动一起丢掉;改走上面「前置」那条正常合并。那时 `dev` 上的里程碑提交会与 `$FV1` 之后 `feature/v1` 改过的每一处冲突,要逐个文件对照 `$FV1` 解,量可能很大 —— 这正是要「一合完就做」的原因。

## `staging` 与 `prod` 两个分支(J163 / J163a / J172)

| 分支 | 指向 | 什么时候前移 |
|---|---|---|
| `staging` | 最新一个**已发布**的版本,**含预发布** | 每次在 Releases 页点了发布之后(`vX.Y.Z-rc.N` 与 `vX.Y.Z` 都算) |
| `prod` | 最新一个**正式版** | 只在正式版 `vX.Y.Z` 发布之后;rc 一律不动 `prod` |

- rc 发布后前移 `staging`;正式版发布后先前移 `staging`,再前移 `prod`。两个分支都要指向**这次 tag 指向的那个提交本身**(快进),所以不走 PR —— GitHub 的 PR 合并不做快进,会多出一个合并提交,分支就不等于 tag 提交了。
- **两个分支都开着保护,直推会被拒**:保护要求走 PR,而且「管理员也不能绕过」(`enforce_admins`)是开着的。按 J172,前移时**由用户(仓库管理员)临时允许管理员绕过,直推快进,推完立刻恢复**;也可以临时整条放开该分支的保护,推完原样恢复。每次只动正在前移的那一个分支。**push 不管成败,都先执行恢复那一行再排查** —— 别让保护开着口子等排查;下面把 push 的退出码先存下来,恢复之后再看。以 rc 前移 `staging` 为例(正式版把 tag 换成 `vX.Y.Z`;前移 `prod` 时把命令里的 `staging` 全部换成 `prod`,整组再走一遍):

  ```bash
  gh api -X DELETE repos/synchain-oss/scvb/branches/staging/protection/enforce_admins     # 临时允许管理员绕过
  git push origin vX.Y.Z-rc.N^{commit}:refs/heads/staging; rc=$?                           # 快进,不加 --force
  gh api -X POST repos/synchain-oss/scvb/branches/staging/protection/enforce_admins       # 不管 push 成败,立刻恢复
  echo "push-exit=$rc"                                                                      # 不是 0 就按下面「推不上时」那条排查
  gh api repos/synchain-oss/scvb/branches/staging/protection/enforce_admins -q .enabled   # 必须输出 true
  git ls-remote origin refs/heads/staging                                                   # 必须等于下一行的输出
  git rev-parse vX.Y.Z-rc.N^{commit}
  ```

- 都**不加 `--force`**。推不上时先看报错:带 `GH006` / `protected branch` 的是保护还在生效(第一条命令没执行或没生效),与祖先关系无关;报 `non-fast-forward` / `fetch first` 的才是目标分支不是这次 tag 的祖先,先查清再动。
- 前移发生在「点了发布」之后,不在推 tag 时:tag 推上去只建草稿,草稿不算已发布。演练 tag(`v0.0.0-test*`)从不发布,两个分支都不动。
- 预发布分支用仓库里**已有的** `staging`,不另建新分支(J163a)。`staging` 与 `prod` 截至 2026-09-28 都停在仓库首个提交 `ae61f5f`(骨架,里面没有用户手册与 DAW 兼容表);它是之后所有提交的祖先,所以首个 rc(`v0.9.0-rc.1`)那次前移 `staging`、首个正式版那次前移 `prod`,都是快进。

## 文档链接:插件里指向 `prod`,发布说明指向 tag

- **插件里的文档链接一律指向 `prod` 分支上的固定路径**(用户裁定 J149:`prod` 是稳定正式版分支,`dev` 是研发分支)。设置页「使用说明」一栏的「文档」按钮打开 `https://github.com/synchain-oss/scvb/blob/prod/docs/USER_GUIDE.zh-CN.md`(中文界面)或 `https://github.com/synchain-oss/scvb/blob/prod/docs/USER_GUIDE.md`(英文、法文界面);九条使用规则里 DAW 兼容表的地址是 `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`。这些地址**不随插件版本号变,也不 pin 到 tag** —— 此前「按插件版本号 pin 到同号 tag」的做法(SL-220 / #298)已由 J149 取代,不要改回去。代价照实写:旧版插件打开的是最新正式版的手册,不是它自己那一版的。
- **rc 期间这些链接打开是 404,已接受(J163)。** `prod` 只跟正式版,首个正式版发布之前 `prod` 上没有这三个文件,所以 `v0.9.0-rc.N` 这批 rc 构建里设置页的「文档」按钮与九条规则里的 DAW 兼容表地址都打不开。首个正式版发布、`prod` 前移之后,**同一批 rc 构建里的链接也随之恢复**(地址没变,变的是 `prod` 上的内容),插件不用更新。首个正式版之后再发的 rc,这些链接打开的是上一个正式版的手册。**首个正式版之前的每个 rc,发布说明都必须写明这一点**(模板「升级须知」里有对应一句),并在 [KNOWN_ISSUES](KNOWN_ISSUES.md) KI-7 登记。
- 地址写在 `web/output/tab-settings.js` 的 `docsUrl()` 与红字真源 `docs/USER_GUIDE.zh-CN.md#硬约束` 里(后者经 `scripts/gen-hard-rules.mjs` 生成到插件词条);`web-preview/tests/smoke-tab4-settings.mjs` 扫 `web/` 下的仓库 `blob/` 链接,指向 `prod` 以外的分支或 tag 即红。
- **插件一旦发出去,里面的地址就改不了了。** 所以 `docs/USER_GUIDE.md`、`docs/USER_GUIDE.zh-CN.md`、`docs/DAW_COMPATIBILITY.md` 在 `prod` 上不要改名、不要挪位置 —— 改了,已经发出去的每一版插件里的这几个链接都会一起失效。
- **发布说明(GitHub Release 正文)里的链接仍固定在本次的 tag 上**(见下方模板与模板后的说明):那是这一版自己的记录,不跟着 `prod` 走。

## zip 内必须携带的合规文件组

GPLv3 §4/§5 要求分发时保留法律声明,§6 要求目标码分发伴随源码获取途径,OFL 要求许可证随字体分发。**用户拿到的只有这个 zip,他们没有义务访问仓库** —— 把合规主张寄托在「仓库里有 LICENSE」是不成立的。

```
SCVB-vX.Y.Z-win64.zip
├── SCVB Input.vst3/            完整 bundle 目录层级(必装)
├── SCVB Output.vst3/           (必装)
├── SCVB Monitor.vst3/          (可选的只读观察窗)
├── LICENSE.txt                 GPLv3 全文(= 仓库根 LICENSE)
├── THIRD-PARTY-NOTICES.md      第三方依赖与各自许可证
├── LICENSES/                   仓库 LICENSES/ 下的全部许可证全文(应有哪些由 THIRD-PARTY-NOTICES.md 的「随二进制分发」表决定)
├── third_party/notices/        仓库同名目录的全部文件:JUCE 内置库与 WebView2 loader 的上游版权 / 许可声明原文
│                               (NOTICES 对 HarfBuzz 只写了首行版权,其余各行见这里的 harfbuzz.COPYING)
└── INSTALL.txt                 安装步骤 + 未签名插件的「解除锁定」与 SmartScreen 说明 + 九条规则前 3 条
                                + 精确到 tag 的源码获取地址(GPLv3 §6 的书面声明)
```

U2 裁定**不附** `LICENSE-EXCEPTION.md`(依赖 GPLv3 系统库例外的默认解释,见 `THIRD-PARTY-NOTICES.md`),所以 zip 里没有它。
以上每一项都由 `scripts/package.ps1` 在打包后重新打开 zip 断言(三个 bundle 的 DLL 条目、每个合规文件、`THIRD-PARTY-NOTICES.md` 点名的每个声明文件路径、`INSTALL.txt` 的源码声明行、逐条目与源文件字节一致、根目录无清单外条目且 `third_party/` 下只有 `notices/`);`INSTALL.txt` 里的九条规则原文从用户手册的生成区读取,不另存副本。

## 发布说明模板

维护者发版时复制下面这段填写(12 §4.4)。没有内容的整节删掉,不要留空标题。

```markdown
# Synchain Vocal Balancer v{X.Y.Z} (Windows x64)

> SCVB 是一对配套插件(Input + Output),**必须同时安装、成对使用**;zip 里的第三个插件 SCVB Monitor 是**可选**的只读旁观窗口。

## ⚠️ 升级须知
<!-- 「三个插件一起升级」与「文档链接」两条每次都留;「rc 期间 404」那条首个正式版之前的 rc 必留、之后删;其余有则填,无则删 -->
- **Input、Output、Monitor 三个一起升级,不要混装。** IPC 协议版本不同的两侧会拒绝互连;协议相同时虽然能连上,但新 Output 配旧 Input 做离线渲染,两侧的交接方式不同,人声可能被**双路叠加**(不升 IPC abi 的有意取舍,见 `docs/contract-changes/` 相应变更文档)—— 所以升级一律三个一起换。
- state abi:{旧}→{新},旧工程{可自动迁移 / 需手动重新分析}。**用本版保存的工程,拿到 state abi 更低的旧版本(含此前的内部测试包)里打开时会被拒载**:Output 显示「工程来自较新版本」横幅,Input 以默认值运行(没有横幅)。建议不要在旧版本里打开并保存这类工程。
  <!-- 首个公开版本填写时:当前 state abi = 6(abi 5→6 来自 SL-472 的 channels 配置落盘,J114 批准,变更文档 docs/contract-changes/20260927-sl472-channel-config-persist.md);发版前以 src/core/state/StateCodec.h 的 kCurrentAbi 为准 -->
- IPC abi:{旧}→{新},**必须同时升级 Input 与 Output**,混装会互不识别
- DSP 可闻变化:{有/无};有则说明旧工程重渲染会有什么差异
- 本说明里的文档链接固定在 `v{X.Y.Z}` 这个 tag 上,是这一版的手册;插件里的文档链接(设置页「文档」按钮、九条使用规则里的 DAW 兼容表地址)固定指向 `prod` 分支,打开的是最新正式版的手册。
- **本版是预发布(rc):插件里的这些文档链接在 rc 期间打开是 404,正式版发布后恢复**,插件不用更新(`prod` 分支只跟正式版,首个正式版之前上面还没有这些文件)。在那之前请用本说明里的链接,或 zip 里 `INSTALL.txt` 的手册链接(同样固定在 `v{X.Y.Z}` 上)。

## 如遇问题如何回退
<!-- 每次都留。首个公开版本之前没有公开版本可回:第 2 步改成「装回你之前在用的版本(例如内部测试包)」,或只留第 1、3 步 -->
1. 关掉 DAW,删除 `C:\Program Files\Common Files\VST3\` 下的 `SCVB Input.vst3`、`SCVB Output.vst3` 与 `SCVB Monitor.vst3` 三个文件夹;
2. 到 [Releases 页](https://github.com/synchain-oss/scvb/releases) 下载上一版的 zip,核对 SHA-256 后照同样的步骤装回三个插件(三个一起换,不要混装);
3. **工程兼容性**:用本版保存过的工程,拿到 state abi 更低的旧版本里打开会被拒载(见上「升级须知」)。回退前先确认手上有一份用旧版保存的工程副本;建议不要在旧版本里打开并保存用本版保存过的工程。

## 本次更新
### ⚠️ 契约变更
### 新增
### 变更
### 修复
<!-- 直接从 CHANGELOG 对应版本节复制 -->

## 下载与安装
| 资产 | 说明 |
|---|---|
| `SCVB-v{X.Y.Z}-win64.zip` | 含 `SCVB Input.vst3`、`SCVB Output.vst3`(必装)与 `SCVB Monitor.vst3`(可选),都是完整 bundle 目录;解压前先解除锁定(见下),解压后把整个 `.vst3` 文件夹复制到 `C:\Program Files\Common Files\VST3\`;zip 根目录另含 `LICENSE.txt`、`THIRD-PARTY-NOTICES.md`、`LICENSES/`、`third_party/notices/`、`INSTALL.txt` |
| `SCVB-v{X.Y.Z}-win64.zip.sha256` | 独立校验文件(`sha256sum -c` 可直接用) |
| `package-summary.md` | 版本 / 文件名 / 大小 / SHA-256 / 发布日期 / 源码提交 / zip 内逐条目哈希 |

SHA-256(直接从 `package-summary.md` 复制,不要手抄):

    <zip 的哈希>

系统要求:Windows 10 1809+ / WebView2 Evergreen Runtime(通常已随 Windows 预装)

源码(GPL-3.0-or-later):本版对应的完整源码在 https://github.com/synchain-oss/scvb/tree/v{X.Y.Z}(Release 页下方 GitHub 自动附带的「Source code」压缩包是同一份),构建方法见 [README「从源码构建」](https://github.com/synchain-oss/scvb/blob/v{X.Y.Z}/README.zh-CN.md#从源码构建)。

<!-- 未签名时必填 -->
> 本项目未做代码签名(U13),浏览器或 Windows 可能提示「未知发布者」。先核对 SHA-256;**解压前**右键 zip → 属性 → 常规 → 勾选「解除锁定」→ 确定(或 PowerShell `Unblock-File .\SCVB-v{X.Y.Z}-win64.zip`);SmartScreen 拦下时点「更多信息 → 仍要运行」(浏览器里选「保留」)。zip 里的 `INSTALL.txt` 有同样的中英文步骤,分步说明也见[用户手册 · 安装](https://github.com/synchain-oss/scvb/blob/v{X.Y.Z}/docs/USER_GUIDE.zh-CN.md#安装)。你也可以自行从源码构建校验(见 [CONTRIBUTOR_ONBOARDING.md](https://github.com/synchain-oss/scvb/blob/v{X.Y.Z}/docs/CONTRIBUTOR_ONBOARDING.md))。

## 首次使用?
先读 **[九条使用规则](https://github.com/synchain-oss/scvb/blob/v{X.Y.Z}/docs/USER_GUIDE.zh-CN.md#硬约束)** —— 路由摆错会直接出静音。

## 验证
- pluginval strictness 5(CI,`--skip-gui-tests`):✅
- pluginval strictness 5 全量含 GUI(本地 Windows 11):✅
- Catch2 单测:{N} passed
- DAW 实测矩阵:见 [DAW_COMPATIBILITY.md](https://github.com/synchain-oss/scvb/blob/v{X.Y.Z}/docs/DAW_COMPATIBILITY.md)

## 完整变更
**Full Changelog**: https://github.com/synchain-oss/scvb/compare/v{上一版}...v{X.Y.Z}
```

> 模板里的链接文案统一写「**九条**」。这是唯一一处会同时出现在插件 UI、README、发布说明与官网的文案,写错一次就是四个面一起错。

> **模板里的链接必须是 pin 到 tag 的绝对 URL,别用仓库相对路径。** 这段文本的落地面是 **GitHub Release 正文**,那里没有「当前文件所在目录」这个上下文,`docs/USER_GUIDE.zh-CN.md#硬约束` 之类的相对链接不会解析到仓内文件;而且它们落在 ```markdown 围栏里,`markdown-link-check` 不提取围栏内的链接,**没有任何机检会替你发现**。即便相对链接能解析,它指的也是 HEAD 而不是这次发的 tag —— 用户下了 `v1.2.0` 却读到 dev 上已经改过的手册,是这套「唯一真源」链条里最后一处会漂的地方。`{X.Y.Z}` 本来就是模板里已有的占位符,填的时候顺手带上即可。这与第 8 步「SHA-256 不要手抄」是同一类要求:模板里就该填对。

## 分发渠道

- **权威产物来源**:GitHub Releases —— 单个 zip(三个 `.vst3`:Input / Output / 可选的 Monitor + `INSTALL.txt` + 合规文件组)+ 独立 `.sha256` + `package-summary.md`。三者都由 CI 产出,**Release 正文里的那个哈希是唯一权威值**。README 与用户手册目前只把用户指向 GitHub Releases。
- **官网下载页**:是否作为用户入口**待定**。若上线,官网必须发布**同一份** zip 与 `.sha256`,并与 Release 正文的哈希逐字一致 —— 否则用户会被引到一条只验传输、不验来源的弱路径上;上线时同步改 README 的安装小节。

**为什么合并成一个 zip**:Input 与 Output 本来就配对使用,分开下载最常见的用户故障就是「只装了一个」;Monitor 跟它们同版本同一次发布,放进同一个 zip 也免得版本对不上。
