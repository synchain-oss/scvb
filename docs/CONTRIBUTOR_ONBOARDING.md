# CONTRIBUTOR_ONBOARDING —— 新贡献者上手

> **目标**:一个从没碰过 JUCE 的 C++ 开发者,照本文从零操作,**30 分钟内**(不含下载时间)构建出两个 `.vst3` 并跑通单测。
> 读完本文再看 [CONTRIBUTING.md](../CONTRIBUTING.md)(协作规则)与 [CLAUDE.md](../CLAUDE.md)(常驻法条,§6 是工具链清单的单一真源)。

SCVB 目前只支持 **Windows 10/11 x64**。macOS / Linux 不在 v1 范围内 —— UI 依赖 WebView2,IPC 依赖 Windows 命名共享内存段。

---

## 1. 你需要装什么

| 组件 | 版本要求 | 怎么装 |
| --- | --- | --- |
| Visual Studio 2022 | Desktop development with C++ 工作负载 | https://visualstudio.microsoft.com/downloads/ |
| CMake | ≥ 3.22 | `winget install Kitware.CMake` |
| Git | 任意近版 | `winget install Git.Git` |
| PowerShell 7 | `pwsh` | `winget install Microsoft.PowerShell` |
| JUCE | **`.juce-version` 里那个 tag** | 见下方 §2 |
| WebView2 Evergreen Runtime | 最新 | https://developer.microsoft.com/microsoft-edge/webview2/ (Windows 11 通常已自带) |
| clang-format | **必须正好 18.1.8** | LLVM 18.1.8 安装包,或 `pip install clang-format==18.1.8` |
| pluginval | `.pluginval-version` 里那个 tag | https://github.com/Tracktion/pluginval/releases |
| Node.js | ≥ 20(跑 prettier / `check-i18n.mjs` / web smoke) | `winget install OpenJS.NodeJS.LTS` |
| Python 3 | 仅在需要重新生成子集字体时 | https://www.python.org/downloads/ |

WebView2 **SDK**(NuGet 包)不用手装 —— CMake 配置时会按 `CMakeLists.txt` 里的 `WEBVIEW2_VERSION` 自动拉到构建目录。

> **版本纪律**:JUCE / pluginval / gitleaks 的版本真源分别是仓库根的 `.juce-version` / `.pluginval-version` / `.gitleaks-version`。
> 脚本一律从这些文件读,**请不要在任何脚本或 workflow 里写版本号字面量**。`gates.ps1` 的依赖预检会拿 `git describe --tags` 的结果和 `.juce-version` 对比,不一致直接判红。

## 2. 拉代码与 JUCE

```powershell
git clone https://github.com/synchain-oss/scvb.git
cd scvb

# JUCE 单独检出到仓库外,tag 必须与 .juce-version 一致
$juce = (Get-Content .juce-version -Raw).Trim()
git clone --branch $juce --depth 1 https://github.com/juce-framework/JUCE.git ..\juce
```

把 JUCE 路径固化到环境变量,后面所有脚本都能免参数跑:

```powershell
$env:JUCE_PATH = (Resolve-Path ..\juce).Path
[Environment]::SetEnvironmentVariable('JUCE_PATH', $env:JUCE_PATH, 'User')   # 持久化
```

## 3. 一条命令构建

```powershell
pwsh scripts/build.ps1                      # 配置 + 构建 Release + 跑 ctest
pwsh scripts/build.ps1 -Install             # 顺便装进系统 VST3 目录
pwsh scripts/build.ps1 -Target Output -Config Debug -SkipTests
```

`build.ps1` 会先做依赖预检(cmake / MSVC / JUCE tag),缺什么直接告诉你缺什么,不会让你对着一屏 CMake 报错猜。
产物在 `build/` 下,两个 bundle:`SCVBInput.vst3` 与 `SCVBOutput.vst3`(都是**目录**,不是单文件)。

## 4. 装到 DAW 里

`-Install` 会把两个 bundle 复制到 `C:\Program Files\Common Files\VST3\`(需要管理员权限)。手动装也行,直接整目录复制过去即可。

装完在 DAW 里重新扫描插件。**装的时候两个都要装** —— Input 与 Output 版本必须一致,SCVB 检测到两端版本不匹配会**拒绝连接**(这是设计,不是 bug)。

用法上的硬约束(路由、插槽位置、宿主 pan 居中等)见用户手册 `docs/USER_GUIDE.md` 的九条硬约束。**开发时也请照着摆** —— 大部分「插件没反应」的自查最后都落在这九条上。

## 5. 不装 DAW 也能开发 UI

```powershell
pwsh web-preview/serve.ps1 -Open      # 固定端口 8823,起服并打开导航页
```

导航页:<http://127.0.0.1:8823/web-preview/index.html>

预览器把 mock 后端注入**真源页面**(`web/output/index.html` / `web/input/index.html`)的同源 `<iframe>`,
所以你在浏览器里看到的就是插件里那一套 UI —— `web-preview/` 里**没有一行复制来的 UI 代码**,改 `web/` 立刻可见。

必须经 HTTP 打开,`file://` 不行(ES module 被 CORS 拒、`file://` 文档是不透明源导致 iframe 跨源)。细节见 `web-preview/README.md`。

