# LUNA 轻验收包(macOS beta,约 1 小时)

> 状态:演进中(验收前的模板;结果由统筹脱敏后另存为 `docs/validation/luna-acceptance-<日期>.md`)
> 最后更新:2026-10-11
> 适用:SCVB macOS beta(Apple Silicon、arm64、VST3 + Audio Unit,最低 macOS 11)在 LUNA 里的一次真机验收。
> 口径:本文里所有关于 LUNA 行为的话都是**设计上的预期**,验收之前没有一条是实测结论。Logic Pro 不在本次验收范围内(beta / 未在 Logic 实测,Tier 3)。

## 0. 这份包是什么、不是什么

- **是**:一个人在一台 Apple Silicon Mac 上、约 1 小时内能照着做完的清单,外加要带回的材料和回收表模板。
- **不是**:对 LUNA 的"支持声明"。结果要由统筹脱敏后再回填到 [DAW_COMPATIBILITY.md](../DAW_COMPATIBILITY.md) 的 LUNA 节和 [daw-support-tiers.md](daw-support-tiers.md)(按其 §0 四处同改)。
- **不做**:Pro Tools(不在 macOS beta 范围内)、GarageBand(不支持)、Logic(未在 Logic 实测)。
- 统筹会先把不需要 DAW 的项目测完(zip 内容、哈希、统筹机器上的 `auval`),这些不派给验收人。验收人在自己的机器上再跑一次 `shasum` 和 `auval`(§2),用来确认本机的 AU 登记正常,不是重复统筹的工作。

## 1. 测什么包、用什么素材

- **包**:统筹提供的 macOS zip(`SCVB-v<版本>-macos-arm64.zip`)和同名 `.sha256`。
- **测试素材**:由统筹提供 **4 条不同音高的测试音频**(在仓库之外生成,不入库;验收人不需要自己生成)。4 条音高不同,是为了在总线上能分辨"每一条轨都在"。
- 收到包后先把 zip、`.sha256` 和 4 条音频放在同一个文件夹,下面的命令都从那个文件夹执行。

## 2. 安装(约 10 分钟,前一天做也可以)

**0. 先记下环境**(写进 §8 回收表的"环境"一栏):

```
sw_vers
uname -m
```

另记:Safari 版本(Safari > 关于 Safari)、LUNA 版本和授权状态、音频接口型号。**如果接口是 Apollo,要切到 Core Audio 模式,或者关掉 ARM**(设计上预期 ARM 模式会改变插件的调用方式;未实测)。

**1. 安装。以 zip 里的 `INSTALL.txt` 为准**;下面是它的形态(`<版本>` 换成包的版本号)。每一步都用 `ditto`,不要用 `cp -r`:

```
shasum -a 256 -c SCVB-v<版本>-macos-arm64.zip.sha256
ditto -x -k SCVB-v<版本>-macos-arm64.zip SCVB-v<版本>-macos-arm64 && cd SCVB-v<版本>-macos-arm64
rm -rf ~/Library/Audio/Plug-Ins/VST3/SCVB\ {Input,Output,Monitor}.vst3
rm -rf ~/Library/Audio/Plug-Ins/Components/SCVB\ {Input,Output,Monitor}.component
for b in SCVB\ *.vst3; do ditto "$b" ~/Library/Audio/Plug-Ins/VST3/"$b"; done
for b in SCVB\ *.component; do ditto "$b" ~/Library/Audio/Plug-Ins/Components/"$b"; done
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/SCVB\ *.vst3
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/SCVB\ *.component
killall -9 AudioComponentRegistrar
```

- `shasum` 必须输出 `OK`;对不上就停下,告诉统筹。
- `killall` 提示 `No matching processes` 可以忽略。
- 这个 beta **没有签名也没有公证**,所以 `xattr` 那两行不能省。

**2. 验证三个 Audio Unit**(命令与 `INSTALL.txt` 第 7 步相同,只是多截取了末三行(`| tail -3`);每条都应以 `AU VALIDATION SUCCEEDED` 结尾)。把三条的最后几行保存下来,是要带回的材料之一:

```
auval -v aufx Scvi Snch | tail -3
auval -v aufx Scvo Snch | tail -3
auval -v aufx Scvm Snch | tail -3
```

