# null test 日志

> 状态:演进中(只追加,不改旧记录)
> 最后更新:2026-09-30(rc.1 第三包的 D2 / D1 两条记录,SL-581)
> 来源:masterPlan 10 §4.1–§4.3;发版清单 D1 / D2 / D5

每次 null test 的结果与素材指纹。`scripts/nulltest.ps1` 每跑一次,会把工具原始输出**追加到本文件末尾**(§3 之后);跑完再在 §2 汇总表补一行。

## 1. 流程

### 1.1 每次必记的前提(10 §4.1,逐条确认)

1. 所有人声轨与总线的**宿主 pan 居中、推子 0 dB**(null test 的可比性前提,J45)。
2. 宿主的 **pan law 设置**(REAPER 可配 0 / −3 / −4.5 / −6 dB;Cubase 默认 −3 dB Equal Power;Live 固定;Studio One 可配)—— 这是补偿量的依据。
3. 无 dither、主输出无限制器、总线上除 SCVB Output 外无处理。
4. 导出 32-bit float。
5. 两次导出的起点与长度完全相同。
6. 被测包的 tag 提交 sha、DAW 与版本、采样率、buffer。

### 1.2 模式 A —— 透明性 null(发版清单 D1)

- **渲染 A(参考)**:不装 SCVB,人声轨直接汇入 stereo 总线,导出 `ref_A.wav`。
- **渲染 B(被测)**:装 SCVB(每条人声轨末格 Input + 总线首格 Output),Output 的 pan / vol / 每轨 width / 全局 width / MS Balance / Lead Select 全在默认值(不分析、输出开关 OFF、不写自动化),导出 `test_B.wav`。
- **判据**(10 §1.1.3 S1-P1 / P2):样本偏移 0,残差峰值 < −120 dBFS(理想为按位相等)。
- ⚠ **补偿口径(已定,J178①)**:10 §4.2 的补偿公式按 S1 spike 口径写(spike 版 Output 把 mono 原样复制到 L / R,0 dB);**成品 Output 对居中 mono 轨每侧给 0.7071(−3.01 dB)**(`tests/core/test_transition.cpp` 的 PAN-1)。按这个推:宿主 Equal Power(等功率)时 mono 部分不用补偿;宿主 0 dB 时 mono 部分要 +3.01 dB;stereo 轨(每轨 width 100、pan 0 时 L→L、R→R)不用补偿。所以 **0 dB 档下 mono + stereo 的完整格用单一增益调不平**(`scvb_nulltest --gain-db` 只能对整份 test 乘一个数),完整格只能在 Equal Power 档下跑,0 dB 档只用于纯 mono 的定口径。J178① 把它定成:c = 宿主居中增益(dB)+ 3.0103,`-PanLawDb` 填 −c —— Equal Power 档填 `0`,0 dB 档填 `-3.0103`(要写到小数点后 4 位,见发版清单 U-3)。Cubase 对居中 stereo 轨是否也施加 pan law(与 panner 类型有关)没实测,要记下 stereo 轨的 panner 类型。**以上是按源码推的,没实跑过。**
- `scripts/nulltest.ps1` 的 `-PanLawDb` 按 spike 口径把参数取反后作为 test 的增益。成品口径下传进去的是「补偿量取反」,**不是宿主设置**,而脚本会把它原样写进原始记录的「宿主 pan law」那一行(同一行括号里的 `--gain-db` 才是实际施加的补偿)。原始记录不手改,**宿主的真实设置以 §2 汇总表为准**。
- 建议先只放 mono 轨跑一遍定补偿,再加 stereo 轨跑完整格(10 §4.2),两次的宿主 pan law 都要记。

### 1.3 模式 B —— engine vs follow(发版清单 D2)

