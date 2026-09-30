# 契约变更说明 —— 20260906-sl363-pannow-segment-readback

> **状态:已批准并落地。** 用户 2026-09-06 批准(见文末「审批」);同一 PR(#245)的 `e42000e`
> 起把 `docs/IPC_CONTRACT.md` §6.1 两处正文与 `docs/constitution/ipc-contract-v0.md` 副本一并落地,
> `status/frozen-contract` 已挂。本文档是这条契约变更的审计记录。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— §6.1 `VizTrackState` 的代码块注释行与其下
      那条 ⚠「`panNow` / `volDb` 口径」。**只改语义、不改布局**:字段、类型、偏移、定点标度、
      哨兵值一个字节没动(`tests/golden/ipc-layout.txt` 与 `test_ipc_layout.cpp` 无需改动)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] docs/SCVB_CONTRACT.md(桥面契约)
- [ ] tests/golden/(golden 快照)

## 变更内容

`VizTrackState.panNow` / `volDb` 的**取值方式**由「`CurveEvaluator` 在播放头**精确时刻**的求值
(+ [SL-361] 的参数回落)」改为「**Output 分布图那一页显示的同一个数**」,即
`web/shared/readback.js` 的 `readbackSegsOf`(native 侧同一份口径 = `src/core/output/DistReadback.h`):

| 档 | 条件 | 值 |
| --- | --- | --- |
| ① | 该维**已冻结**(`v{v}_t{tt}_freeze` 的对应位) | 该版本该轨的**参数当前值**([J85]) |
| ② | 该轨是**手动接管常值段**(单段 `user_edited`) | 该段的值,**不看输出档** |
| ③ | 否则 `output_enabled` 为 **ON** | 播放头**所在段**的段值 —— 首段之前回填首段、末段之后保持末段、**段间空隙保持前一段** |
| ④ | 否则(输出 OFF,跟随宿主) | 参数当前值 |
| ⑤ | 上面取不到段值,而该轨**已连接**(`slotState=2 ∧ heartbeat 新鲜`,§2.3 同款判据) | 参数当前值([SL-361] 那一档,原样保留) |
| ⑥ | 其余(未连接,或参数句柄未就绪) | 哨兵 `−32768` |

**为什么要换**:用户 v5.6.8 真机实测「显示不一样的问题还是有……是明显的柱子位置不对。不过在
一些地方是对齐的,一些片段不对。」两页画的是**同一个量**却各走一条链,实测分叉两类:

- **段间空隙**:Output 保持**前一段**的值;`CurveEvaluator` 过了 ramp 中点就切到**后一段**。
  段只在已分析区域上产生,真工程里播放头落在空隙里的时刻远多于落在段内 —— 这是面积最大的一类。
- **过渡斜坡**:`CurveEvaluator` 在相邻段边界做 smoothstep 插值,`T_eff` 在 `gap=0` 那一支由限速
  反推 `1.5 × max(|ΔP|/15, |Δv|/3)` 并夹在 `[80ms, 6s]` ——**最长 6 秒**,不是「80ms 一闪」。

段**前** / 段**后**两侧本来就一致(都回填首段 / 保持末段),不是病灶。

**为什么改 Monitor 一侧而不是改 Output**:Output 那条链是用户裁定过的(SL-211「切进版本就该
显示曲线的起始值」、[J85] 冻结维读参数面、SL-241 两图共用同一份读回),把 Output 改成曲线求值
要一次推翻这三条,而且 web 侧没有 `CurveEvaluator`。反过来只动写方,Output 现状一字不变。

## 兼容性影响

- **布局零变化** ⇒ 新旧读方互通不受影响:段名、`abi`、结构体字段与偏移、定点标度、哨兵值全部
  未动。`abi` **不升**,段名**不升 v2**。
- **车道 `VizLanes` 不变** —— 仍是曲线在列中心时刻的点采样。轨迹图画的就是曲线本身,那条线
  该有 ramp;只有分布图那根柱换了口径。
- **旧读方(Monitor)读新写方**:分布图的柱在段间空隙与段边界上不再走曲线的 ramp,而是与
  Output 同步跳变 —— 那正是本卡要修的用户可见缺陷,**行为变化即修复目标**。
- **用户可读的取舍**(统筹裁定要求写明):改后 Monitor 在**过渡斜坡与段间空隙**里显示的是
  **「计划值」**(与 Output 那张图同源),而不是那几秒里**真实写进宿主的**声像。要看真实的
  逐时刻曲线,看**轨迹图**(车道)—— 它没变。
- **不影响任何自动化参数 / state schema / 桥面载荷**。

### 已知口径差(登记,本卡不做)

1. **「未连接但有段」的轨:Monitor 画、Output 不画。** SL-361 已登记过,与本卡的取值方式无关
   (两卡改的都是「往里写什么值」,不是「画不画」)。已另立 **SL-365**。
2. **参数句柄未就绪时两侧方向相反。** `tab-master.js` 的回落是 `num(vals[...], 0)` —— 取不到
   参数就画 **0**(居中 / 0 dB);native 侧是 NaN ⇒ 留哨兵 ⇒ 不画。一个多画一个少画,窗口极窄
   (句柄未就绪那一瞬)。#245 第 1 轮复审建议 5 提出,建议与 SL-365 一起收。

## 审批

**2026-09-06 18:22 UTC 用户批准**(原话「批」,统筹会话转达);`status/frozen-contract` 已挂;本次提交把 `docs/IPC_CONTRACT.md` §6.1 两处正文与 `docs/constitution/ipc-contract-v0.md` 的对应两处(从 `masterPlan/constitution/` 真源同步)一并落地。本变更**接续并改写 [SL-361] 的档①**(曲线精确时刻求值 → 段读回链),档②(已连接回落参数值)原样保留为上表 ⑤。

**落地记录**:`status/frozen-contract` 标签已挂;`docs/IPC_CONTRACT.md` §6.1 的两处正文(`:261` 字段注释行 + `:279` 那条 ⚠)已随 `e42000e` 落地,`286de27` 另补「①/④ 的参数面同样过已连接 ∧ 句柄就绪闸」一句;`docs/constitution/ipc-contract-v0.md` 的对应两处(`:183` / `:201`)已从`masterPlan/constitution/` 真源同步(`scripts/check-constitution-sync.ps1` PASS),未就地编辑(CLAUDE.md §5 / 06 §3.7)。
