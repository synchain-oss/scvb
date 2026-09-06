# 契约变更说明 —— 20260905-sl279-applied-analysis-settings

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [x] docs/STATE_SCHEMA.md(state schema)—— abi 2→3 + migrate_2_to_3 + CFGS 行尾扩 applied.\* + state payload 加 `analysis.applied.*`
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.1 `requestInitialState` 返回值与 §2.1 `scvb.state` 载荷的 `analysis` 组加 `applied:{loudness_mode, center_slot_policy}`(**载荷字段只增不改**,CLAUDE.md §7.4);§1.21 补 `applied.*` 的语义、写入面与前移条件
- [x] tests/golden/(golden 快照)—— **新增** abi3.bin,abi1.bin / abi2.bin **保留**作迁移基线(三份并存,不是替换)。
      ⚠ **金样锁的是容器头**(magic / abi / flags / chunkCount / TLV 框):夹具的 CFGS 载荷是
      `opaque("CONFIG")` 字面量,不是 `encodeOutputState` 的产物 —— 所以 `abi3.bin` 与 `abi2.bin`
      只差 abi 那一个字节是**必然**而非巧合,`applied.*` 那 8 个字节它一次都没见过。
      CFGS 载荷的 wire 布局由 `test_output_session.cpp` 的长度断言(`42u` / `24u+5u+16u`)与
      本卡新增的四格(往返 / 两级回退 / 半截拒载 / 回落计数)承担。改 `applied` 的编码顺序
      **不会**让金样红,别去金样那里找原因。

## 变更内容

Output CFGS chunk(fourcc=CFGS)在既有两个枚举 u32 之后**再尾扩两个 u32**,持久化
`analysis.applied.loudness_mode` / `analysis.applied.center_slot_policy` —— 即「上次全量分析所用的那一档」。
state 容器 abi 由 2 → 3,新增 no-op `migrate_2_to_3`。
web 侧 state payload 的 `analysis` 组加 `applied` 子对象(两字段,与上面同一套枚举串)。

**为什么要落盘**:03 §6.3 把 `analysis_settings_stale` 定义为「当前 ≠ applied.\*」,但此前契约没有暴露
`applied.*`,设置页只能拿「Tab4 mount 那一刻的本地快照」当基线。而 mount 早于首次 state 到达,
于是派生出一对方向相反的偏差 —— 存成非默认档的工程一进 Tab4 就误报「需重新分析」;
同一会话里把口径切回 mount 默认值时提示又立刻消失,而段表其实还是按旧档分析的(漏报)。
两条同根,补本地同步只是把误报换成漏报,所以真源改成「工程 state 里的上次分析口径」。

## 何时前移 `applied.*`(产品决定)

前移的判据是**「这一轮真的重算了全轨 × 整条已采集时间线吗」**,不是「桥面 scope 字面是不是
`"all"`」。判据只此一处 —— `AnalyzeRange::wholeTimeline`(`src/output/AnalyzeScopeMath.h`),由
范围推导在选分支时置,两个调用方**读**它,不各自按 `rangeMode` 现算(现算就是第二把尺子)。

今天有**两个**来源,都要求 `global.range` 处于 `follow` 档:

1. §1.6 `analyze` 的 `"all"` 档(含 `analyze()` / `null` / `undefined` 等一切非对象形);
2. §1.18 / §1.19 松手档触发的**自动重分段**(改完 VAD / 分段参数 300ms 后)。第 2 条容易漏:
   它不经桥面 scope,却同样是全轨全时间线的整表替换,漏了它就是「整表已按新档重算完、注记还亮着」。

**`daw_loop` / `manual` 档下点「分析(全部)」不前移** —— 这条推导在范围档取的是 `global.range`,
重算只覆盖范围内,范围外的段仍是旧口径。前移会把注记灭掉,那是一条**漏报**,与本卡要修的误报
同族、方向相反。代价写明:范围档下点完「分析(全部)」注记仍亮,**唯一出路是把 `global.range` 切回 `follow`
再分析**。取这条是因为注记的语义是「段表未按当前档分析过」—— 范围档下它亮着是**真话**。