**3. 让 LUNA 扫描**:打开 LUNA,Plug-In Manager,Scan New(或 Rescan All)。三个 SCVB Audio Unit 的状态应是 **scanned**,不是 rejected。LUNA 在 macOS 上默认启用 AU、默认不启用 VST3;本次验收**用 AU**,所以不用动 VST3 的开关。

**4. 可选(约 5 分钟,若随包提供)**:`scvb_auhost --oop` 探针。它在系统的 AUHostingService 上检查进程外共享内存,不需要 Logic。用它之前同样要对它执行 `xattr -dr com.apple.quarantine`。是否随包提供、命令的最终形态由统筹之后回填;没有随包就跳过这一步,不算失败。

## 3. 工程设置

- 采样率 **48 kHz**,buffer **128**。
- **4 条人声轨**:2 条 mono、1 条 stereo、1 条"mono 输入接 stereo 轨"(可选)。每条轨放一条测试音频。
- 建一条 **Stereo Bus**,**不要用 Folder 轨**(设计上预期 Output 放在 Folder 轨上收不到预期的路由;未实测)。把 4 条人声轨的输出都送到这条 Bus。
- 每条人声轨:在 insert 1–4 中**最后一个已用的槽位**放 **SCVB Input(AU)**。**不要放进 Record FX**(设计上预期 Input 在 Record FX 里不会按预期工作;未实测)。
- Bus 的 **Insert 1** 放 **SCVB Output(AU)**。
- 再在任意一条轨上放一个 **SCVB Monitor(AU)**,验收期间**一直开着它的窗口**(§6 的刷新率日志靠它;关着时推送计数恒为 0)。
- 给每个 Input 分配 channel(1–4),捕获、分析,打开 Output 的输出开关(第一次要在确认条上点"开始")。
- 宿主 pan 全部居中(硬约束第 4 条)。

## 4. 验收项

级别:**P0** = 挡发版(其中 T4 / T5 失败可经所有者签字降级为 KI,见 §4.2);**P1** = 如实记录,由统筹定处置(写进 KNOWN_ISSUES 还是修);**P2** = 只记录。

| 级别 | 项 | 操作 | 通过标准 | 要带回的东西 |
|---|---|---|---|---|
| P0 | T1 | 从头播放 60 秒 | 4 条轨全部显示已连接;gapCount 为 0;4 个音高都听得到;轨名自动填入 | Output 状态截图 |
| P0 | T2 | 4 小节循环 ×10,播放中往回、往前各定位 5 次 | 每一圈 4 条轨都在 | 截图 |
| P0 | T3 | 循环开着,给人声轨 A 的输入监听或 arm 开关 5 次 | 每一圈都不出现某条轨整圈无声;读不到时必须亮告警("路由失准") | 截图,并注明是否亮了告警 |
| P0 | T4 | Export Mixdown,勾选和不勾选 Real-Time 各导一次 | 两个文件都不是静音,开头没有静音窗,时长没有被拉长 | **两个 WAV**(统筹用 `scvb_nulltest` 对比);**只记录导出的时长**,AU tail 策略本版没做(M14 推迟) |
| P0 | T5 | Bus 设为 **Latch**(LUNA 没有 Write),全曲打印一次;再只打印一个局部区间 | 车道有曲线;区间外**不被平线覆盖** | 车道截图 |
| P0 | T6 | 保存,退出 LUNA,重新打开 | 分析结果和各项设置都还在 | — |
| P1 | UI | Output 编辑器开关 5 次;在 125% 和 150% 缩放下点击、拖拽波形和轨迹;给版本改名;⌘Z;编辑器有焦点时按空格 | 开窗不卡在占位、不白闪;命中位置不偏(Safari 低于 26.4 时应该被锁在 100%);空格是否漏给 LUNA 的走带(只记录现象) | 诊断行截图(含 zoom 模式);见下面 §4.1 |
| P1 | T7 | Output 用 **IN**(软旁路)和 **Power**(硬停用)各停 10 秒 | IN:直通,没有咔哒声;Power:记录是否出现约 5.5 秒无声(KI-6) | — |
| P2 | T8 | 对一条人声轨做 Bounce(选 **Keep**) | 预期得到静音(KI-4),源轨还在 | — |

### 4.1 P1 UI 一项里并入的三件事(M07 留给 LUNA 的)

