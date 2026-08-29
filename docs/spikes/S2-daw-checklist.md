# S2 周日上机逐 DAW 操作清单(自动化写入)

> 本清单把 07-execution-plan T03 的验收 1-9 + 03 待定项①②⑦ + 04 异议 4 整理成照着做的人话步骤。
> 判据真源 = T03 验收 / 03 §4(DAW 作战手册)/ research/08 §2.1。
>
> **DAW 范围(用户决定 U27 口径)**:Cubase 15 为主测 DAW → REAPER 7 只做 Cubase 覆盖不了的兜底格 → Live / Studio One / FL 标为可选/尽力(能测就测,不测不阻塞)。
>
> **路径占位符**:`<toolchain>` = 本机工具链根目录(JUCE / CMake 解包处),`<workspace>` = 本机实测工作目录(不入库,自选)。

## 0. 装机与准备(一次性)

1. 构建并装机(把 S2 spike 装进用户级 VST3 目录):

   ~~~powershell
   $env:JUCE_PATH = '<toolchain>\juce'
   $env:PATH = '<toolchain>\tools\cmake-3.31.12-windows-x86_64\bin;' + $env:PATH
   pwsh scripts/gates.ps1 -Quick -BuildDir build-T03
   # 手动把产物拷到用户级 VST3:
   # build-T03\SCVBS2Output_artefacts\Release\VST3\SCVB S2 Output.vst3
   #   → %LOCALAPPDATA%\Programs\Common\VST3\SCVB S2 Output.vst3
   ~~~

2. 确认 DAW 扫到插件:**SCVB S2 Output**(Synchain 厂商)。打开其编辑器,应见:
   - 标题 "SCVB S2 Automation Spike";
   - 一个 **Output: OFF/ON** 按钮;
   - 状态行显示 FOLLOW / ARMED / PRINTING @ 秒 + v1_t01_pan / v1_t01_vol 回读值。
3. **S2 没有 scvb_diag csv 工具**(那是 S1 的),证据 = 截图 + DAW 自动化车道 + 编辑器回读行。所有产物存 §0b 的目录。

## 0b. 记录分工(重要:没有自动 csv,证据主要靠截图)

**唯一工作目录(周日开始前先建好)**

~~~powershell
mkdir '<workspace>\S1-2026-08-16\S2'
~~~

| 产物 | 谁记录 | 你要做的动作 |
| --- | --- | --- |
| 自动化车道落点 | 🟡 手动 | 打印完看宿主自动化车道有没有阶梯点;截图存档 |
| 插件状态 / 回读值 | 🟡 手动 | 编辑器状态行(PRINTING/FOLLOW + v1_t01 值)截图或抄一行 |
| 区间外手工段完好 | 🟡 手动 | 对比打印前后该段车道,截图 |
| 结果格 | 🟡 一句 | 每格写「过/不过/现象」,填入 §4 汇总表 |

- 截图命名:s2-<daw>-<格子号>.png(如 s2-cubase-C1.png),存进 S2 目录。
- 工程文件(.cpr / .rpp)也存进 S2 目录,别删,汇总阶段可能复查车道。
- 周日晚把 S2 目录路径交给调度者即可,不用自己写报告。

## 1. C 系列(Cubase 15,主测——先跑完这里)

> 通用前置:新建工程 48000/buffer 512,插 SCVB S2 Output 到任意一轨(建议总线);打开插件编辑器。
> 打印窗口固定 [0,30s],包络 = pan 阶梯 4 档(-60/-20/+20/+60,每 4s)+ vol 阶梯 5 档(-6/-3/0/+3/+6 dB,每 3s)。

