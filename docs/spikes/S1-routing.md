# S1 路由可靠性 spike 结果

<!-- 会话表头(10-validation §1.0 固定开场,每次会话填写) -->

| 字段            | 值                                       |
| --------------- | ---------------------------------------- |
| 日期 / 执行人   | (待周日上机填写)                         |
| SCVB commit SHA | (待填)                                   |
| 构建配置        | RelWithDebInfo / MSVC 19.29 / JUCE 8.0.8 |
| 机器            | (CPU / 核数 / RAM / OS build)            |
| 音频接口 & 驱动 | (ASIO 驱动名与版本;WASAPI 也记)          |

## 1. 前提确认

- [ ] 所有轨/总线 pan 居中、推子 0 dB(截图:img/S1-<daw>-preflight.png);**stereo 轨(V14/V15)的宿主平衡器居中、源宽度未被塌成 mono**([J57])
- [ ] 参考渲染 A 已生成:ref_A_<daw>.wav(sha256: ______)
- [ ] 参考渲染 A′(PDC 变体)已生成:ref_Aprime_<daw>.wav(sha256: ______)

## 2. 逐 DAW 结果

| DAW / 版本   | 场景                               | 判据                                   | 实测值                      | P/F | 备注                 |
| ------------ | ---------------------------------- | -------------------------------------- | --------------------------- | --- | -------------------- |
| REAPER 7.xx  | R-1 实时播放                       | gapCount==0                            | 0×15                        |     |                      |
| REAPER 7.xx  | R-2 anticipative 开                | gapCount==0                            |                             |     |                      |
| REAPER 7.xx  | R-3 anticipative max               | gapCount==0                            | render-ahead=_**ms → gap=** |     | **ring_frames 依据** |
| REAPER 7.xx  | R-4 anticipative 关                | gapCount==0                            |                             |     |                      |
| REAPER 7.xx  | R-5 离线渲染                       | null 峰值 < −120dBFS                   | ___ dBFS / 偏移 __ 样本     |     |                      |
| REAPER 7.xx  | R-6 loop ×100                      | gapCount 增量==0                       |                             |     |                      |
| REAPER 7.xx  | R-7 定位 ×20                       | gapCount 增量 ≤20                      |                             |     |                      |
| REAPER 7.xx  | R-8 buffer 32/128/512/2048         | 各档 gapCount==0                       |                             |     |                      |
| REAPER 7.xx  | R-9 SR 44100/96000                 | 无崩溃/SR 不符禁用                     |                             |     |                      |
| REAPER 7.xx  | R-10 solo/mute/推子                | 记录三类行为                           |                             |     |                      |
| REAPER 7.xx  | R-11 freeze/stem                   | 记录行为(静音产物)                     |                             |     |                      |
| REAPER 7.xx  | R-12 dedicated process             | 跨进程 shm 仍工作                      |                             |     |                      |
| REAPER 7.xx  | R-13 强杀重开                      | 自动重连,gapCount 从 0                 |                             |     |                      |
| REAPER 7.xx  | R-14 双 Input ch3                  | 后到者冲突,绝不双写                    |                             |     |                      |
| REAPER 7.xx  | R-15 双 Output                     | 第二只读,删除后 ≤3s 接管               |                             |     |                      |
| Cubase 14.x  | C-1 首块 timeInSamples             | 记录起始值                             | ___                         |     |                      |
| Cubase 14.x  | C-2 Direct Offline/Render in Place | 记录(预期不触发 process)               |                             |     |                      |
| Cubase 14.x  | C-3 Real-Time/Export Mixdown       | 两个导出与 A 各 null                   |                             |     |                      |
| Cubase 14.x  | C-4 ASIO-Guard 开/关               | gapCount==0                            |                             |     |                      |
| Live 12.x    | L-1 Arrangement/Session            | 记录 gapCount                          |                             |     |                      |
| Live 12.x    | L-2 Freeze & Flatten               | 记录(核对原 clip)                      |                             |     |                      |
| Live 12.x    | L-3 Export Normal/Fast             | 与 A null                              |                             |     |                      |
| Live 12.x    | L-4 loop ×100                      | gapCount 增量==0                       |                             |     |                      |
| Studio One 7 | S-1 Dropout Protection 四档        | 记录 Input/Output block 差,gapCount==0 |                             |     |                      |
| Studio One 7 | S-2 Z 低延迟监听                   | gapCount==0                            |                             |     |                      |
| Studio One 7 | S-3 Export Mixdown                 | 与 A null                              |                             |     |                      |

