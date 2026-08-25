[English](README.md) | **简体中文**

[![License](https://img.shields.io/github/license/synchain-oss/scvb?style=flat-square)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/synchain-oss/scvb/build-vst3.yml?branch=dev&style=flat-square&label=build)](../../actions)
[![pluginval](https://img.shields.io/badge/pluginval-strictness%205-brightgreen?style=flat-square)](https://github.com/Tracktion/pluginval)
[![Release](https://img.shields.io/github/v/release/synchain-oss/scvb?style=flat-square)](../../releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%C2%B7%20VST3-blue?style=flat-square)](#系统要求)

# SCVB — Synchain Vocal Balancer

> 多人多声部人声的自动声像与音量平衡,以一对配套 VST3 插件的形式落地。

## 它解决什么问题

- **平衡的是整个人声声部,不是单轨。** SCVB 采集每一条人声轨,放在一起分析,再给每轨一个声像位置和一条音量曲线,让各声部彼此错开,而不是挤在同一个位置互相打架。
- **两个插件,一套系统。** **SCVB Input** 插在每条人声轨上负责采集;**SCVB Output** 插在人声总线上,负责分析、平衡、求和,并替换总线输入。
- **结果以自动化的形式写回 DAW。** Write / Latch 走一遍打印 30 条(15 轨 × pan/vol),此后宿主恒权威 —— 你在 DAW 里手画的东西,引擎不会再抢回去。
- **改分析参数不需要重新采集。** 存下来的是每 10ms 一帧的连续特征而不是判定结果,所以 VAD 阈值与分段参数随时可拖、实时预览。
- **手改的东西不会被冲掉。** 手动编辑或锁定过的段,自动重分析一律不覆盖;对某个维度「冻结」则等于把它永久交到你手上。

每组最多 15 轨人声,8 个互相独立的组(A–H),2 个版本槽。自动化参数面冻结在 123 个(宿主可见 124),其余一切走 state。

## 截图

截图随首个正式发布版一并提供。在此之前,界面逐 tab 的说明见[用户手册](docs/USER_GUIDE.zh-CN.md)。

## 系统要求

- Windows 10 1809+ 或 Windows 11,x64
- 任一 VST3 宿主
- WebView2 Evergreen Runtime,用于编辑器 UI(Windows 通常已预装)

## 支持的 DAW

<!-- 本表转贴自 docs/DAW_COMPATIBILITY.md §4(该节标题即「README 支持等级表(供 T39b 转贴)」)。
     真源在那一节:改等级只改那里,再同步回本表与 README.md 的对等表。 -->

转贴自 [docs/DAW_COMPATIBILITY.md](docs/DAW_COMPATIBILITY.md) §4,该节始终是本表的真源。Tier 1 = 完全支持 / Tier 2 = 有限制 / Tier 3 = 不支持。

| DAW | 版本 | 支持等级 | 状态与已知限制 |
|---|---|---|---|
| Cubase | 14 / 15 | **Tier 1(主测)** | S1 路由(实时/离线/state)已实测通过;自动化写入待 S2 上机(RD-01 已知风险);自动化藏 Ins 隐藏车道;Input 须在 pre-fader 区最后一格 |
| REAPER | 7(建议装) | **Tier 1(附条件)** | S1 路由(实时/离线)已实测通过;关 GUI 可能不写自动化(需 process all notifications);同机单工程限制 |
| Ableton Live | 12 | **Tier 1(附条件)** | 128 参数上限(124 口径,余 4);Re-Enable Automation 需点击;S1/S2 待上机 |
| Studio One | 6 | **Tier 1(附条件)** | 自动化模式须在插件窗口内设 Write/Latch;Dropout Protection 异 block size;S1/S2 待上机 |

> **支持等级说明**:v1 首发前「Tier 1(附条件)」中的「附条件」将在 S2 自动化上机回填后收敛为定版(可能降 Tier 2);FL Studio 不在 v1 支持矩阵内。

## 安装

SCVB 尚未发布正式版本。发布之后,安装步骤是:

1. 从 Releases 页下载 `SCVB-vX.Y.Z-win64.zip`,并用随附的 `.sha256` 校验;
2. 解压,把 `SCVB Input.vst3` 与 `SCVB Output.vst3` **两个完整的 bundle 文件夹**都复制到 `C:\Program Files\Common Files\VST3\`;
3. 在 DAW 里重新扫描插件。

**两个都要装。** 这两个插件是一对,共用同一个版本号;版本不匹配时它们会**拒绝连接**,这是刻意的。

在有正式发布之前,请从源码构建(见下)。

## 快速上手

新建一条立体声人声总线,把所有人声轨输出指向它。在每条人声轨插件链的**最后一格**放一个 SCVB Input,在总线的**第一格**放一个 SCVB Output。给每个 Input 选一个 channel id,然后依次采集、分析、打开输出。完整流程见[用户手册](docs/USER_GUIDE.zh-CN.md)的「5 分钟上手」。

动手之前先读这几条。违反其中任何一条,得到的不是「效果差一点」,而是直接坏掉:

<!-- BEGIN GENERATED hard-rules:zh -->
> ⚠️ **必读:SCVB 的九条使用规则,违反其中任何一条都会导致静音、错音或分析失效。**
>
> 1. **人声轨必须保持 DAW 原有路由,指向 SCVB Output 所在的总线。** 不要把人声轨改成直接送主输出,也不要绕开总线。(ADR-002)
> 2. **SCVB Input 必须插在人声轨插件链的最后一格;SCVB Output 必须插在总线的第一格。** 位置不对会破坏 DAW 的处理顺序假设;各宿主对这一格的具体叫法见 `docs/DAW_COMPATIBILITY.md`。(ADR-002 / J45)
> 3. **只有在检测到健康的 SCVB Output 时,Input 才会向下游输出静音——这是设计行为,不是 bug。** 这条静音通路保住了 DAW 依赖图里"先人声轨、后总线"的排序,离线渲染与 REAPER 的预测性多线程下依然成立。**检测不到健康 Output 时(未装、未连上、对端已退出),Input 自动切回直通**,80ms ramp 过渡、5 秒滞回防抖(滞回只作用于"静音 → 直通"方向;"直通 → 静音"在确认健康后立即 80ms ramp),所以你不会因为只装了一个插件就得到一条哑轨。(ADR-002 / J12 + J32)
> 4. **人声轨与总线的宿主 pan 必须保持居中。** SCVB 内部用 equal-power pan,与宿主 pan law 无关;宿主 pan 不居中会叠加出错误声像。(ADR-010)
> 5. **每个 channel id 在同一个组内唯一,而同一条人声轨只能属于一个组。** 同组内两个 Input 抢同一个 channel 时,后来者会看到"channel 冲突"警告并且不会生效;不同组的同号 channel 是两条互不相干的通路。(ADR-002 / J66)
> 6. **同一个组同一时间只能有一个生效的 Output 实例。** 同组的第二个实例进入只读观察模式并显示警告;八个组(A–H)各自是独立的总线域,互不影响。(ADR-002 / J66)
> 7. **stereo 人声轨默认不参与自动声像分配,需要它参与时必须手动打开。** mono 源经 equal-power pan 摆位;stereo 源走 dual-pan + width 模型(pan = 弧中心,width = 张开度),默认保留你已有的声像宽度,不会被自动分配改写。(ADR-003 / J57 + J60)
> 8. **SCVB Output 不向 DAW 报告额外延迟。** 对齐靠时间线寻址完成,不要试图用 PDC(延迟补偿)去"修正"它。(ADR-002)
> 9. **看到"时间线缺口 / 重叠"警告时,不要继续导出。** 先按 `docs/DAW_COMPATIBILITY.md` 的通用坑清单排查路由,警告计数不归零就说明有轨的音频没被正确接管。
<!-- END GENERATED hard-rules:zh -->

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

[GPL-3.0-or-later](LICENSE);JUCE 与 VST3 SDK 的依赖声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

## 相关项目

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) —— 把 DAW 音频桥接到浏览器的 VST3 插件
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) —— `@synchain/cli` 命令行客户端
- [synchain.ca](https://synchain.ca) —— 项目官网
