# PARAMETERS —— SCVB 自动化参数表(冻结契约)

> 状态: 冻结
> 最后更新: 2026-09-30([J183] / [SL-585] **零参数面变更**,123 参数表逐字未改、ParamID / index / 顺序 / 范围 / 默认 / versionHint 一个字节未动:宪法 `params-v0.md` 升 v2.6,本文件的蒸馏版本号随之改 v2.6;Monitor 一条「不占自动化位」订正为「不占 123 的预算」(JUCE 合成的 bypass 在 VST3 里带 `kCanAutomate | kIsBypass`);§二 / §三 state 树向实现对齐 —— `abi` 由 1 改为当前值 6,Output 补 `analysis.applied` / `channels[].auto_label` / `lead_timeline[]` / `versions[].meta` / `segments[].manual_pan·manual_vol` / `excluded_ranges[]`、`ui` 补 `master_chart_mode` / `lang_chosen`,`capture_enabled` / `source_channels` 补落盘口径,Input `ui` 补 `guide_seen`(不随工程保存),新增 Monitor state 一小节;字段 / 编码 / abi 零变化,详见 `docs/contract-changes/20260930-j183-params-v0-v2.6-state-register.md`);上一次更新 2026-09-29([J167] / [SL-570] `lead_select` 的说明列:分析取值由「每个区间取宿主记录的多数值、整段套用」改为「在主唱变值的时刻切开区间,切换点 ±1 s 内有段边界 / 停顿就对齐过去」—— **零参数面变更**,ParamID / index / 顺序 / 范围 / 默认 / versionHint 一个字节未动,详见 `docs/contract-changes/20260929-j167-lead-switch-immediate.md`);上一次更新 2026-09-28(同日三处,均**零参数面变更**:① [J136] / [SL-216] `lead_select` 的说明列补「播放时记下的值进入分析」:选中轨在分析里按主唱锁处理、其余声部围绕它排布;实时居中覆盖照旧 —— ParamID / index / 顺序 / 范围 / 默认 / versionHint 一个字节未动,详见 `docs/contract-changes/20260928-sl216-lead-into-analysis.md`;② [SL-218] / [SL-219]:§四「读到高版本 → 拒载并提示升级」那条里 CRVS minor 一支的接线实况改实,详见 `docs/contract-changes/20260928-sl218-219-state-not-restored.md`;③ [J143b] / [SL-545] ① 那一说明列补取值规则 —— 只有宿主写进来的值记下的那几段算自动化、按记录取,其余取点分析那一刻的值(J143b 取代同日的 J143a「记录 ≥ 2 个不同的值才按记录」),详见 ① 那份变更文档「追加:J143 / J143a / J143b」);上一次更新 2026-09-26([J102] §二 state 树移出 `global.range`(运行期状态,不随工程走;**零参数面变更**),与 `docs/STATE_SCHEMA.md` §一 同步,详见 `docs/contract-changes/20260926-j101-j103-contract-withdrawals.md`);上一次更新 2026-09-15(**两处,均零参数面变更**):[SL-416] `analysis.vad` 五字段与 `transition_ramp_ms` 改为**随工程落盘**(§一 那两行补注;容器 abi 4→5,详见 `docs/contract-changes/20260914-sl416-vad-persist.md`);[SL-413] `analysis.segmentation.mode` 标注为 **v1 保留位** —— UI 不露出、引擎不消费、恒写 0=valley;§四 命名与兼容规则那条「读到高版本 → 拒载并提示升级」补上 [SL-412] 的接线实况。**123 参数表逐字未改**、ParamID / index / 顺序 / versionHint 一个字节未动(变更文档 `docs/contract-changes/20260914-sl413-seg-mode-reserved.md`);上一次更新 2026-08-25(J81 修宪转正;内容依据 `docs/constitution/params-v0.md` **v2.3**——**123 参数表逐字未改**)
> 真源: 本文件(由 `docs/constitution/params-v0.md` 蒸馏转正)

