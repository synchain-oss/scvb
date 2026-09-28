# DAW 实测矩阵(发版层)

> 状态:演进中(每次发版追加一节,旧节不改)
> 最后更新:2026-09-28
> 来源:masterPlan 10 §3.2–§3.3。对外的支持等级与逐 DAW 操作说明以 [DAW_COMPATIBILITY.md](../DAW_COMPATIBILITY.md) 为准;本文件只记发版层的逐格实测。

## 口径

- 本文件是 10 §3.1 里的**发版层**:只跑 spike 验证过的场景与回归卡,不重复探索性步骤。每个版本新增一节 `## vX.Y.Z — <日期>`。
- 格子填 **P / F / N-A + 实测值**;**⏳** = 待上机;**—** = 本版不测(理由写在节内)。
- 每一节只认**该版本的发布包**上的结果。更早的 spike 与内部测试包结论列在节内「功能 × 测试包证据」里作参考,**不能拿来填格**。

### 场景代号(10 §3.2)

| 代号 | 场景 | 说明 |
| --- | --- | --- |
| RT | 实时播放 | 全曲一遍,buffer 512 |
| OFF | 离线导出 | 实时导出与快速导出分两格 |
| LOOP | 循环播放 | 4 小节循环 × 20 遍 |
| BLK | 可变 block | 32 / 128 / 512 / 2048 各 30 秒 |
| SR | 采样率 | 44.1 / 48 / 96 kHz |
| SEEK | 定位跳变 | 播放中拖播放头 20 次 |
| MUTE | 静音 / 独奏 | mute 5 轨、solo 1 轨 |
| FRZ | Freeze / stem | 冻结单轨、导出 stem(记录行为,不判 fail) |
| LIFE | 生命周期 | 强杀重开 / 复制实例 / 双 Output / channel 冲突 |
| AUTO | 自动化写入 | Write / Latch × GUI 开 / 关;回读一致 |
| STATE | 工程存取 | 满配 state 保存 / 重开 / 另存副本 |
| UI | 编辑器 | 打开 / 关闭 20 次;缩放四档;FPS 与内存 |

### 每格的判据(10 §3.3)

- RT / LOOP / BLK / SR / SEEK:`gapCount` 增量按 10 §1.1.3 的 S1-P3 / P4 / P5 / P6 阈值。
- OFF:与参考渲染的 null test,按 10 §4.2(即发版清单 D1 / D2)。
- MUTE / FRZ:记录行为,与上一版的记录**逐字比对**,行为变了即 F(说明宿主升级改了语义,要改文档)。
- LIFE:01 §4.3 的时序场景逐条。
- AUTO:10 §1.2.4 的 S2-P1..P7。
- STATE:10 §1.4.3 的 S4-P3 / P4 / P5 / P8。
- UI:10 §1.3.3 的 S3-P1 / P2 / P5 / P7。

## v0.9.0-rc.1 — 2026-09-28(待上机)

### 范围

- **首发只验 Cubase**(J124:Live / Studio One 没上机,文档标未验证;U14「每次 release 跑全矩阵」按此修订)。
- **REAPER**:只有 S1 spike 层证据(2026-08-16),本版不复测,等级沿用 Tier 2(部分验证)。
- **Ableton Live 12 / Studio One 6**:两层都没有证据,标未验证(Tier 3)。
- **FL Studio**:不在 v1 支持矩阵。
- 被测包(上机时填):tag 提交 40 位 sha `________`;Cubase 版本 `________`;音频接口与驱动 `________`;测试工程的组号 `____`。

### 矩阵

| DAW / 版本 | RT | OFF(实时) | OFF(快速) | LOOP | BLK | SR | SEEK | MUTE | FRZ | LIFE | AUTO | STATE | UI | Tier |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Cubase 15 Pro(RC 包) | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ | Tier 1(本行全绿后确认) |
| REAPER 7 | — | — | — | — | — | — | — | — | — | — | — | — | — | Tier 2(沿用) |
| Ableton Live 12 | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | Tier 3(未验证) |
| Studio One 6 | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | Tier 3(未验证) |
| FL Studio 21 | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | 不在 v1 矩阵 |

REAPER 一行的「—」:J124 首发只验 Cubase,REAPER 本版不复测。Live / Studio One 一行的「N/A」:未上机(J124、U27)。FL Studio 一行的「N/A」:不在 v1 支持矩阵(`docs/DAW_COMPATIBILITY.md` §4)。

### Cubase 逐格怎么跑(RC 包)

测之前先把工程另存一份副本。想看时间线缺口计数,播放时开着 `scvb_diag`(命令见发版清单 U-1);不开也行,那就只看 Output 顶部有没有「路由失准:… 时间线缺口」横幅。