- **渲染 C**:输出开关 ON(引擎驱动),导出 `engine_C.wav`。
- **渲染 D**:先用 Write / Latch 把自动化打印进 DAW,再把输出开关 OFF(跟随宿主),同一区间导出 `follow_D.wav`。
- **判据**:逐声道残差 RMS < −40 dBFS(10 §1.2.4 S2-P5)。超了就分开打印(只打印 pan、只打印 vol)各比一次,定位是哪条链。
- 某些宿主的离线导出下参数行为可能与实时不同;离线结果异常时改用实时导出或录到新轨再比,并在备注写明用了哪条路径(10 §4.3)。

### 1.4 素材与工具

- 真实人声素材**不入库**(U15),只记 sha256(`scripts/nulltest.ps1` 自动记两份 wav 的 sha256)。
- 对位用的 click 素材现场生成,逐机一致:`scvb_nulltest --gen-click click15.wav --tracks 15 --stereo-tracks 2 --seconds 30 --fs 48000`。
- 比对:`pwsh scripts/nulltest.ps1 <ref.wav> <test.wav> [-PanLawDb <dB>] -Align -BuildDir <构建目录>`(工具构建见发版清单 U-0)。

### 1.5 不许做

- 凭听感调增益去凑 null —— 补偿值只能来自宿主 pan law 的标称值(10 §4.2)。
- 只报合并残差 —— 必须**分声道**报,合并残差对 L / R 互换是盲的。

## 2. 汇总表

每次跑完补一行;「原始记录」写 §3 里对应那节的时间戳。「宿主 pan law」一列写宿主里的真实设置(模式 A 的原始记录里那一行不是它,见 §1.2),stereo 轨的 panner 类型写进「素材」或「结论」。

| 日期 | 版本 / tag 提交 | DAW / 版本 | 模式 | 素材 | 宿主 pan law | 补偿(dB) | 样本偏移 | 残差峰值 L / R(dBFS) | 残差 RMS L / R(dBFS) | 判据 | 结论 | 原始记录 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-09-30 | `518ab466`(rc.1 第三个测试包,CI run 36653086798;不是 tag 提交) | Cubase 15 | B(engine vs follow,D2) | 用户自有多轨人声(不入库);24-bit PCM、48 kHz 立体声、1,401,962 帧(约 29.2 s);engine_C `b1a61736…` / follow_D `23a52437…` | 工程原设置(未回报;两份相同,模式 B 不补偿) | 0 | 0 | −72.5 / −75.6 | −99.0 / −102.7 | 逐声道 RMS < −40 dBFS | **过**(余量约 59 dB);前提见下 | 2026-09-30 00:57:04 -04:00 |
| 2026-09-30 | 同上 | Cubase 15 | A(透明性,D1) | 同一组人声;32-bit float、48 kHz 立体声、1,401,962 帧;ref_A(完整、不装 SCVB)`4beea6d7…` / test_B(第三轮)`583af831…` | Equal Power | 0(`-PanLawDb 0`,J178①) | 0 | −168.6 / −168.6 | −192.1 / −192.1 | 偏移 0、峰值 < −120 dBFS | **过**(浮点舍入量级,非逐位相同);口径见下 | 2026-09-30 00:57:29 -04:00 |

**2026-09-30 两条记录(rc.1 第三包)的说明**。两条由统筹用 `scripts/nulltest.ps1`(带 `-Align`)比对,wav 按角色名传入,完整 sha256 在 §3 的原始记录里。区间起止、buffer 与 Realtime Export 没有回报;两份文件帧数相同、样本偏移 0。

- **D2**:
  - 第二份文件用户命名为 `D2-engine_D`(清单要的名字是 `D2-follow_D`)。按清单第 5–6 步的角色记为 follow_D,即 03 输出关回「跟随宿主」后导出的那一份;用户 2026-09-30 确认它就是关回跟随宿主后导出的那份。清单要的两张截图没有附。旁证:两份不是逐位相同,而两份若都在写入自动化档下离线导出,预期逐位相同。
  - 两份是 24-bit PCM,不是 §1.1 第 4 条要的 32-bit float。24-bit 的量化步长约 −138 dBFS,比判据低约 100 dB,不影响判定。
