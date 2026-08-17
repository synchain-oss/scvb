# 契约变更说明 —— 20260817-t15-params-golden-freeze

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [x] **tests/golden/(golden 快照)—— 新增 params_v0.tsv(123 参数布局 golden)**
- [ ] docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)

## 变更内容

T15 新建 `tests/golden/params_v0.tsv`:123 行参数 golden,布局逐字取 `docs/constitution/params-v0.md` **v2.2 §一**(v2.2 = v2.1 + [J66] state `group_id`,§一明文「参数表不变」,即参数面与卡文所称 v2.1 逐字一致)。表头含双口径注记:123=声明数 / 124=宿主可见(wrapper 合成 bypass;Live 128 上限余 4,J65 预算封顶)。布局:index 0-2 = `width`/`ms_balance`/`lead_select`(根组);index 3.. = 版本 v(1..2)× 轨 t(1..15)× (Pan,Vol,Width,Freeze),公式 `index(v,t,k) = 3 + (v-1)*60 + (t-1)*4 + k`(k:pan=0/vol=1/width=2/freeze=3),末位 122。本 PR 同时删除 `spikes/s2/`(J16:旧 81/93 参数表 spike 作废,不得复活)。

## 兼容性影响

无既有工程影响:golden 为新建,`ParamsGoldenTest` 从此锁定 123 参数布局;此后任何参数增删/改序/改 ParamID 都会使 golden 测试变红(冻结语义)。

## 审批

- [x] 用户授权:2026-08-17 凌晨「自动继续后续全部任务」+ 下午合并纪律「comment 都说没问题就可以合」;T15 卡(07-execution-plan)明文交付本 golden(验收 ①②③④)。
- [x] DeepSeek native 评审:PR #39 由三 bot 审查后按纪律合并。
