> 本文件是 masterPlan/constitution 的仓内只读副本，改动须走修宪流程（sha256 同步由 scripts/check-constitution-sync.ps1 断言）。
# SCVB 参数表——P1 宪法(自动化参数的 ID/顺序/命名冻结,冻结点=首个公开 rc)

状态:**v2.2**(2026-08-11,J57-J66 用户变更修宪:**2 版本 × 15 轨 × 4 参数**;v0/v1 历史见文末修订节)。03-params-automation.md 负责细化语义/默认值论证,但**不得**增删自动化参数、改 ID、改顺序。

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

分组与字段(YAML 视图,实际为版本化二进制/JSON chunk,04 文档定编码):

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

---

# v1 修订(2026-08-10,编号对应 plan/adjudications.md)

- **[J01]** Input `channel_id` 值域改 **0..10**,0=未分配(不 claim 任何 slot,UI 强制引导选择);首次插入默认 0。
- **[J02]** 参数计数双口径注记:**81=我方声明数,82=宿主可见数**(JUCE VST3 wrapper 自动合成 bypass);一切余量计算按 82 口径,S2 验证。
- **[J03]** Vol skew 显式冻结:**dB 线性(skew=1.0)**,0dB 位于行程 2/3 处(近传统推子手感)。
- **[J04]** `global.range.mode` 改三值枚举 **`follow|daw_loop|manual`**,默认 `follow`(全曲跟随:播放到哪采到哪,无预设终点)。
- **[J05]** `versions[]` 增 **`name: string`** 字段,默认 "V1".."V4",用户可命名。
- **[J06]** `vad_posterior[]` 标注为**可选缓存**(可由 kw_mean_square 按 ADR-008 离线重算,序列化时允许省略)。
- **[J07]** `pan_curve.points[]` 增 **`side: out|left|right`**(shelf/cut 方向,默认 out);tilt 效果用双 shelf 组合表达。
- **[J23]** `vad.padding_ms` 拆为 **`padding_pre_ms` / `padding_post_ms`**(默认 120 / 200)。
- **[J21]** ParamID/index/skew 冻结生效点 = **首个公开 rc(含)起**;rc 前允许修宪调整。

## v1.1 补充(2026-08-10,R1 补裁)

- **[J34]** `versions[].curves_per_track[].segments[]` 字段修订:`{t0_samples, t1_samples, pan, vol_db, origin: auto|user_edited|user_created, locked: bool}`(**替换** `manual_edited: bool`)。ADR-008「重分析不覆盖 manual/显式解锁」语义由 origin+locked 承载。

## v1.2 补充(2026-08-11,R2 收口裁决)

- **[J50]** Output `ui` 组增补字段:`guide_seen: bool`(默认 false,首启引导页已读标记)。即 `ui: {scale, language, active_tab, guide_seen}`。纯 state、rc 前增补零成本;03 §6.1 / 05 §1.4 / 07 T31 三处依赖据此落地。

## v2.0 修订(2026-08-11,J57-J60 用户变更;详见 adjudications)

- **[J59]** 自动化参数全表重排:81→**93**(2 版本×15 轨×Pan/Vol/Width + 全局 width/ms_balance/lead_select);versions[] 4→2;channels[] 10→15;旧 v1 表作废(rc 前重排合法,J21)。
- **[J57]** channels[] 增 `source_channels`(mono/stereo 检测);每轨 Width 参数承载 stereo 源宽度(dual-pan 模型)。
- **[J58]** 增全局 `lead_select` 自动化参数(实时覆盖层);`lead_lock`(分析期)与 `lead_vol_exempt`(独立豁免)保持 state,三者语义解耦。
- **[J60]** channels[] 增 `participate_in_auto_pan`(stereo 默认 false / mono 默认 true)。
- **[J62]** ui 组增 `tour_seen: bool`(默认 false;首启交互式引导已完成标记,独立于 guide_seen;J50a 全局镜像同适用)。即 `ui: {scale, language, active_tab, guide_seen, tour_seen}`。

## v2.2 修订(2026-08-11,J66 分组)

- **[J66]** Input 与 Output state 各增 `group_id: 1..8`(默认 1;UI 显示 A-H)。每组独立 IPC 域(ipc v1.5);参数表不变(123 per-实例,预算零影响)。