## 6. 代码地图

| 目录 | 是什么 |
| --- | --- |
| `src/core/` | `scvb_core` 静态库:DSP、分析、IPC、state。**不链接 JUCE 插件层,能离线单测** |
| `src/input/` | SCVB Input 插件(装人声轨);`InputBridgeApi.h` 是它的桥常量表 |
| `src/output/` | SCVB Output 插件(装人声总线);`OutputBridgeApi.h` 同上 |
| `src/plugin-common/` | 两插件共用的 WebView2 装配层(INTERFACE 库,源编入各插件) |
| `web/` | 插件 UI 的**唯一真源**;`web/shared/i18n.js` 是三语文案唯一真源 |
| `web-preview/` | 浏览器预览壳 + mock 桥后端(见 §5) |
| `tests/` | Catch2 单测 + L1 IPC 契约测试 + `tests/tools/` 的验证工具 |
| `docs/` | 契约与手册;`docs/constitution/` 是宪法原文的**只读副本** |

**先看哪三个文件**:`docs/ARCHITECTURE.md`(全局)、`docs/SCVB_CONTRACT.md`(JS↔C++ 桥契约,冻结)、`docs/IPC_CONTRACT.md`(两插件之间的共享内存布局,冻结)。

## 7. 我该从哪个 issue 开始

- `good first issue` —— 范围清楚、不碰冻结契约、有明确验收。建议从这里起步。
- `help wanted` —— 需要一点上下文,但欢迎外部接手。
- `status/frozen-contract` —— **不要碰**,这类改动需要用户批准 + 变更文档,见 [CONTRIBUTING.md](../CONTRIBUTING.md) §8。

社区最需要的低门槛贡献是 **DAW 兼容性报告**:开一个 issue,写清 DAW + 版本、Windows 版本、实时与离线渲染下各自的实测结果(时间线对齐 / 静音通路 / 自动化 write 录制 / state 往返四项),附截图。这类报告直接进 `docs/DAW_COMPATIBILITY.md` 的兼容性矩阵。

## 8. 提 PR 之前

```powershell
pwsh scripts/gates.ps1 -PluginOnly      # gate 1-7,与 CI 等价
pwsh scripts/gates.ps1                  # 全量,含 gate 8 真机 GUI pluginval
pwsh scripts/gates.ps1 -Quick -BuildDir build-mine   # 跳过 pluginval,快速回环
```

然后逐项过一遍 `.github/pull_request_template.md`。两件最容易忘的:

1. **每个 commit 都要 `git commit -s`**(DCO,`branch-gate` 会机器检);
2. **含 GUI 的 pluginval 只能本地跑** —— CI runner 是无头的,那一项勾选是有实际意义的。

## 9. ★ 哪些东西碰不得

见 [CONTRIBUTING.md](../CONTRIBUTING.md) §8 的四条,一句话版本:**不动 123 个自动化参数的增删与顺序、不动 ParamID、不在不升 `abi` 的前提下改 IPC 布局、不在 `processBlock` 里分配/加锁/做 I-O**。

四份冻结文档(`docs/PARAMETERS.md` / `docs/IPC_CONTRACT.md` / `docs/STATE_SCHEMA.md` / `docs/SCVB_CONTRACT.md`)与 `tests/golden/` 由 CI 的 path guard 看守。`docs/constitution/` 是只读副本,只能随上游同步。

用户可见的**九条硬约束**文案有唯一真源(`docs/USER_GUIDE.zh-CN.md` 的 `#硬约束` 小节),其余 6 处落地面由 `node scripts/gen-hard-rules.mjs` 生成,**禁止手抄**。

## 10. 常见构建问题

| 症状 | 原因与处理 |
| --- | --- |
| `JUCE_PATH must be set` | 没设 `-JucePath` 也没设 `JUCE_PATH` 环境变量,见 §2 |
| `JUCE tag 'x' 与 .juce-version 不一致` | JUCE 检出的 tag 不对:`git -C ..\juce fetch --tags && git -C ..\juce checkout <.juce-version 的值>` |
| `nuget.exe not found` | 装 NuGet CLI,或把 WebView2 包预放到 `<BuildDir>/packages/` |
| WebView2 NuGet 拉不下来 | 多为代理/网络问题。包会缓存在构建目录,拉过一次后离线可用;换 `-BuildDir` 会重新拉 |
| 链接期报 CRT 冲突(`MT_StaticRelease` vs `MD_DynamicRelease`) | 本项目全程静态 CRT `/MT`。任何手工加进来的第三方库都必须用静态 CRT 变体重新编 |
| 编辑器窗口空白 | WebView2 Evergreen Runtime 没装,或被安全软件拦。先用 §5 的浏览器预览排除是不是 UI 代码本身的问题 |
| `clang-format` gate 红但代码看着没问题 | 版本不对。必须正好 **18.1.8**(J38 钉死),别的版本格式化结果不同 |
| ctest 全绿但 pluginval 红 | 大概率是插件层(参数、state、编辑器生命周期)的问题,不是 `scvb_core`。带上 pluginval 的完整输出开 issue |

还是卡住的话,开一个 issue 并附上 `pwsh scripts/gates.ps1 -Quick` 的完整输出 —— 依赖预检那一段通常已经指出了问题。
