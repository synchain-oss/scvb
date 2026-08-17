# 契约变更说明 —— 20260816-scvb-contract-v1-initial

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] tests/golden/(golden 快照)
- [x] **docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)——初版创建**(CLAUDE.md §10 点名的冻结契约;本文件即其入库冻结的变更记录,模板清单据实补此行)

## 变更内容

新建 `docs/SCVB_CONTRACT.md` v1.0(T25):SCVB Input/Output 两插件的 JS↔C++ 桥面唯一真源——Output 34 函数 / 9 事件,Input 7 函数 / 5 事件,共享枚举与错误降级语义、ctrl 命令环 op 核对表(J36 镜像)、机器可读 manifest、归并映射与禁止复活名单。函数名/签名以 05 §1.4 为基线,载荷与线程/防回环语义以 01 §6.4 为准(T25 卡裁定规则)。相对 05 §1.4 的授权增量 3 项(`setAnalysisConfig` / Input `scvb.error` / `confirmPrintGuard`),T25 定名补白 32 项,均在契约 §8.4/§9.2 列明。配套 `scripts/check-bridge-parity.mjs`(契约 manifest ↔ mock ↔ C++ 常量表三方比对,当前后两侧 SKIP)。

## 兼容性影响

无既有工程/自动化影响(初版创建,尚无实现消费)。冻结后:mock 桥(web-preview)与真桥(T29/T30)必须同契约(CLAUDE.md §10);任何改动走「只增不改」纪律(契约 §0.1/§9.0),破坏性变更 `contractVersion` 主版本 +1,不触发 ipc/params abi。

## 审批

- [x] DeepSeek native 可实现性评审:**通过**(2026-08-16;34+7 函数全可实现、32 项定名确认、`requestWaveform` 定案「异步」、`heartbeatAgeMs`/`generation`/`occupiedMask`/`channelLabels` 四载荷字段可实现;评审附注已回填契约 §1.8/§1.27/§2.6/§4.1/§9.3)
- [x] 用户批准(2026-08-16):契约状态行已改「已冻结」,PR 挂 `status/frozen-contract` 标签(用户人工审核后合入)