> ⛔ **本文件是冻结契约。** 修改前必读 `CONTRIBUTING.md` §8 与 `CLAUDE.md` §7。未经批准的改动 PR 会被直接关闭。

仲裁规则:`docs/constitution/` 只读副本是**修订源**(改动须走修宪流程);本文件是**实现/审查基准**(06 §3.4 review bot 比对对象)。两者分歧时,以已冻结实现代码与 `tests/golden/` 快照为准。

本文件是自动化参数冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。内容蒸馏自 `docs/constitution/params-v0.md`(**v2.6**;本表 = J57-J66 修宪的结果:2 版本 × 15 轨 × 4 参数;J81 与 v2.4-v2.6 修宪对本表零变动)。ParamID / index / 顺序 / skew 永久冻结(冻结点 = 首个公开 rc)。

> **[J81] SCVB Monitor = 0 自动化参数。** 第三个 VST3 主目标(ADR-001 v2.1)**没有** `AudioProcessorValueTreeState`,`getParameters()` 恒为空,`getBypassParameter()` 为 `nullptr`;宿主自带 bypass 由 JUCE wrapper 合成,**不占 123 的预算**(它在 VST3 里带 `kCanAutomate | kIsBypass`,宿主照样能自动化它;[J183] 订正原句「不占自动化位」)。故下方 123 参数表对 Monitor 完全不适用,本次修宪对该表**逐字未改**。铁律见 ADR-001a;断言见 `tests/core/test_monitor_processor.cpp`。

## 一、Output 插件:自动化参数(共 **123** 个,全部 versionHint=1)[J59/J65]

排序规则:index 0-2 = 全局三件;之后按 版本 v(1..2)→ 轨道 t(1..15)→ (Pan, Vol, Width, Freeze) 展开。
ParamID = `width` / `ms_balance` / `lead_select` / `v{v}_t{t:02d}_{pan|vol|width|freeze}`。
index 公式:`3 + (v-1)*60 + (t-1)*4 + k`,k∈{0=pan,1=vol,2=width,3=freeze}。

| index | ParamID | 显示名 | 范围 | 默认 | 说明 |
|---|---|---|---|---|---|
| 0 | `width` | Width | 0..150 % | 100 | 全局期望宽度(几何角度缩放系数) |
| 1 | `ms_balance` | MS Balance | -100..+100 | 0 | 总线 M/S 音量比(0=不变,负偏 M 正偏 S)[J58 需求组] |
| 2 | `lead_select` | Lead Select | 0..15(int,step 1) | 0 | 0=遵循分析;1-15=强制该轨实时居中(**不**联动音量豁免,J58)。[J136] 走带播放时 Output 逐块记下它的值(state `LEAD` 块),分析时选中轨按主唱锁处理(居中、不占槽),其余声部围绕它排槽、平衡。[J143b] 取值按**写入来源**:只有宿主写进来的值(自动化回放 / 宿主参数面)记下的那几段算自动化 ⇒ 被它们盖到的地方按记录值,其余地方取点分析那一刻的值;一段都没有 ⇒ 整窗取点分析那一刻的值。插件界面、撤销 / 重做、载入工程写的值记下的段不看。[J167] 这样得到的主唱在哪一刻变,分析就在哪一刻切开(不再按区间取多数值整段套用):切换点 ±1 s 内有段边界(任一轨开始 / 停止发声处)就对齐到最近的那个,没有就对齐到 ±1 s 内活跃各轨合起来的能量谷(停顿;门槛与分段灵敏度的切分谷相同),都没有就在原位切;持续不到 0.15 s 的值不算一次换人。改了之后要重新分析才进段表 |
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
abi: 6                          # state 容器 abi,[J183] 登记时的值(= `src/core/state/StateCodec.h` 的 kCurrentAbi);往后的值、逐级沿革与迁移链以 STATE_SCHEMA.md §三 为准;与 IPC abi 独立计数
session_guid: <自生成>
group_id: 1..8               # [J66] 本 Output 所属组(默认 1,UI 显示 A-H);组=独立总线域
global:
  capture_enabled: bool        # 采集开关(默认 off);[J91] 布局里保留、恒写 0,载入一律为关 —— 采集态不随工程走
  output_enabled: bool         # 输出开关:on=引擎驱动参数(write),off=follow host
  version_active: 1..2         # 当前版本(非自动化,防 write 自录;J59 4→2)
  # range{mode, start_s, end_s} 不在本树:[J102] 运行期状态,不随工程走(见 STATE_SCHEMA.md §三「global.range」一条)
