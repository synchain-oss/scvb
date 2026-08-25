# PARAMETERS —— SCVB 自动化参数表(冻结契约)

> 状态: 冻结
> 最后更新: 2026-08-24(转正回填;内容依据 `docs/constitution/params-v0.md` v2.2)
> 真源: 本文件(由 `docs/constitution/params-v0.md` 蒸馏转正)

> ⛔ **本文件是冻结契约。** 修改前必读 `CONTRIBUTING.md` §8 与 `CLAUDE.md` §7。未经批准的改动 PR 会被直接关闭。

仲裁规则:`docs/constitution/` 只读副本是**修订源**(改动须走修宪流程);本文件是**实现/审查基准**(06 §3.4 review bot 比对对象)。两者分歧时,以已冻结实现代码与 `tests/golden/` 快照为准。

本文件是自动化参数冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。内容蒸馏自 `docs/constitution/params-v0.md`(v2.2,J57-J66 修宪:2 版本 × 15 轨 × 4 参数)。ParamID / index / 顺序 / skew 永久冻结(冻结点 = 首个公开 rc)。

## 一、Output 插件:自动化参数(共 **123** 个,全部 versionHint=1)[J59/J65]

排序规则:index 0-2 = 全局三件;之后按 版本 v(1..2)→ 轨道 t(1..15)→ (Pan, Vol, Width, Freeze) 展开。
ParamID = `width` / `ms_balance` / `lead_select` / `v{v}_t{t:02d}_{pan|vol|width|freeze}`。
index 公式:`3 + (v-1)*60 + (t-1)*4 + k`,k∈{0=pan,1=vol,2=width,3=freeze}。

| index | ParamID | 显示名 | 范围 | 默认 | 说明 |
|---|---|---|---|---|---|
| 0 | `width` | Width | 0..150 % | 100 | 全局期望宽度(几何角度缩放系数) |
| 1 | `ms_balance` | MS Balance | -100..+100 | 0 | 总线 M/S 音量比(0=不变,负偏 M 正偏 S)[J58 需求组] |
| 2 | `lead_select` | Lead Select | 0..15(int,step 1) | 0 | 0=遵循分析;1-15=强制该轨实时居中(**不**联动音量豁免,J58) |
| 3 | `v1_t01_pan` | V1 T01 Pan | -100..+100 | 0 | mono:equal-power 点;stereo:弧中心(dual-pan,J57) |
| 4 | `v1_t01_vol` | V1 T01 Vol | -24..+12 dB | 0 | 段音量推子 |
| 5 | `v1_t01_width` | V1 T01 Width | 0..100 % | 100 | stereo:源宽度(0=收成 mono);mono:v1 no-op 占位(注明) |
| 6 | `v1_t01_freeze` | V1 T01 Freeze | 0..3(int,step 1) | 0 | [J65] 0=全自动/1=冻结pan/2=冻结vol/3=全冻结;UI 显示为两个独立开关写同一参数;冻结维度引擎不驱动、host/手动权威 |
| 7..62 | `v1_t02_pan` … `v1_t15_freeze` | … | 同上 | | 版本1 其余轨道(每轨 4 个) |
| 63..122 | `v2_t01_pan` … `v2_t15_freeze` | V2 … | 同上 | | 版本2(15 轨 × 4) |

- 宿主可见 **124**(+wrapper 合成 bypass,J02 双口径);Ableton Live 128 上限余 **4——预算封顶**
- ParameterGroup(units):`Version {v}` → `Track {t:02d}`;全局三件在根组
- Pan/Width skew 线性;Vol dB 线性显示(skew=1.0,J03);lead_select/freeze 离散步进
- **绝对禁止**再增加自动化参数(余量仅 4,J65 后封顶);新需求一律走 state
- 引擎 write 打印面不变:仍仅 30 条(15 轨×pan/vol,J63);freeze 由用户驱动,引擎不打印

## 二、Output 插件:state(非自动化)

分组与字段(YAML 视图,实际为版本化二进制/JSON chunk,编码见 STATE_SCHEMA.md):

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
  participate_in_auto_pan: bool # [J60] stereo 默认 false,mono 默认 true;参与时以中心点入槽位分配
  priority: 0..10              # 宽度优先级,高→角度大
  lead_lock: bool              # 分析期主唱配置(逐段可变;与 lead_select 参数为两层,J58)
  lead_vol_exempt: bool        # 音量豁免——独立选项,不与任何 lead 机制强制关联(J58 用户澄清)
  pair_id: 0|1..7              # 成对关联(0=无;15 轨最多 7 对)
  # auto_pan/auto_vol 已删除(J65):被每轨 freeze 自动化参数取代
versions[2]:                   # [J59] 4→2;name: string(J05,默认 "V1"/"V2");复制语义不变
  curves_per_track[15]:        # 分析产物:分段时间线曲线(真身,ADR-005);[J59] 10→15
    segments[]: {t0_samples, t1_samples, pan, vol_db, origin: auto|user_edited|user_created, locked: bool}   # J34
  pan_curve:                   # pan 角度域增益曲线(EQ 式)
    points[]: {angle: -100..100, gain_db, shape: bell|shelf|cut, q, side: out|left|right}   # J07
features:                      # 采集特征(ADR-007)
  embedded: bool               # 超 8MB 转 sidecar
  per_channel[]: {hop_ms: 10, kw_mean_square[], peak[], vad_posterior[], coverage_ranges[]}
ui: {scale, language, active_tab, guide_seen, tour_seen}   # J50/J62
```

## 三、Input 插件:state(无自动化参数)

```yaml
abi: 1
group_id: 1..8               # [J66] 本轨所属组(默认 1);同一人声轨只能属一组
channel_id: 0..15             # 本轨绑定的 channel;0=未分配(J01);[J59] 上限 15
ui: {scale, language}
```

其余一切配置从 Output 经控制面 IPC 读写(Input UI 只是远程视图)。

## 四、命名与兼容规则

- ParamID 字符串与 index 双冻结;VST3 参数 ID 由 JUCE 从 ParamID hash——**首个 release 后不可改 ParamID**
- state chunk 带 `abi` 字段;读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格
- 显示名可在 UI/i18n 层变化,ParamID/index 不动
