# KNOWN_ISSUES —— SCVB v1 已知限制

本文件记录 v1 已裁定接受、且不构成发版阻断的已知限制。每个条目写明「现象 / 原因 / 影响 / 缓解 / 彻底修复方向」。新增条目遵循同一格式。

## KI-1:run 切换时旧 run 尾部 ≤40ms 漏拉(04 异议 1)

- **现象**:增量拉取(04 §3.3)的读方以 base_hop 前后双读 + write_hop 做 seqlock 校验。若 run 切换(跳变 / 停止 / 开关)发生在两次 25Hz 轮询之间,旧 run 尚未拉取的尾部 hop 会被跳过,不并入 FrameStore。
- **原因**:FeatHeader 只有 base_hop / write_hop 两个寻址字段(ipc-contract §3 冻结布局),没有 run_id。切换后 base_hop 已指向新 run 起点,读方见 base != lastBase 即重置 lastPulled = base,旧 run 未拉积压整段跳过——旧 write_hop 已被新 run 覆盖,无法界定,永不补拉。
- **影响(已裁定接受)**:常态 ≤40ms(≈4 hop),且 run 切换通常落在换气/静音概率区,分析层 guard(04 §4.3 R⁺)与 padding 吸收;最坏情形 = 切换发生在 [M] 长停顿期间(加载工程/模态框/插件扫描,J10 场景),上限 = 停顿时长。**无假覆盖**:coverage 如实显洞 → UI 显示未采集 → 补播即复原。
- **缓解**:无(数据不可追回);后果可自愈(补播)。受限追赶(kMaxBurstHops=256)只保证 **run 不变**时的积压分拍补完,不覆盖 run 切换。
- **彻底修复方向**:FeatHeader 增 run_id(需 abi+1 且段名升 v2,04 文末异议 1 / J35)。v1 不做。
