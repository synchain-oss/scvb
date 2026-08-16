# S1 周日上机逐 DAW 操作清单

> 本清单把 10-validation §1.1.2 的 R/C/L/S + G 系列整理成可照着做的人话步骤。
> 判据真源 = 10 §1.1.3;每格做完**立即**读 csv + 跑 null test 记录,不要攒到最后。
>
> **DAW 范围(用户决定 2026-08-14,U27)**:Cubase 15 为主测 DAW → REAPER 7 只跑 Cubase 覆盖不了的兜底格 → Ableton Live 跳过 → Studio One 仅作可选对照。

## 0. 装机与工具(一次性准备)

1. 构建并装机(两个 .vst3 进系统 VST3 目录):
   `pwsh scripts/build.ps1 -Config RelWithDebInfo -BuildDir build-T02 -Install`
2. 确认 DAW 扫到两个插件:**SCVB Input**(插在人声轨末格)与 **SCVB Output**(插在总线第一格)。
3. 生成 15 轨对位素材(含 2 条 stereo,在任意目录):
   `scvb_nulltest --gen-click click --tracks 15 --stereo-tracks 2 --seconds 120 --fs 48000`
   → 得到 click_01.wav … click_13.wav(mono)、click_14.wav / click_15.wav(stereo,L/R 相位错开)。
4. 诊断工具:每个场景播放时另开一个终端跑
   `scvb_diag --out s1-<daw>-<场景>.csv --group 1`,播完 Ctrl+C 停止。
   **一个场景一个 csv 文件**(如 `s1-reaper-R1.csv`),文件命名 = 格子号,做完后不用读、不用记——汇总阶段有人读。
   **Ctrl+C 误触没事**:每行即时落盘、最坏丢最后一行;误触/按早后重跑同一条命令即可续写(工具检测到文件已存在会跳过表头直接追加);按晚只是多几行空闲数据,汇总时忽略。

## 0b. 配置开关(环境变量,按需)

| 变量             | 作用                            | 默认                            |
| ---------------- | ------------------------------- | ------------------------------- |
| SCVB_GROUP       | 组号 G(1..8,段名带 g{G})        | 1                               |
| SCVB_CHANNEL     | 强制 channel 号(1..15)          | 不设 = 自动 claim 最低空闲 slot |
| SCVB_RING_FRAMES | 环长覆盖(2 的幂;G-5 套圈测试用) | 1<<19                           |

- 15 轨工程**不设 SCVB_CHANNEL**,让 15 个 Input 按加载顺序自动占 ch1..ch15(求和可交换,null test 不依赖具体号);V14/V15 是 stereo 轨,channels 列应显示 2。
- 冲突/单实例测试才设 SCVB_CHANNEL(如 SCVB_CHANNEL=3 让两个实例抢 ch3)。
- **设置方式**:环境变量在插件加载时读取一次。先关闭 DAW → PowerShell 执行 `$env:SCVB_CHANNEL="3"`(G-5 用 `$env:SCVB_RING_FRAMES="8192"`,G-10 用 `$env:SCVB_GROUP="2"`)→ **从同一个窗口**启动 DAW 再加载工程;该格做完后关闭 DAW、重新普通启动即回到默认。

## 0c. 真人声素材(可选:真实数据格)