| 步 | 做 | 判据 / 存什么 |
| --- | --- | --- |
| C-1(S2-C2,最高优先) | Output ON;轨道自动化模式设 **Write**;播放 0-30s | 30 条 pan/vol 车道出现阶梯点;截图 s2-cubase-C1.png;车道值匹配包络档位 |
| C-2(S2-C2b) | 同上但用 **Latch** | 同上,截图 s2-cubase-C2.png |
| C-3(S2-A0) | 先在 **35-40s** 手工画一段 v1_t01_pan 自动化 → Output ON → 播放 0-30s 打印 → 停止 | 35-40s 手工段原样完好(区间外零 gesture);截图 s2-cubase-C3.png |
| C-4(回读一致性) | 打印完 → Output OFF → 回放 0-30s | 车道回放驱动参数;编辑器 v1_t01 读数与打印一致;无新增点;截图 s2-cubase-C4.png |
| C-5(04 异议 4) | Output ON;轨道自动化模式设 **Read**;播放(试听) | 不产生新自动化点(Read 不录);截图 s2-cubase-C5.png |
| C-6(待定项②) | 打开轨道自动化参数列表,找 bypass | 记 bypass 在列表中的位置(应在 123 之后,即索引 123);截图 s2-cubase-C6.png |
| C-7(待定项⑦) | 打印 0-30s 后看车道断点密度与时间偏移 | 记断点数(稀疏?)与整体偏移 ms;据此校准 deadband/lookahead;截图 s2-cubase-C7.png |
| C-8(S2-C1) | 确认自动化写进 **Ins 隐藏车道** | Show/Hide Automation → More → Ins → 槽号 → 参数;截图引导路径 s2-cubase-C8.png |

## 2. R 系列(REAPER 7,仅兜底——只做 Cubase 没有的这 4 格)

| 步 | 做 | 判据 / 存什么 |
| --- | --- | --- |
| R-1(S2-R1) | GUI **开**;Write/Latch;播放打印 | 落点正确;截图 s2-reaper-R1.png |
| R-2(S2-R1b/待定项①) | GUI **关**;Write/Latch;播放打印 | 是否复现「关 GUI 不写」;截图 s2-reaper-R2.png |
| R-3(待定项①) | Preferences → Plug-ins → VST → VST compatibility → Parameter automation notifications 设 **process all notifications**;再关 GUI 打印 | 是否修复;截图 s2-reaper-R3.png |
| R-4(待定项②) | 开参数列表找 bypass | 记 bypass 位置(与 Cubase C-6 交叉验证);截图 s2-reaper-R4.png |

## 3. L / S / F 系列(可选 / 尽力,能测就测)

| 步 | 做 | 判据 / 存什么 |
| --- | --- | --- |
| L-1(S2-L1) | Live 12:打印 → 看 Re-Enable Automation | 亮起为预期;点后恢复;车道数据完整;截图 s2-live-L1.png |
| L-2(S2-L2) | Live 12:Configure 列表 | 124 参数全出现(123+bypass),Live 128 上限余 4;截图 s2-live-L2.png |
| S-1(S2-S1) | Studio One:在 **插件窗口内** 设 Write/Latch(仅轨道上开不够)后打印 | 落点正确;截图 s2-s1-S1.png |
| F-1(S2-F1) | FL Studio:gesture 写入能否生成 automation clip | 能 → 记 Tier 1;不能 → 记 Tier 2(可读不可写);截图 s2-fl-F1.png |

## 4. 结果汇总格(每格一句)

| 格 | 结果(过/不过/现象) | 备注 |
| --- | --- | --- |
| C-1(S2-C2 Write) | | |
| C-2(S2-C2 Latch) | | |
| C-3(S2-A0) | | |
| C-4(回读一致性) | | |
| C-5(04 异议 4) | | |
| C-6(待定项② bypass) | | |
| C-7(待定项⑦ deadband/lookahead) | | |
| C-8(S2-C1 隐藏车道) | | |
| R-1(S2-R1 GUI 开) | | |
| R-2(S2-R1 GUI 关) | | |
| R-3(待定项① 设置后) | | |
| R-4(待定项② 交叉) | | |
| L-1(S2-L1) | | |
| L-2(S2-L2) | | |
| S-1(S2-S1) | | |
| F-1(S2-F1) | | |