analysis:
  vad: {threshold_db, hysteresis_db, hangover_ms, padding_pre_ms, padding_post_ms}   # 默认宁多勿少(J23 拆分,默认 120/200);[SL-416] 五项**自本版起随工程落盘**(值域/默认真源 = 02 §0.3:−60..−10/默认 −38、3..12/6、100..600/250、20..400/120、50..400/200)
  segmentation: {mode, sensitivity, min_segment_ms}
  # mode = **v1 保留位**([SL-413],用户 2026-09-14 裁 ②;真源 02 §0.3):UI 不露出
  # (全仓没有对应控件)、引擎不消费(`SegmentationParams` 里没有 `mode`,恒按 valley 走 S1),
  # 生产路径恒写 0=valley;`vad_only` 的语义留待接线那张卡启用。**CFGS 布局与本条不升 abi**
  # (同窗口的 [SL-416] 已把容器 abi 升到 5,与本条互不相交),读侧仍按原样往返 ——
  # 「恒写 0」是生产路径的实况,不是编解码的不变量。
  # min_segment_ms = 每轨最短**自动**段长,两层生效([SL-414]):① VAD 核心丢短(02 §2.3 P1,
  # 在前后留白之前判定);② 段表兜底 —— 回写层成形的段表里短于它的 auto 段并入同轨**时间相接**
  # 的相邻段(值取被并入段;用户段/锁定段不参与)。手动段不受影响。整条时间线重新分析后,段表里
  # 不再有「短于它、且存在相接自动邻段」的自动段;两侧都不相接的孤立短段按设计保留 —— 两条路:
  # 选区/范围档重分析在窗边裁出的残段([SL-399 R8]),或邻段因与手动段冲突而整条落选后剩下的
  # 孤段(与裁剪无关,整条时间线重分析时同样会出)。
  transition_ramp_ms: 80                          # [SL-416] **自本版起随工程落盘**(§1.20:20..300,默认 80)
  loudness_mode: "kw_integrated"|"rms"|"peak_dbfs"            # [J69/U24①] 第二响度指标口径,默认 "kw_integrated"
  center_slot_policy: "priority_queue"|"lead_exclusive"|"even_spread"   # [J69/U24④] 中心槽策略,默认 "priority_queue"
  applied: {loudness_mode, center_slot_policy}   # [SL-279] 上次全量分析所用的那一档(取值域同上两行);当前 ≠ applied 即该项「需重新分析」,两项分开判
channels[15]:                  # 配置唯一真源在 Output(ADR-004);[J59] 10→15
  enabled: bool
  label: string                # UI 显示名
  auto_label: string           # [J150] 最近一次自动填进 label 的 DAW 轨道名;label 为空或等于它 ⇒ 跟随轨道名,否则 ⇒ 用户命名(不再被轨道名覆盖)
  source_channels: 1|2         # [J57] 自动检测:mono/stereo 源;不落盘(运行期每拍重测)
  participate_in_auto_pan: bool # [J83] 未显式设置一律 true(取代 J60 的按源声道推导);参与时以中心点入槽位分配
  priority: 0..10              # 宽度优先级,高→角度大
  lead_lock: bool              # 分析期主唱配置(逐段可变;与 lead_select 参数为两层,J58;[J136] lead_select 的播放记录在分析里与它走同一条路径)
  lead_vol_exempt: bool        # 音量豁免——独立选项,不与任何 lead 机制强制关联(J58 用户澄清)
  pair_id: 0|1..7              # 成对关联(0=无;15 轨最多 7 对)
  # auto_pan/auto_vol 已删除(J65):被每轨 freeze 自动化参数取代