1. **遮挡闸关闭后有无白闪**:macOS 上 Windows 用的"遮挡闸"是关闭的(KI-18)。开窗 5 次,记录第一帧前有没有白色一闪。
2. **日志里 `reveal gate off` 的行数**:打开 `~/Library/Logs/Synchain/SCVB/output.log`(和 `input.log`),数 `first-frame signal after … ms (reveal gate off)` 这种行有几行。开窗 5 次,预期每次一行;只记数字和有没有缺。
3. **日志落盘**:这三个文件在跑过之后确实存在、有内容。不存在或为空,本身就是一条结论,记下来。

键盘焦点(空格)、缩放(KI-16)的观察也记在 UI 一项里,只写现象。

### 4.2 通过标准

- **T1、T2、T3、T6 任一不过 = 挡发版。**
- **T4、T5 失败**:不能直接当成已知限制放行。要经**所有者签字**,才能降级写进 KNOWN_ISSUES(一个 KI 条目);没有签字就同样挡发版。
- P1 的 UI、T7 失败或只是观察到现象:如实记录,由统筹决定写进 KNOWN_ISSUES 还是修。
- P2 的 T8:只记录。
- 拿不准的现象:拍照 / 截图记录,不要自己判通过。

## 5. 要带回的东西

- **两个 mixdown WAV**(T4)。
- `~/Library/Logs/Synchain/SCVB/` 整个文件夹的打包(含 `input.log`、`output.log`、`monitor.log`,以及满 1 MB 轮转出的 `*.1.log`)。
- `~/Library/Logs/Universal Audio/` 的打包。
- **活动监视器截图**:搜 `LUNA` 和 `AUHostingService`,看插件是不是在 LUNA 进程里运行。
- `auval` 输出(§2 第 2 步)。
- 各项截图(见 §4 表格)。
- 填好的回收表(§8)。

> **日志里可能有你的用户名**(形如 `/Users/<名字>/...` 的路径)。**不要把日志、截图原样贴进公开 PR 或 issue**,交给统筹脱敏之后再引用。截图同样不进公开仓,公开的只有脱敏后的文字结论。

## 6. M07b:viz 刷新率日志怎么看(带回 `output.log` 与 `monitor.log`)

这一节是给统筹判读用的,验收人只需要保证下面的前提成立,并把文件带回。

**带回的文件**:`~/Library/Logs/Synchain/SCVB/output.log` 与 `monitor.log`;若同目录还有 `output.1.log` / `monitor.1.log`(满 1 MB 轮转出的旧份),一并带回。

**前提**:Output 在跑(宿主音频引擎开着、Output 已 prepare);Monitor 窗口**开着**(关着时 push 恒为 0),所以验收期间请把一个 SCVB Monitor 放在任意轨上并打开它的窗口,保持开着至少几分钟。每个实例每约 10 秒写一行;Output 停用 / 引擎停了就不写(一行都没有本身也是信息:定时器没在跑)。

**Output 那一行**(示例,单测里的理想时钟值):

```
<时间戳> viz-rate output AU#1 group=1 writer=on window=10.0s tick=60.0Hz publish=30.0Hz tick-max=17ms publish-max=34ms
```

- `AU#1`:格式 + 本二进制里第几个实例;`group`:组号;`writer=on`:本实例是该组的 viz 写方(`off` 时 publish 必为 0,属正常,例如同组第二个 Output)。
- `tick`:viz 定时器实得回调频率,标称 60Hz;`publish`:实得发布率,标称 30Hz。
- `tick-max` / `publish-max`:窗口内相邻两次回调 / 两次发布的最大间隔,标称约 17 ms / 34 ms。

**Monitor 那一行**:

```
<时间戳> viz-rate monitor AU#1 group=1 seg=online editor=on window=10.0s tick=60.0Hz poll=60.0Hz fresh=30.0Hz push=25.0Hz tick-max=17ms poll-max=17ms fresh-max=34ms push-max=40ms
```

- `seg=online`:段已接上(`offline` = 该组没有 Output 在发);`editor=on`:Monitor 窗口开着。
- `tick` / `poll`:Monitor 定时器与读段,标称 60Hz;`fresh`:读到新帧,约等于 Output 的 publish(标称 30Hz);`push`:真正推给页面的帧,上限是 WebViewHost 的 25Hz tick。

**判读**(只看 Output 的 `writer=on`、Monitor 的 `seg=online editor=on` 那些行):