⚠ **不要写「或把范围扩到全长」**(`16261a0` 的契约句与本文档一度都这么写,复审第 7 轮红旗):
判据看的是**档位**不是覆盖面 —— `manual` 档下把范围拖成 `[0, 已采集末端]` 仍满足
`rangeMode != 0 && end > start`,照样走范围支、照样不前移,用户照做完徽标一样不灭。
**也不要顺手把判据改成比较覆盖面**:`capturedExtentSeconds()` 随采集增长,同一个 manual
范围今天覆盖全长、明天多采两秒就不覆盖 —— 徽标会自己从灭变亮,那比现状更坏。
「按范围记录 per-range applied」不做:那要把 `applied` 从两个标量变成区间表,落盘、迁移、UI 三处
都要重做,而它解决的只是一个边角。

## 兼容性影响

- **加法,不升 contractVersion 主版本**:按 STATE_SCHEMA §0.1 规则 3,payload 新增可选字段属加法;
  旧 web 不读 `analysis.applied` → 行为不变(继续用它自己的本地基线)。
- **旧版读新 blob**:abi=2 的旧插件读到 abi=3 → `RejectedNewer` → `preservedStateBlob_` 原样回写 +
  提示升级,**绝不静默降级**(CLAUDE.md §7.3)。
- **新版读旧 blob**:abi=2 → `migrate_2_to_3`(no-op)→ CFGS 按「长度回退」补齐。
  **回退语义是 `applied := 当前值`,不是回落默认** —— 旧工程视为「已经按它存着的那档分析过」。
  取默认会让一个存了非默认档的旧工程一打开就误报「需重新分析」,那正是本卡要修的那个误报。
  abi=1 → `migrate_1_to_2`(no-op)→ 两级回退依次生效,行为与本卡之前一致。
- **半截即拒载**:CFGS 尾部 remaining 落在 (0,8) 或 (8,16) 一律拒载(不可信字节,CLAUDE.md §7.3)。
- **Input 侧连带**:Input 与 Output 共用容器 abi,本 PR 之后新 Input 保存 state 一并写 abi=3;
  旧 Input 读新 Input state 整块 `RejectedNewer`(原样回写 + 提示升级);新 Input 读旧工程经两个
  no-op 迁移不受影响(Input CFGS 未变)。
- **回落计数**:`OutputDecodeReport` 新增两个 applied 专属计数器,**不与「当前」那两个合并** ——
  合并之后诊断行会说「loudness_mode 回落了 1 次」而实际回落的是 applied 那一份,把人指到错的字段上。

## 变更文件

- `src/core/state/StateCodec.h`(kCurrentAbi 2→3)
- `src/core/state/StateMigration.{h,cpp}`(migrate_2_to_3;并把头注那句「未来升 abi 时……」改成本卡的兑现)
- `src/core/state/OutputStateCodec.{h,cpp}`(尾扩 u32×2、两级长度回退、applied 专属回落计数)
- `src/core/analysis/AnalysisSettings.h`(新文件:失效标记结构;`LoudnessMode.h` 只留指路)
- `docs/STATE_SCHEMA.md`(abi 2→3 + migrate_2_to_3 + CFGS 行 + payload `analysis.applied.*`)
- `docs/SCVB_CONTRACT.md`(§1.1 / §2.1 载荷字段 + §1.21 语义)
- `docs/contract-changes/TEMPLATE.md`(清单缺 `docs/SCVB_CONTRACT.md` 一格,而 `branch-gate.yml` 的 path guard 管着**五条** —— 照模板走必漏勾,本卡即是实例)
- `tests/golden/state/abi3.bin`(新增;abi1/abi2 保留)

## 审批

挂 `status/frozen-contract` 标签,由用户批准后合入(用户已于 2026-09-05 口头批准本卡方案)。
