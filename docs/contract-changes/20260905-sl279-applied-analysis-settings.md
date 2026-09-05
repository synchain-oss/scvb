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
