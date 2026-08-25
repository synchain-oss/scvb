# 契约变更说明 —— 20260824-t39a-contract-docs

<!-- 本文件按 TEMPLATE.md 生成:PR 触碰冻结契约文件时,branch-gate 冻结契约 path guard
     要求在同一 PR 里新增 docs/contract-changes/<YYYYMMDD>-<slug>.md,并挂 status/frozen-contract 标签。 -->

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md(自动化参数)
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [x] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)

## 变更内容

T39a「契约文档组」对三份冻结契约做**转正回填**(T01 首 commit 落地的骨架占位 → 蒸馏定稿),**不改任何已冻结语义**。内容来源分两类,如实区分:

**A. 逐字蒸馏自宪法只读副本**(`docs/constitution/`):

1. **docs/PARAMETERS.md**:按 `params-v0.md`(v2.2)逐字蒸馏 —— 123 个自动化参数总表、Output/Input state 树、命名与兼容规则。
2. **docs/IPC_CONTRACT.md**:段名前缀、Registry/音频环/特征段/控制面的段布局字段逐字取自 `ipc-contract-v0.md`(v1.5);心跳双阈值(J10)、布局冻结规则(§5)逐字。
3. **docs/STATE_SCHEMA.md §一/§二**:Output/Input state 树逐字取自 `params-v0.md` §二/§三。

**B. 新增转正项**(宪法只读副本之外的规范性内容,来自计划 01 §4.4 / 03 §6 / 04 §5,仓内以实现源码 + golden 快照转正):

1. `OutputGlobalInfo` / `CtrlRecord` 结构体(IPC_CONTRACT.md §4):J09/J46 授权细化,真源 = `src/core/ipc/SegmentLayout.h` + `tests/golden/ipc-layout.txt`。
2. `STATE_SCHEMA.md §三`(state chunk 容器:magic/abi/TLV 分节)与 `§四`(FEAT 节编码 / 8MB↔sidecar 回滞 / sidecar 目录契约):真源 = `src/core/state/StateCodec.h` / `FeaturesCodec.h` + `tests/golden/state/abi1.bin` + 计划 03 §6 / 04 §5。

以上三份文档在回填前均为「从 masterPlan 蒸馏中」的占位骨架,本次把已冻结内容蒸馏落定 + 把宪法之外的授权细化结构体转正,不含任何语义增删。

**修复轮(2026-08-24,reviewer 红旗/重要项)**:① OutputGlobalInfo 补 `u32 _pad`(offset 12,gap_count@16/overlap_count@76/epoch_summary@136 对齐 golden);② state 容器 magic 改 `0x42564353`(小端,对齐 StateCodec.h + abi1.bin);③ ANALYSIS/STATE_SCHEMA 的 `loudness_mode`/`center_slot_policy` 枚举串改用桥面真值 `kw_integrated`/`even_spread`;④ FEAT 压缩容器更正为 zlib(RFC 1950,miniz);⑤ 补 FEAT flags bit1 `vadPresent`、CRVS minor 更高等同拒载、loudness_mode/center_slot_policy 进 analysis 组。

**复评修复轮(2026-08-24,复评口径对齐)**:ARCHITECTURE.md §5 环寻址公式由 `timeline_pos & (ring_frames-1)` 修正为 `(timeline_pos & (ring_frames-1)) * channels + c`,与 IPC_CONTRACT.md §2 同轮修正口径一致(消除同一 PR 内两份文档口径打架)。

**复评轮2(2026-08-24,复评2 口径对齐)**:① PARAMETERS.md §二 analysis 组补 `loudness_mode`/`center_slot_policy`(J69/U24,桥面真值,与 STATE_SCHEMA §一同口径);② STATE_SCHEMA §三 PRMS 的 ui 子集补 `tour_seen`(J62);③ IPC_CONTRACT §4 补 `CtrlHeader`/`CtrlRing`(golden:records@64、CtrlRing size 448、kCtrlRingCapacity=16)。

**复评轮3(2026-08-24,全量逐字段对拍)**:IPC_CONTRACT §1-§4 全部 10 个结构体逐字段与 `tests/golden/ipc-layout.txt` 对拍,补齐 RegistryHeader._reserved[12]@16、InputSlot._pad[7]@36、OutputSlot._pad[10]@24、AudioRingHeader._pad@20 四处尾部填充/对齐缺口,并标注段内偏移(InputSlot0@64 / OutputSlot0@1024)与 ctrl 段落点(CtrlHeader@0 / 广播区@64 / OutputGlobalInfo@9408 / CtrlRing×15@9664,16KB 恰占满)。

## 兼容性影响

- 无。上述内容忠实反映已冻结宪法与已实现 wire 格式(以实现源码 + `tests/golden/` 为准),不改参数 ID/index/顺序、不改 IPC 段名/布局、不改 state `abi` 规则;对既有工程 / 既有 DAW 自动化 / 新旧版本互通零影响。
- 本 PR 不触碰 `docs/SCVB_CONTRACT.md`(T25 冻结的 JS↔C++ 桥契约唯一真源)与 `tests/golden/*`。

## 审批

- 挂 `status/frozen-contract` 标签;用户明确批准后合入。