lead_timeline[]:               # [J136/SL-216] lead_select 的播放记录 {t0_samples, t1_samples, lead: 0..15, automated: bool},按 t0 升序、互不重叠;automated = [J143b] 这段值是宿主写进来的;分析的输入(编码见 STATE_SCHEMA.md §三 `LEAD`)
versions[2]:                   # [J59] 4→2;name: string(J05,默认 "V1"/"V2");复制语义不变
  meta: {copied_from: 0|1..2, copied_at_ms}   # 最近一次「复制版本」的来源版本号与复制时刻(epoch 毫秒);0 = 没被复制过
  curves_per_track[15]:        # 分析产物:分段时间线曲线(真身,ADR-005);[J59] 10→15
    segments[]: {t0_samples, t1_samples, pan, vol_db, origin: auto|user_edited|user_created, locked: bool, manual_pan: bool, manual_vol: bool}   # J34;manual_* = [J162] 手动接管固定了这一段的哪一维
    excluded_ranges[]: {t0_samples, t1_samples}   # 删段防复活记录(02 §3.5 的设计:重分析候选与其中任一区间重叠过半即丢弃);v1 没有产生它的编辑操作、分析也不读它,随工程原样往返
  pan_curve:                   # pan 角度域增益曲线(EQ 式)
    points[]: {angle: -100..100, gain_db, shape: bell|shelf|cut, q, side: out|left|right}   # J07
features:                      # 采集特征(ADR-007)
  embedded: bool               # 超 8MB 转 sidecar(v1 默认关,恒内嵌;见 STATE_SCHEMA.md §4.2)
  per_channel[]: {hop_ms: 10, kw_mean_square[], peak[], vad_posterior[], coverage_ranges[]}
ui: {scale, language, active_tab, master_chart_mode, guide_seen, tour_seen, lang_chosen}   # J50/J62/J75/J81(scale / language 落 CFGS,master_chart_mode 落 UICF,其余落 PRMS;见 STATE_SCHEMA.md §三)
```

## 三、Input 插件:state(无自动化参数)

```yaml
abi: 6                          # Input 与 Output 共用容器 abi(同 §二)
group_id: 1..8               # [J66] 本轨所属组(默认 1);同一人声轨只能属一组
channel_id: 0..15             # 本轨绑定的 channel;0=未分配(J01);[J59] 上限 15
ui: {scale, language, guide_seen}   # [J80/J81] guide_seen 默认 false;[J177] guide_seen 不随工程保存(只在本次会话内有效);scale / language 随工程落盘
```

其余一切配置从 Output 经控制面 IPC 读写(Input UI 只是远程视图)。

**Monitor 插件 state**(0 自动化参数;[J183] 补登记):

```yaml
abi: 6                          # 与 Output / Input 共用容器 abi(同 §二)
group_id: 1..8                  # 观察哪一组(默认 1)
ui: {scale, language}           # 随工程落盘
```

编码复用 Input 的 state 布局(`channel_id` 恒写 0;载入时只做范围校验(≤15,越界整块拒载),不使用其值);与 Output / Input 在兼容处理上的差别见 STATE_SCHEMA.md §二「Monitor state」。

## 四、命名与兼容规则

- ParamID 字符串与 index 双冻结;VST3 参数 ID 由 JUCE 从 ParamID hash——**首个 release 后不可改 ParamID**
- state chunk 带 `abi` 字段;读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格(**本条是要求,不是「已接线」的事实断言**:[SL-412] 起 **Output 侧**已按 §2.9 发 `scvb.error{newerState}` 到红横幅④;**Input 侧同一通路**统筹 2026-09-16 裁「本版不做、另立卡封存」;CRVS minor 那一支:原样回写与琥珀横幅⑪(`stateNotFullyRestored`)已接([SL-524] / [SL-218]),横幅不专说「请升级」—— 实况口径见 `docs/STATE_SCHEMA.md` §三)
- 显示名可在 UI/i18n 层变化,ParamID/index 不动