## 2b. 通用增补场景(G 组)结果

| DAW / 版本   | G-1a 直通 null | G-1b 切换/叠加 | G-1c 恢复时序 | G-1d 滞回 | G-2 PDC 错位 | G-3 锚定恒等 | G-4 对端缺席 | G-5 套圈 | G-6 负 playhead | G-7 bypass | G-8 双工程 | G-9 声道语义 | G-10 双组([J66],单 DAW) |
| ------------ | -------------- | -------------- | ------------- | --------- | ------------ | ------------ | ------------ | -------- | --------------- | ---------- | ---------- | ------------ | ----------------------- |
| REAPER 7.xx  |                |                |               |           |              |              |              |          |                 |            |            |              |                         |
| Cubase 14.x  |                |                |               |           |              |              |              |          |                 |            |            |              | n/a                     |
| Live 12.x    |                |                |               |           |              |              |              |          |                 |            |            |              | n/a                     |
| Studio One 7 |                |                |               |           |              |              |              |          |                 |            |            |              | n/a                     |

## 2c. 声道判定逐轨核对(G-9 ②,[J57])

| DAW          | V01-V13 channels | V14 channels | V15 channels | V14 L/R null(dBFS) | V15 L/R null(dBFS) | 改声道数后 gapCount 增量 | P/F |
| ------------ | ---------------- | ------------ | ------------ | ------------------ | ------------------ | ------------------------ | --- |
| REAPER 7.xx  |                  |              |              |                    |                    |                          |     |
| Cubase 14.x  |                  |              |              |                    |                    |                          |     |
| Live 12.x    |                  |              |              |                    |                    |                          |     |
| Studio One 7 |                  |              |              |                    |                    |                          |     |

## 3. 整体裁定

- S1-P1..P7、S1-P9、S1-P11..P18 与 S1-P21 全绿:是 / 否(R4 [J55] + v2.0 [J57] 口径)
- 裁定:PASS / FAIL
- S1-P20(L-5/F-1)不在本表裁定([J55] 非 S1 阻断,执行时点=T24);其结果回填 §6.1-H 发布勾选项
- 若 FAIL:失败项 ______;按 [J15] 的停线粒度处置 ______;触发的降级路径 ______(11-risks §3.1/§4;预选 F2+通路C,U11)

## 4. 派生的定值(必须回填到其他文档)

| 项                                      | 值                   | 回填目标                                                                                                                                   |
| --------------------------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| ring_frames                             | 1<<__                | 01 待定项②、spikes/s1/SegmentLayout.h → T06 的 src/core/ipc/SegmentLayout.h。[J57] 按 stereo 预算取值;同时记录本值表示的是帧对数还是样本数 |
| [J57] channels 判定逐轨结果             | 见 §2c 表            | ipc v1.4 §2、T06/T23 验收、11 A-63                                                                                                         |
| 无 timeInSamples 的宿主                 | 有/无:______         | 01 待定项③、11-risks A-03                                                                                                                  |
| 首块 timeInSamples 跨 pass/会话是否恒等 | 逐宿主:______        | 04 §2.1(是否需 ppq/tempo-map 锚定兜底)、11-risks A-03/A-57                                                                                 |
| 上游 PDC 错位样本数                     | 逐宿主:______        | 01(是否实现 IAudioPresentationLatency 检测告警)、11-risks A-04                                                                             |
| Studio One block size 差                | Input __ / Output __ | docs/daw-notes                                                                                                                             |
| 直通↔静音 ramp / 滞回实测值             | ramp __ms / 滞回 __s | 01 §5.1、07 T23 验收                                                                                                                       |

