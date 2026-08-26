# SCVB 参数表——P1 宪法(自动化参数的 ID/顺序/命名冻结,冻结点=首个公开 rc)

状态:**v2.3**(2026-08-25,J81 修宪:state 侧 ui/analysis 组增补 + state 容器 abi 1→2,**自动化参数面 123 个零变动**;v0/v1/v2 历史见文末修订节)。03-params-automation.md 负责细化语义/默认值论证,但**不得**增删自动化参数、改 ID、改顺序。

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
- **[J81] 自动化参数面零变动的显式声明**:本次修宪(J81)并批的 9 份契约变更文档**没有一份**增删/改名/改序/改值域任何自动化参数。逐项核对:
  - viz 段(#89)、ctrl 广播区(#87)= IPC 面,不进参数面
  - SCVB Monitor(#94)= **0 自动化参数**(无 `AudioProcessorValueTreeState`,`getParameters()` 恒为空,`getBypassParameter()` 为 `nullptr`;宿主自带 bypass 由 JUCE wrapper 提供,不占自动化位)
  - `master_chart_mode`(#77/#80)、Input `guide_seen`(#84)、`lang_chosen`(#87)、`loudness_mode`/`center_slot_policy`(#81)= 全部走 state,非自动化
  - 建议表 CSV 导出(#91)**只读** `v{v}_t{tt}_width` 等既有参数,零 gesture、零写入
  - `setTrackManual`(#87)**改的是写入面而非参数集合**:它开始向当前激活版本的 `v{v}_t{tt}_pan|_vol` 落值(带 gesture),但 123 个参数的 ParamID / index / 值域 / 默认值一字未动,`tests/golden/params_v0.tsv` 不受影响
  - 结论:**index 公式、ParamID 全表、`tests/golden/params_v0.tsv` 与「Live 余 4、绝对禁止再加自动化参数」的封顶条款(J65)全部原样成立**

## 二、Output 插件:state(非自动化)

分组与字段(YAML 视图,实际为版本化二进制/JSON chunk,04 文档定编码):

```yaml
abi: 2                         # [J81/#81] state 容器 abi(kCurrentAbi=2);migrate_1_to_2(no-op)承接 abi=1;与 IPC abi 独立计数
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
  loudness_mode: "kw_integrated"|"rms"|"peak_dbfs"                     # [J69/U24① → J81 登记] 第二响度指标口径,默认 "kw_integrated"
  center_slot_policy: "priority_queue"|"lead_exclusive"|"even_spread"  # [J69/U24④ → J81 登记] 中心槽策略,默认 "priority_queue"
channels[15]:                  # 配置唯一真源在 Output(ADR-004);[J59] 10→15
  enabled: bool
  label: string                # UI 显示名
  source_channels: 1|2         # [J57] 自动检测:mono/stereo 源
  participate_in_auto_pan: bool # [J83 修订 J60] 未显式设置一律默认 true(参与);排除权=轨道页逐轨开关(J60 的检测前提=总线布局非素材声道,不成立);参与时以中心点入槽位分配
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
ui: {scale, language, active_tab, master_chart_mode, guide_seen, tour_seen, lang_chosen}   # J50/J62/J75/J81
```

**[J81] Output `ui` 组新增字段说明**

| 字段 | 类型 / 取值 | 默认 | 全局镜像位 | 来源 |
|---|---|---|---|---|
| `master_chart_mode` | `"distribution"` \| `"trajectory"` | `"distribution"` | 无 | J75 / T43 / #77+#80 |
| `lang_chosen` | bool(**可选**) | `false`(缺失即 false) | **有**:`lang_chosen_global` | T37 v4 实测 P1-6 / #87 |

- **`master_chart_mode`**:Tab1「声像 / 音量分布」卡片的视图态,与 `active_tab` 同族(纯显示偏好,随工程走)。读到没有该键的旧工程、或读到未知取值(手改工程文件 / 跨版本)一律回落 `"distribution"`,**不报错、不提示**。不需要迁移函数,`abi` 不因它递增。
- **`lang_chosen`**:首启语言选择卡的抑制位。**`ui.language` 本身不能兼任** —— 它默认 `"en"`,「从没选过」与「用户就是选了英文」不可区分。**不新增桥函数**:置位点在既有 `setLang` 桥入口(web 启动时的语言回填走 `setLang(..., {push:false})` 不经桥,所以「桥的 setLang 被调用过」正好等价于「用户显式选过语言」)。
- **全局镜像位口径(J50a 同款,本次扩用到 lang)**:`guide_seen_global` / `tour_seen_global` / `lang_chosen_global` 均为**系统级用户目录小文件**里的判定位,**只读、不属工程 state、不回写 state**;新工程 `xxx_seen=false` 时先读全局默认决定是否弹出,「已看过 / 已选过」的承诺跨工程成立。落点 = `UiDefaultsStore`(`juce::PropertiesFile`,每次读写现开一份、不驻留进程内状态)。
- **两侧全局位各存一份**(`input.*` / `output.*` 分键):两侧引导讲的是两个界面、两套内容;共用一个位会让先装 Output 的用户永远看不到 Input 的引导 —— 而 J80 立 T48 的**全部理由**就是「Input 是用户见到的第一个界面却零引导」。`UiDefaultsStore` 的命名空间本来就按侧分(`scvb::output::uidefaults` / `scvb::input::uidefaults`)。

## 三、Input 插件:state(无自动化参数)

```yaml
abi: 2                       # [J81/#81] Input 与 Output 共用容器 abi(kCurrentAbi=2)
group_id: 1..8               # [J66] 本轨所属组(默认 1);同一人声轨只能属一组
channel_id: 0..15             # 本轨绑定的 channel;0=未分配(J01);[J59] 上限 15
ui: {scale, language, guide_seen}   # [J80/J81] guide_seen 默认 false;全局镜像位 guide_seen_global 按侧独立
```
其余一切配置从 Output 经控制面 IPC 读写(Input UI 只是远程视图)。

**[J81] Input `ui.guide_seen`**:Input 首启轻量引导([J80]:独立语言卡 + 5 步 mini tour)的**已读位**,bool,默认 `false`(首装 = 没看过)。拼写**逐字沿用 Output 侧的 `guide_seen`**,不新造 `input_guide_seen` 之类的名字 —— 两侧表达的是同一件事(只是引导内容不同),同一语义两个落点正是命名纪律要禁的;判据代码因此可两侧共用(`shouldShowLangStart` 就是一件共用的)。首启判据(两侧同构,J50a):**工程 `ui.guide_seen === false` 且 全局默认 `guide_seen_global === false`** 才弹。header「?」重看入口**不看本位**,已置位也能再开(与 Output 侧 `tour_seen` 的「重看引导」同款)。编码 = `InputStateCodec` 的 `InputState` 尾部追加 `u32 uiGuideSeen`(向后兼容的**追加段**:老工程按旧长度解码、该位取默认 0)。

## 四、命名与兼容规则

- ParamID 字符串与 index 双冻结;VST3 参数 ID 由 JUCE 从 ParamID hash——**首个 release 后不可改 ParamID**
- state chunk 带 `abi` 字段;读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格
- 显示名可在 UI/i18n 层变化,ParamID/index 不动
- **[J81] state 字段的编码落点注记**(宪法只登记「字段存在与语义」,编码细则归 04 §5 / 仓内 `STATE_SCHEMA.md`;此处只钉死落点,避免同一字段两处编码):
  - `ui.master_chart_mode` → **独立 fourcc 块 `UICF`**(`kFourccUiConfig`,定长 4 字节 u32:`0`=distribution / `1`=trajectory),**非 CFGS 尾字段**。选独立块的理由:反向兼容(新工程被旧版本读到)因此**零丢失** —— 旧版本不认识的 `UICF` 按容器「未知 fourcc 原样保留、save 原样回写」机制保真回写,只是不显示该偏好;若挂 CFGS 尾字段则会与 CFGS/CRVS 的解析纠缠
  - `ui.lang_chosen` → **`PRMS` 的 ValueTree**(与 `guide_seen`/`tour_seen`/`active_tab` 同处)。**不落 CFGS**:CFGS 是定长解码,追加字段会让旧构建整块拒载;ValueTree 增删字段零成本、老工程读不到即 false
  - Input `ui.guide_seen` → `InputStateCodec` 的 `InputState` **尾部追加 `u32`**(向后兼容追加段,老工程按旧长度解码取默认 0)。同批的 Output 侧对应改动是 CFGS 布局尾部追加 `u32 uiGuideSeen` / `u32 uiTourSeen`(此前这两位只活在运行时结构里,重开工程即回到「首启」—— T37 真机 bug)
  - `analysis.loudness_mode` / `analysis.center_slot_policy` → **CFGS 尾部追加两个 u32 枚举序号**(#81)。CFGS 已知字段之后若出现未知尾部(未来小版本追加),解码保留、编码原样回写(`unknownTail`),消除下次追加静默丢字段
- **[J81/#81] state 容器 abi 1 → 2**(与 IPC abi 独立计数):随上一条的两个 analysis 字段进 CFGS 尾部而升;迁移函数 `migrate_1_to_2` 为 **no-op**(abi=1 的 CFGS 无这两个尾字段,解码按「长度回退」回落默认 `kw_integrated` / `priority_queue`,无需重写 payload)。旧版(abi=1)读新(abi=2)blob → `RejectedNewer` → 整块原样回写 + 提示升级,**绝不静默降级**;**Input 与 Output 共用容器 abi**,故两侧 YAML 的 `abi` 同步升 2(Input CFGS 本身未变)。golden 新增 `tests/golden/state/abi2.bin`,**`abi1.bin` 保留**作迁移基线(两份并存,不是替换)

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
- **[J60]** channels[] 增 `participate_in_auto_pan`(stereo 默认 false / mono 默认 true)。**[J83,2026-08-26 修订]** 默认档改「未显式设置一律 true」——J60 的前提(检测值=素材声道)实测不成立(source_channels 取自总线布局,Cubase mono 素材置 stereo 轨即报 2),保护意图由轨道页逐轨开关承载;v5.1 实测 P0-B 定谳,仓内变更文档 20260826-j83-participate-default.md。
- **[J62]** ui 组增 `tour_seen: bool`(默认 false;首启交互式引导已完成标记,独立于 guide_seen;J50a 全局镜像同适用)。即 `ui: {scale, language, active_tab, guide_seen, tour_seen}`。

## v2.1 修订(2026-08-11,J65 每轨冻结开关;**追述节,2026-08-25/J81 补立**)

> **补立说明**:J65 的裁决说明写的是「A(修宪 params **v2.1**)」,ADR-004 正文亦引用「详见 params-v0.md v2.1」,但当时只就地改了 §一 正文(123 参数 / freeze 四态 / index 公式),**漏建 v2.1 修订节** —— 修订节从 v2.0 直接跳到 v2.2,ADR-004 的指向成了死链。本节按 J65 原裁决追述,**不改 §一 正文一个字**(那里早已是 J65 后的终态)。

- **[J65]** 每轨每版本增一个四态自动化参数 `freeze`(int 0..3,stepped:0=全自动 / 1=冻结 pan / 2=冻结 vol / 3=全冻结),UI 显示为两个独立开关**写同一参数**的两个位。**取代** state 的 `channels[].auto_pan` / `auto_vol`(概念单层:冻结 = 引擎不驱动该维度,host/手动权威)。
- 总量 93 + 30 = **123 声明 / 124 宿主可见**,Ableton Live 128 上限余 **4 —— 参数预算封顶**;ADR-004 的「禁止再加自动化参数」条款随之升为**绝对**。
- index 公式改为 `3 + (v-1)*60 + (t-1)*4 + k`,k ∈ {0=pan, 1=vol, 2=width, 3=freeze}。
- 引擎打印面**不变**:仍仅 30 条(15 轨 × pan/vol,J63);`freeze` 由用户驱动,引擎不打印。

## v2.2 修订(2026-08-11,J66 分组)

- **[J66]** Input 与 Output state 各增 `group_id: 1..8`(默认 1;UI 显示 A-H)。每组独立 IPC 域(ipc v1.5);参数表不变(123 per-实例,预算零影响)。

## v2.3 修订(2026-08-25,J81 修宪并批)

- **[J81c→§二 ui 组]** 增 `master_chart_mode`(J75/T43,默认 `"distribution"`,编码落点独立 fourcc `UICF`)与 `lang_chosen`(可选 bool,默认 false,编码落点 `PRMS` ValueTree,全局镜像位 `lang_chosen_global`)。即 `ui: {scale, language, active_tab, master_chart_mode, guide_seen, tour_seen, lang_chosen}`。
- **[J81d→§二 analysis 组]** 增 `loudness_mode`(默认 `"kw_integrated"`)与 `center_slot_policy`(默认 `"priority_queue"`)—— J69/U24①④ 当时只落到 03 文档层,本次补入宪法(仓内 `STATE_SCHEMA.md` 已先行登记,此为回追平)。
- **[J81e→§三 Input ui 组]** 增 `guide_seen`(J80/T48,默认 false;拼写逐字镜像 Output 侧,不新造名字;全局镜像位按侧独立)。即 `ui: {scale, language, guide_seen}`。
- **[J81f→§四]** 追加 state 字段的**编码落点**注记(`UICF` / `PRMS` / `InputState` 尾扩 / CFGS 尾扩),防同一字段两处编码。
- **[J81g→§一]** 追加**自动化参数面 123 个零变动**的显式声明(逐份变更文档核对);J65 的「Live 余 4、绝对禁止再加自动化参数」封顶条款原样成立;`tests/golden/params_v0.tsv` 不受本次修宪影响。
- **[J81j→§二/§三/§四]** state 容器 `abi` **1 → 2**(#81,已合入 `feature/v1` 的 `69ec45a`):CFGS 尾部追加 `loudness_mode` / `center_slot_policy` 两个 u32 枚举序号 + no-op `migrate_1_to_2` + CFGS 未知尾部保留回写(`unknownTail`)。Input 与 Output 共用容器 abi,故两侧 YAML 的 `abi` 同步升 2(Input CFGS 本身未变)。**与 IPC abi 无关** —— 那是 ipc-contract-v0 §5 的独立计数,本次 ipc v1.6 明确不 +1。
- **[J81 待办结转]** ①`storage` 组三字段(J79/T47 未开卡),不在本次;②Input 侧 `ui.lang_chosen` **不加**(裁 C5,超授权;挂账 `suggestion-ledger`,标「T48 真机观察后再定」);③geometry 不符与 `kAbiMismatch` 的区分:裁 C11 采 (b),v1 保持同码(理由入 ipc §6.5),`InitResult::kGeometryMismatch` 记入 ipc §5 **abi+1 增补清单**。本次修宪已收掉 adjudications 文末「[修宪待办登记]」块登记的三笔中的两笔(`master_chart_mode`、Input `guide_seen`);第三笔 `storage` 组随 T47 开卡再走。
