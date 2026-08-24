# ADR-002 · 路由架构(=D5,细则)

> 状态: 冻结
> 最后更新: 2026-08-11(对应 constitution/ADR.md v2.0)
> 真源: 镜像自 docs/constitution/ADR.md(ADR-002 拆分)

> 本文件由 `docs/constitution/ADR.md`(仓内只读副本,sha256 由 `scripts/check-constitution-sync.ps1` 断言)逐条拆分;改动须走修宪流程,不得单独改本文件。

- Input 插在人声轨最后一个推子后插槽:捕获 → 写共享内存 → 向下游输出**静音**(保住 DAW 依赖图排序)
- Output 插在总线第一格:按自身 block 的时间线区间从各 channel 环形缓冲读取 → 每轨 gain/pan → 求和 → 替换总线输入
- 人声轨必须保持 DAW 路由指向该总线(用户手册红字;Output 检测不到对应时间线数据时 UI 警告)
- Output 不向 host 报告额外 latency(对齐靠时间线寻址,不靠 PDC);Input 报告 latency=0
- 多 Output 实例:registry 里 channel 归属唯一,第二个实例抢占同一 channel 时 UI 警告并只读
- 失准语义:缺口→该轨该块静音+警告计数;重叠→取时间线正确者

## 修订历史(摘自 constitution/ADR.md)

- **[J12→ADR-002 实质修订]** Input 检测不到健康 Output 时输出**直通**(80ms ramp + 5s 滞回防抖),仅在连接健康时静音。消除「无 Output=全轨静音」事故面;S1 增加该切换的验证项。
- **[J32→ADR-002]** J12 切换协议:5s 滞回仅作用于 静音→直通 方向;直通→静音在确认健康后立即 80ms ramp;Output 置 mask 位后延迟 ≥200ms 再注入(或等 Input muted 确认位);S1 增双路叠加验证项。
- **[J45→ADR-002]** 「人声轨最后一个推子后插槽」措辞修订为「**人声轨插件链最后一格**」(多数宿主无推子后插槽概念);「推子 0 dB」仅为 null test 可比性前提(10 文档),非产品要求;逐宿主插槽位置指南归 DAW_COMPATIBILITY 文档。