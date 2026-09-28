# 契约变更说明 —— 20260928-j150-track-name-auto-label

> **状态:实质已批。** 依据:裁定 **J150**(用户,发布前 17 条拍板 #6「宿主专属提示 + 轨道名自动填 label」,
> 结论「**现在都做**」;裁定原文 ②:「04 §7 步 2:Input 由 updateTrackProperties 取轨道名,经 IPC 带给 Output
> 自动填 channels[].label(用户未改名时);**IPC/契约变更实质已批,走变更文档**」)。本文档只覆盖 ②(轨道名);
> ①(REAPER / Live 宿主提示)另走自己的 PR。本文档与实现放在**同一个 PR** 里,挂 `status/frozen-contract`。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:没有新参数,ParamID / index / 顺序 / versionHint 一个字节没碰。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— §4 ctrl 段**新增**一个块「Input → Output 轨道名区」
      (`CtrlTrackName` × 15,偏移 2112,落在 T06 冻结的 9344 字节广播区预算内);J81 表里「余 7296 字节」一句
      补注其中 1920 字节已被本块占用;命令环一条补注「轨道名不经命令环」。**段名 / abi / 既有结构体偏移与尺寸
      全部不变**(论证见下)。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 `channels[15]` 加 `auto_label`;§三 `PRMS` 行加根节点属性
      `channels_auto_label`,并加一条落点与迁移语义说明。**容器 abi 不变**(仍为 6)、`CFGS` 布局零变化、
      不加迁移函数。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.15 `setChannelConfig` 的「语义」格补 label 自动填入与
      「用户命名 / 清空回到自动」的判定规则;§8 命令环 op 表「字符串型」一行补注。**函数名 / 签名 / 事件名 /
      载荷字段零变化**(`check-bridge-parity` 不受影响),`contractVersion` 不升。
- [x] tests/golden/(golden 快照)—— `ipc-layout.txt` **只新增**:`struct CtrlTrackName` 一块(5 个字段)与
      一行 `offset ctrl_track_names 2112`。**既有行一行没改**。金样由 `test_ipc_layout` 逐行对拍编译期真值,
      `check-ipc-doc-parity` 对拍 golden ↔ IPC_CONTRACT ↔ 头文件。

## 变更内容

**一句话**:Input 把宿主经 `updateTrackProperties` 告知的 DAW 轨道名写进 ctrl 段的轨道名区;Output 在该轨
「用户没亲手改过名」时把它(截到 24 码点)自动填进 `channels[].label`,DAW 改名跟着改;Input 断开后保留最后
一次的名字。落实 04 §7 步 2 验收列「轨道名自动填入 label」(此前 `src/` 里没有任何 `updateTrackProperties`)。

### IPC:ctrl 段轨道名区(IPC_CONTRACT §4)

| 项 | 值 |
| --- | --- |
| 位置 | `kCtrlTrackNamesOffset = 64 + sizeof(CtrlBroadcast) = 2112`,15 条 × 128 字节,止于 4032 |
| 预算 | 仍在 `kCtrlBroadcastOffset + kCtrlBroadcastBytes = 9408` 之内(`static_assert` 钉死);广播区剩余 7296 → 5376 |
| 条目 | `atomic<u32> seq` @0 / `u32 _pad` @4 / `u64 owner_heartbeat_ms` @8 / `char utf8[100]` @16 / `char _tail[12]` @116 |
| 写方 | **实际持有**该 channel 的那个 Input 的 [M],25Hz 每拍重写(只在 claim 为活跃时);每条单写 |
| 读方 | 本组 `kActive` 的 Output 的 [M];只读观察实例不采信 |
| 一致性 | 按条 seqlock:写前 `seq |= 1`(置奇)→ 写载荷 → `seq + 1`(偶);读方撕裂 / 奇数即本拍放弃,不自旋 |

- **为什么不是命令环 op**:契约写明「`set_label` 等字符串型 op 不在 v1,需 abi+1 增补变长区」。本块**不是**
  那条路 —— 它不经命令环、不直接改 label,是一块定长的「每轨一条最新值」,填不填由 Output 按规则决定。
