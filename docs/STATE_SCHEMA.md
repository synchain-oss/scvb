# STATE_SCHEMA —— SCVB state schema 与 abi 兼容规则(冻结契约)

> 状态:从 masterPlan 蒸馏中,当前版本 = constitution v2.1 系——ADR v2.0 含 J57-J61 / ipc v1.5 / params v2.2(以宪法文件实际为准)。

本文件是 state 冻结契约的仓内转正文档(06 §3.4 review bot prompt 明文要读的比对基准之一)。
正式内容由 T39a 从 `docs/constitution/params-v0.md` §二/§三(仓内只读副本)蒸馏填充;在此之前以只读副本为唯一可比对基准。

state chunk 带 `abi` 字段:读到高版本 → 拒载并提示升级;读到低版本 → 迁移函数升格(不得静默丢数据)。
`setStateInformation` 处理的是用户工程文件里的不可信字节,长度/范围字段必须先校验再用于分配或索引。

## 一、Output state(占位)

<!-- T39a 填充:abi/session_guid/group_id/global/analysis/channels/versions/features/ui -->

## 二、Input state(占位)

<!-- T39a 填充:abi/group_id/channel_id/ui -->

## 三、编码与兼容(占位)

<!-- T39a 填充 -->
