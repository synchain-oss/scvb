[English](README.md) | **简体中文**

[![License](https://img.shields.io/github/license/synchain-oss/scvb?style=flat-square)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/synchain-oss/scvb/build-vst3.yml?branch=dev&style=flat-square&label=build)](https://github.com/synchain-oss/scvb/actions)
[![pluginval](https://img.shields.io/badge/pluginval-strictness%205-brightgreen?style=flat-square)](https://github.com/Tracktion/pluginval)
[![Release](https://img.shields.io/github/v/release/synchain-oss/scvb?include_prereleases&style=flat-square)](https://github.com/synchain-oss/scvb/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%C2%B7%20VST3-blue?style=flat-square)](#系统要求)

# SCVB — Synchain Vocal Balancer

> 多人多声部人声的自动声像与音量平衡,以一对配套 VST3 插件的形式落地。

SCVB 是由 [Synchain](https://synchain.ca) 主导的开源插件项目,源码与文档完全公开,欢迎在 [GPL-3.0-or-later](LICENSE) 的条款下自由使用、修改与分发。如果它帮你省下了时间,也欢迎去 [synchain.ca](https://synchain.ca) 看看我们的其他产品 —— 用得顺手的话,把 Synchain 推荐给你的朋友和同事,就是对我们最好的支持。

## 它解决什么问题

混音师经常需要花大量时间,为几十轨不同人的主人声和和声画音量与声像自动化:让人声在空间上不要挤在一起,或者确保多个演唱者的唱段之间响度听上去一致。这些工作非常花费时间和精力,大多枯燥无味、重复性强,还会显著加重腱鞘炎风险。

SCVB 正是为了解决这个问题、节约大家的时间而设计:它采集每一条人声轨,放在一起分析,再给每轨一条声像位置曲线和一条音量曲线,让各声部彼此错开而不是挤在同一个位置互相打架,并确保段落之间的响度接近一致。

**两个插件,一套系统。** **SCVB Input** 插在每条人声轨上负责采集;**SCVB Output** 插在人声总线上,负责分析、平衡、求和,并替换总线输入。另有一个可选的第三件 **SCVB Monitor**:只读窗口,用来看整组的声像与音量变化。界面逐 tab 的说明见[用户手册](docs/USER_GUIDE.zh-CN.md)。

觉得引擎分析的效果一般、跟你想象中的安排不符,想要微调?没问题!你可以参考 Waves 的 Vocal Rider 那种「先打印自动化、再手工微调」的工作流,使用自动化写入功能把引擎的分析结果写成自动化记录到宿主中,再在宿主里基于这些结果微调 —— 相信这能省下你不少时间。

每组最多 15 轨人声,8 个互相独立的组(A–H),2 个版本槽。自动化参数面冻结在 123 个(宿主可见 124),其余一切走 state。

## 系统要求

- Windows 10 1809+ 或 Windows 11,x64
- 任一 VST3 宿主
- WebView2 Evergreen Runtime,用于编辑器 UI(Windows 通常已预装)

## 支持的 DAW

<!-- 本表转贴自 docs/DAW_COMPATIBILITY.md §4(该节标题即「README 支持等级表(供 T39b 转贴)」)。
     真源在那一节:改等级只改那里,再同步回本表与 README.md 的对等表。 -->

转贴自 [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md) §4,该节始终是本表的真源。Tier 1 = 完全支持 / Tier 2 = 有限制(有你自己能做的规避办法)/ Tier 3 = 未验证或不支持。

| DAW | 版本 | 支持等级 | 状态与已知限制 |
|---|---|---|---|
| Cubase | 14 / 15 | **Tier 1(主测)** | 路由(实时/离线)、存工程重开、自动化写入已用成品插件真机实测通过(Cubase 15 Pro);Cubase 14 在路由 spike 阶段验过路由;自动化藏 Ins 隐藏车道;Input 须在 pre-fader 区最后一格 |
| REAPER | 7 | **Tier 2(部分验证)** | 只在路由 spike 阶段验过路由(实时/离线);成品插件与自动化写入尚未在 REAPER 上测过;关 GUI 可能不写自动化(需 process all notifications;插件界面会提示保持窗口打开);同机单工程限制 |
| Ableton Live | 12 | **Tier 3(未验证)** | 尚未真机测试。设计上已知:128 参数上限(本插件占 124,余 4);Re-Enable Automation 需点击(写入结束时插件界面会提示);停用 Output 设备后约 5.5 秒无声,之后人声转直通 |
| Studio One | 6 | **Tier 3(未验证)** | 尚未真机测试。设计上已知:自动化模式须在插件窗口内设 Write/Latch;Dropout Protection 会改变 block size |

> **支持等级说明**:未验证的宿主不代表不能用(插件是标准 VST3),只是还没有人在真机上确认过;等级只在真机实测之后才上调。FL Studio 不在 v1 支持矩阵内。

## 安装

正式版本发布在本仓库的 [Releases 页](https://github.com/synchain-oss/scvb/releases)。如果那里还没有任何版本,请从源码构建(见下)。

1. 从 Releases 页下载 `SCVB-v<版本号>-win64.zip` 与对应的 `.sha256`;
2. 用 `.sha256` 校验下载到的 zip。**权威校验值以 GitHub Release 正文里的 SHA-256 为准**(它由 CI 在构建时产出),两处应当一致;**对不上就不要安装,并告诉我们**;
3. **SCVB 没有做代码签名。** 浏览器或 Windows 可能提示「未知发布者」;解压之前,右键 zip → **属性** → 勾选 **解除锁定** → **确定**。分步说明见[用户手册](docs/USER_GUIDE.zh-CN.md#安装);
4. 解压,把 `SCVB Input.vst3`、`SCVB Output.vst3` 与(可选的)`SCVB Monitor.vst3` **整个 bundle 文件夹**复制到 `C:\Program Files\Common Files\VST3\`;
5. 在 DAW 里重新扫描插件。

**Input 与 Output 都要装。** 这两个插件是一对,共用同一个版本号。**升级时所有 SCVB 插件一起升**:两侧的共享内存协议版本不同时,它们会**拒绝连接**,这是刻意的;即便能连上,混装不同版本也不受支持(新 Output 配旧 Input 时,离线渲染的人声可能被叠加两次)。

**SCVB Monitor 是可选的。** 它是一个只读的旁观窗口,用来看整组的声像运动与分布。它原样直通音频,**没有任何自动化参数**,只读共享数据 —— 从不占用 slot、从不写任何共享段,所以装不装它都不会改变 Input 与 Output 的行为。

## 快速上手

新建一条立体声人声总线,把所有人声轨输出指向它。在每条人声轨插件链的**最后一格**放一个 SCVB Input,在总线的**第一格**放一个 SCVB Output。给每个 Input 选一个 channel id,然后依次采集、分析、打开输出。完整流程见[用户手册](docs/USER_GUIDE.zh-CN.md)的「5 分钟上手」。

动手之前先读这几条。违反其中任何一条,得到的不是「效果差一点」,而是直接坏掉:

<!-- BEGIN GENERATED hard-rules:zh -->
> ⚠️ **必读:SCVB 的九条使用规则,违反其中任何一条都会导致静音、声像位置错误或分析失效。**
>
> 1. **人声轨必须保持 DAW 原有路由,指向 SCVB Output 所在的总线。** 不要把人声轨改成直接送主输出,也不要绕开总线。(ADR-002)
> 2. **SCVB Input 必须插在人声轨插件链的最后一格;SCVB Output 必须插在总线的第一格。** 位置不对会破坏 DAW 的处理顺序假设;各宿主对这一格的具体叫法见 `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`。(ADR-002 / J45)
> 3. **只有在检测到健康的 SCVB Output 时,Input 才会向下游输出静音——这是设计行为,不是 bug。** 这条静音通路保住了 DAW 依赖图里"先人声轨、后总线"的排序,离线渲染与 REAPER 的预测性多线程下依然成立。**检测不到健康 Output 时(未装、未连上、对端已退出),Input 自动切回直通**,80ms ramp 过渡、5 秒滞回防抖(滞回只作用于"静音 → 直通"方向;"直通 → 静音"在确认健康后立即 80ms ramp),所以你不会因为只装了一个插件就得到一条没有声音的轨道。(ADR-002 / J12 + J32)
> 4. **人声轨与总线的宿主 pan 必须保持居中。** SCVB 内部用 equal-power pan,与宿主 pan law 无关;宿主 pan 不居中会叠加出错误声像。(ADR-010)
> 5. **每个 channel id 在同一个组内唯一,而同一条人声轨只能属于一个组。** 同组内两个 Input 抢同一个 channel 时,后来者会看到"channel 冲突"警告并且不会生效;不同组的同号 channel 是两条互不相干的通路。(ADR-002 / J66)
> 6. **同一个组同一时间只能有一个生效的 Output 实例。** 同组的第二个实例进入只读观察模式并显示警告;八个组(A–H)各自是独立的总线域,互不影响。(ADR-002 / J66)
> 7. **所有轨默认参与自动声像;立体声轨如需保留原有声像宽度与位置,请在轨道页关闭该轨的「参与自动声像」。** mono 源经 equal-power pan 摆位;stereo 源走 dual-pan + width 模型(pan = 弧中心,width = 张开度),关闭参与后保留你已有的声像宽度,不会被自动分配改写。(ADR-003 / J57 + J83)
> 8. **SCVB Output 不向 DAW 报告额外延迟。** 对齐靠时间线寻址完成,不要试图用 PDC(延迟补偿)去"修正"它。(ADR-002)
> 9. **看到"时间线缺口 / 重叠"警告时,不要继续导出。** 先按 `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md` 的通用坑清单排查路由,警告计数不归零就说明有轨的音频没被正确接管。
<!-- END GENERATED hard-rules:zh -->

> ⚠️ **别单独渲染装了 SCVB Input 的人声轨。** 对单条人声轨做就地渲染(Render in Place)、冻结(Freeze)或单轨导出,得到的是静音文件;如果选了「替换原音频」,原素材会被换成静音。请对人声总线整体导出;确实需要单轨素材,先旁路或移除该轨的 SCVB Input 再渲染。详见[用户手册「导出与渲染」](docs/USER_GUIDE.zh-CN.md#导出与渲染)。

## 隐私

SCVB 不联网:没有更新检查、没有使用统计、没有账号。它只会在你点击时,用默认浏览器打开说明文档和 WebView2 下载这两类网页。它在 `%APPDATA%\Synchain\SCVB` 下存几项跨工程的偏好,在 `%LOCALAPPDATA%\Synchain\SCVB` 下存界面的浏览器缓存;除了你自己导出的文件,其余都存在你的工程里。完整清单见[用户手册「隐私与本机文件」](docs/USER_GUIDE.zh-CN.md)。

## 从源码构建

```powershell
git clone https://github.com/synchain-oss/scvb.git
cd scvb
pwsh scripts/build.ps1 -JucePath C:\path\to\JUCE
```

完整工具链见 [CLAUDE.md](CLAUDE.md) §6;本地质量门禁跑 `pwsh scripts/gates.ps1`。

## 文档

- [用户手册](docs/USER_GUIDE.zh-CN.md) —— 安装、工作流、故障排查、FAQ
- [已知限制](docs/KNOWN_ISSUES.md) —— v1 已裁定接受的限制
- [发布流程](docs/RELEASE.md) —— 版本号、tag、发布说明
- 契约与架构在 `docs/` 下:`PARAMETERS.md`、`IPC_CONTRACT.md`、`STATE_SCHEMA.md`、`SCVB_CONTRACT.md`
- 逐宿主的说明在 [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md)
- 宪法文档的只读副本在 `docs/constitution/`

## 贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md) 与[行为准则](CODE_OF_CONDUCT.md)。所有 commit 必须签署(`git commit -s`)。安全问题请走 [SECURITY.md](SECURITY.md),不要开公开 issue。

九条硬约束有**唯一真源**:[docs/USER_GUIDE.zh-CN.md](docs/USER_GUIDE.zh-CN.md) 的 `## 硬约束` 小节,译文在 `docs/hard-rules.i18n.json`。**中文为语义权威**,翻译有歧义时以中文为准。任何其他位置都不要改红字 —— 改真源,跑 `node scripts/gen-hard-rules.mjs`,其余六处随生成物更新。

## 许可证

本仓源码采用 [GPL-3.0-or-later](LICENSE)。发布的 `.vst3` 二进制里还编进了只按 GPLv3 授权和按 AGPLv3 授权的组件,所以二进制整体按 GPLv3 分发(说明见 THIRD-PARTY-NOTICES.md)。编进插件的第三方组件(JUCE、VST3 SDK、JUCE 编进来的各个库、WebView2 loader、字体)及其许可证见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md),许可证全文在 `LICENSES/`。

## 相关项目

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) —— 把 DAW 音频桥接到浏览器的 VST3 插件
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) —— `@synchain/cli` 命令行客户端
- [synchain.ca](https://synchain.ca) —— 项目官网
