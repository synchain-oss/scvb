# RELEASE —— SCVB 发布流程与发布说明模板

> 状态:演进中
> 最后更新:2026-09-28
> 真源:12 §4.1–§4.5(版本号 / tag / CHANGELOG / release note / 分发渠道)

本文件是**维护者**发版时照着走的清单,以及发布说明的模板。用户侧的安装说明在 [README](../README.zh-CN.md),使用说明在[用户手册](USER_GUIDE.zh-CN.md)。

## 版本号的唯一真源

SCVB 的版本号真源是顶层 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。运行时经 JUCE 的 `ProjectInfo::versionString` / `JucePlugin_VersionString` 读出。

**铁律:除真源外,任何地方都不得硬编码版本号** —— README、docs、UI HTML、脚本、workflow 一律不写死。README 里的版本靠 badge 动态显示(shields.io 读 GitHub Release)。

下游镜像(发版时必须同步):`CHANGELOG.md`、Release tag;官网下载页上线后再加上它的常量。

**三个插件(Input / Output / Monitor)共用同一个版本号、同一次发布、同一个 zip。** Input 与 Output 本来就配对使用,分开编号的直接后果是用户装出不匹配的组合,而 SCVB 会拒绝半兼容连接;Monitor 是可选的只读观察窗,跟着同一个版本走。

## Tag 规则