- **D1**:
  - **口径**:这次的中性状态是「输出开(写入自动化档)、无自动化」。§1.2 与清单 U-3 写的「输出开关 OFF(跟随宿主)」没有单独导出,本条不覆盖那一种状态。test 与不装 SCVB 的 ref 在浮点舍入量级一致,说明导出时没有任何分析结果或自动化在起作用。用户 2026-09-30 确认:工程里没有分析结果(未点分析),各 Input 显示「已连接」—— 声音走的是 Input 静音本轨、Output 总线求和这条路;残差在浮点舍入量级而非逐位为零,与换了一条求和路径一致。
  - 宿主 Stereo Pan Law 为 Equal Power,按 J178① 填 `-PanLawDb 0`,不补偿。残差能到 −168.6 dBFS,也印证了宿主的居中增益与成品 Output 对 mono 轨的 1/√2 一致。
  - 装 SCVB 的只有 13 条 mono 轨(各插 SCVB Input,VOX BUS 第一格插 SCVB Output)。两条立体声轨没插 SCVB Input,在这段里内容也很少:完整 ref 与不含立体声轨的纯 mono ref 只差 RMS −81.5 dBFS。所以本条不验立体声轨经 SCVB Input 的透明性,U-3 第 9 步「完整 · 装 SCVB」那一份没有导出。
  - 只在 Cubase 上跑,Cubase 代替 10 §6.1 要求的 REAPER(J178②)。
  - 同一天前两轮 D1 不成立,不记入本表。那两轮用的是同一份 test(第一轮导出,24-bit),它是在做过 D2 的工程里导出的,带着 D2 写入的逐段音量,残差峰值 −38.5 dBFS、RMS −54.5 dBFS,从第 0 帧起就超。用户删掉全部自动化后重导,就是上表这一条。

## 3. 原始记录

以下由 `scripts/nulltest.ps1` 追加,每次一节(标题是时间戳),不要手改。

## 2026-09-30 00:57:04 -04:00

- ref: D2-engine_C.wav (sha256 b1a617368e66a82944ef42ee18ba0a124006f16247a8c11a97794b428af124e3)
- test: D2-follow_D.wav (sha256 23a52437af6951bcf0792e68f7526496d8655d8ef2ebbc2cf92aeb77f37259ef)
- 宿主 pan law: 0 dB(补偿 --gain-db 0;对齐: True)

结果:
    ref:              D2-engine_C.wav
    test:             D2-follow_D.wav
    channels:         2
    sample_rate:      48000
    frames:           1401962 (ref) / 1401962 (test)
    frames_compared:  1401962
    sample_offset:    0 (aligned)
    gain_db:          0.000
    residual_peak_db: merged -72.523, per-channel [-72.523, -75.626]
    residual_rms_db:  merged -100.435, per-channel [-98.958, -102.690]
    first_over_threshold (>= -120.000 dBFS): frame 570459 channel 0
    bit_exact:        false

## 2026-09-30 00:57:29 -04:00

- ref: D1-full-ref_A.wav (sha256 4beea6d79f27445044b66aa17db936569bde8226ddb84c54351015f9ae59718b)
- test: D1-test_B-r3.wav (sha256 583af831ac221a00995c58ae24ed6d8e0bb78db32c0184f03e1695b0f1ff2e00)
- 宿主 pan law: 0 dB(补偿 --gain-db 0;对齐: True)

结果:
    ref:              D1-full-ref_A.wav
    test:             D1-test_B-r3.wav
    channels:         2
    sample_rate:      48000
    frames:           1401962 (ref) / 1401962 (test)
    frames_compared:  1401962
    sample_offset:    0 (aligned)
    gain_db:          0.000
    residual_peak_db: merged -168.577, per-channel [-168.577, -168.577]
    residual_rms_db:  merged -192.073, per-channel [-192.073, -192.073]
    first_over_threshold (>= -120.000 dBFS): 无
    bit_exact:        false