## 5. 搬进自动化测试的断言

- [ ] IPC-3/IPC-4(§2.3)已覆盖 R-6/R-7 的语义
- [ ] IPC-5b(套圈弃块)已覆盖 G-5;IPC-11a/11b + IPC-12a/12b(双阈值接管)已覆盖 G-1c;IPC-19(stereo interleaved 环)已覆盖 G-9 ①([J57]);IPC-20a/20b(组隔离)已覆盖 G-10 ①②([J66])
- [ ] spikes/s1/ 已由 T06 的 PR 删除(J16);scvb_diag 已迁到 tests/tools/ 常驻(不删除)
## 6. 收口报告(2026-08-16/17 实测,PR #30 已合入 feature/v1)

### 6.1 执行概况

- **宿主覆盖**:Cubase 14(本机)、Cubase 15 + REAPER 7(新机)。Live 12 跳过 / Studio One 7 可选对照(用户 08-14 裁定 U27)。
- **格覆盖**:C-1..C-14 全 ✅;G-1a..G-10 全 ✅(G-9③ 由压测 mono2stereo 覆盖);R-2..R-5/R-12 全 ✅。逐格实测与判读 = `docs/spikes/S1-daw-checklist.md`(完成列),证据 wav/csv 归档于调度者侧 workv7。
- **修复链 v1..v10**:mono/stereo 重建越界(v5/v6 几何快照+容量)、宿主异步销毁 AV(v4 寿命泄漏)、采样率切换总线静音(v7 槽位自愈)、channel 塌缩(v8 claimInputAuto)、re-prepare 迁槽幽灵槽 + 未认领永不重试(v9)、复制体同 pid 抢槽(v10 conflict 语义)。
- **离线验证**:单测 19 例 / 24262+ 断言;压测 5 场景(mono2stereo/flip/outputcheck/churn/multiclaim,组 8 隔离);gates 1-7 全绿(PluginOnly)。

### 6.2 关键定值(回填 T06)

- **ring_frames 默认 = 1<<19(524288 帧,按 stereo 预算 ≈10.9s @48k)**;SCVB_RING_FRAMES 可压到 8192 做套圈压测;
- **环容量规则**:段恒按 stereo 容量创建(CreateFileMapping 对已存在段忽略请求大小,必须读回真实段大小并 clamp 声道数);
- **认领语义**:自动认领只拿 Free / 陈旧+死 pid 接管;同 pid 对 Active 槽一律 conflict(复制体直通);本实例自己刷新走 updateOwnedInputSlot;
- **R-3 实测 render-ahead 上限 5000ms(REAPER)下 gap 全 0** → 生产 ring_frames 按 stereo 预算 1<<19 有余量;
- **G-5 实测**:8192 帧环 + render-ahead 5000ms → gap 大量被计数(最大 1110),无计数为 0 的撕裂;mask 部分掉位 = 健康回退按设计;
- **上游 PDC 实测**:Cubase 15 高延迟插件在 V03/V07 前插 → rt/offline 均零错位(offset 0),无需 IAudioPresentationLatency 检测告警(记录在案,发布矩阵兜底);
- **起播瞬态**:按播放瞬间一次性 ~350ms 缺口计数(全通道 ≈15 gap)为已知瞬态,之后零 gap;offline 导出存在 J32 注入延迟 200ms 的起始位移(产品级待办:T24 处理)。

### 6.3 遗留与去向

- spikes/s1/ 由 T06 的 PR 删除(J16);scvb_diag / scvb_nulltest / scvb_stress 迁 tests/tools/ 常驻;
- 声像法则残余(mix 为轨前直和 vs ref 过 pan law)属 spike 设计边界,T06 平衡引擎应用声像/电平。
