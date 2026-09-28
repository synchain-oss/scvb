# Third-Party Notices

本文件列出 SCVB 分发产物中随附的第三方依赖的许可证信息(依赖 / 版本 / 许可证 / URL 四列)。
版本号一律来自仓内的版本真源文件或 vendored 源码本身,不手写字面量:JUCE 见 `.juce-version`,
pluginval 见 `.pluginval-version`,WebView2 SDK 见 `CMakeLists.txt` 的 `WEBVIEW2_VERSION`,
Catch2 / libebur128 见 `tests/CMakeLists.txt` 的 `FetchContent_Declare(... GIT_TAG ...)`;
随 JUCE 编进来的库(VST3 SDK、zlib、libpng、libjpeg、HarfBuzz、SheenBidi)见 JUCE 源码里各自的版本宏,
表中「当前」值是在 JUCE 8.0.8 源码里读出来的。

## 随二进制分发(进 `.vst3`)

| 依赖 | 版本 | 许可证(SPDX) | URL |
| --- | --- | --- | --- |
| JUCE Framework(静态链接) | 见 `.juce-version`(当前 8.0.8) | AGPL-3.0-or-later(双授权:AGPLv3 / 商业;本项目取 AGPLv3) | https://github.com/juce-framework/JUCE |
| VST3 SDK(随 JUCE 分发,`juce_audio_processors/format_types/VST3_SDK/`) | 随 JUCE(当前 3.7.12,`pluginterfaces/vst/vsttypes.h` 的 `kVstVersionString`) | GPL-3.0-only(双授权:Steinberg VST3 License / GPLv3;本项目取 GPLv3) | https://github.com/steinbergmedia/vst3sdk |
| JUCE JS helper(`web/js/juce/*.js`) | 随 JUCE,原样副本 | AGPL-3.0-or-later(双授权) | https://github.com/juce-framework/JUCE |
| Microsoft WebView2 SDK(静态 loader) | 见 `CMakeLists.txt` 的 `WEBVIEW2_VERSION`(当前 1.0.2957.106) | BSD-3-Clause(Microsoft) | https://www.nuget.org/packages/Microsoft.Web.WebView2 |
| miniz(vendored,`third_party/miniz/`) | 2.2.0 | MIT | https://github.com/richgel999/miniz |
| zlib(随 JUCE `juce_core` 静态编入) | 随 JUCE(当前 1.3.1,`zlib.h` 的 `ZLIB_VERSION`) | Zlib | https://zlib.net |
| libpng(随 JUCE `juce_graphics` 静态编入) | 随 JUCE(当前 1.6.37,`png.h` 的 `PNG_LIBPNG_VER_STRING`) | libpng-2.0 | http://www.libpng.org/pub/png/libpng.html |
| libjpeg(IJG,随 JUCE `juce_graphics` 静态编入) | 随 JUCE(当前 6b,`jversion.h` 的 `JVERSION`) | IJG | https://ijg.org |
| HarfBuzz(随 JUCE `juce_graphics` 静态编入) | 随 JUCE(当前 10.1.0,`hb-version.h` 的 `HB_VERSION_STRING`) | MIT-Modern-Variant(HarfBuzz 自称 "Old MIT") | https://github.com/harfbuzz/harfbuzz |
| SheenBidi(随 JUCE `juce_graphics` 静态编入) | 随 JUCE(JUCE 8.0.8 内置副本,上游未随附版本号) | Apache-2.0 | https://github.com/Tehreer/SheenBidi |
| Space Grotesk(子集 `web/fonts/SpaceGrotesk.woff2`) | 2.000(`text=` 子集) | OFL-1.1 | https://github.com/floriankarsten/space-grotesk |
| IBM Plex Sans(子集并按 OFL-1.1 §3 改名,分发为 `web/fonts/ScvbSans.woff2` / `'SCVB Sans'`) | 3.201(`text=` 子集) | OFL-1.1 | https://github.com/IBM/plex |
| IBM Plex Mono(子集并按 OFL-1.1 §3 改名,分发为 `web/fonts/ScvbMono.woff2` / `'SCVB Mono'`) | 2.3(`text=` 子集) | OFL-1.1 | https://github.com/IBM/plex |
| Noto Sans SC(子集 `web/fonts/NotoSansSC.woff2`) | 2.004-H2(上游全量可变字体本地子集) | OFL-1.1 | https://github.com/google/fonts/tree/main/ofl/notosanssc |

