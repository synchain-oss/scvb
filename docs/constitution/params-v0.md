> 本文件是 masterPlan/constitution 的仓内只读副本，改动须走修宪流程（sha256 同步由 scripts/check-constitution-sync.ps1 断言）。
# SCVB 参数表——P1 宪法(自动化参数的 ID/顺序/命名冻结,冻结点=首个公开 rc)

状态:**v2.6**(2026-09-30,J183 修订:state 登记向已冻结的实现与 `tests/golden/` 对齐 —— §二 / §三 YAML 的 `abi` 由 2 改为当前值 6(此后四次升 abi 都没回写本文件);§二 撤回 `global.range` 的 state 登记([J102]);§二 补登实现早已落盘的 `analysis.applied`、`channels[].auto_label`、`lead_timeline[]`、`versions[].meta`、`segments[].manual_pan` / `manual_vol`、`curves_per_track[].excluded_ranges[]`,并给 `capture_enabled` / `source_channels` / `features.embedded` 补注实际的落盘口径;§三 补登 Monitor state;§四 补这些字段的编码落点;§一 订正 wrapper 合成 bypass 的说法;**字段 / 类型 / 默认值 / 编码字节 / 容器 abi / 自动化参数面全部零变动** —— 只把实现已有的东西登记进来,详见文末 v2.6 修订节;v2.5 = 2026-09-29,J177 修订:§三 / §四 撤回 Input `ui.guide_seen` 的编码落点 —— 该位不随工程保存、只在本次会话内有效,实现从没编码过它;同段不再写 Input 导览的步数(J177a);**字段 / 类型 / 默认值 / 首启判据 / 容器 abi / 自动化参数面全部零变动**;v2.4 = 2026-09-28 J160 修订:§二 Output `ui` 组「两侧全局位各存一份」一条的说明文字按 J132 改成实现的实际写法,**字段 / 默认值 / 编码落点 / 容器 abi / 自动化参数面全部零变动**;v2.3 = 2026-08-25 J81 修宪:state 侧 ui/analysis 组增补 + state 容器 abi 1→2,**自动化参数面 123 个零变动**;v0/v1/v2 历史见文末修订节)。03-params-automation.md 负责细化语义/默认值论证,但**不得**增删自动化参数、改 ID、改顺序。

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
  - SCVB Monitor(#94)= **0 自动化参数**(无 `AudioProcessorValueTreeState`,`getParameters()` 恒为空,`getBypassParameter()` 为 `nullptr`;宿主自带 bypass 由 JUCE wrapper 合成,不占 123 的预算 —— 它在 VST3 里带 `kCanAutomate | kIsBypass`,宿主照样能自动化它;[J183] 订正原句「不占自动化位」)
  - `master_chart_mode`(#77/#80)、Input `guide_seen`(#84)、`lang_chosen`(#87)、`loudness_mode`/`center_slot_policy`(#81)= 全部走 state,非自动化
  - 建议表 CSV 导出(#91)**只读** `v{v}_t{tt}_width` 等既有参数,零 gesture、零写入
  - `setTrackManual`(#87)**改的是写入面而非参数集合**:它开始向当前激活版本的 `v{v}_t{tt}_pan|_vol` 落值(带 gesture),但 123 个参数的 ParamID / index / 值域 / 默认值一字未动,`tests/golden/params_v0.tsv` 不受影响
  - 结论:**index 公式、ParamID 全表、`tests/golden/params_v0.tsv` 与「Live 余 4、绝对禁止再加自动化参数」的封顶条款(J65)全部原样成立**

## 二、Output 插件:state(非自动化)

分组与字段(YAML 视图,实际为版本化二进制/JSON chunk,04 文档定编码):

```yaml
abi: 6                         # state 容器 abi,v2.6 登记时的值(= kCurrentAbi);[J81/#81] 1→2,其后 [SL-279] / [SL-411] / [SL-416] / [SL-472] 各升一级,migrate_1_to_2 … migrate_5_to_6 均 no-op;往后的值与逐级沿革以仓内 STATE_SCHEMA §三 为准;与 IPC abi 独立计数
session_guid: <自生成>
group_id: 1..8               # [J66] 本 Output 所属组(默认 1,UI 显示 A-H);组=独立总线域
global:
  capture_enabled: bool        # 采集开关(默认 off);[J91] 布局里保留、恒写 off,载入一律为 off —— 采集态不随工程走
  output_enabled: bool         # 输出开关:on=引擎驱动参数(write),off=follow host
  version_active: 1..2         # 当前版本(非自动化,防 write 自录;J59 4→2)
  # range{mode, start_s, end_s} 不在本树:[J102] 运行期状态,不随工程走(v2.6 撤回登记;[J04] 的三值与默认 follow 仍是运行期语义)
analysis:
  vad: {threshold_db, hysteresis_db, hangover_ms, padding_pre_ms, padding_post_ms}   # 默认宁多勿少(J23 拆分,默认 120/200)
  segmentation: {mode, sensitivity, min_segment_ms}
  transition_ramp_ms: 80
  loudness_mode: "kw_integrated"|"rms"|"peak_dbfs"                     # [J69/U24① → J81 登记] 第二响度指标口径,默认 "kw_integrated"
  center_slot_policy: "priority_queue"|"lead_exclusive"|"even_spread"  # [J69/U24④ → J81 登记] 中心槽策略,默认 "priority_queue"
  applied: {loudness_mode, center_slot_policy}   # [SL-279 → J183 登记] 上次全量分析所用的那一档(取值域同上两行);当前 ≠ applied 即该项「需重新分析」,两项分开判
channels[15]:                  # 配置唯一真源在 Output(ADR-004);[J59] 10→15
  enabled: bool
  label: string                # UI 显示名
  auto_label: string           # [J150 → J183 登记] 最近一次自动填进 label 的 DAW 轨道名;label 为空或等于它 ⇒ 跟随轨道名,否则 ⇒ 用户命名(不再被轨道名覆盖)
  source_channels: 1|2         # [J57] 自动检测:mono/stereo 源;不落盘(运行期每拍重测)
  participate_in_auto_pan: bool # [J83 修订 J60] 未显式设置一律默认 true(参与);排除权=轨道页逐轨开关(J60 的检测前提=总线布局非素材声道,不成立);参与时以中心点入槽位分配
  priority: 0..10              # 宽度优先级,高→角度大
  lead_lock: bool              # 分析期主唱配置(逐段可变;与 lead_select 参数为两层,J58)
  lead_vol_exempt: bool        # 音量豁免——独立选项,不与任何 lead 机制强制关联(J58 用户澄清)
  pair_id: 0|1..7              # 成对关联(0=无;15 轨最多 7 对)
  # auto_pan/auto_vol 已删除(J65):被每轨 freeze 自动化参数取代
lead_timeline[]:               # [J136/SL-216 → J183 登记] lead_select 的播放记录 {t0_samples, t1_samples, lead: 0..15, automated: bool},按 t0 升序、互不重叠;automated = [J143b] 这段值是宿主写进来的;分析的输入
versions[2]:                   # [J59] 4→2;name: string(J05,默认 "V1"/"V2");复制语义不变
  meta: {copied_from: 0|1..2, copied_at_ms}   # [J183 登记;03 §5.3] 最近一次「复制版本」的来源版本号与复制时刻(epoch 毫秒);0 = 没被复制过
  curves_per_track[15]:        # 分析产物:分段时间线曲线(真身,ADR-005);[J59] 10→15
    segments[]: {t0_samples, t1_samples, pan, vol_db, origin: auto|user_edited|user_created, locked: bool, manual_pan: bool, manual_vol: bool}   # J34;manual_* = [J162 → J183 登记] 手动接管固定了这一段的哪一维
    excluded_ranges[]: {t0_samples, t1_samples}   # [J183 登记;02 §3.5 / 03 §6.1] 删段防复活记录(设计:重分析候选与其中任一区间重叠过半即丢弃);v1 没有产生它的编辑操作、分析也不读它,随工程原样往返
  pan_curve:                   # pan 角度域增益曲线(EQ 式)
    points[]: {angle: -100..100, gain_db, shape: bell|shelf|cut, q, side: out|left|right}   # J07
features:                      # 采集特征(ADR-007)
  embedded: bool               # 超 8MB 转 sidecar;v1 自动切换关闭,写出恒内嵌(仓内 STATE_SCHEMA §4.2)
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
- **两侧全局位各存一份**(指 `guide_seen` 的全局位):两侧引导讲的是两个界面、两套内容;共用一个位会让先装 Output 的用户永远看不到 Input 的引导 —— 而 J80 立 T48 的**全部理由**就是「Input 是用户见到的第一个界面却零引导」。两侧共用 `UiDefaultsStore` 的**同一个**落盘文件(`ui-defaults.settings`),靠**键名**分开 —— Output 用 `guide_seen_global`、Input 用 `guide_seen_global_input`;`tour_seen_global` 只有 Output 一个键(Input 没有交互式导览)。([J160] 按 J132 口径改写,见文末 v2.4 修订节)

## 三、Input 插件:state(无自动化参数)

```yaml
abi: 6                       # Input 与 Output 共用容器 abi(同 §二,v2.6 登记时 = 6);Input 自己的 state 布局从 T23 起没变过
group_id: 1..8               # [J66] 本轨所属组(默认 1);同一人声轨只能属一组
channel_id: 0..15             # 本轨绑定的 channel;0=未分配(J01);[J59] 上限 15
ui: {scale, language, guide_seen}   # [J80/J81] guide_seen 默认 false;全局镜像位 guide_seen_global 按侧独立;[J177] guide_seen 不随工程保存(只在本次会话内有效)
```
其余一切配置从 Output 经控制面 IPC 读写(Input UI 只是远程视图)。

**[J81] Input `ui.guide_seen`**:Input 首启轻量引导([J80]:独立语言卡 + mini tour,步数以仓内 `web/input/tour-in.js` 的 `TOUR_IN_STEPS` 为准)的**已读位**,bool,默认 `false`(首装 = 没看过)。拼写**逐字沿用 Output 侧的 `guide_seen`**,不新造 `input_guide_seen` 之类的名字 —— 两侧表达的是同一件事(只是引导内容不同),同一语义两个落点正是命名纪律要禁的;判据代码因此可两侧共用(`shouldShowLangStart` 就是一件共用的)。首启判据(两侧同构,J50a):**工程 `ui.guide_seen === false` 且 全局默认 `guide_seen_global === false`** 才弹。header「?」重看入口**不看本位**,已置位也能再开(与 Output 侧 `tour_seen` 的「重看引导」同款)。**不随工程保存**([J177]):本位只在本次会话内有效,每成功载入一份工程 state 就清零;跨工程的「看过了」由全局镜像位承担(Input 的键是 `guide_seen_global_input`,见 §二 末条)。原先登记的编码落点实现从没做过,已撤回,见文末 v2.5 修订节。

**Monitor 插件:state(无自动化参数)**([J81] #94 立项;[J183] 补登记):

```yaml
abi: 6                       # 与 Output / Input 共用容器 abi(同 §二)
group_id: 1..8               # 观察哪一组(默认 1)
ui: {scale, language}        # 随工程保存
```
Monitor 是只读观察器,state 只有这三项;不认领任何 channel,编码复用 Input 的 state 布局(`channel_id` 恒写 0、载入时不读),没有新字段、没有新 fourcc。编码细则与它和 Output / Input 在兼容处理上的差别见仓内 `STATE_SCHEMA.md` §二 Monitor 节。

## 四、命名与兼容规则

- ParamID 字符串与 index 双冻结;VST3 参数 ID 由 JUCE 从 ParamID hash——**首个 release 后不可改 ParamID**
- state chunk 带 `abi` 字段;读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格(本条是要求;三个插件各自的接线实况见仓内 `STATE_SCHEMA.md` §三 与 §二 Monitor 节)
- 显示名可在 UI/i18n 层变化,ParamID/index 不动
- **[J81] state 字段的编码落点注记**(宪法只登记「字段存在与语义」,编码细则归 04 §5 / 仓内 `STATE_SCHEMA.md`;此处只钉死落点,避免同一字段两处编码):
  - `ui.master_chart_mode` → **独立 fourcc 块 `UICF`**(`kFourccUiConfig`,定长 4 字节 u32:`0`=distribution / `1`=trajectory),**非 CFGS 尾字段**。选独立块的理由:反向兼容(新工程被旧版本读到)因此**零丢失** —— 旧版本不认识的 `UICF` 按容器「未知 fourcc 原样保留、save 原样回写」机制保真回写,只是不显示该偏好;若挂 CFGS 尾字段则会与 CFGS/CRVS 的解析纠缠
  - `ui.lang_chosen` → **`PRMS` 的 ValueTree**(与 `guide_seen`/`tour_seen`/`active_tab` 同处)。**不落 CFGS**:CFGS 是定长解码,追加字段会让旧构建整块拒载;ValueTree 增删字段零成本、老工程读不到即 false
  - Input `ui.guide_seen` → **不落 state chunk**([J177]):只在本次会话内有效,见 §三。`InputStateCodec` 的 payload 是严格等长解码,从来没有编码过这一位
  - `analysis.loudness_mode` / `analysis.center_slot_policy` → **CFGS 尾部追加两个 u32 枚举序号**(#81)。CFGS 已知字段之后若出现未知尾部(未来小版本追加),解码保留、编码原样回写(`unknownTail`),消除下次追加静默丢字段
  - **[J183] v2.6 补登字段的落点**(均为实现现状,不是新编码):`analysis.applied` → CFGS 尾部再追加两个 u32 枚举序号([SL-279],abi 2→3);`channels[].auto_label` → `PRMS` 根节点属性 `channels_auto_label`(15 元 JSON 字符串数组,[J150]);`lead_timeline[]` → 独立 fourcc 块 `LEAD`([J136] / [J143b]);`versions[].meta` → `CRVS` 每版本头(与 `name` 同处);`segments[].manual_pan` / `manual_vol` → `CRVS` 段 `flags` 的 bit3 / bit4([J162],段记录布局与 CRVS minor 不变);`excluded_ranges[]` → `CRVS` 每轨记录(紧跟该轨段表);Monitor state → 容器里一块 `CFGS`,payload 与 Input 同布局
- **[J81/#81] state 容器 abi 1 → 2**(与 IPC abi 独立计数):随上一条的两个 analysis 字段进 CFGS 尾部而升;迁移函数 `migrate_1_to_2` 为 **no-op**(abi=1 的 CFGS 无这两个尾字段,解码按「长度回退」回落默认 `kw_integrated` / `priority_queue`,无需重写 payload)。旧版(abi=1)读新(abi=2)blob → `RejectedNewer` → 整块原样回写 + 提示升级,**绝不静默降级**;**Input 与 Output 共用容器 abi**,故两侧 YAML 的 `abi` 同步升 2(Input CFGS 本身未变)。golden 新增 `tests/golden/state/abi2.bin`,**`abi1.bin` 保留**作迁移基线(两份并存,不是替换)。**[J183] 后续**:其后四次尾扩各升一级(2→3 [SL-279]、3→4 [SL-411]、4→5 [SL-416]、5→6 [SL-472],迁移函数均 no-op),v2.6 登记时 kCurrentAbi = 6,`abi1.bin` … `abi6.bin` 六份并存、`abi6.bin` 是当前格式锁;本条上文的「升 2」是 J81 当时的事实,不改

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

## v2.4 修订(2026-09-28,J160 说明文字改实)

- **[J160→§二 Output `ui` 组「两侧全局位各存一份」条]** 原括注「`input.*` / `output.*` 分键」与原末句「`UiDefaultsStore` 的命名空间本来就按侧分(`scvb::output::uidefaults` / `scvb::input::uidefaults`)」**与实现不符**:实现里没有 `input.*` / `output.*` 这种键,`UiDefaultsStore` 也只有一个命名空间 `scvb::uidefaults`、一个落盘文件。按 **J132** 口径(仓内 `docs/SCVB_CONTRACT.md` §3.1 与 `docs/STATE_SCHEMA.md` §二 已由 #301 改实)改成实现的实际写法:同一个落盘文件 `ui-defaults.settings`,两个键 —— Output `guide_seen_global`、Input `guide_seen_global_input`;`tour_seen_global` 只有 Output 一个键。「各存一份」的**结论不变**,错的只是描述「怎么分」的那半句;括注改为范围限定「指 `guide_seen` 的全局位」(紧上一条同时列了 `lang_chosen_global`,不限定就会被读成三个全局位都按侧分)。
- **口径说明**:J132 裁定行的备注写「单键口径」,那是 SL-258(#167)给 Input 另起 `guide_seen_global_input` 之前的事实;#301 已按实现写成两个键,本次与之一致(详见仓内 `docs/contract-changes/20260928-j132-guide-seen-global-keys.md` 的 ⚠ 节)。
- **零变动面**:字段、类型、默认值、编码落点、首启判据、state 容器 `abi`、自动化参数面(123 个)与 `tests/golden/` 一律不动;实现侧零字节(键名真源 `src/plugin-common/UiDefaultsStore.cpp`)。本文件除本节外只改了两处:状态行(v2.3 → v2.4)与 §二 该条的括注和末句。仓内变更文档 `docs/contract-changes/20260928-j160-constitution-guide-keys.md`。

## v2.5 修订(2026-09-29,J177 撤回 Input `ui.guide_seen` 的编码落点)

- **[J177→§三 / §四]** 撤回 v2.3([J81e] / [J81f])给 Input `ui.guide_seen` 登记的编码落点「`InputStateCodec` 的 `InputState` 尾部追加 `u32`」。实现从没做过:`InputStateCodec` 的 payload 是 4 个 `u32` + 语言字节,解码严格等长,容不下尾部多出的字节;这一位只活在 Input 实例的内存里,每成功载入一份工程 state 就清零。用户裁定按实现撤回、不补做 —— 跨工程的「看过了」本来就由全局镜像位(Input 键 `guide_seen_global_input`)承担,首启链里走完或跳过 mini tour 都会连全局位一起写。
- **本文件改了四处**(本节之外):状态行(v2.4 → v2.5);§三 YAML `ui` 行的行尾注释补一句;§三「[J81] Input `ui.guide_seen`」段:段首括注不再写导览步数(见下),末句(原为编码落点)改为「不随工程保存」;§四 编码落点注记里 Input 那一条改写。
- **§四 那一条里 Output 侧的半句一并删去**([J177a]):原文说「同批的 Output 侧对应改动是 CFGS 布局尾部追加」guide / tour 两个 `u32`,与紧上一条(`lang_chosen` 条)自述的「与 `guide_seen`/`tour_seen`/`active_tab` 同处」`PRMS` 相矛盾;实现在 `PRMS`(仓内 `src/output/OutputUiState.h`)。删的是一句与本文件自相矛盾的旧描述,Output 侧的落点与行为都不变。
- **[J177a→§三] 导览不再写步数**:§三 括注原写「独立语言卡 + 5 步 mini tour」;[J176] 给 Input 导览加一步(仓内 #344),写死的步数随之过期。改为「mini tour,步数以仓内 `web/input/tour-in.js` 的 `TOUR_IN_STEPS` 为准」—— 步数是界面内容,不是 state 契约,写在这里每加减一步就得修一次宪。[J80] 立轻量引导时定的 5 步是当时的基线,仓内历史变更文档里的「5 步」照原文保留。
- **零变动面**:字段名、类型、默认值、首启判据、全局镜像位、state 容器 `abi`、自动化参数面(123 个)与 `tests/golden/` 一律不动;实现侧零行为改动。仓内变更文档 `docs/contract-changes/20260929-j177-withdraw-input-uiguideseen.md`。

## v2.6 修订(2026-09-30,J183 state 登记向实现对齐)

- **起因**:[J182] 交叉验证的文档面在 dev `b813f52d`(v0.9.0-rc.1 里程碑)上发现本文件的 state 登记与已冻结的实现不一致。[J183] 裁:rc tag 前修宪(07 §6:tag 之前是唯一的修宪窗口),一律向已冻结的实现与 `tests/golden/` 对齐。
- **§二 / §三 `abi`:2 → 6**。v2.3(J81)写 2 时是对的;其后 [SL-279] / [SL-411] / [SL-416] / [SL-472] 四次 CFGS 尾扩各升一级,每次都只改了仓内 `STATE_SCHEMA.md`,没回写本文件。现写 v2.6 登记时的值 6,并写明往后的值以 `STATE_SCHEMA.md` §三 为准 —— 本文件只登记存在与语义,不追每一次升级。实现:`src/core/state/StateCodec.h` 的 `kCurrentAbi = 6u`;golden `tests/golden/state/abi6.bin` 头部的 abi 字段为 6。
- **§二 撤回 `global.range`**:[J102](2026-09-22,用户裁「契约撤回」)已把它移出仓内 `STATE_SCHEMA.md` / `PARAMETERS.md` 的 state 树 —— 实现有意不存它(载入带 CFGS 的工程时复位到默认 follow,理由见 J102),本文件当时漏改。改为一行注释说明它不在本树;[J04] 的三值枚举与默认 follow 作为运行期语义保留(v1 修订节的 [J04] 条是历史,不改)。
- **§二 补登**(实现早已落盘、仓内 `STATE_SCHEMA.md` 早已登记、本文件没有):
  - `analysis.applied{loudness_mode, center_slot_policy}`([SL-279],CFGS 尾扩,abi 2→3);
  - `channels[].auto_label`([J150],`PRMS` 根节点属性 `channels_auto_label`,不动 abi);
  - `lead_timeline[]`([J136] / [SL-216] 的新块 `LEAD`;[J143b] 把块的 minor 升到 2、加了 `automated`;不动 abi);
  - `segments[].manual_pan` / `manual_vol`([J162] / [SL-548],`CRVS` 段 `flags` 的 bit3 / bit4;段记录布局、CRVS minor、abi 都不动);
  - `curves_per_track[].excluded_ranges[]`:没有单独的裁定号 —— 计划层审查时把它的唯一编码落点定在 03 §6.1 `CRVS` 每轨记录(02 §3.5 / SEG-6 的删段防复活),T19 起就在 CRVS 布局里。v1 没有产生它的编辑操作(五个段编辑 op 里没有「删除」),分析路径也不读它;登记的是「布局里有、随工程原样往返」,不是「已经在用」;
  - `versions[].meta{copied_from, copied_at_ms}`:03 §5.3「复制版本」写下的元数据(T18 引入),T19 起在 `CRVS` 每版本头里、与 `name` 同处。这一项不在 J183 点名的清单里,是按「实现往 state 里写了什么」逐块枚举时补出来的,与上面几项同属一类。
- **§二 补注落盘口径**(字段本身不动,只写明实际口径):`capture_enabled`([J91]:布局里保留、恒写 off,载入一律 off);`source_channels`(不落盘,运行期每拍重测);`features.embedded`(v1 自动切换关闭,写出恒内嵌,[SL-395])。
- **§三 补登 Monitor state**:[J81] 立项的第三个插件,state = `group_id` + `ui{scale, language}`,复用 Input 的 state 布局。当时的仓内变更文档 `20260825-monitor-target.md` 因此写「STATE_SCHEMA 零新增」—— 布局确实没有新增,但「Monitor 有 state、用的是这个布局」一直没有登记。
- **§四**:编码落点注记追加一条(上列补登字段的落点);「读到高版本 → 拒载并提示升级」一条补括注(它是要求,各插件的接线实况见仓内 `STATE_SCHEMA.md`);[J81/#81] abi 那一条末尾补「后续」一句,原文不改。
- **§一 bypass 一句**:「宿主自带 bypass 由 JUCE wrapper 提供,不占自动化位」改为「……合成,不占 123 的预算」。JUCE 8.0.8(仓内 `.juce-version`)的 VST3 wrapper 在插件没有自己的 bypass 参数时自建一个 `AudioParameterBool`(ID `byps`),它的参数信息带 `kCanAutomate | kIsBypass`(`juce_audio_plugin_client_VST3.cpp`)—— 宿主能自动化它,「不占自动化位」这句因此不对(§一 首条 Output「宿主可见 124」本来就把它算在内);它不占的是插件自己声明的参数(Output 的 123 个)的预算。
- **为什么是「登记对齐」,不是字段 / 布局 / abi 变更**:上面每一条都是先有已冻结的实现(落点与裁定号见各条),本次修宪没有让任何一个字节的编码发生变化 —— 仓内 `src/` / `web/` / `tests/`(含 `tests/golden/`)零改动,`kCurrentAbi` 仍为 6,123 参数表与 `tests/golden/params_v0.tsv` 逐字未改。改的只是本文件对实现的描述。仓内变更文档 `docs/contract-changes/20260930-j183-params-v0-v2.6-state-register.md`。
- **本文件改动位置**(本节之外):状态行;§一 [J81] 块的 Monitor 一条;§二 YAML(`abi`、`capture_enabled`、`range`、`applied`、`auto_label`、`source_channels`、`lead_timeline[]`、`meta`、`segments[]`、`excluded_ranges[]`、`embedded`);§三 YAML 的 `abi` 与新增的 Monitor 小节;§四 三处。历史修订节一字未动。
