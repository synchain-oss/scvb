# RELEASE —— SCVB 发布流程与发布说明模板

> 状态:演进中
> 最后更新:2026-08-24
> 真源:12 §4.1–§4.5(版本号 / tag / CHANGELOG / release note / 分发渠道)

本文件是**维护者**发版时照着走的清单,以及发布说明的模板。用户侧的安装说明在 [README](../README.zh-CN.md),使用说明在[用户手册](USER_GUIDE.zh-CN.md)。

## 版本号的唯一真源

SCVB 的版本号真源是顶层 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。运行时经 JUCE 的 `ProjectInfo::versionString` / `JucePlugin_VersionString` 读出。

**铁律:除真源外,任何地方都不得硬编码版本号** —— README、docs、UI HTML、脚本、workflow 一律不写死。README 里的版本靠 badge 动态显示(shields.io 读 GitHub Release)。

下游镜像(发版时必须同步):官网下载页常量、`CHANGELOG.md`、Release tag。

**两个插件共用同一个版本号、同一次发布。** 它们本来就配对使用,分开编号的直接后果是用户装出不匹配的组合,而 SCVB 会拒绝半兼容连接。

## Tag 规则

- 格式 **`vX.Y.Z`**,纯 semver 无前缀;预发布 `vX.Y.Z-rc.N`(GitHub Release 勾 pre-release)。
- tag 只由维护者在 `dev`(或将来的 release 分支)上打,**不在 feature 分支打 tag**。
- CI 的 `version-check` 在 tag push 时比对 tag 与 `CMakeLists.txt` 的 VERSION,不一致即 fail。

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

## 发版清单

1. **确认 CHANGELOG**:`## [Unreleased]` 的内容完整(每条带 PR 号),契约变更条目齐全且各自有 `docs/contract-changes/` 文档。
2. **下移版本节**:把 Unreleased 内容改写成 `## [X.Y.Z] - YYYY-MM-DD`,补底部对比链接,留一个空的 Unreleased。
3. **改版本号**:改 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`。这是唯一一处。
4. **跑全量门禁**:`pwsh scripts/gates.ps1`(含真机 GUI pluginval),必须全绿。
5. **红字真源自检**:`node scripts/gen-hard-rules.mjs --check` 退出码 0;`docs/hard-rules.i18n.json` 的 `frReview.status` 必须是 `reviewed` —— **fr 红字未经人工审校不得发版**(05 §5:机翻安全警告发到公开产品是明确禁止项)。
6. **打 tag 并推送**:`git tag vX.Y.Z && git push origin vX.Y.Z`。`version-check` 与 release workflow 随之触发。
7. **核对产物**:zip 里 `SCVB Input.vst3` / `SCVB Output.vst3` 两个完整 bundle 齐全,合规文件组齐全(见下),`.sha256` 独立文件存在。
8. **填发布说明**:用下面的模板,SHA-256 **直接从 CI job summary 的 `dist/package-summary.md` 复制,不要手抄**。
9. **发布后**:同步官网下载页常量;若本次含契约变更,确认 KNOWN_ISSUES 与 DAW_COMPATIBILITY 的相关条目已同步。

## zip 内必须携带的合规文件组

GPLv3 §4/§5 要求分发时保留法律声明,§6 要求目标码分发伴随源码获取途径,OFL 要求许可证随字体分发。**用户拿到的只有这个 zip,他们没有义务访问仓库** —— 把合规主张寄托在「仓库里有 LICENSE」是不成立的。

```
SCVB-vX.Y.Z-win64.zip
├── SCVB Input.vst3/            完整 bundle 目录层级
├── SCVB Output.vst3/
├── LICENSE.txt                 GPLv3 全文
├── THIRD-PARTY-NOTICES.md      第三方依赖与各自许可证
├── LICENSES/OFL-1.1.txt        字体许可证
├── LICENSE-EXCEPTION.md        若 U2 采纳:GPLv3 §7 的 WebView2 链接例外
└── INSTALL.txt                 安装路径 + 对应源码获取地址(GPLv3 §6 的书面声明)
```

## 发布说明模板

维护者发版时复制下面这段填写(12 §4.4)。没有内容的整节删掉,不要留空标题。

```markdown
# Synchain Vocal Balancer v{X.Y.Z} (Windows x64)

> SCVB 是一对配套插件(Input + Output),**必须同时安装、成对使用**。

## ⚠️ 升级须知
<!-- 有则填,无则整节删除 -->
- state abi:{旧}→{新},旧工程{可自动迁移 / 需手动重新分析}
- IPC abi:{旧}→{新},**必须同时升级 Input 与 Output**,混装会互不识别
- DSP 可闻变化:{有/无};有则说明旧工程重渲染会有什么差异

## 本次更新
### ⚠️ 契约变更
### 新增
### 变更
### 修复
<!-- 直接从 CHANGELOG 对应版本节复制 -->

## 下载与安装
| 资产 | 说明 |
|---|---|
| `SCVB-v{X.Y.Z}-win64.zip` | 含 `SCVB Input.vst3` 与 `SCVB Output.vst3`(完整 bundle 目录),解压后把整个 `.vst3` 文件夹复制到 `C:\Program Files\Common Files\VST3\`;zip 根目录另含 `LICENSE.txt`、`THIRD-PARTY-NOTICES.md`、`LICENSES/OFL-1.1.txt`、(若 U2 采纳)`LICENSE-EXCEPTION.md`、`INSTALL.txt` |
| `SCVB-v{X.Y.Z}-win64.zip.sha256` | 独立校验文件(由 `scripts/package.ps1` 生成) |

SHA-256(直接从 CI 的 job summary `dist/package-summary.md` 复制,不要手抄):

    <zip 的哈希>

系统要求:Windows 10 1809+ / WebView2 Evergreen Runtime(通常已随 Windows 预装)

<!-- 未签名时必填 -->
> 首次运行 Windows SmartScreen 可能提示「未知发布者」,点「更多信息 → 仍要运行」。本项目当前未做代码签名,你可以自行从源码构建校验(见 CONTRIBUTOR_ONBOARDING.md)。

## 首次使用?
先读 **[九条硬约束](docs/USER_GUIDE.zh-CN.md#硬约束)** —— 路由摆错会直接出静音。

## 验证
- pluginval strictness 5(CI,`--skip-gui-tests`):✅
- pluginval strictness 5 全量含 GUI(本地 Windows 11):✅
- Catch2 单测:{N} passed
- DAW 实测矩阵:见 [DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md)

## 完整变更
**Full Changelog**: https://github.com/synchain-oss/scvb/compare/v{上一版}...v{X.Y.Z}
```

> 模板里的链接文案统一写「**九条**硬约束」。这是唯一一处会同时出现在插件 UI、README、发布说明与官网的文案,写错一次就是四个面一起错。

## 分发渠道

- **主渠道**:GitHub Releases —— 单个 zip(两个 `.vst3` + `INSTALL.txt` + 合规文件组)+ 独立 `.sha256`。
- **次渠道**:官网下载页。

**为什么合并成一个 zip**:两个插件本来就配对使用,分开下载最常见的用户故障就是「只装了一个」。
