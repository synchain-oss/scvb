# null test 日志

> 状态:演进中(只追加,不改旧记录)
> 最后更新:2026-09-28(建骨架,尚无记录)
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
- ⚠ **补偿口径待统筹定**:10 §4.2 的补偿公式按 S1 spike 口径写(spike 版 Output 把 mono 原样复制到 L / R,0 dB);**成品 Output 对居中 mono 轨每侧给 0.7071(−3.01 dB)**(`tests/core/test_transition.cpp` 的 PAN-1)。按这个推:宿主 −3 dB Equal Power 时 mono 部分不用补偿;宿主 0 dB 时 test 要 +3.01 dB;stereo 轨(每轨 width 100、pan 0 时 L→L、R→R)不用补偿。`scripts/nulltest.ps1` 的 `-PanLawDb` 按 spike 口径把参数取反后作为 test 的增益。**以上是按源码推的,没实跑过。**
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

每次跑完补一行;「原始记录」写 §3 里对应那节的时间戳。

| 日期 | 版本 / tag 提交 | DAW / 版本 | 模式 | 素材 | 宿主 pan law | 补偿(dB) | 样本偏移 | 残差峰值 L / R(dBFS) | 残差 RMS L / R(dBFS) | 判据 | 结论 | 原始记录 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

## 3. 原始记录

以下由 `scripts/nulltest.ps1` 追加,每次一节(标题是时间戳),不要手改。