## 仅构建 / 测试 / CI 使用(不链接进 `.vst3`,不随产物分发)

| 依赖 | 版本 | 许可证(SPDX) | URL |
| --- | --- | --- | --- |
| Catch2(仅测试目标) | 见 `tests/CMakeLists.txt`(当前 v3.5.4) | BSL-1.0 | https://github.com/catchorg/Catch2 |
| libebur128(可选参考测试,`SCVB_TESTS_WITH_EBUR128=ON` 时才拉) | 见 `tests/CMakeLists.txt`(当前 v1.2.6) | MIT | https://github.com/jiixyj/libebur128 |
| pluginval(仅 CI / 本地 gate 下载执行) | 见 `.pluginval-version`(当前 v1.0.4) | GPL-3.0-or-later | https://github.com/Tracktion/pluginval |

## 版权行

上表各组件的版权行,逐字取自各自的上游声明文件(原文副本在 `LICENSES/` 与 `third_party/notices/`);原文有多行的,逐行各占一个代码段、用分号隔开。

| 组件 | 版权行 |
| --- | --- |
| JUCE Framework / JUCE JS helper | `Copyright (c) Raw Material Software Limited` |
| VST3 SDK | `(c) 2024, Steinberg Media Technologies GmbH, All Rights Reserved` |
| Microsoft WebView2 SDK(静态 loader) | `Copyright (C) Microsoft Corporation. All rights reserved.` |
| miniz | `Copyright 2013-2014 RAD Game Tools and Valve Software`;`Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC` |
| zlib | `(C) 1995-2022 Jean-loup Gailly and Mark Adler` |
| libpng | `Copyright (c) 1995-2019 The PNG Reference Library Authors.`;`Copyright (c) 2018-2019 Cosmin Truta.`;`Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.`;`Copyright (c) 1996-1997 Andreas Dilger.`;`Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.` |
| libjpeg | `This software is copyright (C) 1991-1998, Thomas G. Lane.` |
| HarfBuzz | 共 17 行,见 `third_party/notices/harfbuzz.COPYING` 开头(首行 Copyright © 2010-2022 Google, Inc.) |
| SheenBidi | `Copyright (C) 2014-2022 Muhammad Tayyab Akram`(取自源文件头;各文件年份不一,这里取最早到最晚;`LICENSE` 本身是不带版权行的 Apache-2.0 正文) |

**libjpeg(IJG)要求的声明**:this software is based in part on the work of the Independent JPEG Group.

**字体版权行**:每个 `.woff2` 的 `name` 表 nameID 0 原文(用 fontTools 读出,Windows 平台 / 英语 0x409 记录),
`REUSE.toml` 的四个字体块与此逐字相同。

| 分发文件 | 上游家族 | 版权行(nameID 0 原文) |
| --- | --- | --- |
| `web/fonts/SpaceGrotesk.woff2` | Space Grotesk | `Copyright 2020 The Space Grotesk Project Authors (https://github.com/floriankarsten/space-grotesk)` |
| `web/fonts/ScvbSans.woff2` | IBM Plex Sans | `Copyright 2019 IBM Corp. All rights reserved.` |
| `web/fonts/ScvbMono.woff2` | IBM Plex Mono | `Copyright 2017 IBM Corp. All rights reserved.` |
| `web/fonts/NotoSansSC.woff2` | Noto Sans SC | `(c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.` |

## 说明

- **JUCE 与 GPLv3 的关系**:SCVB 本身以 GPL-3.0-or-later 发布,取 JUCE 的 AGPLv3 授权分支。
  按 JUCE 官方口径,分发自己的 GPLv3 代码时只需附本项目的 GPLv3 全文(`LICENSE`),不必附 JUCE 许可证本体。
  每个已发布二进制的完整对应源码在本仓库公开可得(AGPLv3 §13 的保守合规做法)。