| 格 | 在 Cubase 15 里怎么做 | 记什么 / 判据 |
| --- | --- | --- |
| RT | buffer 512,实时播全曲一遍 | 没有「时间线缺口」横幅;开着 scvb_diag 的话 gapCount 增量 0(起播那一下除外);听感正常、无咔哒 |
| OFF(实时) | File → Export → Audio Mixdown,勾 Real-Time Export,导出一个区间(32-bit float) | 按 10 §4.2 判(发版清单 D1 / D2);另把本格与下一格的两份导出用 `scvb_nulltest --align` 互比一次,记样本偏移与残差(参照 v5.6.19 B53 的做法) |
| OFF(快速) | 同上,不勾 Real-Time Export | 同上 |
| LOOP | 4 小节循环区播 20 遍 | gapCount 增量 0;无咔哒 |
| BLK | 音频驱动控制面板把 buffer 依次设 32 / 128 / 512 / 2048,各播 30 秒 | 每档无缺口横幅、gapCount 0;驱动不支持的档位写 N/A |
| SR | Project Setup 的采样率切 44.1 / 48 / 96 kHz,各播 30 秒 | 无崩溃;记下出现了哪些横幅(采样率不一致的轨按设计被禁用,v1 不做重采样) |
| SEEK | 播放中拖播放头 20 次 | gapCount 增量 ≤ 20(每次 ≤ 1 块);无崩溃 |
| MUTE | 5 条人声轨各 mute 一次;solo 1 条;一条 mute 轨的推子拉到 −∞ | **逐字记下每种操作听到了什么**(本版建基线,发版清单 F4);设计预期:DAW 的 mute / solo / 推子对 SCVB 通路无效(J45) |
| FRZ | 用副本工程:Freeze 一条含 Input 的轨;对一条轨做 Render in Place;导出一条单轨 stem | **逐字记下每个产物是什么**;设计预期:静音文件(`docs/KNOWN_ISSUES.md` KI-4) |
| LIFE | ① 任务管理器强杀 Cubase 后重开工程;② 同组两个 Input 设同一 channel;③ 总线再插一个 Output;④ 复制粘贴一个 Input 实例;⑤ 删一条 Input;⑥ 单声道轨换成立体声(同一 channel);⑦ 停用再启用 Output | ① 自动重连;② 后到者显示冲突、不生效;③ 第二个进只读观察态,删掉第一个后 ≤ 3 秒接管;④ 粘贴出的实例进冲突态;⑤ 其他 Input 不受影响;⑥ 不撕裂;⑦ 启用后照常工作 |
| AUTO | Write、Latch 各一次 × Output 窗口开、关各一次,各打印一段;再切回 Read、输出开关 OFF 回放 | 车道真被写入(在 Ins 隐藏车道下,`docs/DAW_COMPATIBILITY.md` §2.1);冻结维度写成平直线;回放与打印时一致(回读一致性的量化 = 发版清单 D2) |
| STATE | 满配(15 轨、两个版本、轨道页七项都改过)存工程 → 完全关 Cubase → 重开;再「另存为」一份副本重开 | 七项、段表、曲线、版本都在 |
| UI | Output 编辑器开关 20 次;缩放四档;Input、Monitor 各开关几次 | 不出现空白窗口或兜底面板;缩放后不超框;记下开窗大致耗时 |

### 功能 × 测试包证据(参考,不能填格)

证据分两层:**S1 路由 spike**(Cubase 14 / 15,2026-08-16/17,逐项记录在 [S1-daw-checklist.md](../spikes/S1-daw-checklist.md) §3 C 系列、§4 R 系列、§7 G 系列的完成列)与**成品测试包**(Cubase 15 Pro,v5.6 – v5.6.19;最后一包 v5.6.19 = CI run 36364806176,`72e4501`,用户 2026-09-28 实测,逐项结果在 masterPlan `review/kit-drafts/v5.6.19-user-results.md`)。

| 格 | Cubase 的历史证据 | REAPER 的历史证据 |
| --- | --- | --- |
| RT | S1 C-5 实时全曲 ✅;v5.6.19 B52 关轨无咔哒 过 | S1 R-2 / R-3 / R-4(anticipative FX 开 / 最大 / 关)、R-12(dedicated process)✅ |
| OFF | S1 C-3 勾 / 不勾 Real-Time 两次导出 ✅;v5.6.19 B53 实时与离线导出逐样本对齐、无断音 过 | S1 R-5(1× 与 Full-speed Offline)✅ |
| LOOP | S1 C-6 循环 × 100 ✅ | — |
| BLK | S1 C-8 buffer 四档 ✅ | — |
| SR | S1 C-9 ✅;v5.6.19 B54 换采样率有提示 过 | — |
| SEEK | S1 C-7 ✅ | — |
| MUTE | S1 C-10 ✅(只记了 ✅,没记原话) | — |
| FRZ | S1 C-11、C-2 ✅(同上) | — |
| LIFE | S1 C-12 / C-13 / C-14 ✅;v5.6.19 B45(删一条 Input 不踢掉另一条)、B46(单声道换立体声同号不撕裂)、B51(停用再启用 Output)过 | — |
| AUTO | v5.6 测试包全量清单第 32–34 条过;v5.6.19 B51 过;RD-01 在 Cubase 15 上未复现(Cubase 14 没用成品复测) | 未测(RD-04 已知风险) |
| STATE | S1 C-12、G-3 ✅;v5.6.19 A1–A7(轨道页七项随工程保存)与 A-P8(旧工程在新包打开)过,A-P9 未测 | 未测 |
| UI | v5.6.19 B47 / B49 过,B48 / B50 测不出(分析太快来不及操作);B56–B58 过;B55 与清单不符(滚轮不调音量,用户裁维持,J117);B59 过(只读态切版本仍弹确认,用户裁不改,J118) | — |

### 结论(上机后填)

- Cubase:`________`(全绿 ⇒ Tier 1 维持;有 F ⇒ 按 10 §3.4 重判,并同一 PR 改 `docs/DAW_COMPATIBILITY.md` §4、两份 README 与 [daw-support-tiers.md](daw-support-tiers.md))
- MUTE / FRZ 基线原话:`________`
