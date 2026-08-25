# 契约变更说明 —— 20260825-cfgs-persistence

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [x] docs/STATE_SCHEMA.md(state schema)—— abi 1→2 + migrate_1_to_2 + CFGS 字段清单同步(#75 转正后由本 PR 承担)
- [x] tests/golden/(golden 快照)—— 新增 abi2.bin,保留 abi1.bin 作迁移基线

## 变更内容

Output CFGS chunk(fourcc=CFGS)尾部追加两个 u32 枚举序号,持久化 analysis.loudness_mode / center_slot_policy;
state 容器 abi 由 1 → 2,新增 no-op migrate_1_to_2。docs/STATE_SCHEMA.md 同步:kCurrentAbi=2、
migrate_1_to_2 迁移函数说明、CFGS 行编码改为紧凑二进制并注明两 u32 枚举序号 + 未知尾部回写。

## 兼容性影响

- 旧版(abi=1)读到新(abi=2)blob → RejectedNewer → preservedStateBlob_ 原样回写 + 提示升级,绝不静默降级(CLAUDE.md §7.3)。
- 新版(abi=2)读到旧(abi=1)blob → migrate_1_to_2(no-op)→ CFGS 按「长度回退」把缺失两字段回落默认(kw_integrated / priority_queue)。
- CFGS 已知字段之后若出现未知尾部(未来小版本追加),解码保留、编码原样回写(unknownTail),消除下次追加静默丢字段。
- Input 侧连带:Input 与 Output 共用容器 abi(kCurrentAbi),本 PR abi 1→2 后新 Input 保存 state 一并写 abi=2;旧 Input(abi=1)读新 Input state 整块 RejectedNewer(原样回写 + 提示升级);新 Input 读旧工程(abi=1)经 no-op migrate_1_to_2 不受影响(Input CFGS 未变)。
- 变更文件:src/core/state/StateCodec.h(kCurrentAbi 1→2)、StateMigration.{h,cpp}(migrate_1_to_2)、OutputStateCodec.{h,cpp}(尾扩 u32×2 + unknownTail)、docs/STATE_SCHEMA.md(abi 1→2 + migrate_1_to_2 + CFGS 行)、tests/golden/state/abi2.bin。

## 审批

挂 status/frozen-contract 标签,由用户批准后合入。