- **VST3 SDK 的许可证**:JUCE 8.0.8 内置的是 VST3 SDK **3.7.12**,其 `LICENSE.txt`(原文见
  `third_party/notices/vst3sdk.LICENSE.txt`)写的是「Steinberg VST3 License,**或** GPL Version 3」二选一。
  本项目按 **GPLv3** 使用它,不签 Steinberg 的专有许可协议(JUCE 自己的 `LICENSE.md` 对这一版也写
  「Proprietary Steinberg VST3 License/GPLv3」)。本行此前写的「MIT」不是这一版 SDK 的许可证,已订正;
  将来 JUCE 升级换了内置 SDK,以新 SDK 自带的 `LICENSE.txt` 为准改本行。
- **JUCE 实际编进 `.vst3` 的第三方库**:三个插件用到的 JUCE 模块只有 `juce_audio_basics` / `juce_audio_processors` /
  `juce_core` / `juce_data_structures` / `juce_events` / `juce_graphics` / `juce_gui_basics` / `juce_gui_extra`,
  外加 `juce_add_plugin` 自动带上的插件封装 `juce_audio_plugin_client`(直接链接的只有
  `src/plugin-common/CMakeLists.txt` 里的 `juce_audio_processors` 与 `juce_gui_extra`,其余是它们依赖的模块;
  生成的 `.vcxproj` 里的 JUCE 编译单元也只有这些),模块开关都取 JUCE 默认。由此编进来的是:`juce_core` 里的 zlib
  (`JUCE_INCLUDE_ZLIB_CODE` 默认 1)、`juce_graphics` 里的 libpng / libjpeg(`JUCE_INCLUDE_PNGLIB_CODE` /
  `JUCE_INCLUDE_JPEGLIB_CODE` 默认 1)、HarfBuzz(`juce_graphics_Harfbuzz.cpp`)与 SheenBidi
  (`juce_graphics_Sheenbidi.c`),以及 `juce_audio_processors` 里的 VST3 SDK。JUCE `LICENSE.md` 列出的
  其余依赖**没有**进本项目的二进制:FLAC / Ogg Vorbis 在 `juce_audio_formats`,GLEW 在 `juce_opengl`,
  CHOC / QuickJS 在 `juce_javascript`,Box2D 在 `juce_box2d`(这四个模块都没链);AudioUnitSDK / AAX /
  Oboe 属于没构建的格式或平台;LV2 / ARA 宿主代码只在开启插件宿主功能时编译,本项目没开。
  `pslextensions` 头文件随 VST3 封装一起被包含,属公有领域(public domain),无随附义务。
- **WebView2 Runtime(Evergreen)不随本仓库分发。** 插件通过上表的静态 loader 加载宿主机器上已安装的
  WebView2 Runtime(Windows 平台组件,由微软 Evergreen 引导器安装),故 Runtime 本身不进第三方声明闭包。
  这与 U2「不附 `LICENSE-EXCEPTION.md`,依赖 GPLv3 系统库例外的默认解释」一致。
- **`web/js/juce/*.js` 不加 Synchain 版权头**:它是 JUCE 官方 helper 的原样副本(仅行尾按本仓
  `.gitattributes` 取 LF),版权归 Raw Material Software Limited,由 `REUSE.toml` 的 `web/js/juce/**`
  特例块声明 AGPL-3.0-or-later;`.gitattributes` 另标 `linguist-vendored`。
- **字体是被 `juce_add_binary_data` 编进 `.vst3` 分发的**,不是仓库里躺着的素材,所以 OFL-1.1 的
  随附义务在本仓成立:`LICENSES/OFL-1.1.txt` 存全文,上表存来源,「版权行」一节存各家族版权行,发布 zip 内同样携带。
