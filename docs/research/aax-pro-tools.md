# AAX(Pro Tools)调研记录

> **调研记录,2026-10-03;本轮不实施 AAX;结论以当时版本为准。**
>
> 本文是一份只读调研的整理稿,供以后给 SCVB 做 AAX 版本的人参考。调研时没有改任何代码、没有跑构建,也没有在 Pro Tools 里实测过 SCVB。外部流程(Avid / PACE 的申请与签名)变动很快,执行前请重新核对官方页面。
>
> 文中引用 [KNOWN_ISSUES.md](../KNOWN_ISSUES.md) 的 KI-6 时,**以当时 KNOWN_ISSUES 为准**:宿主调度加固工作会改变 KI-6 的实际行为,本文里关于 KI-6 的数字只是调研时的状态。

## 可信度标记与出处写法

- **【已核实】**:读到了一手原文、官方页面或 JUCE 8.0.8 源码,后面给出出处。
- **【二手】**:开发者论坛、博客、第三方仓库里的第一人称经历,后面给 URL。
- **【推测】**:由已核实事实推出来的,或只有单一间接来源。
- **【不确定】**:来源互相矛盾,或查不到。

出处简写:

- `JUCE` = JUCE 8.0.8(自带 AAX SDK 2.8.0)。`AAX.cpp` = `modules/juce_audio_plugin_client/juce_audio_plugin_client_AAX.cpp`;`JUCEUtils.cmake` = `extras/Build/CMake/JUCEUtils.cmake`。行号是 8.0.8 的行号。
- `dox/` = AAX SDK 2.9 Doxygen 源文件的第三方镜像 [mgz0227/sonobus 的 sonobus8 分支](https://github.com/mgz0227/sonobus/tree/sonobus8/SDK/aax/Documentation/Doxygen/dox)。GSG = `AAX_Getting_Started_Guide.doxygen`,PTG = `AAX_Pro_Tools_Guide.doxygen`,DIST = `AAX_DistributingYourPlugIn.doxygen`,DSH = `DSH_Guide.doxygen`。
- 「调研结论」指调研时对 SCVB 实现的阅读与推演,没有对应的外部出处。

---

## 0. 一页摘要

**结论**:AAX 在技术上没有硬阻碍(JUCE 8 自带 AAX 支持)。真正耗时、也决定成败的是外部认证:要拿到 Pro Tools Developer,要过 Avid 的人工审核拿到 PACE 签名工具,还要有一个实体 iLok USB。SCVB 本身有三处需要实测才能定论的风险:DPP 停调、Aux 上的位置补偿、Developer 版不能保存 session(见第 7 节)。

**硬门槛**:

| #   | 门槛                                                                                                                                                       | 证据                                                                                                                                                                                                               |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | 公开发布的 Pro Tools(零售版和 Beta 版)只加载带 PACE Eden 签名的 AAX;开发版不受此限                                                                         | 【已核实】`dox/GSG:112`、`dox/PTG:150-152,186-190`;[PACE 入门指南](https://paceap.com/getting-started-with-aax-code-signing-for-pro-tools-plugins/)(「fail to load in commercial versions of Pro Tools」);JUCE README |
| 2   | 签名工具要先过 Avid 审核才给:Avid 要确认申请者是「legitimate publisher」,通过后才把资料转给 PACE。**批不批没有保证**。工具许可条款不公开                     | 审核环节【已核实·一手,PACE 指南】;「不保证批」【推测】;条款不公开【不确定】                                                                                                                                        |
| 3   | 签名工具要求**实体 iLok USB**,不想用 USB 只能找 PACE 买云签名                                                                                              | 【已核实】`dox/PTG:175`;PACE 指南(「You'll need an iLok USB to safely store the signing certificate」)                                                                                                              |
| 4   | Pro Tools Developer 要单独的许可,文档原文是「loaded on an iLok USB device」,向 devauth 申请                                                                | 【已核实】`dox/GSG:123`;能不能只用 iLok Cloud【不确定】                                                                                                                                                            |
| 5   | Windows 上 wraptool 要一张 Authenticode 证书,**可以是自签**;但 Avid 建议商用发布前换 EV 证书,Dplug 指南也说自签只用于临时构建                               | 【已核实】`dox/PTG:177-179,188`;Dplug 指南【二手】                                                                                                                                                                 |
| 6   | mac 上 Eden 工具一步同时完成 PACE 签名和 Apple 签名;指定 Apple 身份的参数据公开脚本是 `--signid`;要公证就必须用 Developer ID(付费会员)                      | 一步完成【已核实】`dox/PTG:188`;`--signid`【二手,见 2.8】;公证必须付费会员【已核实·一手,Apple 会员对比页】                                                                                                        |

**费用**:

- **最低现金**:一个 iLok 3 USB,按 $45–60 加国际运费预算。官方店价记录为 USB-A $49.95、USB-C $59.95【二手】;2026-10-03 Sweetwater 页面实时价是 USB-A $44.95、USB-C $48【二手,零售价会浮动】。
- **其余都是 $0**:Avid 账号、Pro Tools Developer、PACE 工具许可(JUCE README 原文「provided free of charge by Avid」)、自签证书、Pro Tools Intro(免费,磁盘授权,不需要 iLok;【已核实,Avid Intro 介绍页】)。
- **可选**:Apple Developer $99/年(mac 公证);PACE 云签名约 $1000/年【二手】。
- 完整表见第 8 节。

**日历**:

- 典型 3–6 周,主要在等 devauth、PACE 和 iLok 寄到;最坏的情况是一直批不下来【推测】。
- SCVB 的 AAX Windows beta 工程量约 10–16 人日当量(见 7.6)【推测】。

**推荐顺序(中性写法)**:

1. 先注册 Avid / iLok 账号、发 devauth 邮件、下单 iLok USB(iLok 同时卡住 Developer 激活和签名,是关键路径)。
2. 拿一个**简单插件**(参数少、不依赖时间线位置、没有跨实例共享内存)先把认证和签名流程跑通:Developer 里跑起来并录屏 → PACE 申请 → wraptool 签名 → 零售版里加载冒烟。
3. 流程跑通后,再做 SCVB:账号、iLok、PACE 资质、签名脚本和验证 job 可以复用【推测】。SCVB 的 AAX 本身要先解决第 7 节的前置项。

---

## 1. 许可:JUCE 8 AGPLv3 + AAX SDK 2.8.0 GPLv3(没有 or-later)+ 本项目 GPL-3.0-or-later

### 1.1 三方许可原文

| 组件                       | 原文要点                                                                                                                                                                                                                                | 出处                                                                                                                                                                             |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JUCE 8.0.8 模块            | 「dual-licensed under the AGPLv3 and the commercial JUCE licence」                                                                                                                                                                      | 【已核实】JUCE `LICENSE.md:6-8`                                                                                                                                                  |
| AAX SDK 2.8.0(JUCE 自带)   | 「The AAX SDK is subject to commercial or open-source licensing.」「By using the AAX SDK, you agree to the terms of both the Avid AAX SDK License Agreement and Avid Privacy Policy.」「**Or: You may also use this code under the terms of the GPL v3**」 | 【已核实】JUCE `modules/juce_audio_plugin_client/AAX/SDK/LICENSE.txt:3-6,11-12`;版本见 `AAX/SDK/Interfaces/AAX_Version.h:56,60`(0x0208 / 20208000)                              |
| JUCE 对 AAX 依赖的标注     | 「Proprietary Avid AAX License/GPLv3」                                                                                                                                                                                                  | 【已核实】JUCE `LICENSE.md:48`                                                                                                                                                   |
| SDK 的形态                 | 只有源码(148 个 .h、38 个 .cpp 及少量其他文件),没有预编译库。JUCE 构建 AAX 时会自动用源码编出 AAX 库                                                                                                                                    | 【已核实】调研时的 `find` 计数;`JUCEUtils.cmake:1157-1164,1602-1605`                                                                                                             |
| SCVB                       | GPL-3.0-or-later;现有 VST3 二进制里的 VST3 SDK 按 GPL-3.0-only 取用                                                                                                                                                                    | 【已核实】本仓 [THIRD-PARTY-NOTICES.md](../../THIRD-PARTY-NOTICES.md)                                                                                                            |

### 1.2 组合结论

1. **可以组合分发**:
   - GPLv3 §13 原文允许把 GPLv3 作品与 AGPLv3 作品组合成一个整体【已核实,本仓 [LICENSE](../../LICENSE) §13】。
   - AAX 二进制作为一个整体,**只能按 GPLv3 分发,不能写 or-later**;JUCE 那部分同时受 AGPLv3 约束。项目自己的源码仍然可以标 GPL-3.0-or-later【推测,依据上表原文】。
   - 先例:[ADLplug-Next](https://github.com/yumasansansan/ADLplug-Next) 的 README 原话是二进制「distributed under GPLv3, not GPLv3-or-later, because: the bundled ASIO and AAX SDKs are available under GPLv3, but not GPLv3-or-later」【已核实】。
2. **这不是新风险**:SCVB 的 VST3 二进制本来就以 GPL-3.0-only 方式取用 VST3 SDK【已核实,同上 THIRD-PARTY-NOTICES】。
3. **走 GPL 选项不需要 Avid 商业许可**:
   - Avid 2026-03 给一位**免费插件**作者的回信原话是「You don't need a commercial license, you only need to sign your plugin using the PACE signing tools」【二手,[note.com 记录](https://note.com/kawato3/n/ne11473420ad5)】。
   - 作者本人也猜是因为插件免费才豁免的。**没有任何来源专门讲 GPL 或开源项目能不能拿到签名资质**【已核实「查无来源」】。
4. **不需要交出签名凭据**:
   - FSF FAQ 的 `#GiveUpKeys` 写明,只有把 GPL 软件装进 User Product(消费类硬件)交付,并且由硬件校验签名时,才必须交出签名密钥【已核实·一手,GPL FAQ,`https://www.gnu.org/licenses/gpl-faq.html#GiveUpKeys`(CI 链接检查对该站点超时,故不做成链接)】。
   - SCVB 是可下载的纯软件,所以不需要公开 iLok 或 PACE 凭据【推测】。

### 1.3 注意事项

- **Avid SDK 协议的歧义**:LICENSE.txt 里「By using the AAX SDK, you agree to…」写在「Or」之前,严格读法有歧义。CtrlrX 的做法比较保守,让用户同时确认 Avid 协议【不确定】。
- **PACE 条款**:
  - 条款看不到,可能有 NDA【不确定】。
  - 工具许可一年续一次【二手】。
  - PACE 的文档站 docs.paceap.com 要登录才能看(调研时访问被重定向到登录页)【已核实】。
  - wraptool 签名时会不会往二进制里注入 PACE 的专有代码,没有查到原文【不确定】。拿到工具后先读它的文档;如果会注入,要重新评估 GPL 分发。
- **签名工具是 PACE 的专有软件**(wraptool 在 Eden/Fusion SDK 里,2026 年申请者描述的下载项叫「Fusion SDK」):
  - 不能放进公开仓库,也不能在 workflow 里从公开地址下载【推测】;
  - 它是通用签名工具,不属于 GPL 意义上的「Corresponding Source」【推测】。
- **用户自己从源码编译**:编出来的 AAX 只能在 Pro Tools Developer 里加载,或者用户自己去申请 PACE 签名。README 要写清楚这一点,可以参考 ADLplug-Next 的写法【推测】。
- **THIRD-PARTY-NOTICES 要补的行**:AAX SDK 2.8.0:Avid AAX SDK License 或 GPL v3,本项目取 GPLv3;版权行「Copyright (c) 2024 Avid Technology Inc.」(SDK `LICENSE.txt:1`);写明 AAX 二进制按 GPLv3(不是 or-later)分发。
- **商标**:
  - 可以用「for use with / compatible with」这类文字指代 Avid 商标;Logo 未经书面许可不能用(调研当日原 PDF 打不开)【二手】。
  - 建议:网站和 README 只写文字「AAX (Pro Tools)」,另加一句「AAX and Pro Tools are trademarks of Avid Technology, Inc.」【推测】。
- **网站纪律**:AAX 正式落地之前,官网和 README 不要提前写支持【推测,与项目既有的「未落地不宣传」做法一致】。

---

## 2. 认证与签名全流程(逐步清单)

### 2.0 总表

| 步  | 动作                                                                                                                                                                                      | 状态标记                                                      | 预计等待                                                                    |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- | --------------------------------------------------------------------------- |
| 1   | 注册 Avid 账号和 AAX 开发者身份;注册 iLok 账号;在测试机和签名机上装 iLok License Manager                                                                                                  | 【已核实】[developer.avid.com/aax](https://developer.avid.com/aax/) | 当天                                                                        |
| 2   | 发 devauth 邮件,申请 Pro Tools Developer 许可(可以顺带申请 NFR)                                                                                                                          | 地址和主题【已核实】`dox/GSG:123`;正文是【推测】模板          | 有当天、约 1 天的;也有 2026-07/08 多人反映等了 2–3 周【二手】               |
| 3   | 买 iLok USB                                                                                                                                                                               | 【已核实】`dox/PTG:175`、`dox/GSG:123,126`                    | 国际物流【不确定】                                                          |
| 4   | 把 Developer 激活码兑换到 iLok;下载并安装 Pro Tools Developer;跑起插件;录屏                                                                                                                | 【已核实】`dox/GSG:115,123`、`dox/PTG:805`                    | 取决于 2、3                                                                 |
| 5   | 发 PACE 签名工具申请到 audiosdk@avid.com,附录屏                                                                                                                                           | 主题有三种说法,见 2.5【已核实】                               | 2 天到 2 周以上【二手】                                                     |
| 6   | PACE 邀请 → 72 小时内填表 → PACE 销售来信 → 把 PACE Central Access 授权下发到 iLok(登出再登入)→ 从 iLok License Manager 进入 PACE Central → 建 Product 和 Wrap configuration,拿到 wcguid → 下载 Fusion SDK(含 wraptool) | 【二手】JUCE 论坛帖、Dplug 指南                               | 收到邀请后约 2–4 天                                                         |
| 7   | 准备证书:Windows 生成自签 .pfx;mac 准备签名身份                                                                                                                                           | 【已核实】可以自签,`dox/PTG:179`                              | 当天                                                                        |
| 8   | wraptool 签名 + verify                                                                                                                                                                    | 命令形态【二手】;路径细节【推测】                             | 每版 15–30 分钟                                                             |
| 9   | 每次发版的签名操作                                                                                                                                                                        | 流程【推测】                                                  | 每版 15–30 分钟                                                             |
| 10  | (可选)PACE 云签名「Cloud 2 Cloud」,在 CI 里自动签                                                                                                                                       | 有此服务【已核实】;价格约 $1000/年【二手】                    | 不明                                                                        |
| 11  | (mac)Apple codesign 和公证                                                                                                                                                               | 见 2.11                                                       | Apple 审核约 1–2 天【推测】                                                 |

### 2.1 账号与下载入口

- 打开 [developer.avid.com/aax](https://developer.avid.com/aax/) 注册 AAX 开发者。页面原文(2026-10-03 抓取):「Note that as well as an Avid account, an iLok account is required to run Pro Tools for AAX testing. Commercial AAX development also requires an iLok USB key as part of the AAX digital signing process.」【已核实】
- 在 ilok.com 注册 iLok 账号(免费)。PTG 列的申请字段里有 iLok username【已核实,`dox/PTG:168`】。
- **下载入口**:
  - Pro Tools Developer 和 DigiShell/AAX Validator 都在 my.avid.com 的「AAX SDK Toolkit / My Toolkits and Downloads」下载区【已核实,`dox/GSG:115`、`dox/PTG:805,809`】;
  - developer.avid.com/aax 页面上的入口按钮叫「Download Evaluation Toolkit」【已核实】。
- **iLok License Manager**:Developer 许可要装进 iLok USB,签名时要激活许可并 Synchronize(2.6),所以测试机和签名机都要装它【推测,依据 `dox/GSG:123` 和 2.6 的二手流程】。

### 2.2 devauth 邮件:申请 Pro Tools Developer

- **地址和主题**【已核实,`dox/GSG:123`】:发到 `devauth@avid.com`,主题写 **「License Request」**。原文:「You can request this Pro Tools Developer license, as well as any other NFR (Not For Resale) licenses which you require for your AAX product development and testing, by writing to devauth@avid.com with "License Request" in the subject.」
- **JUCE README 的写法**:「Request a Pro Tools Developer Bundle activation code by sending an email to devauth@avid.com」【已核实】。
- **不要把这封信发到 audiosdk**:
  - developer.avid.com/aax 页面写的是「contact audiosdk@avid.com for information about how to obtain the necessary tools and license」【已核实,2026-10-03 抓取】;
  - 有开发者按这句发到 audiosdk,两周多没有回音。改发 devauth 后立即收到自动回复,说 5 天内答复。约 1 天后收到「Pro Tools Developer NFR Bundle Activation Code」和操作说明【二手,[JUCE 论坛 69201](https://forum.juce.com/t/releasing-aax-plugin-no-response-from-avid-tips/69201) #6、#11】。
- 申请时要填许可数量【二手】。

**正文模板**【推测,字段取自上面几处来源;尖括号里换成实际信息,不要把个人邮箱写进仓库】:

```
Subject: License Request

Hello Avid Developer Authorization team,

I have registered as an AAX developer and would like to request the
AAX Developer License Bundle (Pro Tools Developer).
  Avid account:   <Avid 开发者账号邮箱>
  iLok user ID:   <iLok 用户名>
  Company:        <公司名或个人名义>
  Admin name:     <全名>
  Phone:          <含国际区号的电话>

We are developing "<插件名>", a free, open-source AAX Native plug-in
(JUCE 8.0.8 / AAX SDK 2.8.0) for Windows 11 x64.

Could you please grant:
  1 x Pro Tools Developer license (for testing unsigned builds)
  (optional) 1 x NFR license of Pro Tools (Artist or Studio) for testing
  signed builds including session save/restore.

Thank you.
```

- **等待时间**:有人当天就拿到了(2026-03),也有约 1 天的(2026-08);2026-07/08 有多位开发者反映 2–3 周没人回【二手,[JUCE 论坛 69239](https://forum.juce.com/t/anyone-got-through-to-avid-recently-pro-tools-developer-aax-signing/69239)】。
- **没人回时**:有人建议走 Avid 客服等其他渠道转人工(69201#2),或联系 support@paceap.com(69239#3)【二手】。
- **NFR**:能不能批下来没有核实【不确定】。SCVB 的 session 保存测试依赖它或 Artist 订阅(见第 4 节)。

### 2.3 买 iLok USB(放在最前面)

- **为什么是关键路径**:
  - 签名工具「will require a physical iLok USB key」【已核实,`dox/PTG:175`】;
  - Pro Tools Developer 的许可也是「loaded on an iLok USB device」【已核实,`dox/GSG:123`】;
  - 所以 **iLok 在 Developer 激活和签名两条路径上都是关键**。能不能只用 iLok Cloud 激活 Developer,不确定【不确定】。
- **价格**:
  - 官方店价记录为 USB-A $49.95、USB-C $59.95【二手】;
  - 2026-10-03 Sweetwater 实时价是 USB-A $44.95、USB-C $48【二手,[USB-A 页](https://www.sweetwater.com/store/detail/iLok3--pace-ilok-3rd-generation)、[USB-C 页](https://www.sweetwater.com/store/detail/iLok3USBC--pace-ilok-3-usb-c)】。
- **渠道**:原文「PACE Anti-Piracy Inc. or a reseller, including most music shops which sell audio software」【已核实,`dox/GSG:126`】。国内渠道和运费没有查【不确定】。
- **多台机器**:一个 iLok 不能同时插在两台机器上。Windows 和 Mac 都要签名时,要来回拔插,或者向 PACE 多申请一个签名许可(有先例)【二手,[KVR 帖](https://www.kvraudio.com/forum/viewtopic.php?t=601298)】。

### 2.4 在 Pro Tools Developer 里跑起来并录屏

1. 在 my.avid.com 下载最新的 Pro Tools Developer,把许可激活到 iLok【已核实,`dox/GSG:115,123`】。
2. 把未签名的 `.aaxplugin` 装进系统 AAX 目录(见 5.3),再启动 Pro Tools Developer。插件首次加载时,AAE 会调用 Describe 并缓存结果【已核实,`dox/PTG:225-231`】。
3. 按第 7 节的 P 系列(及插件自己的)测试清单测一遍。
4. **录屏**:
   - JUCE README 要求「a screen recording showing the plug-in running in Pro Tools Developer, with audio if possible」【已核实】。
   - 2026 年有申请者说并没有被要求附录屏,他自己附了一个演示视频【二手,69201#12】。为稳妥,建议先录好再发。
5. **限制**:Developer 版大概率**不能保存或导出 session**:
   - 原文是「Some Pro Tools developer builds are feature-limited; for example, developer builds of Pro Tools do not allow saving or exporting sessions」【已核实,`dox/PTG:799`,原文带「Some」】;
   - 另有两个独立来源也这么说(Antares 2025、BlindCard 2026)【二手】;
   - 所以 state 往返要等签名后在零售版或 NFR 版上测(第 4 节)。

### 2.5 PACE 签名工具申请

- **地址**:`audiosdk@avid.com`。三份一手来源一致【已核实】:JUCE README、`dox/PTG:162`、PACE 指南(页面邮箱经 Cloudflare 混淆,解码后是 audiosdk@avid.com)。
- **主题有三种写法**:

| 来源                                                  | 主题                                    | 必填信息                                                                                                   |
| ----------------------------------------------------- | --------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| JUCE README【已核实】                                 | **PACE Eden Signing Tools Request**     | 每个插件的简介 + 在 Pro Tools Developer 里运行的录屏;公司名、管理员全名、电话                              |
| AAX SDK 2.9 文档(`dox/PTG:162-168`)【已核实】         | Pace Tools Request                      | 公司名、管理员全名、Email、电话、**iLok username**                                                         |
| PACE 官方指南(2026-07-13 发布,07-17 修改)【已核实】   | PACE AAX Code Signing Tools Request     | 公司名、管理员姓名和邮箱、电话(含国际区号)、**公司网站**、插件简介、**Avid Developer 邮箱**                |

- **用哪个主题**:用 **「PACE Eden Signing Tools Request」**,逐字照抄。理由【二手】:
  - 2026 年两位申请者收到的 Avid 指示都是这个主题;
  - 一位申请者(2026-10-03,69201#12)报告说,主题不对的邮件会被直接丢弃,而且没有自动回复;
  - 另有开发者按 PACE 七月指南的主题发信,17 天没有回音(69239#1)。
- **正文要把三处来源要求的字段并集写全**:
  - 该申请者第一次漏了「Avid Developer email address」,5 天后被退回要求重发(69201#12)【二手】;
  - iLok username 只有 PTG 要求,一并写上【推测】。

**正文模板**【推测】:

```
Subject: PACE Eden Signing Tools Request

Company name:        <公司名或个人名义>
Company website:     <网站>
Admin full name:     <全名>
Admin email:         <管理员邮箱>
Avid Developer email address: <Avid 开发者账号邮箱>
Telephone (with international dialling code): <+区号 …>
iLok username:       <iLok 用户名>

Plug-in overview:
- <插件名> (AAX Native; Windows x64). <一两句功能说明>. Free of charge.
  Source code is public (GPL-3.0-or-later; AAX binaries distributed under GPLv3):
  <仓库地址>
Screen recording (Pro Tools Developer <版本>, Windows 11, with audio): <链接>

Questions:
1. Are there any terms in the PACE signing tools agreement that would prevent us
   from distributing signed binaries of a GPLv3 open-source plug-in (we keep the
   signing credentials private)?
2. If we later add more plug-ins from the same company, will each need a
   separate request?
```

- **要不要主动写开源和 GPL**:没有任何可引用的先例。建议主动写明并直接发问,在投入签名链路之前就把条款冲突问清楚【推测】。
- **审核**:Avid 要确认申请者是「legitimate publisher」【已核实,PACE 指南】;批不批没有保证【推测】。2026 年有个人开发者成功的先例【二手,69201#12】。
- **等待时间**:
  - 2026 年的报告里有 2–3 天、5 天、约 1 周、约 2 周以上不等;有人用错地址或主题,2 周以上没人回【二手】。
  - **7 天没回音时**,先核对主题逐字一致、字段齐全,再原样重发一次。仍然没有回音,可以试 Avid 客服渠道或 support@paceap.com【二手,69201#2、69239#3】。

### 2.6 收到邀请之后

2026-10-03 一位申请者在 69201#12 里的完整流程最详细【二手】,与另一位申请者在 note.com 的记录一致:

1. Avid 审核通过后把资料转给 PACE【已核实·一手,PACE 指南第 2 步】。
2. PACE 发来邮件「Invitation to the PACE AAX Code Signing Tools」,要求在 **72 小时内**通过链接填完申请表【二手】。
3. 约 2 天后收到 PACE 销售的邮件「Accessing the PACE AAX Code Signing Tools」【二手】。
4. 在 iLok License Manager 里把「PACE Central Access」授权下发到 iLok USB,然后**登出再登入**【二手】。
   - note.com 那位的记录是:激活许可后**必须右键执行「Synchronize」**,否则签名证书不会下发到 iLok,签名时会报错【二手】。
   - 两者都做一遍。
5. 从 iLok License Manager 的界面进入 PACE Central 网站:
   - 先建 **Product**,再建 **Wrap configuration**;
   - 拿到的 GUID 就是 wraptool 的 `--wcguid` 参数;
   - 来源【二手】:69201#12;[Dplug AAX Guide](https://github.com/AuburnSounds/Dplug/wiki/Dplug-AAX-Guide)(「fill up product and "wrap configurations" in the PACE Central website. PACE Central is only accessible when clicking on a button in your iLok Licence Manager UI」)。
6. 下载 **Fusion SDK**(签名和 wrap 工具都在里面;Dplug 指南里 mac 的路径是 `/Applications/PACEAntiPiracy/Eden/Fusion/Versions/5/bin/wraptool`)【二手】。
7. PACE 文档提到要从 PACE Central 的 Admin → Company Details 取 customer name 和 customer number;申请者说那里实际只显示 customer number【二手】。新版 wraptool 会不会因此要求额外参数,不确定【不确定】,以 Fusion SDK 自带文档为准。
8. 工具许可有效期 1 年,每年续期【二手】。

### 2.7 证书准备

- **Windows**:
  - 原文:「Although it is possible to use self-signed certificates for AAX digital signatures, before making your AAX plug-ins commercially available it is recommended that you acquire … an "Extended Verification" (EV) Authenticode certificate」【已核实,`dox/PTG:179`】。
  - Dplug 指南也说,自签证书只适合在等正式证书期间做**临时**构建,「Don't use it for final builds」【二手】。
  - 所以 beta 阶段可以用**自签 .pfx**;免费分发的项目自签能不能长期用,要以实测和 PACE 文档为准【推测】。正式 OV/EV 证书每年要数百美元【推测】。
  - 生成自签证书,在 PowerShell 里执行【推测,标准 Windows 做法,未实测】:

    ```powershell
    $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=<签名名称>" -CertStoreLocation Cert:\CurrentUser\My
    Export-PfxCertificate -Cert $c -FilePath <安全目录>\aax-signing.pfx -Password (Read-Host -AsSecureString)
    ```

  - 凭据纪律:.pfx、口令、iLok 账号口令都不进仓库、不写进脚本,口令运行时输入或从环境变量读。若要放进 CI,要走 secret。**这与本仓已定案的约束直接相关**:[CLAUDE.md](../../CLAUDE.md) §0 的 U13 规定 v1 不做代码签名(将来引入签名时,证书与凭据必须走 secret、不落盘明文、不进仓库),CLAUDE.md §6 规定构建流水线不需要任何 secret。而公开发布的 Pro Tools 只加载 PACE 签名的 AAX(硬门槛 1),所以发布 AAX 本身就要先由用户对 U13 重新裁定,云签名(2.10)与 Validator CI(3.3)也都要引入 secret,适用同一条【推测】。
- **mac**:
  - 用 `--signid "<钥匙串里的签名身份>"`。
  - 2021 年 JUCE 论坛的说法:如果不打 pkg、不公证,钥匙串里任何一张证书都可以;要公证或打 pkg,就必须用 Developer ID Application【二手,[JUCE 论坛 48418](https://forum.juce.com/t/aax-plugin-and-package-builder-in-mac/48418)】。
  - 能不能只用 ad-hoc 身份(`-`)【不确定】。
  - Windows 上的 wraptool 能不能签 mac 的 bundle【不确定】;推测必须在 macOS 上执行。

### 2.8 wraptool 签名命令

命令形态来自公开构建脚本【二手】:

- ChowTapeModel:[AnalogTapeModel](https://github.com/jatinchowdhury18/AnalogTapeModel) 的 `Scripts/aax_builds.sh:84-111`。调研时复核过:mac 用 `--signid` 加 `--dsig1-compat off`;Windows 用 `--keyfile`、`--keypassword`,`--in`/`--out` 传外层 bundle,verify 传内层 DLL。
- NeuralAmpModelerPlugin:[仓库](https://github.com/sdatkinson/NeuralAmpModelerPlugin)的 `NeuralAmpModeler/scripts/makedist-win.bat:94-95`,这是 **REM 注释掉的模板行**,`--in` 传内层 DLL;`makedist-mac.sh:222`。
- Dplug:[仓库](https://github.com/AuburnSounds/Dplug)的 `tools/dplug-build/source/main.d:603-651`;mac 上传 bundle(`:1219` 注释:「wraptool won't accept the executable only」)。
- Venn Audio(云签名,2023-04):[文章](https://www.vennaudio.com/how-we-incorporate-paces-cloud-signing-tools-into-our-build-pipeline-for-free-suite/)。

**以 Fusion SDK 自带的 PACE 文档为准。** 2026 年版可能要额外参数(2.6 第 7 步)。

**Windows(USB iLok + 自签 pfx)**:

```powershell
# 口令从环境变量读,不要写进脚本,也不要留在 shell 历史
wraptool sign --verbose `
  --account   $env:ILOK_ACCOUNT `
  --password  $env:ILOK_PASSWORD `
  --wcguid    $env:PACE_WCGUID `
  --keyfile   <安全目录>\aax-signing.pfx `
  --keypassword $env:AAX_PFX_PASSWORD `
  --in  "<构建输出>\<插件名>.aaxplugin" `
  --out "<构建输出>\<插件名>.aaxplugin"

wraptool verify --verbose `
  --in "<构建输出>\<插件名>.aaxplugin\Contents\x64\<插件名>.aaxplugin"
```

**macOS**:

```bash
wraptool sign --verbose \
  --account  "$ILOK_ACCOUNT" --password "$ILOK_PASSWORD" \
  --wcguid   "$PACE_WCGUID" \
  --signid   "Developer ID Application: <名称> (<TEAMID>)" \
  --dsig1-compat off \
  --in  "<构建输出>/<插件名>.aaxplugin" \
  --out "<构建输出>/<插件名>.aaxplugin"
wraptool verify --verbose --in "<构建输出>/<插件名>.aaxplugin"
```

`--signid` 可以换成钥匙串里的其他签名身份,见 2.7。

**参数含义**:

| 参数                             | 含义                                                                                                          | 标记                |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------- | ------------------- |
| `sign` / `verify`                | 签名;校验签名                                                                                                 | 【二手】            |
| `--account` / `--password`(`-p`) | iLok 账号和密码                                                                                               | 【二手】            |
| `--wcguid`                       | PACE Central 里那份 wrap configuration 的 GUID                                                                | 【二手】            |
| `--keyfile` / `--keypassword`    | Windows:Authenticode 证书 .pfx/.p12 和口令                                                                    | 【二手】            |
| `--signid`                       | mac:Apple 签名身份。Windows:证书存储里的指纹,用来代替 keyfile(Dplug 的做法)                                   | 【二手】            |
| `--in` / `--out`                 | 输入和输出路径,可以相同(原地签名)                                                                             | 【二手】            |
| `--dsig1-compat off`             | 关掉旧版 dsig1 兼容(ChowTape 和 Venn 的 mac 命令都带;Dplug 注释掉了,注明「不确定是否无害」)                   | 含义【推测】        |
| `--dsigharden`                   | Venn 的 mac 命令里带了;推测与 hardened runtime 有关                                                           | 【推测】            |
| `--allowsigningservice`          | 允许走 iLok 云会话,云签名才用。Dplug 2022 年注释说「只签名时加不加没区别」                                    | 【二手】            |
| `--explicitsigningoptions`       | Eden 5.10 起,Windows 可以对接 Azure 或云 KMS 证书                                                             | 【二手,moonbase.sh】 |

**两处要注意**:

- **Windows 上 `--in` 传哪个路径,先例不一致**【推测】:ChowTape 传外层 bundle 目录、verify 内层 DLL;NAM 的注释模板传内层 DLL;Dplug 在 Windows 上传二进制,在 mac 上传 bundle。
- **签名必须是最后一步**:「any operation that modifies the contents of the bundle will invalidate its digital signature」【已核实,`dox/PTG:152`】。
  - 签名之后不能再 strip 符号,也不能往 bundle 里加文件或改文件。
  - 之后打 zip 不会改 bundle 内容【推测】。mac 版打包要用能保住符号链接和扩展属性的方式(如 `ditto`)。

### 2.9 每次发版的签名操作【推测】

1. 推 tag → release 流水线构建 → 下载 CI 产出的**未签名** AAX workflow artifact。
2. 在插着 iLok 的 Windows 电脑上 `wraptool sign` + `verify` → 本地打包出 zip 和校验和。
3. (做 mac 版时)把 iLok 拔下来插到 Mac 上,签 mac 版并 verify;可选再公证(2.11)。
4. 装进系统 AAX 目录,在零售版(Intro、Artist 或试用版)里冒烟加载。
   - 同一台 Windows 上大概率不能同时装 Pro Tools Developer 和零售版,切换要卸载重装【推测,未查到原文】;
   - 冒烟机最好固定为零售版,或单独准备一台。
5. 把签名后的 zip 和校验和附到 draft Release,再转正式。

每版约 15–30 分钟,而且要有人在场插着 iLok。

### 2.10 可选:PACE 云签名

- PACE 官方(「Cloud 2 Cloud」,2021-06-02 发布):「no longer requires a physical iLok USB device to be attached to the machine that is handling the code signing」,定位是给云 CI 用;**官网不公开价格**,要联系 PACE【已核实,[PACE 文章](https://paceap.com/cloud-aax-code-signing-with-ci-build-systems/)】。
- 价格:两份独立的 2026 年报告都说约 **$1000/年**,其中一份说是 PACE support 直接报的价【二手,note.com 记录;69201#12】。
- 用法:`iloktool cloud --open …` → `wraptool sign … --allowsigningservice` → `iloktool cloud --close`【二手,Venn】。
  - tone3000 在 GitHub Actions 里就是这么签的【已核实,tone-3000/tone3000-plugin 仓库 `.github/workflows/build.yml:314-337`】。
- 对本项目的影响【推测】:要把 iLok 凭据和 wcguid 放进 GitHub secrets;beta 阶段不建议上云签名。
- 另一条路是在自己的电脑上挂 self-hosted runner。公开仓库这样做有 fork PR 在本机上跑代码的风险【推测】。

### 2.11 mac:Apple codesign、公证与 wraptool 的关系

- **一步完成两种签名**:Eden 工具集「integrates fully with platform-specific signatures, so you only need to do one post-build step … to sign your plug-in with both the Eden signature and the relevant Apple GateKeeper or Microsoft Authenticode signature」【已核实,`dox/PTG:188`】。
- **签名顺序**:wraptool 的签名会**替换**原有的 codesign 签名,顺序是 codesign → wraptool → 公证【二手,[KVR 帖](https://www.kvraudio.com/forum/viewtopic.php?t=540054)】。所以 JUCE 构建时自动加的 ad-hoc 签名会被覆盖【推测】。
- **公证**:必须用 Developer ID($99/年)【已核实·一手,Apple 会员对比页】。
- **公证命令**【推测,标准 Apple 工具链;`stapler` 能不能处理 `.aaxplugin` 没有核实】:

  ```bash
  ditto -c -k --keepParent "<插件名>.aaxplugin" aax-notarize.zip
  xcrun notarytool submit aax-notarize.zip --apple-id <…> --team-id <…> --password <app 专用密码> --wait
  xcrun stapler staple "<插件名>.aaxplugin"
  ```

- **不交 $99 的做法**【推测】:
  - 自签身份 + PACE 签名 + 让用户执行 `sudo xattr -dr com.apple.quarantine` 去掉隔离;
  - 去掉 quarantine 这个 xattr 不在代码签名的覆盖范围内,不会让签名失效【推测】;
  - macOS 15 起取消了 Control-click 放行,只能去系统设置或用终端【已核实·一手,[Apple 开发者新闻](https://developer.apple.com/news/?id=saqachfa)】。

---

## 3. 没有宿主时怎么验证(AAX Validator)

### 3.1 获取方式

- **组成**:DigiShell(`dsh`,命令行 AAX 宿主)+ `aaxval` dish(Validator 的测试模块)+ DTT(Ruby 写的自动化层),打包成「DigiShell and AAX Validator」,在 my.avid.com 的 AAX SDK Toolkit 下载区【已核实,`dox/PTG:809`】。
- **当前包**:调研时是 `aax-validator-dsh-2024-6-0-…`,里面的 DigiShell 是 v24.9.0x14【已核实】。
- **费用和许可**:
  - 免费,要 Avid 账号和 iLok 账号,还要接受 click-through 许可;
  - **推测不需要 devauth 审批**【推测】;所以拿到 Avid 账号后,在 Developer 激活码到手之前,就可以先用它自检。
- **未签名插件也能测**:原文「This requirement does not apply to Development builds of Pro Tools or to other developer tools which can load unsigned binaries」【已核实,`dox/PTG:152`】。
  - 实证:novonotes 的 wrac-plugin-template 在 CI 里只给插件做了 ad-hoc 签名,12 项测试全过【已核实】。
  - 例外:**已经做过 PACE 签名**的插件,跑 Validator 时机器上要有 iLok License Manager 或 PACE 运行时【已核实】。

### 3.2 能测什么,不能测什么

| 能测【已核实】                                                                                                  | 不能测【推测】                                                                               |
| --------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| `test.describe_validation`:stem format、PlugInID、类别。`AAX_CATEGORY` 为 None 只报警告                         | 音频处理是否正确(`aaxh` dish 明确「不带音频路由」,`dox/DSH:68`)                              |
| `test.load_unload`:构造、析构;Describe 时 JUCE 会新建处理器实例                                                 | 宿主的走带和时间线(dsh 不给播放中的位置)                                                     |
| `test.parameters`、`test.parameter_traversal.*`                                                                 | GUI(WebView2/WKWebView 编辑器)                                                               |
| `test.data_model`:数据模型和状态,可能包括 chunk 存取                                                            | DPP、Multi-Mono 的实际行为、离线 Bounce、session 存取、自动化写入                            |
| `info.productids`、`info.support.audiosuite` 等:可以用来确认 AudioSuite 确实关掉了、stem 变体收敛了             | 跨实例行为(SCVB 的核心就在这里)                                                              |

不要跑以下两项:

- `test.page_table.load`:没有页表必然失败;
- `test.cycle_counts`:全量 `runtests` 会死在这一项。

**结论**:Validator 通过,只能说明「单个实例是合规的 AAX」【已核实】。

### 3.3 在 GitHub 托管 runner 上跑

- **公开先例只有一个,而且不是 JUCE 项目**:[novonotes/wrac-plugin-template](https://github.com/novonotes/wrac-plugin-template) 的 `aax-validation` job(`.github/workflows/ci.yml:166-263`)。
  - 在 `macos-latest` 和 `windows-latest` 上各跑 12 项,只在 push 和 workflow_dispatch 时触发;
  - 2026-06-10 的运行 27263728219 两个平台都是 success【已核实】。
- **JUCE 生态里没有人在托管 CI 上跑 Validator**。主流做法是 CI 出包,开发者在本机跑 dsh,再用 Pro Tools Developer 实测【已核实】。
- **已知的坑**【已核实,都是别人实测出来的】:
  1. Windows 上往 dsh 的 stdin 灌脚本会丢输入,要改用 DTT 脚本;
  2. Windows 包的 `Main.valconfig` 用的是 POSIX 单引号,会退回去测示例插件 Trim,要改成转义双引号;
  3. mac 上下载的包带 quarantine,`run_test.command` 也可能丢可执行位,要 `xattr -dr` 加 `chmod +x`;
  4. 新版 macOS 上 DTT 的 `SysInfo.rb` 会崩,要打补丁;
  5. 不要全量 `runtests`,不要并发起多个 dsh(会报端口冲突);要么单会话逐条跑,要么串行逐项起 DTT;
  6. 插件架构要和 dsh 包一致,否则会卡住、没有输出;
  7. 第一项测试经常是 `E_ABORTED`,先跑一项 warm-up 把它吸收掉;
  8. Windows 上 DTT 需要 Ruby,GitHub 的 Windows 镜像已经预装 3.3.12;
  9. 不必装进系统 AAX 目录,用 `pi_path` 指向一个暂存目录就行。
- **公开仓库的约束**:
  - Avid 的包放在私有 URL,经 secret 下载【已核实·先例】;
  - **不要用 actions/cache 缓存它**。GitHub 原文:能对仓库开 PR 的人都能读 base 分支的缓存【已核实,[GitHub 文档](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)】;
  - 不要把它当 artifact 上传【推测】;
  - 只在 push 到主干和 workflow_dispatch 时跑;
  - 能不能在 CI 上用,没有公开条文【不确定】,建议先写信问 audiosdk。
- **对本项目的含义**【推测】:做 CI 自动验证就要引入 secret。第一版建议只在本机手动跑 Validator;CI 版作为可选项另行决定。

### 3.4 pluginval 和 AudioPluginHost 都不支持 AAX

- JUCE 8.0.8 宿主侧只编入 VST、VST3、AU、LADSPA、LV2【已核实,`modules/juce_audio_processors/format/juce_AudioPluginFormatManager.cpp:42-72`】。
- pluginval 的 README 写的是「Test VST/AU/VST3 plugins」【已核实】。
- Avid 不允许第三方 AAX 宿主【二手,[JUCE 论坛 44472](https://forum.juce.com/t/aax-plugins-host/44472)】。
- 所以现有 CI 里的 pluginval 对 AAX 不起作用【已核实】。

---

## 4. 测试宿主

| 宿主                    | 未签名 AAX                                                                                      | 保存 session                                                                                    | 规模                                                                                                                                                                      | 费用                                           | 对 SCVB                                                  |
| ----------------------- | ----------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------- | -------------------------------------------------------- |
| **Pro Tools Developer** | 能加载【已核实,`dox/PTG:132,152`】                                                              | **大概率不能**【已核实,`dox/PTG:799`,原文带「Some」;另有两个二手来源】;能不能 Bounce 导出【不确定】 | 轨道上限不明【不确定】                                                                                                                                                    | $0,向 devauth 申请                             | 能跑 P-1/2/3/4/11;P-8 不行                               |
| **Pro Tools Intro**     | 不能。原文「Publicly available builds of Pro Tools require that AAX plug-ins be digitally signed」【已核实,`dox/GSG:112`】 | **能**:「Pro Tools Intro supports saving sessions locally」【已核实,Avid Intro 介绍页】          | **8 条音轨、4 条 Aux**;「all qualified third-party AAX plugins will run in Pro Tools Intro」;磁盘授权,不需要 iLok【已核实,[Avid 介绍页](https://www.avid.com/resource-center/introducing-pro-tools-intro)】 | $0                                             | **不够**(SCVB 有 15 条人声轨)【调研结论】                |
| **Pro Tools Artist**    | 不能                                                                                            | 能                                                                                              | 32 条音轨、32 条 Aux                                                                                                                                                      | $9.99/月,或 $99/年【二手,Production Expert 2026-02】 | 签名后测 P-8、P-12                                       |
| **30 天试用**           | 不能,签名后才能用【二手】                                                                       | —                                                                                               | —                                                                                                                                                                         | $0                                             | 一次性窗口                                               |
| **NFR 许可**            | —                                                                                               | —                                                                                               | —                                                                                                                                                                         | $0,发 devauth「License Request」申请【已核实,`dox/GSG:123`】 | 优先尝试                                                 |

- **Pro Tools 2026.4.1 的系统要求**【已核实·一手,[Avid 系统要求页](https://kb.avid.com/pkb/articles/compatibility/Pro-Tools-System-Requirements)】:
  - Windows 11 64 位,Intel Core;
  - macOS 26.3.x / 15.7.x / 14.8.x;M1–M5 或 Intel;
  - 终端用户可以只用 iLok Cloud。
- **含义**【推测】:Windows 版 AAX 只需要 x64;mac 版 AAX 要不要出 universal 需要另行决定。
- **Pro Tools Developer 不能在 CI 上无头运行**:它是 GUI 应用,要特殊许可,也没有先例【推测,把握高】。
- **Developer 和零售版大概率不能装在同一台机器上**,切换要卸载重装【推测】。规划签名前后的测试机时要考虑这一点。

---

## 5. JUCE 构建要点

### 5.1 开关和宏(JUCE 8.0.8 源码已核实)

| 需求                      | 做法                                                                                                                                                                                              | 出处                                                                                                                                                                                  |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 生成 AAX                  | `FORMATS … AAX`                                                                                                                                                                                   | JUCE `docs/CMake API.md:479-481` 写「必须先调用 `juce_set_aax_sdk_path`」,**文档滞后**;8.0.8 的代码会自动用自带的 SDK:`JUCEUtils.cmake:1157-1164,1277,1602-1605`【已核实】           |
| 平台过滤                  | 只有在 Darwin,或 Windows 且 `JUCE_TARGET_ARCHITECTURE` = x86_64、编译器不是 GNU 时,AAX 才会被构建;其他情况**静默跳过**                                                                             | `extras/Build/CMake/JUCEModuleSupport.cmake:293-297`;`JUCEUtils.cmake:1492-1501`【已核实】                                                                                            |
| 类别                      | `AAX_CATEGORY <None/EQ/Dynamics/…/SoundField/Effect/…>`。普通效果器默认是 None,Validator 会报警告                                                                                                 | `JUCEUtils.cmake:1622-1667,1851-1867`;`CMake API.md:579-582`【已核实】                                                                                                                |
| 关 Multi-mono             | `DISABLE_AAX_MULTI_MONO TRUE`,映射到宏 `JucePlugin_AAXDisableMultiMono`                                                                                                                           | `JUCEUtils.cmake:1567,1761`;`AAX.cpp:2494-2498`(默认 `MultiMonoSupport=true`)【已核实】                                                                                               |
| 关 AudioSuite             | **没有对应的 CMake 选项**,要用 `target_compile_definitions(... JucePlugin_AAXDisableAudioSuite=1)`                                                                                                 | `AAX.cpp:2487-2492`(`#if ! JucePlugin_AAXDisableAudioSuite` 时注册 AudioSuite ID);JUCEUtils 和 Projucer 里都搜不到这个宏【已核实】                                                    |
| AlwaysProcess(关闭 DPP 停调) | **没有对应的 CMake 选项**,要用 `target_compile_definitions(... JucePlugin_AAXDisableDynamicProcessing=1)`。**Input 与 Output 两个插件都要开**(原因见 7.1)                                           | `AAX.cpp:2500-2502`;`JUCEUtils.cmake:1562-1567` 只映射了 Bypass 和 MultiMono【已核实】                                                                                                |
| 其他可用的宏              | `JucePlugin_AAXDisableDefaultSettingsChunks`、`JucePlugin_AAXDisableSaveRestore`;CMake 选项 `DISABLE_AAX_BYPASS`                                                                                   | `AAX.cpp:2460-2464,2504-2510`;`JUCEUtils.cmake:1566,1760`【已核实】                                                                                                                   |
| 宏要用 PUBLIC             | 宏定义要传到格式 wrapper 的编译单元里,`target_compile_definitions` 用 PUBLIC                                                                                                                      | 【推测】,依据 JUCE 的 wrapper 是独立编译单元                                                                                                                                          |
| AAX ID                    | `AAX_IDENTIFIER` 默认等于 `BUNDLE_ID`;ManufacturerID = `PLUGIN_MANUFACTURER_CODE`,ProductID = `PLUGIN_CODE`                                                                                       | `JUCEUtils.cmake:1562-1564,1766`;`AAX.cpp:2457-2458`【已核实】                                                                                                                        |
| 参数 ID                   | 用 ParamID 字符串(非 legacy;`JUCE_FORCE_USE_LEGACY_PARAM_IDS` 默认 0);AAX 参数 ID 上限 31 字符                                                                                                    | `AAX.cpp:1718-1748`;`juce_audio_plugin_client.h:89-91`;上限出自 SDK `AAX/SDK/Interfaces/AAX.h:362,377`【已核实】                                                                      |
| Master Bypass             | 处理器没有 bypass 参数时,wrapper 会自建一个 `Master Bypass`,宿主可见参数数 = N + 1(SCVB 为 123 + 1 = 124);宿主旁路时调 `processBlockBypassed`                                                      | `AAX.cpp:1729-1744,1665-1668`【已核实】                                                                                                                                               |
| 名字                      | 长名取 `getName(31)`,**短名一律取 `getName(4)`**;步数上限 2048                                                                                                                                   | `AAX.cpp:1711,1760,1766`【已核实】                                                                                                                                                    |
| prepare                   | 初始化时一律按最大块 1024 调 `prepareToPlay`(`maxSamplesPerBlock = 1 << AAX_eAudioBufferLength_Max`)                                                                                               | `AAX.cpp:1998-2004,2233-2239`;`AAX_Enums.h:164`【已核实】                                                                                                                             |
| stem 变体                 | 对 **34** 种 AAX stem format 做「输入 × 输出」笛卡尔积,逐个经 `fullBusesLayoutFromMainLayout` → `checkBusesLayoutSupported` 过滤。JUCE 默认的 `isBusesLayoutSupported` 对任何 layout 都返回 true   | `AAX.cpp:252-287,1455-1500,2597-2621`;`juce_AudioProcessor.h:1384`【已核实】                                                                                                          |
| 非实时                    | `EnteringOfflineMode`、`ExitingOfflineMode` 和 AudioSuite pass 会调 `setNonRealtime`                                                                                                              | `AAX.cpp:1305-1318`【已核实】                                                                                                                                                         |
| 「工程已修改」            | `updateHostDisplay(...withNonParameterStateChanged(true))` 会让 `numSetDirtyCalls` 加 1,经 `GetNumberOfChanges` 报给宿主                                                                          | `AAX.cpp:990-995,1285-1286`【已核实】                                                                                                                                                 |
| Timer                     | 「Pro Tools 里定时器失效」的老 bug 在 7.0.12 已修                                                                                                                                                 | JUCE `CHANGE_LIST.md:87-89`【已核实】                                                                                                                                                 |

### 5.2 bundle 结构

- **Windows**:
  - 结构是 `<Name>.aaxplugin/Contents/x64/<Name>.aaxplugin`(内层是 DLL),外加 `desktop.ini`、`Plugin.ico`。有图标时(默认用 SDK 自带的 `PlugIn.ico`),JUCE 会对 `desktop.ini` 和外层目录执行 `attrib +s`【已核实,`JUCEUtils.cmake:859-891,1280-1290`】。
  - 架构:JUCE 只在 Windows x86_64 上构建 AAX(5.1「平台过滤」)。所以 `JUCEUtils.cmake:1281-1287` 里的 Win32 和 FATAL_ERROR 分支实际走不到。**没有 Windows ARM64 版,也没有 32 位版**【已核实】。
- **macOS**:标准 bundle `.aaxplugin/Contents/MacOS/…`【已核实,`JUCEUtils.cmake:1267-1275`】。
- **命名规则**:bundle 必须用 `.aaxplugin` 后缀;Windows 的 DLL 本身也必须用这个后缀【已核实,`dox/PTG:144`】。

### 5.3 安装路径和权限

- **路径**【已核实】:
  - Windows 64 位:`C:\Program Files\Common Files\Avid\Audio\Plug-Ins`;
  - macOS:`/Library/Application Support/Avid/Audio/Plug-Ins`;
  - 出处:`dox/DIST:118-126`;JUCE 默认拷贝目录 `JUCEUtils.cmake:126,137`。
- **子目录**:「This directory is searched recursively」,可以装到子目录里【已核实,`dox/DIST:129`】。
- **停用**:系统上保留但已停用的插件会放在旁边的「Plug-Ins (Unused)」目录【已核实,`dox/PTG:138`】。
- **调试**:Pro Tools 也会扫描应用程序旁边的 Plug-Ins 目录,这是调试用的功能【已核实,`dox/PTG:140`】。
- **权限**:两个路径都要**管理员权限**(Windows 用管理员 PowerShell;mac 用 `sudo`)【推测,依据系统目录的权限惯例】。
  - AAX 没有 VST3 那种「放任意目录、在宿主里加扫描路径」的免管理员办法【推测】。
  - CI 里**不要**开 `COPY_PLUGIN_AFTER_BUILD`(它要写系统目录)【推测】。
- **升级**:先删旧 bundle 再拷,避免残留旧文件【推测】。

---

## 6. Pro Tools 宿主行为要点

1. **DPP(Dynamic Plug-in Processing)**:
   - 链路输出静音持续一段时间后,Pro Tools 会**停调整条插件链**。只要链上有一个插件声明 AlwaysProcess,整条链都会一直被处理。SDK 同时提醒:除非 DPP 确实干扰了插件,否则不要用这个属性【已核实,SDK `AAX/SDK/Interfaces/AAX_Properties.h:865-880`】。
   - 第三方在 Pro Tools Developer 2026.4 上实测:静音约 8 秒后停调;开了 AlwaysProcess 后,整条链连走带停止时也一直被调用【二手,单一来源,[kirin_hypha PR 54](https://github.com/heyalohaloha/kirin_hypha/pull/54)】。实测链上全是直通插件,输入静音也就等于输出静音,所以分不出判据。
   - DPP 判「静音」看的是输入还是输出,没有核实。SDK 原文更像是看输出:「the chains' output drops to silence」【不确定】。
   - JUCE 的 AAX wrapper 不把 tail 长度报给宿主,所以「无限 tail」救不了 DPP【已核实,JUCE 源码阅读】。
2. **延迟补偿与 `GetCurrentNativeSampleLocation`**:
   - JUCE 在播放时取 `GetCurrentNativeSampleLocation`;停止时优先取**时间线选区的起点**,取不到再退回当前位置【已核实,`AAX.cpp:1133-1141`】。
   - 单一第三方实测:同一条插入链上、位于延迟插件下游的插件,拿到的位置已经扣掉了上游延迟。但 Aux 和别的轨上的插件**没人测过**,离线 Bounce 也没测过【二手/推测】。
   - **不读 playhead 的插件不受这一条影响。**
3. **高、低延迟两个处理域**:
   - 录音布防或输入监听的轨,以及接收实时输入的 Aux,走低延迟域;其他插件走高延迟域,44.1/48 kHz 下块长 1024【二手,Production Expert】。
   - Low Latency Monitoring 会旁路录音布防轨上的插件【二手,Sweetwater】。
4. **离线 Bounce**:
   - JUCE 会收到 `EnteringOfflineMode` 并 `setNonRealtime(true)`【已核实,`AAX.cpp:1305`】。
   - 离线期间 JUCE 的消息线程定时器还派不派发【不确定】。
   - Track Freeze、Commit、Track Bounce 只取这一条轨自己的输出【推测】。
5. **自动化**:
   - 插件参数**默认没有开启自动化**,要在 Plug-in Automation 对话框里开启,或按 Ctrl+Win+Alt 点击控件。WebView 界面上没有原生控件,后一条路走不通【二手,多源】。
   - **Write** 会把所有已开启的参数整遍覆写;**循环只录第一遍**;推荐用 **Latch**。Latch 松手后会不会一直写到停止,属于推测【二手/推测】。
   - gesture 会映射成 Touch/Release【已核实,`AAX.cpp:1289-1299`】。
6. **mono → stereo 等 stem 限制**:
   - JUCE 会为每一对「输入 × 输出」stem 注册一个变体。不重写 `isBusesLayoutSupported` 的话,34×34 种组合都会出现,包括 stereo→mono 这类会**改变轨道宽度**的变体【已核实源码;Pro Tools 菜单里怎么显示属于推测】。
   - 默认还会出现 Multi-Mono 版,即在立体声轨上为 L、R 各建一个实例【推测】。
7. **WebView 键盘焦点**:
   - WebView2 或 WKWebView 拿到焦点后,按键不会转发给宿主,所以**空格不能启停走带**。这是 JUCE WebView 的通病,社区反复报告,没有官方解法【二手,[JUCE 论坛 62439](https://forum.juce.com/t/webview-and-keyboard-input-propagation-issue-to-the-host/62439) 等】。
   - Windows 缩放大于 100% 时,界面可能模糊或尺寸不对【二手】。
8. **其他**:
   - HDX/Carbon 的 DSP Mode 会停用没有 DSP 版本的 Native 插件,文档要提示开 **DSP Mode Safe**【二手】;
   - Make Inactive 会停止处理【推测】;
   - Pro Tools 同一时间只能开一个 session【推测】;
   - Master Fader 的插槽**在推子之后**,Insert A–J 在推子之前【二手/通识】。

---

## 7. SCVB 专属事项

> **现状**:调研结束时的决定是本轮不做 AAX。本节只备忘,等以后启动 AAX 版本时作为起点。下列条目都来自对 SCVB 实现的阅读与推演,**没有一条在 Pro Tools 里实测过**【调研结论,以实测为准】。

### 7.1 DPP 与 KI-6 链路

- **AlwaysProcess 必须给 Input 和 Output 都开**:
  - Output 所在 Aux 的输入恒为静音;**Input 健康时向下游输出的也恒为数字静音**。不管 DPP 按输入还是按输出判静音,SCVB 都会坏【调研结论】。
  - 代价:每条人声轨的整条插件链在走带停止时也会持续处理(第三方实测:停止时 1024 帧的块大约每秒被调用 47 次)【二手】。
- **SCVB Monitor**:本仓有三个插件,Monitor 是只读监视器(参见宪法 ADR-001a)。调研时没有单独分析 DPP 对它的影响。推测:若它所在的链被 DPP 停调,投影面会停止更新,但不影响 Input/Output 的音频路径;要不要也开 AlwaysProcess 应在 P-2 里顺带观察后再定【推测,未实测】。
- **KI-6 链路**:
  - 调研时的行为:Output 被停调后,看门狗 0.5 秒断开(`src/core/ipc/CtrlPlane.cpp` 的 `kWatchdogStallMs`)→ Input 经 5 秒滞回转为直通(`src/core/input/OutputStage.h` 的 `kPassthroughHysteresisMs`)→ 合计约 5.5 秒无声,之后是未平衡的原始声像。描述见 [KNOWN_ISSUES.md](../KNOWN_ISSUES.md) 的 KI-6,**以当时 KNOWN_ISSUES 为准**(宿主调度加固会改变它)【已核实】。
  - 要用 P-2 的三组对照把 DPP 的判据测出来。

### 7.2 Aux 一侧的位置补偿没有实测

- 如果 Aux 拿到的位置没有补偿,读方会一直领先写头,primed 门永远过不去,失败也不计数,结果是这条轨**静默无声、不亮横幅**【调研结论,未实测】。
- 动手之前要先补一条「正向在场」判据和告警,让这种静默失败可见【推测】。
- 用 P-3 测:Input 之前挂高延迟插件,Output 放在 Aux 上,实时和离线各一次。

### 7.3 参数、桥面与宪法

- **参数是 123 + 1**:AAX 下宿主可见 124 个(Master Bypass 由 JUCE 合成,口径见 [PARAMETERS.md](../PARAMETERS.md))。4 字符短名下,120 个分轨参数会塌成 `V1 T` / `V2 T` 两种,彼此区分不开(3 个全局参数是 `Widt` / `MS B` / `Lead`)。修它只改显示名,可选,约 0.5 人日【调研结论】。若要按格式分别说明参数口径,改冻结文档的措辞需要批准。
- **桥面 `host` 闭集加 `"protools"`**:`host` 字段取值域在 [SCVB_CONTRACT.md](../SCVB_CONTRACT.md) 里是闭集,只许放宽;加值属于契约变更,要写 `docs/contract-changes/` 文档并获批准;beta 不是必须【调研结论】。
- **ADR-001 修宪**:宪法 [ADR-001](../constitution/ADR.md) 把三个插件写死为 VST3,加 AAX 需要走修宪流程并获批准【已核实,宪法文本】。

### 7.4 构建与宿主摆放

- 三个插件都关 AudioSuite 和 Multi-mono(5.1)。
- Output 放在**立体声 Aux 的 Insert A**,不能放在 Master Fader 上【调研结论】。
- 补「工程已修改」通知(`updateHostDisplay` 带 `withNonParameterStateChanged`,见 5.1)【推测】。
- mac 版 AAX 要等 POSIX 共享内存后端和 macOS 移植完成之后才能做【调研结论】。

### 7.5 测试宿主

- Intro 只有 8 条音轨,不够 15 条人声轨(第 4 节)。
- P-8 要等签名后在 Artist 或 NFR 上做。

### 7.6 P 系列实测清单(压缩版)与工作量

| 编号 | 测什么                                                                         | 优先级             |
| ---- | ------------------------------------------------------------------------------ | ------------------ |
| P-1  | 5 条人声轨 → Aux 实时播放,缺口为 0,**逐轨能量在场**(不能只看缺口计数)         | P0                 |
| P-2  | DPP 三组对照(不加 AlwaysProcess):(a) 连续演唱 3 分钟、中间没有全体静默,看人声会不会中途消失;(b) 人为制造 ≥10 秒的全体人声静默再进入,看会不会出现一次约 5.5 秒的缺口;(c) 观察 Output 有没有周期性振荡。同时在 Input 和 Output 的处理回调里记录调用间隙 | P0                 |
| P-3  | Input 之前挂高延迟插件:Output 放在 Aux 上,实时和离线 Bounce 各一次,Windows 先做,mac 待 macOS 移植完成后再做;同时看缺口计数、这条轨有没有声音、null 对比 | P0                 |
| P-4  | 切换录音布防和输入监听、改缓冲大小、44.1/48/96 kHz                             | P0                 |
| P-11 | 插件菜单里没有 Multi-Mono 和 AudioSuite 版本                                   | P0                 |
| P-5  | 离线和实时 Bounce 的 null 对比                                                 | P1(Developer 版能不能导出不确定) |
| P-7  | Latch、Write、Loop 分别打印 30 条车道                                          | P1                 |
| P-7b | 只打印一个局部区间,看区间外的车道有没有被平线覆盖                             | P1                 |
| P-8  | 保存后重开;只采集、不改参数时,关闭工程会不会提示保存                           | P1(签名版 + 零售版或 NFR) |
| P-9  | 键盘和 DPI                                                                     | P1                 |
| P-12 | 签过名的构建能在零售版里加载                                                   | 发布前             |

P 系列在调研原稿中共 12 项;上表只列出了压缩后的主要项,**P-6 与 P-10 在压缩时略去**,补在这里:

- **P-6**:Track Freeze、Commit、Track Bounce 各对一条人声轨做一次,记录产物,确认原素材没丢(调研时未标优先级;关联 KI-4)。
- **P-10**:Mute / Solo 人声轨;停用 Output 插件;旁路 Output、旁路 Input;判据是与 KI-6 / KI-8 的描述一致(调研时未标优先级)。

Windows 一轮约 1.5–2 天,再加 1 天;mac 一轮要等 macOS 移植完成之后才能做,不计入上面的 10–16 人日【推测】。

**工作量**【推测,人类工程师当量;由 agent 驱动的实际耗时通常更低】:只做 AAX Windows beta 约 **10–16 人日**,其中包含建议先做的两项前置修复(失败可见性与位置补偿判据);真机测完后每轮另留 2–5 人日修复。

**可复用的前序工作**【推测】:Avid/iLok 账号、PACE 资质、wraptool 签名脚本、打包脚本、Validator job、文档模板。如果先拿别的简单插件跑通认证,这些都可以直接给 SCVB 用;PACE 申请里没有提到 SCVB 时,要确认需不需要单独申请或单独建 Product 和 wrap configuration。

---

## 8. 费用与日历汇总

| 项                               | 一次性                                                                      | 年费或订阅                          | 必须?                                     | 等待                      | 标记                                 |
| -------------------------------- | --------------------------------------------------------------------------- | ----------------------------------- | ----------------------------------------- | ------------------------- | ------------------------------------ |
| Avid 账号和 AAX 开发者身份       | $0                                                                          | $0                                  | 必须                                      | 当天                      | 一手/二手                            |
| iLok 账号 + iLok License Manager | $0                                                                          | $0                                  | 必须                                      | 即时                      | 一手                                 |
| **iLok 3 USB**                   | **约 $45–60 + 国际运费**(官方店价 $49.95/$59.95;2026-10-03 Sweetwater $44.95/$48) | —                                   | **必须**(Developer 和签名都要)            | 物流【不确定】            | 要求是一手;价格是二手                |
| Pro Tools Developer              | $0                                                                          | $0                                  | 强烈建议                                  | 当天到 1 天;最坏 2–3 周   | 二手                                 |
| PACE 签名工具许可                | $0(JUCE README「provided free of charge by Avid」)                          | $0,一年一续                         | 公开发布必须                              | 2 天到 2 周以上;最坏批不下来 | 免费是准一手;续期和等待是二手        |
| Windows 签名证书                 | 自签 $0                                                                     | 正式 OV/EV 每年数百美元【推测】     | 必须有一张,自签可以(商用建议 EV)          | 当天                      | 一手(`dox/PTG:179`)                  |
| Pro Tools Intro / 30 天试用      | $0                                                                          | —                                   | 简单插件签名后的测试宿主;SCVB 不够用      | 即时                      | Intro 一手;试用二手                  |
| Pro Tools Artist                 | —                                                                           | $9.99/月 或 $99/年                  | SCVB 的 P-8 需要(NFR 不批时)              | 即时                      | 二手                                 |
| Apple Developer                  | —                                                                           | $99                                 | 可选(mac 公证)                            | 1–2 天【推测】            | 一手                                 |
| PACE 云签名                      | ?                                                                           | 约 $1000                            | 可选(CI 自动签)                           | 联系 PACE                 | 二手                                 |
| GitHub 托管 runner               | $0(公开仓库)                                                                | $0                                  | —                                         | —                         | 一手;org 级预算没核实                |

- **最低现金**:约 $45–60 加运费。
- **典型日历**:3–6 周(devauth → iLok → 录屏 → PACE)【推测】。
- **人力投入**:申请 2–3 小时;实测 1–1.5 天;每次发版签名 15–30 分钟(要有人插着 iLok)【推测】。

---

## 9. 未决问题与待实测项

**需要项目层面决定的事**(调研时没有决定):

0. **U13 重新裁定**:AAX 公开发布要求代码签名,与 CLAUDE.md U13(v1 不做代码签名)和「构建流水线不需要任何 secret」冲突,需用户先裁定(口径怎么改、凭据放哪里)。这是其余各项的前提。
1. 以公司名义申请,还是以个人名义?PACE 邮件里要不要主动写明 GPL 开源并直接发问(2.5 的建议是写)?
2. 凭据放哪里:本地手动签名(.pfx 加密放本机、口令运行时输入),还是上云签名(约 $1000/年、凭据进 GitHub secrets)?要先对照项目现有的凭据与流水线约束。
3. 先只发 AAX Windows,还是同时做 mac?mac 只出 arm64,还是出 universal(Pro Tools 仍然支持 Intel)?
4. `isBusesLayoutSupported` 只对 AAX 收紧,还是三种格式一起收紧(会改变 VST3 现有行为)?
5. AlwaysProcess:按 P-2 的结果定,还是现在直接开(SCVB 按 7.1 的推演应当开)?
6. `AAX_CATEGORY` 选 `Effect` 还是 `SoundField`?
7. Validator 放进 CI(要引入 secret),还是只在本机手动跑?
8. 零售版冒烟用哪台机器?Developer 和零售版大概率不能共存。
9. 要不要买 Apple Developer($99/年)给 mac 版 AAX 公证?

**事实上的不确定项**:

- Developer 能不能只用 iLok Cloud 激活;
- Developer 的轨道上限,以及能不能 Bounce 导出;
- Avid 会不会批 NFR;
- PACE 条款里有没有与 GPL 分发冲突的内容;wraptool 会不会注入专有代码;
- 2026 年 Fusion SDK 版 wraptool 的确切参数(会不会要 customer name/number 之类);
- Windows 上 wraptool 的 `--in` 应该指向 bundle 还是内层 DLL;
- mac 上能不能用 ad-hoc 身份签;`stapler` 能不能处理 `.aaxplugin`;
- Validator 能不能在 CI 上用;
- DPP 看的是输入静音还是输出静音;
- Aux 拿到的位置有没有补偿(P-3);
- 离线 Bounce 期间 JUCE 的消息线程还跑不跑;
- WebView2 在 Pro Tools 插件窗口里的焦点、DPI 和界面显示时机表现;
- Pro Tools Intro 的 Windows 版可用性;Developer 和零售版能不能共存;
- iLok 的国内购买渠道和运费。

**待实测**:SCVB 的 P 系列(7.6)。

---

## 10. 参考链接

**官方和一手来源**:

- JUCE README 的 AAX 段:[JUCE 8.0.8 README](https://github.com/juce-framework/JUCE/blob/8.0.8/README.md)
- AAX SDK 2.9 文档镜像:[mgz0227/sonobus](https://github.com/mgz0227/sonobus/tree/sonobus8/SDK/aax/Documentation/Doxygen/dox)(`AAX_Getting_Started_Guide.doxygen:108-127`;`AAX_Pro_Tools_Guide.doxygen:126-190,223-231,795-809`;`AAX_DistributingYourPlugIn.doxygen:109-129`;`DSH_Guide.doxygen:66-68`)
- Avid AAX 开发者页:[developer.avid.com/aax](https://developer.avid.com/aax/)
- Pro Tools Intro 介绍页:[avid.com](https://www.avid.com/resource-center/introducing-pro-tools-intro)
- PACE AAX 签名入门(2026-07-13):[paceap.com](https://paceap.com/getting-started-with-aax-code-signing-for-pro-tools-plugins/)
- PACE 云签名「Cloud 2 Cloud」:[paceap.com](https://paceap.com/cloud-aax-code-signing-with-ci-build-systems/)
- PACE 文档站(要登录):`https://docs.paceap.com/`
- Pro Tools 系统要求:[kb.avid.com](https://kb.avid.com/pkb/articles/compatibility/Pro-Tools-System-Requirements)
- FSF GPL FAQ:`https://www.gnu.org/licenses/gpl-faq.html#GiveUpKeys`
- Apple:[Developer ID](https://developer.apple.com/developer-id/);[macOS 15 放行方式变化](https://developer.apple.com/news/?id=saqachfa);[会员对比](https://developer.apple.com/support/compare-memberships/)
- GitHub 依赖缓存的安全说明:[docs.github.com](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)

**二手来源和先例**:

- note.com(2026-03 的申请过程):[note.com 上的申请记录](https://note.com/kawato3/n/ne11473420ad5)
- JUCE 论坛:
  - [69201](https://forum.juce.com/t/releasing-aax-plugin-no-response-from-avid-tips/69201)(#12 是 2026-10-03 的完整流程)
  - [69239](https://forum.juce.com/t/anyone-got-through-to-avid-recently-pro-tools-developer-aax-signing/69239)
  - [48418](https://forum.juce.com/t/aax-plugin-and-package-builder-in-mac/48418)
  - [44472](https://forum.juce.com/t/aax-plugins-host/44472)
  - [62439](https://forum.juce.com/t/webview-and-keyboard-input-propagation-issue-to-the-host/62439)
- KVR:[540054](https://www.kvraudio.com/forum/viewtopic.php?t=540054);[532850](https://www.kvraudio.com/forum/viewtopic.php?t=532850);[601298](https://www.kvraudio.com/forum/viewtopic.php?t=601298)
- iLok 零售价:[USB-A](https://www.sweetwater.com/store/detail/iLok3--pace-ilok-3rd-generation);[USB-C](https://www.sweetwater.com/store/detail/iLok3USBC--pace-ilok-3-usb-c)
- DPP 实测:[kirin_hypha PR 54](https://github.com/heyalohaloha/kirin_hypha/pull/54)
- wraptool 命令先例:
  - [Venn Audio](https://www.vennaudio.com/how-we-incorporate-paces-cloud-signing-tools-into-our-build-pipeline-for-free-suite/)
  - [AnalogTapeModel](https://github.com/jatinchowdhury18/AnalogTapeModel)(`Scripts/aax_builds.sh:84-111`)
  - [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin)(`NeuralAmpModeler/scripts/makedist-win.bat:94-95`,注释模板;`makedist-mac.sh:222`)
  - [Dplug](https://github.com/AuburnSounds/Dplug)(`tools/dplug-build/source/main.d:603-651,1219`)及 [Dplug AAX Guide](https://github.com/AuburnSounds/Dplug/wiki/Dplug-AAX-Guide)
- Validator CI 先例:[wrac-plugin-template](https://github.com/novonotes/wrac-plugin-template)(`.github/workflows/ci.yml:166-263`,运行 27263728219)
- 开源 AAX 先例:[ADLplug-Next](https://github.com/yumasansansan/ADLplug-Next);[CtrlrX](https://github.com/damiensellier/CtrlrX);[Surge FAQ](https://surge-synthesizer.github.io/faq/)
- moonbase 2025 签名综述:[moonbase.sh](https://moonbase.sh/articles/code-signing-audio-plugins-in-2025-a-round-up/)
