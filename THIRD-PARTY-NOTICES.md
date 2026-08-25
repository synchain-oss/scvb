# Third-Party Notices

本文件列出 SCVB 分发产物中随附的第三方依赖的许可证信息(依赖 / 版本 / 许可证 / URL 四列)。
版本号一律来自仓内的版本真源文件或 vendored 源码本身,不手写字面量:JUCE 见 `.juce-version`,
pluginval 见 `.pluginval-version`,WebView2 SDK 见 `CMakeLists.txt` 的 `WEBVIEW2_VERSION`,
Catch2 / libebur128 见 `tests/CMakeLists.txt` 的 `FetchContent_Declare(... GIT_TAG ...)`。

## 随二进制分发(进 `.vst3`)

| 依赖 | 版本 | 许可证(SPDX) | URL |
| --- | --- | --- | --- |
| JUCE Framework(静态链接) | 见 `.juce-version`(当前 8.0.8) | AGPL-3.0-or-later(双授权:AGPLv3 / 商业;本项目取 AGPLv3) | https://github.com/juce-framework/JUCE |
| VST3 SDK(随 JUCE 分发) | 随 JUCE | MIT(2025-11 起) | https://github.com/steinbergmedia/vst3sdk |
| JUCE JS helper(`web/js/juce/*.js`) | 随 JUCE,原样副本 | AGPL-3.0-or-later(双授权) | https://github.com/juce-framework/JUCE |
| Microsoft WebView2 SDK(静态 loader) | 见 `CMakeLists.txt` 的 `WEBVIEW2_VERSION`(当前 1.0.2957.106) | BSD-3-Clause(Microsoft) | https://www.nuget.org/packages/Microsoft.Web.WebView2 |
| miniz(vendored,`third_party/miniz/`) | 2.2.0 | MIT | https://github.com/richgel999/miniz |
| Space Grotesk(子集 `web/fonts/SpaceGrotesk.woff2`) | 2.000(`text=` 子集) | OFL-1.1 | https://github.com/floriankarsten/space-grotesk |
| IBM Plex Sans(子集 `web/fonts/IBMPlexSans.woff2`) | 3.201(`text=` 子集) | OFL-1.1 | https://github.com/IBM/plex |
| IBM Plex Mono(子集 `web/fonts/IBMPlexMono.woff2`) | 2.3(`text=` 子集) | OFL-1.1 | https://github.com/IBM/plex |
| Noto Sans SC(子集 `web/fonts/NotoSansSC.woff2`) | 2.004-H2(上游全量可变字体本地子集) | OFL-1.1 | https://github.com/google/fonts/tree/main/ofl/notosanssc |

## 仅构建 / 测试 / CI 使用(不链接进 `.vst3`,不随产物分发)

| 依赖 | 版本 | 许可证(SPDX) | URL |
| --- | --- | --- | --- |
| Catch2(仅测试目标) | 见 `tests/CMakeLists.txt`(当前 v3.5.4) | BSL-1.0 | https://github.com/catchorg/Catch2 |
| libebur128(可选参考测试,`SCVB_TESTS_WITH_EBUR128=ON` 时才拉) | 见 `tests/CMakeLists.txt`(当前 v1.2.6) | MIT | https://github.com/jiixyj/libebur128 |
| pluginval(仅 CI / 本地 gate 下载执行) | 见 `.pluginval-version`(当前 v1.0.4) | GPL-3.0-or-later | https://github.com/Tracktion/pluginval |

## 说明

- **JUCE 与 GPLv3 的关系**:SCVB 本身以 GPL-3.0-or-later 发布,取 JUCE 的 AGPLv3 授权分支。
  按 JUCE 官方口径,分发自己的 GPLv3 代码时只需附本项目的 GPLv3 全文(`LICENSE`),不必附 JUCE 许可证本体。
  每个已发布二进制的完整对应源码在本仓库公开可得(AGPLv3 §13 的保守合规做法)。
- **WebView2 Runtime(Evergreen)不随本仓库分发。** 插件通过上表的静态 loader 加载宿主机器上已安装的
  WebView2 Runtime(Windows 平台组件,由微软 Evergreen 引导器安装),故 Runtime 本身不进第三方声明闭包。
  这与 U2「不附 `LICENSE-EXCEPTION.md`,依赖 GPLv3 系统库例外的默认解释」一致。
- **`web/js/juce/*.js` 不加 Synchain 版权头**:它是 JUCE 官方 helper 的原样副本(仅行尾按本仓
  `.gitattributes` 取 LF),版权归 Raw Material Software Limited,由 `REUSE.toml` 的 `web/js/juce/**`
  特例块声明 AGPL-3.0-or-later;`.gitattributes` 另标 `linguist-vendored`。
- **字体是被 `juce_add_binary_data` 编进 `.vst3` 分发的**,不是仓库里躺着的素材,所以 OFL-1.1 的
  随附义务在本仓成立:`LICENSES/OFL-1.1.txt` 存全文,本表存各家族版权行与来源,发布 zip 内同样携带。
- **字体子集化 = 对字体的修改**(OFL-1.1 §3),四款子集都由 `scripts/fetch_fonts.py` 生成,但取源
  分两路:拉丁三款走 Google Fonts CSS2 `text=` 接口(Google 侧子集化),`Noto Sans SC` 下载
  google/fonts 上游全量可变字体后**本地 `fontTools` 子集化**(CJK 字符数已超出 `text=` 的 GET URL
  上限,理由与实测数据见 `web/fonts/README.md`)。两路都保留 name 表的版权与许可条目
  (`--name-IDs='*'`),OFL-1.1 §4 的署名随产物分发。Reserved Font Name(RFN)逐家族核验:
  - **Space Grotesk**:无 RFN,子集命名不受限。
  - **IBM Plex Sans / IBM Plex Mono**:RFN 为 **"Plex"**。当前子集仍以 `IBMPlexSans.woff2` /
    `IBMPlexMono.woff2` 及 `@font-face` family `'IBM Plex Sans'` / `'IBM Plex Mono'` 分发,用到了 RFN。
    按 OFL-1.1 §3,Modified Version 不得使用 RFN,需改名(文件名 + `@font-face` family + `tokens.css`
    的引用 + `fetch_fonts.py` 的输出名)。**此项为仓库转 public 前的必处理项**,与 Bridge 仓同源同结论。
  - **Noto Sans SC**:RFN 为 **"Source"**。子集名 `NotoSansSC.woff2` / family `'Noto Sans SC'` 不含
    "Source",不触发 RFN 限制(注:"Noto" 是 Google 商标,不是 RFN)。
- **许可证全文**存放在 `LICENSES/`:`GPL-3.0-or-later.txt`(本仓主许可证)、`AGPL-3.0-or-later.txt`
  (`REUSE.toml` 给 `web/js/juce/**` 用的标识,用了标识就必须有全文,否则 `reuse lint` 恒非零)、
  `OFL-1.1.txt`(字体)、`MIT.txt`(miniz)。
- 新增任何运行时依赖,必须在同一个 PR 里补本表一行并在 `REUSE.toml` / `LICENSES/` 落对应声明。
