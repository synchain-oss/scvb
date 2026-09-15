# 契约变更说明 —— 20260915-sl414-min-seg-definition

> **状态:待批(随实现 PR 挂 `status/frozen-contract`)。** 定谳:SL-414(用户 v5.6.15 回验
> A20「MIN SEG=2000 下段表仍有 0.21s 的段」);修法统筹已裁「候选 D」(段级、每轨独立,
> 不动区间图)。本文档是该变更的契约面记录,与实现放在**同一个 PR**里。

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md(自动化参数)—— **仅补一句定义**,给 `analysis.segmentation.min_segment_ms`
      加上它一直没有的定义行(此前整份只有 state 镜像行,[SL-398] 变更文档亦确认「无落点」):
      每轨最短**自动**段长,两层生效 —— ① VAD 核心丢短(02 §2.3 P1,在前后留白之前判定);
      ② 段表兜底:回写层成形的段表里短于它的 auto 段并入同轨相邻段(优先前一段,值取被并入段;
      用户段/锁定段不参与)。手动段不受影响。
      ⚠ **自动化参数面零变化**:该参数是 state-only,123/124 个自动化参数的 ParamID / index /
      顺序 / versionHint 一个字节都没碰;`mode` / `sensitivity` 两项同样零改动。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动。
- [ ] docs/STATE_SCHEMA.md(state schema)—— 不动:`min_segment_ms` 的值域(50..2000)与
      编码一字未改([SL-411] 的落盘注保持原样)。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— 不动:`setSegmentation` 入参与 `scvb.state`
      载荷行零改动。
- [ ] tests/golden/(golden 快照)—— 不动:无布局、无 abi、无编码变化。

## 变更内容

**一句话**:给 `min_segment_ms` 补上「两层生效」的定义句 —— 这是**文档对齐**,不是语义变更:
修法落地前(定义句 absent 时)引擎已经这样实现;定义句把「用户预期的段表级约束」从一句
只描述 VAD core 层行为的 USER_GUIDE 措辞里接过来,并以段表兜底(候选 D)把它变成真的。

| 面 | 改前 | 改后 |
| --- | --- | --- |
| PARAMETERS.md 定义句 | 无(只有 state 镜像行) | 补「两层生效」定义句(见上) |
| 引擎行为 | 段表里可有 < MIN SEG 的 auto 段(跨轨交叠切出的独立区间产物) | 短 auto 段并入同轨相邻段(优先前一段,值取被并入段;单段保留;用户段/锁定段不参与) |
| 区间图 / §5 指派 | — | 零变化(候选 D 边界:只收敛段表,不重切区间、不为指派重造输入) |
| 用户可见 | MIN SEG=2000 重分析后段表仍有 0.21s 的段 | 段表里不再有 < 2000ms 的自动段(带 E/C 角标的手动段不算) |

**为什么走变更文档**:定义句落在冻结文档 PARAMETERS.md 内。按 CLAUDE.md §5,冻结文档的
任何改动(哪怕只是把既有实现写清楚)都随实现 PR 记录在本文件里并挂 `status/frozen-contract`。

## 关联

- 定谳报告:SL-414 工作树 `build-sl414\sl414-verdict.md`(不在仓内;四问结论复制进 PR 描述)。
- 修法真源:masterPlan `plan/02-dsp-spec.md` 的对应文案由统筹维护(⚠ 截至 5aa3eaa,02 §3.4
  仍是四步 —— 候选 D 是段级、落在回写层,规格侧落在 04 分析流水线一节,统筹补文案时以
  本文件的定义句为准)。
- 实现落点:`Segmentation.h/.cpp` 的 `mergeShortAutoSegments`(纯函数)+
  `AnalysisPipeline.cpp` 回写层调用;用例 `tests/core/test_sl414_min_segment.cpp`。