| 现象 | 判读 |
|---|---|
| Output `publish` ≥ 28Hz、`tick-max` ≤ 50 ms;Monitor `fresh` ≥ 28Hz、`push` ≥ 23Hz、`fresh-max` / `push-max` ≤ 100 ms | 正常:定时器没被拉长 |
| Output `publish` < 25Hz,或 `tick-max` > 100 ms | Output 的定时器被拉长 / 合并(CI 里看到的现象出现在真实宿主里) |
| Output 正常,但 Monitor `tick` / `poll` 远低于 60Hz,或 `poll-max` > 100 ms | Monitor 自己的定时器被拉长 |
| Monitor `fresh` 正常而 `push` < 20Hz,或 `push-max` > 100 ms | 编辑器的 25Hz tick 被拉长(页面看着会卡) |
| 偶发单行超标、其余行正常 | 记下时间戳,对照当时的操作(切窗口、拖动、导出等);连续多行超标才算 |
| 某一行 Output 的 `publish` 落在 0 与 30Hz 之间,而 `tick` / `tick-max` 正常 | 多半是窗口中途接管 / 让出了 viz 写方(例如同组第一个 Output 被移除、本实例接过段):`writer=` 只反映收窗那一刻。**不是**定时器被拉长;判定时器本身只看 `tick` / `tick-max` |
| 某一行 Monitor 的 `poll` / `fresh` / `push` 落在两档之间,而 `tick` / `tick-max` 正常 | 同理:窗口中途 Output 上线 / 掉线、或开关了 Monitor 窗口(`seg=` / `editor=` 只反映收窗那一刻)。换组不会这样(换组即重开一窗) |
| 带 `clock-back=N` 的行 | 那一窗之前的计数作废(单调时钟正常不会出现);照实记录 |

`window=` 明显大于 10.0s 本身也说明定时器被拉长(收窗那一拍晚到)。一行里各字段的数字只看同一行,不跨行相加。

此外请**用眼睛**看一下 Monitor 的播放头 / 车道动起来顺不顺滑,在回收表里记一句。

## 7. 现场遇到问题怎么办

- 某条轨整圈无声、告警没亮、导出静音:不要自己调参数重试,先截图、记下时间,再继续下一项。
- 宿主崩溃:记下是在哪一项、做了什么操作时崩的,重启后再带回 `~/Library/Logs/Synchain/SCVB/` 和系统崩溃报告(~/Library/Logs/DiagnosticReports/ 里与 LUNA 相关的)。崩溃本身就是一条结论,不用硬把后面的项做完。
- 插件扫描是 rejected:先确认 `xattr` 和 `killall` 都做过、再 Rescan;还是 rejected,把 `auval` 输出和 Plug-In Manager 的截图带回,停止。

## 8. 回收表(模板)

环境:

| 项 | 填写 |
|---|---|
| macOS(`sw_vers`) | |
| 芯片(`uname -m`) | |
| Safari 版本 | |
| LUNA 版本 / 授权 | |
| 音频接口型号 / 模式(Apollo:Core Audio 还是 ARM) | |
| 包的版本、`shasum` 结果 | |
| 验收日期 | |

结果:

| 项 | 结果(P 通过 / F 失败 / N 没做) | 现象和数字 | 附件(文件名) |
|---|---|---|---|
| 安装:`auval` ×3 | | | |
| 安装:LUNA 扫描三个 AU 都是 scanned | | | |
| T1 | | | |
| T2 | | | |
| T3(是否亮了告警) | | | |
| T4(两个文件的时长) | | | |
| T5(区间外有没有被平线覆盖) | | | |
| T6 | | | |
| UI:开窗卡占位 / 白闪 | | | |
| UI:`reveal gate off` 行数 | | | |
| UI:日志三个文件存在且有内容 | | | |
| UI:125% / 150% 命中位置、Safari 低于 26.4 是否锁 100% | | | |
| UI:编辑器有焦点时空格是否漏给走带 | | | |
| UI:Monitor 播放头 / 车道是否顺滑 | | | |
| T7:IN 软旁路 | | | |
| T7:Power 硬停用是否出现约 5.5 秒无声 | | | |
| T8:Bounce(Keep) | | | |
| 可选:`scvb_auhost --oop` | | | |

结论(统筹填):

| 项 | 内容 |
|---|---|
| T1 / T2 / T3 / T6 是否全过 | |
| T4 / T5 是否有失败,是否需要所有者签字降级 | |
| 新增的 KNOWN_ISSUES 条目 | |
| LUNA 的 Tier 回填 | |
