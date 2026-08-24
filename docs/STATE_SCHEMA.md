# STATE_SCHEMA —— SCVB state schema 与 abi 兼容规则(冻结契约)

> 状态: 冻结
> 最后更新: 2026-08-24(转正回填;依据 `params-v0.md` v2.2 §二/§三 + `ipc-contract-v0.md` §3 + 计划 04 §5)
> 真源: 本文件(由 `docs/constitution/params-v0.md` + 计划 04 §5 蒸馏转正)

> ⛔ **本文件是冻结契约。** 修改前必读 `CONTRIBUTING.md` §8 与 `CLAUDE.md` §7。未经批准的改动 PR 会被直接关闭。

仲裁规则:`docs/constitution/` 只读副本是**修订源**(改动须走修宪流程);本文件是**实现/审查基准**(06 §3.4 review bot 比对对象)。两者分歧时,以已冻结实现代码与 `tests/golden/` 快照为准。

本文件是 state 冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。state chunk 带 `abi` 字段:读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格(不得静默丢数据)。`setStateInformation` 处理的是用户工程文件里的不可信字节,长度/范围字段必须先校验再用于分配或索引。

## 一、Output state

分组与字段(YAML 视图,实际为版本化二进制/JSON chunk,编码见 §三):

```yaml
abi: 1
session_guid: <自生成>
group_id: 1..8               # [J66] 本 Output 所属组(默认 1,UI 显示 A-H);组=独立总线域
global:
  capture_enabled: bool        # 采集开关(默认 off)
  output_enabled: bool         # 输出开关:on=引擎驱动参数(write),off=follow host
  version_active: 1..2         # 当前版本(非自动化,防 write 自录;J59 4→2)
  range: {mode: follow|daw_loop|manual, start_s, end_s}   # 作用区间(J04:默认 follow)
analysis:
  vad: {threshold_db, hysteresis_db, hangover_ms, padding_pre_ms, padding_post_ms}   # 默认宁多勿少(J23 拆分,默认 120/200)
  segmentation: {mode, sensitivity, min_segment_ms}
  transition_ramp_ms: 80
  loudness_mode: "kw_integrated"|"rms"|"peak_dbfs"            # [J69/U24①] 第二响度指标口径,默认 "kw_integrated"
  center_slot_policy: "priority_queue"|"lead_exclusive"|"even_spread"   # [J69/U24④] 中心槽策略,默认 "priority_queue"
channels[15]:                  # 配置唯一真源在 Output(ADR-004);[J59] 10→15
  enabled: bool
  label: string                # UI 显示名
  source_channels: 1|2         # [J57] 自动检测:mono/stereo 源
  participate_in_auto_pan: bool # [J60] stereo 默认 false,mono 默认 true
  priority: 0..10              # 宽度优先级,高→角度大
  lead_lock: bool              # 分析期主唱配置(逐段可变;与 lead_select 参数为两层,J58)
  lead_vol_exempt: bool        # 音量豁免——独立选项(J58 用户澄清)
  pair_id: 0|1..7              # 成对关联(0=无;15 轨最多 7 对)
versions[2]:                   # [J59] 4→2;name: string(J05,默认 "V1"/"V2");复制语义不变
  curves_per_track[15]:        # 分析产物:分段时间线曲线(真身,ADR-005)
    segments[]: {t0_samples, t1_samples, pan, vol_db, origin: auto|user_edited|user_created, locked: bool}   # J34
  pan_curve:                   # pan 角度域增益曲线(EQ 式)
    points[]: {angle: -100..100, gain_db, shape: bell|shelf|cut, q, side: out|left|right}   # J07
features:                      # 采集特征(ADR-007);编码见 §四
  embedded: bool               # 超 8MB 转 sidecar
  per_channel[]: {hop_ms: 10, kw_mean_square[], peak[], vad_posterior[], coverage_ranges[]}
ui: {scale, language, active_tab, master_chart_mode, guide_seen, tour_seen}   # J50/J62/J75
```

### `ui.master_chart_mode`([J75] T43;变更文档 20260825-master-chart-mode)