- 格式 **`vX.Y.Z`**,纯 semver 无前缀;预发布 `vX.Y.Z-rc.N`(流水线建的草稿自动勾 pre-release)。**rc 不改 `CMakeLists.txt`**:`v1.2.3-rc.1` 与 `v1.2.3` 对应的都是 `project(SCVB VERSION 1.2.3)`。
- 演练专用 **`v0.0.0-test`**(可加 `.N`):只用来走通「构建 → 打包 → 草稿 Release」全程,不比对 CMake 版本;限死 `0.0.0` 是为了让它不可能冒充真版本。用完删掉 tag 与草稿(见下方发版清单第 0 步)。
- 其他形态(`v1.2.3-beta.1`、`v1.2` 等)一律被拒。
- tag 只由维护者在 `dev`(或将来的 release 分支)上打,**不在 feature 分支打 tag**(唯一例外是演练 tag,见发版清单第 0 步)。
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
| `verify-tag` | 先跑判据自测,再比对 tag 与 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`(`scripts/check-release-tag.ps1`,规则见上方「Tag 规则」);再跑 `package.ps1 -Preflight`(许可证全文覆盖、INSTALL.txt 的规则提取) | 立刻红,不进构建 |
| `build` | **调用 `build-vst3.yml`**(同一份配方):构建(/W4 零 warning)→ ctest → 三个 bundle 的 pluginval(CI 无桌面,`--skip-gui-tests`)→ 按 tag 名上传 `.vst3` artifact | 红,不打包 |
| `release` | 取回 artifact → `scripts/package.ps1` 打 zip / `.sha256` / `package-summary.md` 并解包断言 → `gh release create --draft` 建**草稿** Release(rc 与演练 tag 自动勾 pre-release);summary 同时写进 job summary | 红,不建 Release |

- 权限:workflow 级只读;只有 `release` job 拿 `contents: write`。所有 action 都 pin 到 40 位 SHA。
- 同一个 tag 重跑:已有草稿就覆盖资产,并把正文重置为新的 `package-summary.md`(手改过的正文会丢,改正文放在最后一次重跑之后);**已发布的 Release 流水线一律不碰**。
- 许可证全文:`THIRD-PARTY-NOTICES.md`「随二进制分发」表点名的每个许可证(加本项目的 GPL-3.0-or-later)都必须在 `LICENSES/` 里有全文,缺一个 `package.ps1` 就红;只有演练 tag 降为警告并在 `package-summary.md` 的 `missingLicenseTexts` 行写明。**截至本段写下时,该表登记的 WebView2 SDK 许可证 BSD-3-Clause 在 `LICENSES/` 里没有全文 —— 补上之前,正式版和 rc tag 都会停在 `verify-tag` 的 preflight(构建之前)。**
- 草稿 Release 的正文是 `package-summary.md`(版本 / 文件名 / 大小 / SHA-256 / 发布日期 / 源码提交 / 逐条目哈希),发布前按下方模板改写。
- **「tag 触发 → 调用构建 → 建草稿」这一段只有推 tag 才会执行**:PR 上的 CI 只跑 `build-vst3`,不跑 `release.yml`;`scripts/package.ps1` 可以拿 `build-vst3` 的产物在本地试打包(`-BuildDir <artifact 目录> -Version 0.0.0-dryrun -AllowMissingLicenseTexts`;这个开关只用于本地试打包与演练 tag,BSD-3-Clause 全文补上之前不带它会红在许可证检查上),但覆盖不到 workflow 本身。所以首次发版、以及改过这三处文件之后,先做下面第 0 步。

## 发版清单

0. **(首次,或改过 `release.yml` / `build-vst3.yml` / `scripts/package.ps1` 之后)演练一次**:`git tag v0.0.0-test <要验的提交> && git push origin v0.0.0-test`。tag push 跑的是**被打 tag 那个提交里**的 `release.yml`,所以要验的提交必须已含 T40(`feature/v1` 上 T40 合并之后的任一提交即可;演练 tag 是「不在 feature 分支打 tag」的唯一例外,用完即删)。等 Release workflow 全绿,在 Releases 页打开标着 `[pipeline test - delete me]` 的草稿,下载 zip 与 `.sha256`,核对第 7 步那几项。演练完**删草稿再删 tag**:`gh release delete v0.0.0-test --yes` 然后 `git push origin :refs/tags/v0.0.0-test && git tag -d v0.0.0-test`。要再演练一次就用 `v0.0.0-test.2` 之类的新名字,或先删干净再推。
1. **确认 CHANGELOG**:`## [Unreleased]` 的内容完整(每条带 PR 号),契约变更条目齐全且各自有 `docs/contract-changes/` 文档。
2. **下移版本节**:把 Unreleased 内容改写成 `## [X.Y.Z] - YYYY-MM-DD`,补底部对比链接,留一个空的 Unreleased。
3. **改版本号**:改 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。这是唯一一处(rc 与正式版用同一个 X.Y.Z)。
4. **跑全量门禁**:`pwsh scripts/gates.ps1`(含真机 GUI pluginval),必须全绿;并按 `CLAUDE.md` 的出包硬规对目标 ref dispatch 一次 `build-vst3` 并全绿。
5. **红字真源自检**:`node scripts/gen-hard-rules.mjs --check` 退出码 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 必须是 `reviewed` —— **fr 红字未经人工审校不得发版**(05 §5:机翻安全警告发到公开产品是明确禁止项)。
6. **打 tag 并推送**:`git tag vX.Y.Z && git push origin vX.Y.Z`(预发布用 `vX.Y.Z-rc.N`)。`release.yml` 随之触发,`verify-tag` 先卡版本号。
7. **核对产物**(草稿 Release 的资产):zip 里 `SCVB Input.vst3` / `SCVB Output.vst3` / `SCVB Monitor.vst3` 三个完整 bundle 齐全,合规文件组齐全(见下),`INSTALL.txt` 里的源码链接指向本 tag;`.sha256` 与 zip 实际哈希一致(`sha256sum -c` 或 `Get-FileHash`)。
8. **填发布说明**:用下面的模板改写草稿正文,SHA-256 **直接从 `package-summary.md`(草稿正文 / 资产 / job summary 三处同一份)复制,不要手抄**。核对无误后在网页上点发布。
9. **发布后**:若本次含契约变更,确认 KNOWN_ISSUES 与 DAW_COMPATIBILITY 的相关条目已同步。官网下载页是否上线、何时上线**待定**;上线后它必须发布**同一份** zip 与 `.sha256`,并与 Release 正文里的 SHA-256 逐字一致,同时同步官网下载页常量。

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
└── INSTALL.txt                 安装步骤 + 未签名插件的「解除锁定」与 SmartScreen 说明 + 九条规则前 3 条
                                + 精确到 tag 的源码获取地址(GPLv3 §6 的书面声明)
