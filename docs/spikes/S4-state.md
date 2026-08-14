# S4 状态体量 spike 结果(ADR-014)

> 交付卡:T05 · agent opus 5 · 分支 feat/T05-s4-state-spike · base feature/v1(commit ee6d764)

## 0. 会话表头(10 §1.0 固定开场)

| 字段 | 值 |
|---|---|
| 日期 / 执行人 | 2026-08-13 / opus 5(T05 实施 agent) |
| SCVB commit SHA | ee6d764(base;本分支在其上新增 spike) |
| 构建配置 | Release / MSVC 19.29(VS2019 BuildTools 16.11)/ JUCE 8.0.8 / CMake 3.31.12 |
| 机器(参考机 U16) | **Intel Core Ultra 9 275HX(24 逻辑核)· 31.4GB RAM · Windows 11 家庭版中文版 10.0.26200** |
| 压缩栈 | <code>juce::GZIPCompressorOutputStream</code>(zlib,默认等级 6 / zlib 容器格式 RFC 1950;ADR-011 零新依赖) |

**口径说明**:夹具为**确定性合成特征**(固定种子 AR(1) 包络 + 短语门控,非真实人声素材);体量/耗时结论对真实人声为**保守侧**(真实人声静音段更多、相关性更强,gzip 只会更小更快)。真实素材对拍留 T21。

---

## 1. 结论摘要(先给答案)

| 验收 | 判据 | 实测 | 结论 |
|---|---|---|---|
| ① save→load→save 逐字节一致 | 两次 chunk 逐字节相等 | 1,277,964 B,两次相等 | ✅ **PASS** |
| ② 加载 <1s / 保存(含 gzip)<200ms | 满配 15 轨 × 5 分钟 | load max **10.77ms**;save max **67.65ms** | ✅ **PASS** |
| ③ 15 轨 × 5 分钟 gzip ≤2.25MB | 2,359,296 B | **1,225,082 B(1.168 MiB)** | ✅ **PASS** |
| ④ abi+1 拒载 + 原样回写 | 不毁高版本数据 | RejectedNewer + 回写逐字节一致 | ✅ **PASS** |
| ⑤ sidecar 删除后工程可开、曲线完好 | 曲线不受影响 | Ok + featuresMissing + 曲线逐位一致 | ✅ **PASS** |

**整体**:满配 state 五项验收全 PASS。①④(数据完整性)未触发 fail 停线;②③达标,**不**需要 zstd(codecVer=2)或降量化精度。

---

## 2. 夹具体量(满配口径 J59/J57)

| 数据 | 体量(实测) |
|---|---|
| 特征流 kw+peak+vad(5 B/hop,15 轨 × 5 分钟 = 30000 hop/轨) | **2,250,000 B(2.146 MiB)原始** |
| FEAT 节压缩前(含节头/每轨 coverage 头,SoA 列式) | 2,250,332 B |
| FEAT 节压缩后(zlib level 6) | **1,225,082 B(1.168 MiB)**,压缩率 **1.837×** |
| 2 版曲线 + 配置 + 参数(CRVS+CFGS+PRMS,紧凑二进制,不压缩) | ≈ 52 KB |
| **state chunk 总(内嵌)** | **1,277,964 B(1.219 MiB)** |

- **stereo 轨特征每轨一条**(BS.1770 多通道求和,J57):夹具 15 轨 = 13 mono + 2 stereo,特征仍是 15 条,不因 stereo 加倍。
- **净体量上升口径已验证**:版本 4→2(曲线减半)+ 轨道 10→15(特征 +50%)后,5 分钟满配总 chunk ≈1.22MB,**远低于 8MB 阈值**(见 §7 外推)。

---

## 3. 往返一致性(验收①)

<code>encode(fixture) → bytes1</code>(1,277,964 B)→ <code>decode(bytes1) → state2</code> → <code>encode(state2) → bytes2</code>;断言 <code>bytes1 == bytes2</code> 逐字节成立。

- 一致性来源:容器 TLV 字段序固定、padding 显式置 0、CRVS/CFGS/PRMS 定长小端编码、FEAT 经 zlib(固定等级/窗口,确定性)。f32 经 <code>memcpy</code> 往返逐位一致,故曲线结构相等即逐位相等。
- 未知 fourcc 的块在 decode 时原样保留、encode 时原样回写(03 §6.1 前向小版本兼容;spike 实现为「按出现序追加回写」,正式任务 T39a 需补位序保留)。

---

## 4. 耗时(验收②;计时方法学复用 T04 scvb_bench)

steady_clock ns + mean/p50/p95/p99/max(与 <code>tests/tools/scvb_bench</code> 的 <code>summarize</code> 逐字一致),预热 1 次后跑 10 次:

| 阶段 | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| save(含 gzip) | 67.16 ms | 67.24 ms | 67.65 ms | 67.65 ms | **67.65 ms** |
| load | 10.42 ms | 10.43 ms | 10.77 ms | 10.77 ms | **10.77 ms** |

- **加载 10.77ms ≪ 1s**;**保存 67.65ms < 200ms**。均不触碰 04 §5.2 的 zstd 评估阈值 → **codecVer=1 维持,无升级路径触发**。
- 保存耗时几乎全部来自 zlib gzip(2.25MB 输入);序列化曲线/配置/容器可忽略。