> 与 T39a(#75)转正回填的 §一 YAML 视图握手:字段已并入上方 `ui` 子树,此处字段定义与
> 迁移语义以变更文档 `docs/contract-changes/20260825-master-chart-mode.md` 为唯一口径。

| 项 | 定义 |
|---|---|
| 路径 | `ui.master_chart_mode` |
| 类型 | 字符串枚举 `"distribution" \| "trajectory"` |
| 默认值 | `"distribution"`(05 J75 A 逐字:「默认 `distribution`」) |
| 语义 | `distribution` = 既有的声像/音量分布图;`trajectory` = 新增的 pan 轨迹图(x = 工程时间线,y = pan 角度域) |
| 自动化 | 否。不占参数面,不可被 DAW 录制 |
| 持久化 | 是,随工程走(与 `ui.active_tab` 同族:重开面板恢复上次视图) |
| 运行时态 | 否(不同于 `print_guard` / `recapture` / `analysis_run` 三件) |

**迁移语义**(abi 不递增):
- 读到没有该键的旧工程 → 默认 `"distribution"`,不报错、不提示。
- 读到未知取值 → 回落 `"distribution"`。
- 不需要写迁移函数:字段是纯增量、有确定性默认值,不改变任何既有字段的编码或含义。
- 编码落点:CFGS chunk 尾字段(u32,`0`=distribution / `1`=trajectory);旧 chunk 无该 4 字节即默认档。

## 二、Input state

```yaml
abi: 1
group_id: 1..8               # [J66] 本轨所属组(默认 1);同一人声轨只能属一组
channel_id: 0..15             # 本轨绑定的 channel;0=未分配(J01);[J59] 上限 15
ui: {scale, language}
```

其余一切配置从 Output 经控制面 IPC 读写(Input UI 只是远程视图)。

## 三、编码与兼容(state chunk 容器)

`getStateInformation` 输出(容器 = 定长头 + TLV 块,混合自定义二进制与 ValueTree):

```text
偏移  字段
0     u32 magic = 'SCVB' (0x42564353,小端内存序;写盘后前 4 字节字面拼出 "SCVB")
4     u32 abi   = 1                       # 与 IPC abi 独立计数
8     u32 flags = 0
12    u32 chunkCount
16..  TLV 块 × N: { u32 fourcc; u32 sizeBytes; u8 payload[size]   # 4 字节对齐 }
```

| fourcc | 内容 | 编码 |
|---|---|---|
| `PRMS` | APVTS 参数树(**123** 参数值 [J59/J65])+ ui{scale, language, active_tab, guide_seen, tour_seen} | ValueTree 二进制 |
| `CFGS` | group_id / global / analysis(vad / segmentation / transition_ramp_ms / loudness_mode / center_slot_policy)/ channels[15] —— 含 source_channels / participate_in_auto_pan / print 设置 | ValueTree 二进制 |
| `CRVS` | versions[2] 曲线真身 + pan_curve + versionMeta(含 name)+ 每轨 excluded_ranges | 自定义紧凑二进制(u16 minor 版本;segment = {i64 t0, i64 t1, f32 pan, f32 vol_db, u32 flags}) |
| `FEAT` | 特征流(per-channel kw_ms/peak/vad_posterior/coverage);embedded 标志与 sidecar 引用 | zlib(RFC 1950,miniz),节内编码见 §四 |

- **未知 fourcc 的块在 load 时原样保留、save 时原样回写**(前向小版本兼容);不设独立 SDCR chunk——sidecar 引用是 FEAT 节内 embedded=0 分支。
- **迁移函数框架**:`StateLoadStatus { Ok, Migrated, RejectedNewer, Corrupt }`。load 流程:① 校验 magic/长度 → Corrupt(拒载,保持默认态,UI 报错);② abi > 当前 → RejectedNewer(以默认状态运行 + UI 横幅提示升级;`preservedOriginal` 保留整个 blob,getStateInformation 原样回写,**绝不**让旧插件重写毁掉新版数据);③ abi < 当前 → 依次执行迁移函数升格 → Migrated;④ 逐 TLV 解析,未知 fourcc 存入 unknownChunks(save 时回写)。
- **同 abi 但 CRVS minor 更高(>kCrvsMinorVersion)→ 等同拒载**:`decodeCrvs` 只拒解本块,容器级 loadState 仍返回 Ok,但 Output 接线层必须按「等同拒载 + `preservedOriginal` 原样回写 + 提示升级」处理,**不得让旧插件抹掉新版曲线真身**(StateCodec.h 挂账)。
- **Input 插件 state 同用此容器**(abi 独立演进),只含 `PRMS`(无参数,仅 ui)+ `CFGS`(group_id + channel_id)。

## 四、FEAT 节编码与 sidecar 契约(转正项)

### 4.1 FEAT 节布局(little-endian)

```text
FeatSection(压缩前布局):
  u32 tag = 'FEAT'
  u16 codecVer = 1                  # 节内独立版本,硬失效判据之一
  u16 flags                         # bit0: embedded(1=特征体随节内嵌,0=引用 sidecar);bit1: vadPresent([J06] 可选缓存)
  u32 sampleRate                    # 采集时采样率
  u32 hopMs = 10
  u8  channelCount
  -- embedded=1 时,每 channel 依次:
     u8  channelId; u32 rangeCount
     rangeCount × { u64 beginHop; u64 endHop }          # 即 coverage_ranges
     数据体(只存覆盖 hop,按 range 顺序串联,SoA 列式):
       i16 kw_dBq[总覆盖 hop 数]
       i16 peak_dBq[...]
       u8  vadPosterior[...]        # [J06] 可选缓存:flags bit1=0 时省略;可由 kw 按 ADR-008 重算
  -- embedded=0 时,代之以:
     char sessionGuid[36]; u8 sha256[32]; u64 sidecarBytes
整节经 zlib(RFC 1950,miniz,window_bits=15)压缩后放入容器或 sidecar 文件。
```

- 列式(SoA)+ 0.01dB 量化是压缩友好排列;压缩算法选 **miniz,zlib 格式 RFC 1950**(`window_bits=15`;**不是** gzip RFC 1952、**不是** JUCE 内置;零新依赖,ADR-011 栈内)。
- 加载:codecVer 高于当前 → 该节按空处理 + 提示升级(容器级 abi 规则归 §三);低版本 → 迁移函数。

### 4.2 8MB↔sidecar 切换与回滞

- 阈值判定用**压缩后字节数**;加**回滞**防止在 8MB 附近反复横跳:**一旦转为 sidecar,压缩后 <6MB 才收回内嵌**。
- `getStateInformation()`:① 容器序列化配置/曲线各节 → ② `FeaturesCodec::encode()` → zlib 压缩 → gz → ③ gz ≤ 8MB:embedded=1 内嵌(若存在旧 sidecar → 删除,数据已随工程,防双源分叉);gz > 8MB:走 §4.3 sidecar 流程,节内只写 GUID+sha256+size。
- `setStateInformation()`:embedded=1 → 解码入 FrameStore(重建 CoverageMap);embedded=0 → 按 §4.3 定位 sidecar → sha256 校验 → 通过则解码,失败/缺失 → FrameStore 置空 + UI 横幅「采集数据缺失/过期,请重新采集」。**分段/曲线/配置正常加载不受影响**(sidecar 是缓存不是真相)。

### 4.3 sidecar 目录契约

- **session_guid**:Output 首次 `getStateInformation()` 时 `juce::Uuid().toDashedString()` 自生成,永久随 state(VST3 无工程路径 API,这是唯一可靠方案);Input 不持有 GUID。
- **路径**:`File::getSpecialLocation(userApplicationDataDirectory)` → Windows `%APPDATA%\Synchain\SCVB\sessions\<GUID>\`(macOS 后续 `~/Library/Application Support/...` 同构)。
- **目录内容**:`manifest.json`、`features.bin.gz`(扩展名沿用 .gz,内容为 zlib RFC 1950)、`owner.lock`。
  - `manifest.json`:{schemaVersion, codecVer, createdAt, savedAt, sha256, bytes, channelCount, hostName}
  - 原子写:`features.bin.gz.tmp` 写完 → rename 为 `features.bin.gz`(单文件含全部 channel,布局同 §4.1 embedded 体)
  - `owner.lock`:{pid, processStartTime, heartbeatIso8601},sidecar 模式下 Output 每 10s 由消息线程刷新;判活 = pid 存在 ∧ 心跳 < 30s
- **copy-on-write**:工程复制且两份同时打开 → 后开者检测到 owner.lock 活且 pid 非己 → 生成 newGUID、复制 sidecar 目录、本实例改用 newGUID;先后打开 → 共享同一 sidecar,任一方重采集保存后另一方 sha256 不匹配 → 按「缺失」处理(曲线无损,提示重采集)。
- **孤儿会话清理**(设置页「清理 30 天未访问会话」)推 v1.1;v1 在文档写明手动路径。