用户提供了三组已对齐的真人声单声道轨(48kHz / 24bit / mono),
本地路径:`C:\Users\lenovo\deepseekHarness\SCVB\masterPlan\assets\vocal-test\`(gitignore,不入库)。
**三组是三个不同项目(三首歌),严禁混用**;每次真实数据格只取一个项目。

| 项目目录 | 轨数 | 时长 | 建议用途 |
|---|---|---|---|
| `oeuvre-527-bad-apple` | 9(全干音) | 5:17 | **首选**:9 轨全部导入 V01..V09 做真实数据矩阵(其余轨补 click 或留空);null test 判据不变 |
| `oeuvre-523-kalafina-storia` | 6 | 3:37 | 快速冒烟(6 轨即够) |
| `oeuvre-515-inclusion` | 39 | 4:43 | 压力/多轨格:任选其中 13 轨(主唱+和声混合)导入 V01..V13 |

真实数据格操作与 click 格完全相同:先渲染无插件的 ref,再装插件渲染,
逐样本 null(判据 10 §1.1.3);**离线渲染格建议用真人声**(正是 §8 大块路径的实战触发面)。

## 0d. 修复版重跑须知(2026-08-16 → v6)

本包为**修复版 v6**。v5 及更早版本在 Cubase 冻结/RIP/导出时反复以 mono⇄stereo 重建环段;命名共享内存段在存活视图下无法扩容,旧实现把「请求大小」当「实际大小」发布几何 → 2MB mono 视图 + stereo 几何 → 写环越界 0xC0000005(多轮转储同址实锤,已用免 DAW 压测 1:1 复现)。**v6:段恒按 stereo 最大容量创建 + 实际容量校验(Input 侧降级声道、Output 侧拒绝坏几何)**。
- **强烈建议【新建工程】**(同 v4/v5):旧工程插件实例可能已被宿主禁用,判据失真。
- §1 通用准备**从头重做**(同 v4/v5);**无需重跑**:C-10、C-12、C-14(重启后健康态已完成,结果有效)。
- **开 DAW 前自检(推荐)**:工具包内 `pwsh tools\stress-run.ps1`,4 场景全 PASS 再开始。
- **崩溃证据自动收集**:`pwsh tools\collect-crash.ps1 -Setup` 注册转储(一次性),测试时挂 `pwsh tools\collect-crash.ps1 -Watch`;崩溃自动归档 dmp + 插件 journal 到 `S1-pack\work\crash-<时间戳>\`。
- **必须用 v6 重跑**:C-2、C-3、C-4、C-5、C-6、C-7、C-8、C-9、C-11(历次崩溃格)+ **G 系列全部**;G-2 与重导后的 `ref_Aprime` 对 null。
## 1. 通用准备(每个 DAW 都做)

1. 新建工程,采样率 48000,buffer 512;导入 15 轨素材到 V01..V15(**V14/V15 必须建为 stereo 轨**);建 stereo 总线 **VOX BUS**,15 轨输出全指向它。
2. 所有轨与总线:pan 居中、**所有轨推子统一 −18 dB**、总线推子 0 dB、无其他插件;stereo 轨的宿主平衡器居中。理由:**click 素材是满幅脉冲(前 13 轨为连续相邻样本),true peak ≈ +4 dBTP,单轨就会爆表;人声格 15 轨叠加同样过 0**。统一压 −18 dB 后 click true peak ≈ −14 dBTP、人声回线性区;统一值保证 null test 两侧同条件。若总线峰值仍 > −6 dBFS(宿主总线电平表),全体轨再等量下调并**记录实际推子值**;**ref A 与所有 test 渲染期间推子一律不得改动**。**截图存档**(证明前提成立)。
3. **参考渲染 A**:不装 SCVB,离线导出总线为 ref_A_<daw>.wav(32-bit float,48k),记 sha256。
4. **参考渲染 A′(PDC 变体,与 A 同批「无 SCVB」渲染,在装 SCVB 之前做)**:在 V03 与 V07 的 insert 1 插高延迟插件(线性相位 EQ 或 4096 样本延迟测试件),仍不装 SCVB,导出 ref_Aprime_<daw>.wav(32-bit float,48k),记 sha256。**导出后移除这两个插件,恢复全无插件状态**——G-2 会再插回来(那时高延迟插件在 Input 前一格)。
5. 装 SCVB(15 Input + 1 Output),开 scvb_diag,开始各场景。

## 2. 记录分工(重要:大部分是自动的,你不需要写报告)

**你的工作量**:照着步骤跑 → 文件按格子命名存好 → 每格最多写一句结论(过/不过/听感差异)。
**汇总报告**(S1-routing.md 回填)由调度者从你留下的文件统一做,周日结束把工作目录路径交给调度者即可。

| 产物 | 谁记录 | 你要做的动作 |
|---|---|---|
| 诊断数据(gapCount/overlapCount/channels) | 🟢 自动 | 每格开始前开 `scvb_diag --out s1-<daw>-<场景>.csv`,该格结束 Ctrl+C。csv 自动写好,**你不用看、不用抄** |
| null test 判定 | 🟢 半自动 | 渲染格:把导出文件命名为 `test_<场景>.wav` 存进工作目录;然后跑 `pwsh scripts/nulltest.ps1 ref_A_<daw>.wav test_<场景>.wav -PanLawDb <宿主pan law> -Align`(pan law 0 dB 首选)。脚本自动算残差并打印,**把打印结果截个图或复制一行即可** |
| 渲染文件 | 🟡 你导出 | 离线导出按 `ref_*` / `test_<场景>` 命名存进工作目录,别删 |
| 截图 | 🟡 手动 | 清单里标了"截图存档"的格子(前提确认、冲突/只读/失准等特殊状态) |
| 听感判断 | 🟡 一句话 | R-1"与 A 一致?"、G-1b"有无突起"这类——写"一致/不一致"即可 |
| 汇总报告 | 🟢 调度者 | 周日结束把工作目录路径发调度者 |

**唯一工作目录(所有文件都放这里,周日开始前先建好)**

```powershell
mkdir C:\Users\lenovo\deepseekHarness\S1-2026-08-16
```

- **csv**:scvb_diag 的 `--out` 全部指到这个目录(如 `--out C:\Users\lenovo\deepseekHarness\S1-2026-08-16\s1-reaper-R1.csv`)
- **wav**:所有离线导出(`ref_A_*` / `ref_Aprime_*` / `test_*`)导出时选这个目录
- **截图 png**:也存这个目录,命名 `s1-<daw>-<格子号>.png`
- 目录做完**原样保留**,周日晚把整个目录路径发给调度者

**scvb_diag 的开闭节奏(每格一个,不是全程一个)**

1. 开始某格前:开终端跑 `scvb_diag --out C:\...\s1-<daw>-<格子号>.csv --group 1`
2. 该格播完:Ctrl+C 关闭
3. 下一格:重复 1-2,换新的格子号文件名

不开播放的格子(纯配置/截图类)不用开 scvb_diag;开了也行,不碍事。

**wav 导出必须手动命名**(DAW 不会自动按格子号起名,导出对话框里把默认文件名改成):

- 参考:`ref_A_<daw>.wav` / `ref_Aprime_<daw>.wav`(每个 DAW 各一次)
- 测试:`test_<格子号>.wav`(如 R-5 那格导出为 `test_R5.wav`)
- **只有产生离线导出的格子才要起名**(清单"做"列写着 Render/Export/导出 的),纯播放格什么都不用导
- 忘改名也没关系:导出后手动重命名文件即可,周日晚之前改好就行;实在忘了就发消息告诉我哪个文件对应哪格

- 判据自动化的部分判 FAIL 的标准:nulltest 打印残差峰值 ≥ −120 dBFS 或偏移 ≠ 0。
- 若某格需要你主观判断,清单该格的"判据"列会写明,否则以文件产物为准。

## 3. C 系列(Cubase 15,主测 DAW——先跑完这里)

全部通用步骤收进 Cubase;做完 C-1..C-14 再回来看 §4 的 REAPER 兜底。

| 步  | 做                                                                      | 判据 / 存什么                                                                 |
| --- | ----------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| C-1  | 记首块 timeInSamples(csv 读)                                            | 起始值不一定为 0;寻址不依赖起点 0                                              |
| C-2  | Direct Offline Processing + Render in Place(替换)各一次                 | 预期不触发 process;核对原事件是否被静音替换、能否 Undo                          |
| C-3  | Export→Audio Mixdown 勾/不勾 Real-Time 各一次(导出命名 test_C3rt.wav / test_C3offline.wav) | 两个导出与 A 各 null,都过                                                      |
| C-4  | ASIO-Guard 开/关各 30s                                                  | gapCount 全 0                                                                  |
| C-5  | 实时播全曲 1 遍                                                         | gapCount 全 0;听感与 A 一致                                                     |
| C-6  | 4 小节循环区播 100 遍                                                   | gapCount 增量==0                                                                |
| C-7  | 播放中拖播放头定位 20 次                                                | gapCount 增量 ≤20(每次 ≤1 块)                                                   |
| C-8  | buffer 32/128/512/2048 各 30s                                           | 各档 gapCount==0                                                                |
| C-9  | SR 切 44100/96000 各 30s                                                | 无崩溃;SR 不符轨禁用                                                            |
| C-10 | 单轨 solo/mute 各 5 轨;mute 轨推子拉到 −∞                               | 记录三类行为(跳过处理/照常送静音/照常送原信号);solo/mute 对 SCVB 通路失效预期   |
| C-11 | Freeze 一条轨 / Render in Place;打开产物听                              | 预期整段静音;替换式渲染是否覆盖原素材 → 用户数据警告                            |
| C-12 | 任务管理器强杀 Cubase 后重开工程                                        | 自动重连,gapCount 从 0                                                          |
| C-13 | 两个 Input 都设 ch3:新建 2 轨小工程插 2 个 Input;关闭 Cubase → PowerShell 执行 `$env:SCVB_CHANNEL="3"` → **同一窗口**启动 Cubase 加载工程播放(设置方式见 §0b) | 后到者冲突,不写环;csv 里 ch3 write_head 单调无回退 |
| C-14 | 总线再插一个 Output                                                     | 第二只读、总线直通;删第一个 ≤3s 接管                                            |

## 4. R 系列(REAPER 7,仅兜底——只做 Cubase 没有的这 4 格)

| 步  | 做                                                          | 判据 / 存什么                                             |
| --- | ----------------------------------------------------------- | --------------------------------------------------------- |
| R-2  | Preferences→Audio→Buffering 开 Anticipative FX              | gapCount 全 0                                             |
| R-3  | render-ahead 拉到最大(记 ms)                                | gapCount 全 0;非 0 记首次 gap 值 → 定 ring_frames(S1-P8)  |
| R-4  | 关 Anticipative                                             | gapCount 全 0                                             |
| R-5  | File→Render 1× 与 Full-speed Offline 各一次 → test_R5.wav   | 与 A null(与 C-3 互验);这是 REAPER 特有的 Full-speed 档    |
| R-12 | 插件 Run as dedicated process(右键 FX)                      | 跨进程 shm 仍工作                                         |

> 其余 R 步骤已并入 §3 C 系列(C-5..C-14),不再单跑 REAPER。

## 5. L 系列(Ableton Live 12)—— 跳过

> **用户决定 2026-08-14(U27)**:Ableton Live 本轮不做。L-5(设备停用)原本已挂 T24([J55]),不受影响;Live 宿主特有问题改由发布期兼容矩阵兜底,记录在 docs/DAW_COMPATIBILITY.md 计划内。

## 6. S 系列(Studio One 7)—— 可选对照,非必要不做

> **用户决定 2026-08-14(U27)**:仅当 Cubase 出现疑似宿主特异现象、需要第二宿主对照时才做 S-1/S-2/S-3,平时跳过。

| 步  | 做                            | 判据                                               |
| --- | ----------------------------- | -------------------------------------------------- |
| S-1 | Dropout Protection 四档各 60s | 记 Input/Output 收到的 numSamples 差;gapCount 全 0 |
| S-2 | Z 低延迟监听开启              | gapCount 全 0                                      |
| S-3 | Song→Export Mixdown           | 与 A null                                          |

## 7. G 系列(全宿主通用;G-1..G-4 与 G-9 是阻断项,G-10 记录项)

| 步   | 做                                                                                             | 判据 / 存什么                                                                                                   |
| ---- | ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| G-1a | 只装 15 Input,不装 Output,实时播 + 离线导出                                                    | 人声照常出声(直通);与 A null 通过                                                                               |
| G-1b | 播放中加载 Output;另跑卸载 Output                                                              | 录总线输出:交叉期包络单调、无 >+1dB 突起、无超 ramp 时长空白                                                    |
| G-1c | 播放中删除 Output;强杀宿主进程/心跳停摆                                                        | 显示陈旧 2000ms、接管 ≥5000ms+pid 探活失败;恢复无咔哒                                                           |
| G-1d | 心跳边缘抖动(每 4.5s 停 0.7s)                                                                  | 5s 滞回只静音→直通;切换次数==0                                                                                  |
| G-2  | V03/V07 的 Input 前插高延迟插件,实时 + 离线                                                    | 与 A′ null;若这两轨错位 → 记错位样本数 + 是否调 IAudioPresentationLatency                                       |
| G-3  | 同位置起播 ×3 + 存工程关 DAW 重开 ×1                                                           | 首块 timeInSamples 四次恒等、与走带标尺线性                                                                     |
| G-4  | 两个 bundle 各跑 pluginval strict 5(对端缺席);DAW 内单侧加载各 60s                             | 不阻塞/不崩溃/不超时                                                                                            |
| G-5  | SCVB_RING_FRAMES=8192,开 REAPER anticipative / Cubase 离线快渲染                                 | 必须弃块 + gapCount+1,不得计数为 0 的可闻撕裂                                                                   |
| G-6  | 开 count-in(≥6s)从 0 前起播                                                                    | 负 t0 不写环不越界;跨零点只写尾段;无静音↔直通切换                                                               |
| G-7  | 分别 bypass Output 10s / Input 10s                                                             | 记听感与 gapCount                                                                                               |
| G-8  | 同 DAW 开两个 SCVB 工程 / 两个 DAW 同开                                                        | 后开者 channel 冲突;UI 用 pid 反查进程名提示                                                                    |
| G-9  | ①实时+离线导出后对 V14/V15 逐通道 null;②csv channels 列逐轨核对;③V07 mono 改 stereo 存重开重跑 | ①L 与 R 各自 null 过(互换/单路复制判 FAIL);②V01-13=1/V14-15=2;③channels 随 prepareToPlay 更新、gapCount 增量==0 |
| G-10 | Cubase 或 REAPER 内建两组(g1:2 Input ch1/2 + 1 Output;g2 同号 2 Input + 1 Output;SCVB_GROUP 区分) | 两组 null 各自独立过、gapCount 双组 0、同号零 CAS 冲突、mask/广播不串扰                                         |