---

## 5. gzip 压缩率(验收③ + 04 §5.1 待验证项回填)

- 5 分钟 × 15 轨特征 gzip 后 **1,225,082 B**,≤ 2.25MB 判据,余量约 54%。
- **压缩率 1.837×**,落在 04 §5.1 的「gzip 后 ≈0.9–1.4 MB」估算区间内(1.168 MiB)。**回填真值:0.9–1.4MB 估算成立,实测取中段 1.17MB。**
- 04 §5.2 的「2–3×」是未验证的上乐观估计;本 spike 合成包络实测 1.84×。真实人声(更多静音/更强相关性)预期 ≥2×,但**不改变 ③ 的 PASS 结论**。

---

## 6. abi+1 拒载与 sidecar(验收④⑤)

**验收④**:将正常 chunk 的 abi 字段(offset 4,u32)从 1 改为 2 模拟高版本 blob → <code>decode</code> 返回 <code>RejectedNewer</code> 且 <code>preservedOriginal</code> 与输入逐字节一致;此后 <code>save()</code> 原样回写(不重编码),断言回写 == 输入。**不毁高版本数据**。

**验收⑤**:<code>forceSidecar</code> 强制走 sidecar(sessionGUID 固定)→ 写 <code>sessions/GUID/features.bin.gz + manifest.json + owner.lock</code>;sha256 校验通过后特征读回完整;<code>deleteRecursively()</code> 删目录后 → <code>decode</code> 返回 <code>Ok</code> + <code>featuresMissing=true</code> + 特征清空,**曲线(2 版 × 15 轨 segments/excluded_ranges)逐位完好**。验证「sidecar 是缓存不是真相」(01 §7.2 / 04 §5.3)。

---

## 7. 8MB 阈值外推(T21 设计输入)

实测 gzip 速率 = 1.225MB / 5min = **0.245MB/min**。压缩后触及 8MB(ADR-007 冻结阈值)→ **≈33 分钟**采集(≈14.7MB 原始特征)。

- **保存耗时的预警**:save 由 gzip 主导、随原始体量线性增长。实测 67.65ms@2.25MB,外推 **~15 分钟采集(≈6.7MB raw)即触碰 200ms 保存预算**,33 分钟(8MB)时 ≈440ms。若 T21 要支撑更久采集,需评估 **zstd(codecVer=2)** 或降量化精度——本卡 5 分钟基准不触发,故不停线,仅记为设计输入。
- 阈值切换的**回滞**(04 §7.3:sidecar→内嵌需 <6MB)与**压缩炸弹防护**(gzip 解压前长度上限)本 spike 未实现,留 T21/T39a。

---

## 8. fail 停线规则触发情况

**未触发**。①(逐字节一致)与 ④(abi+1 拒载+回写)均 PASS → 不满足「①或④失败停线」。②③达标 → 不触发「记入 T21」的降级路径,但 §7 的 8MB 外推作为**前瞻性设计输入**随 T21 转交。

---

## 9. 偏离说明

- **计时承载在 spike 自有目标** <code>scvb_s4_state</code>(链接 <code>juce_core</code>):gzip 计时必须在链 GZIPCompressorOutputStream 的目标内完成,而 <code>tests/tools/scvb_bench</code> 是「零 JUCE」定位的常驻工具、且不得依赖 T19 将删除的 spike 代码。计时**方法学**(steady_clock ns + mean/p50/p95/p99/max)与 scvb_bench 逐字一致,已在源码注释与 §4 标注。
- **夹具为确定性合成特征**,非真实人声(卡内「用真实人声素材实测」受离线环境限制);真实素材对拍留 T21。合成数据偏保守(1.84× vs 预期 2–3×),不影响任何 PASS 结论。
- **「gzip」实为 zlib 格式**:<code>juce::GZIPCompressorOutputStream</code> 默认 <code>windowBits=0 → MAX_WBITS</code>,产出 **RFC 1950 zlib** 容器(非 RFC 1952 gzip),<code>features.bin.gz</code> 扩展名仅具名;压缩率与真 gzip 差异 ≈ 数字节头部,可忽略。若未来需与外部工具互操作真 gzip,用其 <code>WindowBitsValues::windowBitsGZIP</code>。记 T21。
- **sidecar 强制路径用 <code>forceSidecar</code> 而非真实 >8MB 采集**:8MB 阈值本身是 ADR-007 冻结常量,spike 验证的是机制(写/读/删),不做 33 分钟真实采集(§7 已给外推)。
- **state schema 为 spike 自有结构**,未改冻结契约 <code>docs/STATE_SCHEMA.md</code>、未写正式迁移;abi+1 模拟 blob 用 spike 自有容器布局。

---

## 10. 交付物落位

- 满配 state 夹具 + 编解码 + 验收跑分:<code>spikes/s4/</code>(J16,由 T19 的 PR 删除):<code>StateSchema.h</code> / <code>StateFixture.{h,cpp}</code> / <code>StateCodec.{h,cpp}</code> / <code>ByteIo.h</code> / <code>S4StateBench.cpp</code> / <code>CMakeLists.txt</code>
- ctest 快速回环:<code>scvb_s4_state --quick</code>(gate 6 内注册)
- 本报告:<code>docs/spikes/S4-state.md</code>