- **归属判据 `owner_heartbeat_ms`**:Input 写入时记下自己那条 slot 此刻的 `InputSlot.heartbeat_ms`;Output 只在
  它**等于**该 slot 此刻的心跳时采信。Input 换通道 / 换组 / 崩溃后,新主人在 claim 时就写了自己的心跳,旧条目
  随即失配 —— 离开的一方不必清,Output 也不会把上一任的名字填给下一任。Output 的采信条件一共三条
  (`OutputSession::readOwnedTrackName`):① 该轨「已连接」(与 UI 连接灯同一判据 `isConnectedForDisplay`);
  ② 归属相等;③ 名字非空(空串 = 宿主没给轨道名)。
- **不可信字节**:写方在最后一个完整 UTF-8 序列边界截断(≤ 99 字节);读方把末字节强制成 NUL,并拒收非严格
  UTF-8(过长编码 / 代理区 / 超 U+10FFFF / 截断的续字节)。
- **奇数残值**:写前用 `seq | 1` 而不是 `fetch_add(1)` —— 上一任写方死在临界区里留下奇数时,`+1` 会让下一次
  写入在**写载荷期间**呈偶数(读方当成稳定值读走半截)、写完反而停在奇数(此后永远读不到)。

**为什么不属 §5 意义上的「布局改动」(不升 abi、段名不升 v2)** —— 与 [J81] 广播区、[SL-362] viz 保留字段同一个论证:

1. **既有结构体的偏移与尺寸零变化**:`CtrlHeader` / `CtrlBroadcast` / `OutputGlobalInfo` / `CtrlRing` 一个字节没动,
   ctrl 段 16 KB 总预算与各落点常量不变(`check-ipc-doc-parity` 的 §4 落点链照旧通过)。
2. **新块落在 T06 冻结时就留白的广播区预算内** —— 那段预算本来就是「后续增补不改既有偏移」的用途。
3. **两向都优雅降级、不拒连、不半兼容**:
   - 旧 Output + 新 Input:旧 Output 不读这块(也从不整块清零广播区 —— 只有段的覆盖式重初始化会清,Input
     下一拍即重写),Input 写了无人采信,行为与之前逐字相同;
   - 新 Output + 旧 Input:这块全零(`seq == 0` = 从未写过),Output 读不到任何轨道名,label 保持原样。

**对照:走 abi+1 + 段名 v2 的代价**(本变更**没有**选它)。abi 与段名前缀是 registry / ctrl / audio / feat / viz
**全段共用**的,升级会让新旧 Input / Output 在所有段上**全部拒连**、音频直接不通 —— 为一个显示名付这个代价
不成比例。

### State:`channels[15].auto_label`(STATE_SCHEMA §一 / §三)

- **语义**:每轨「最近一次自动填进 label 的 DAW 轨道名」。「label 是不是用户亲手起的」**不另设标志位**,由两者
  推导:label 为空、或仍等于 `auto_label` ⇒ 跟随轨道名;否则 ⇒ 用户命名,不再被覆盖。于是桥面
  `setChannelConfig`、撤销、载入改 label 时都不需要同步任何标志:改成别的名字自然成了用户命名,清空自然回到自动。
- **落点**:`PRMS` 根节点属性 `channels_auto_label`(15 个字符串的 JSON 数组,逐轨原样记内存里的上次自动填入值,没自动填过的
  轨为空串)。**不进 `CFGS`**:`CFGS` 定长,加字段要么升 Input / Output / Monitor **三个插件共用**的容器 abi(升了之后
  三个插件的新工程在旧构建里都整块拒载),要么走 `unknownTail`。
- **为什么存名字、不存一位标志**:旧构建不认识这个属性,却会经 APVTS `replaceState` / `copyState` 把它**原样**
  带回来(`unknownTail` 也一样原样带回)。若存的是「自动 / 用户」标志,用户在旧构建里改过名字、再回到新构建时,
  标志仍说「自动」,轨道名就会把用户的名字覆盖掉;存名字时 label 已不等于它,仍判成用户命名 —— **自证**。
- **迁移语义**(abi 不递增、无迁移函数):属性缺席(本功能之前的工程)或不是 JSON 数组 ⇒ 15 条全空 ⇒ 非空 label
  一律视为用户命名。这是保守的一边:那时还没有自动填,非空 label 只可能是用户起的;错判的代价只是「改名不再
  跟随」,反过来错判会把用户的名字覆盖成轨道名。数组短于 15 的缺位记空串、长出的部分忽略;元素只拿来与 label
  比「等不等」、从不写进 label。