- **字体子集化 = 对字体的修改**(OFL-1.1 §3),四款子集都由 `scripts/fetch_fonts.py` 生成,但取源
  分两路:拉丁三款走 Google Fonts CSS2 `text=` 接口(Google 侧子集化),`Noto Sans SC` 下载
  google/fonts 上游全量可变字体后**本地 `fontTools` 子集化**(CJK 字符数已超出 `text=` 的 GET URL
  上限,理由与实测数据见 `web/fonts/README.md`)。两路产物都带着 name 表的版权与许可条目
  (本地子集化那路靠 `--name-IDs='*'` 显式保留),OFL-1.1 §2 的随附署名随产物分发。
  Reserved Font Name(RFN)逐家族核验:
  - **Space Grotesk**:无 RFN,子集命名不受限。
  - **IBM Plex Sans / IBM Plex Mono**:RFN 为 **"Plex"** 一词本身。子集 = Modified Version,
    按 OFL-1.1 §3 不得使用 RFN,故 [SL-267] 已改名分发:`ScvbSans.woff2` / family `'SCVB Sans'` /
    PostScript 名 `ScvbSans-Regular`,`ScvbMono.woff2` / `'SCVB Mono'` / `ScvbMono-Regular`。
    改的是**文件名 + `@font-face` family + 字体 `name` 表**三者:只改前两项不够,§3 管的是
    呈现给用户的字体名,而那存在 `name` 表里(nameID 1/3/4/6/16/17)——文件改了名而 `name` 表
    仍写 "IBM Plex Sans" 的话,装进系统字体册或被 PDF 导出读到的依然是上游名。
    cmap 与字形一字未动,视觉零变化;版权(nameID 0)与许可证(13/14)原样保留,故本表
    仍以上游家族名登记来源 —— §3 限制的是分发名,不是溯源署名。与 Bridge 仓同源同结论。
    回归由 `scripts/check-font-names.py` 守住(gates **3k** + `compliance.yml` 两步):
    既解 woff2 的 `name` 表断言,也扫进包的 `.css`/`.js`/`.html` 里的字体栈字面量。
  - **Noto Sans SC**:RFN 为 **"Source"**。子集名 `NotoSansSC.woff2` / family `'Noto Sans SC'` 不含
    "Source",不触发 RFN 限制(注:"Noto" 是 Google 商标,不是 RFN)。它同样登记进上述断言表,
    守住不回归;其 nameID 0/7 里逐字出现的 "Source" 是 §2 要求保留的版权与商标署名,不参与断言。
- **许可证全文**存放在 `LICENSES/`,文件名 = SPDX 标识:`GPL-3.0-or-later.txt`(本仓主许可证)、
  `AGPL-3.0-or-later.txt`(JUCE 与 `web/js/juce/**`)、`GPL-3.0-only.txt`(VST3 SDK 取的 GPLv3 分支;
  正文与 `GPL-3.0-or-later.txt` 同为 GPLv3 全文,「only / or-later」的区别在授权声明,不在正文)、
  `OFL-1.1.txt`(字体)、`MIT.txt`(miniz)、`BSD-3-Clause.txt`(WebView2 loader,**逐字取自**
  `Microsoft.Web.WebView2` 1.0.2957.106 nupkg 的 `LICENSE.txt`,含微软版权行)、`libpng-2.0.txt`
  (逐字取自 JUCE 8.0.8 的 `pnglib/LICENSE`,含 libpng 作者版权行)、`Apache-2.0.txt`(SheenBidi,
  逐字取自 JUCE 8.0.8 的 `sheenbidi/LICENSE`)、`Zlib.txt` / `IJG.txt` / `MIT-Modern-Variant.txt`
  (SPDX 标准文本;各自的版权行见下「版权行」一节)。用了标识就必须有全文,有全文就必须有文件引用它,
  否则 `reuse lint` 恒非零。
- **`third_party/notices/`** 存上表各 JUCE 内置库与 WebView2 loader 的**上游版权/许可声明原文**
  (逐字副本,文件名 = `<组件>.<上游文件名>`,来源同上)。这些库的源码不在本仓(JUCE 与 nupkg 都在构建期取),
  这一目录让每份声明都能在仓内查到原文,也是 `REUSE.toml` 给这些许可证挂引用的落点。
- 新增任何运行时依赖,必须在同一个 PR 里补本表一行并在 `REUSE.toml` / `LICENSES/` 落对应声明。
