# 契约变更说明 —— 20260822-pan-curve-cut-slope

<!-- 触发 branch-gate 冻结契约 path guard:本 PR 触碰 docs/SCVB_CONTRACT.md §1.17。
     变更文档 + status/frozen-contract 标签就位,属用户已批准变更。 -->

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [x] docs/SCVB_CONTRACT.md(JS↔C++ 桥契约,§1.17 setPanCurve 载荷语义)
- [ ] tests/golden/(golden 快照)

> 注:docs/STATE_SCHEMA.md 当前仍为 T39a 占位文档(§一/§二/§三 均为 T39a 填充占位注释,
> 未列出任何字段),本次语义变更不触碰它、也不为它补字段;pan_curve 字段真源在
> params-v0 §二(仓内只读副本)与 docs/SCVB_CONTRACT.md §1.17。

## 变更内容

pan_curve 点结构 points[] 的 q 字段语义按 shape 分化:

- shape=cut:q 字段承载 slope(dB/oct,取值 6|12|18|24,默认 12);
- shape=bell|shelf:q 仍为 Q(Q ∈ [0.5, 10])。

cut 的 DSP 公式由「有限地板搁架 + Q 谐振凸起 R」改为 slope 模型(02 §7.1 修订):

    d  = 侧向距离:right → (P−P₀);left → (P₀−P);out → sign(P₀) 定(≥0→right,<0→left)
    d  = max(|d|, d0),d0 = 1.0(度)
    u  = log2(d / d0)(倍频程数)
    u_b = |A| / s(A = gain_db,cut 恒 ≤0;s 为 slope)
    G  = A · smoothstep(u / u_b),smoothstep(x) = x²(3−2x) 在 [0,1] 内、外扩 0/1
    结果 clamp 到 [A, 0];d ≤ 0(保留侧)或 d < d0(切点自身及 1° 内)→ G = 0

cut 不再有谐振凸起 R(slope 模型取代 Q+谐振)。

## 兼容性影响

- 无字段增删:points[] 元素仍是 {angle, gain_db, shape, q, side},只是 q 的语义按 shape 分化。
- 无 abi 变更:IPC 段布局 / state chunk 布局不变;STATE_SCHEMA 未触碰(仍 T39a 占位)。
- 旧数据语义变化:旧 cut 点(其 q 曾为 Q)在新求值下按 slope 解释 —— 这是规格批准的有意变更,
  不是迁移 bug;cut 点默认 slope=12,UI 斜率分段钮只发 6|12|18|24。
- 对拍 golden 重生成:scripts/curve-parity-golden.json 随新公式重生成(位于 scripts/ 而非
  tests/golden/,不触发 golden path guard)。

## 回滚路径

- 撤销本 PR 即回滚;STATE_SCHEMA / PARAMETERS / IPC_CONTRACT 均未改,无 schema 迁移。
- 若需回滚到旧「Q+谐振 cut」:恢复 PanCurve.{h,cpp} 的 cutValue/resonanceDb、curve-editor.js 的
  resonanceDb/evalShape、tests/core/test_pancurve.cpp 的 CURVE-5 旧向量、scripts/curve-parity-golden.json
  旧快照即可。

## 审批

- 挂 status/frozen-contract 标签(gh pr edit 61 --add-label status/frozen-contract)。
- 用户已明确批准(见下「用户批准原文」)。

## 用户批准原文

> cut→slope:用户批准实施 —— 档位 6/12/18/24 dB/oct 默认 12;q 字段对 cut 承载 slope;
> DSP = 对数域坡 smoothstep(A·smoothstep(u/u_b),u=log2(d/1°),u_b=|A|/s,无谐振项);
> 冻结面 = SCVB_CONTRACT §1.17 语义注记 + contract-changes/20260822-pan-curve-cut-slope.md
> + status/frozen-contract 标签;02 §7.1 修订提案存 drafts/02-amend-cut-slope.md 交统筹落地。
> (来源:SCVB-AUTOPILOT/state.md「🧭 规格裁决落地(2026-08-23)」)