```

U2 裁定**不附** `LICENSE-EXCEPTION.md`(依赖 GPLv3 系统库例外的默认解释,见 `THIRD-PARTY-NOTICES.md`),所以 zip 里没有它。
以上每一项都由 `scripts/package.ps1` 在打包后重新打开 zip 断言(三个 bundle 的 DLL 条目、每个合规文件、`INSTALL.txt` 的源码声明行、逐条目与源文件字节一致、根目录无清单外条目);`INSTALL.txt` 里的九条规则原文从用户手册的生成区读取,不另存副本。

## 发布说明模板

维护者发版时复制下面这段填写(12 §4.4)。没有内容的整节删掉,不要留空标题。

```markdown
# Synchain Vocal Balancer v{X.Y.Z} (Windows x64)

> SCVB 是一对配套插件(Input + Output),**必须同时安装、成对使用**;zip 里的第三个插件 SCVB Monitor 是**可选**的只读旁观窗口。

## ⚠️ 升级须知
<!-- 「三个插件一起升级」与「文档链接」两条每次都留;其余有则填,无则删 -->
- **Input、Output、Monitor 三个一起升级,不要混装。** IPC 协议版本不同的两侧会拒绝互连;协议相同时虽然能连上,但新 Output 配旧 Input 做离线渲染,两侧的交接方式不同,人声可能被**双路叠加**(不升 IPC abi 的有意取舍,见 `docs/contract-changes/` 相应变更文档)—— 所以升级一律三个一起换。
- state abi:{旧}→{新},旧工程{可自动迁移 / 需手动重新分析}。**用本版保存的工程,拿到 state abi 更低的旧版本(含此前的内部测试包)里打开时会被拒载**:Output 显示「工程来自较新版本」横幅,Input 以默认值运行(没有横幅)。建议不要在旧版本里打开并保存这类工程。
  <!-- 首个公开版本填写时:当前 state abi = 6(abi 5→6 来自 SL-472 的 channels 配置落盘,变更文档 docs/contract-changes/20260927-sl472-channel-config-persist.md);发版前以 src/core/state/StateCodec.h 的 kCurrentAbi 为准 -->
- IPC abi:{旧}→{新},**必须同时升级 Input 与 Output**,混装会互不识别
- DSP 可闻变化:{有/无};有则说明旧工程重渲染会有什么差异
- 本说明里的文档链接都固定在 `v{X.Y.Z}` 这个 tag 上;插件设置页「说明文档」按钮打开的也是与插件版本同号 tag 下的手册。

## 本次更新
### ⚠️ 契约变更
### 新增
### 变更
### 修复
<!-- 直接从 CHANGELOG 对应版本节复制 -->

## 下载与安装
| 资产 | 说明 |
|---|---|
| `SCVB-v{X.Y.Z}-win64.zip` | 含 `SCVB Input.vst3`、`SCVB Output.vst3`(必装)与 `SCVB Monitor.vst3`(可选),都是完整 bundle 目录;解压前先解除锁定(见下),解压后把整个 `.vst3` 文件夹复制到 `C:\Program Files\Common Files\VST3\`;zip 根目录另含 `LICENSE.txt`、`THIRD-PARTY-NOTICES.md`、`LICENSES/`、`INSTALL.txt` |
| `SCVB-v{X.Y.Z}-win64.zip.sha256` | 独立校验文件(`sha256sum -c` 可直接用) |
| `package-summary.md` | 版本 / 文件名 / 大小 / SHA-256 / 发布日期 / 源码提交 / zip 内逐条目哈希 |

SHA-256(直接从 `package-summary.md` 复制,不要手抄):

    <zip 的哈希>

系统要求:Windows 10 1809+ / WebView2 Evergreen Runtime(通常已随 Windows 预装)

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