- **落地时机**:与 label 同在 `CFGS` 那段 —— 只带 `PRMS` 的参数预设不动通道配置(连同本属性)。

### 桥面语义(SCVB_CONTRACT §1.15)

- 经 `setChannelConfig` 写成**非空且不同于**上次自动填入值的 label ⇒ 用户命名,此后不被轨道名覆盖;
- 写成**空串** ⇒ 回到自动,本轨 Input 在线时下一拍按当前轨道名填回;
- 写成恰好等于当前自动填入值 ⇒ 与自动填入无区别,仍跟随;
- 自动填入不经 `setChannelConfig`,但同样 `config_seq+1`,经 `scvb.state` 回推、ctrl 广播区刷新(Input 页与通道
  网格随之显示);只读观察实例不做自动填入;
- Input 断开(掉线 / 改成未分配 / 删轨)后 label 保留最后一次的轨道名;断开期间用户清空的 label 不会被旧名字填回。

## 兼容性影响

- **既有工程**:打开后非空 label 一律视为用户命名(见迁移语义),与今天的行为一致 —— 不会有任何一条已命名的轨被
  改名;空 label 的轨在 Input 连上后自动填入轨道名(这是本功能要的行为)。
- **既有 DAW 自动化**:零影响(不涉及参数)。
- **新旧版本互通**:IPC 两向降级为「没有轨道名」(见上);state 两向都能读 —— 旧构建读新工程时忽略该属性并原样
  带回,新构建读旧工程时按缺席处理。
- **宿主差异**:轨道名来自 JUCE `updateTrackProperties`(VST3 `IInfoListener::setChannelContextInfos`),何时调、
  调不调由宿主决定(计划 11 风险表 A-08「待 S1 逐宿主观察」)。宿主不给名字时 label 保持空、页面显示占位名,
  与本变更之前相同。
- **实时线程**:零改动 —— 读写都在两侧的消息线程(25Hz Timer),音频线程不触碰 ctrl 段。
- **已知边界**:① 新主人 claim 与旧主人最后一次心跳落在同一毫秒时,归属判据会在新主人第一次写条目前的一拍内
  认错人(< 40ms,随即被新主人的条目覆盖;若新主人所在宿主不给轨道名,旧名字会留在 label 上 —— 同一 DAW 里
  两个 Input 一个有名一个没有、且在同一毫秒换手,实际不可达);② 轨道名的 seq 每拍 +2,u32 回绕到 0 需约 2.7 年
  不停机,落到 0 的那一拍读方判「没有名字」、保持现状,下一拍恢复。

## 验证

- `tests/core/test_track_name_ipc.cpp`(`[j150]`):布局算术;写读往返 / 从未写过;码点边界截断;对端非法 UTF-8 与
  缺 NUL;奇数 seq 读不到、奇数残值下一次写入即扶正;并发写读不撕裂;`readOwnedTrackName` 三道门与
  `InputSession::ownSlotHeartbeatMs`。
- `tests/host/test_host_track_name.cpp`(`[host][j150]`,真 Processor + 真 Win32 段):自动填入 + 改名跟随 + Input
  侧广播可见 + 宿主不带 name 时保留;24 码点截断(ASCII / 多字节);用户命名不跟随、清空回到自动;断开保留最后
  名字、断开期间清空不被填回;只读观察实例不改;存盘重开(自动的仍跟随 / 用户的仍不跟随 / 本功能之前的工程 /
  旧构建另存过且改过名)。
- `tests/core/test_ipc_layout.cpp` + `tests/golden/ipc-layout.txt`:新结构体逐字段偏移对拍;`check-ipc-doc-parity` 通过。
- 删除式验证逐落点的注入与结果见 PR 描述。

## 审批

J150 用户拍板(发布前 17 条,#6「现在都做」;「IPC/契约变更实质已批,走变更文档」)。挂 `status/frozen-contract`
标签;本变更**不升 abi、不改段名、不改任何既有字段**,选择理由写在上面,如需改走 abi+1 请在 PR 里裁定。
