# 契约变更说明 —— 20260906-sl361-pannow-param-fallback

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— §6.1 `VizTrackState` 那条 ⚠「`panNow` 口径」
      **只改语义、不改布局**:字段、类型、偏移、定点标度、哨兵值一个字节没动
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] docs/SCVB_CONTRACT.md(桥面契约)
- [ ] tests/golden/(golden 快照)

## 变更内容

`VizTrackState.panNow` / `volDb` 的**取值口径**由一档扩成三档:

| 档 | 条件 | 值 |
| --- | --- | --- |
| ① | 该轨**有分段且有曲线** | 播放头**精确时刻**的曲线求值(原口径,一字未改) |
| ② | 无分段 / 无曲线,**且该轨已连接**(`slotState=2 ∧ heartbeat 新鲜`,§2.3 同款判据) | 该版本该轨的**参数当前值**(`v{v}_t{tt}_pan` / `_vol`) |
| ③ | 其余(未连接,或参数句柄未就绪) | 哨兵 `−32768` |

**为什么要扩 ②**:用户 v5.6.7 真机实测,同一工程 **Output 画 10 根、Monitor 只画 7 根**
(截图 `颜色和不一致.png`)。真因是两侧口径分叉 —— Output 的分布图走「段回读 → **没段就退
参数当前值**」(`web/output/tab-master.js` 的 `renderDist`),而发布器原来只在「有段 ∧ 有曲线」
时才写这两个标量,否则留哨兵,Monitor 见哨兵整根不画。② 就是把 Output 的那一半回落搬到写方,
让两侧读同一口径。

**为什么 ③ 里的「未连接」同样要紧**:Monitor 的逐轨闸只有「`onlineMask` 有位 ∧ 标量非哨兵」
(`web/monitor/viz.js` 的 `vizDistRows`),而 `onlineMask` = `Channel::enabled`、**默认全 true**,
且没有「Input 未连接 ⇒ 自动 disable」的耦合。若回落对所有轨生效,Monitor 会把 15 条全画出来,
而 Output 只画已连接的那几根 —— **从「少画 3 根」变成「多画」,方向反了、幅度更大**。
所以回落的判据与 Output 的 `connectedChannels` **逐字同源**。

## 兼容性影响

- **布局零变化** ⇒ 新旧读方互通不受影响:段名、`abi`、结构体字段与偏移、定点标度、哨兵值
  全部未动,`tests/core/test_ipc_layout.cpp` 的 offsetof/sizeof 对拍无需改动。
- **旧读方(Monitor)读新写方**:原本整根不画的「已连接 + 无分段」轨现在会画出来 —— 那正是
  本卡要修的用户可见缺陷,**行为变化即修复目标**。
- **新写方对未连接轨仍发哨兵** ⇒ 旧读方对那些轨的行为一字未变。
- **不影响任何自动化参数 / state schema / 桥面载荷**。

### 已知口径差(登记,本卡不做)

1. Output 的回落判据是「**播放头处**有没有段」(`panSeg` 是播放头那一段),而写方这一支是
   「**这一轨**有没有任何段」。两者在「有段但播放头落在段外」时仍不一致 —— 记 SL-363 备忘,
   等用户实测再说。
2. 「**未连接但有段**」的轨:Monitor 画、Output 不画。该分叉走的是 ① 那一支,**本卡改前就在**,
   不是本卡引入的。

## 审批

挂 `status/frozen-contract` 标签,待用户批准后合入(06 §3.7)。
