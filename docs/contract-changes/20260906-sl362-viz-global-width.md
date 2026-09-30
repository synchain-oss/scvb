# 契约变更说明 —— 20260906-sl362-viz-global-width

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— §6.1 `VizFrame` 的 `_reserved[0]` 具名为
      `global_width_plus_one`;**尺寸与所有既有字段偏移零变化**,不升 abi、不改段名(论证见下)
- [ ] docs/STATE_SCHEMA.md(state schema)
- [ ] docs/SCVB_CONTRACT.md(桥面契约)
- [x] tests/golden/ —— `ipc-layout.txt` 增一行 `field VizFrame.global_width_plus_one offset 88`,
      并把 `field VizFrame._reserved offset 88` 顺延为 `92`。**没有既有行被改值**,只有 `_reserved`
      的起点随之顺延。金样由 `test_ipc_layout` 逐行对拍代码真身,不是手抄。

## 变更内容

viz 段带上**全局「最大角度」**(Output 的 `width` 参数当前值),让 Monitor 的分布图与 Output
在任何设置下画得一样。

两页共用 `distGeometry(pan, volDb, widthPct, globalWidthPct)` —— 有效 pan = 名义 pan ×
globalWidth/100,张开半宽同缩放。Output 那侧传的是本进程的参数值;**Monitor 此前拿不到这个值、
恒按 100 画**,于是用户把「最大角度」调离 100 时两页的柱位当场对不上(用户 2026-09-06 实测)。

| 面 | 改动 |
| --- | --- |
| 段 | `VizFrame._reserved[0]`(偏移 88)具名为 `global_width_plus_one`;`_reserved` 变 `[9]`,起点 92 |
| 写 | `VizPublisher` 每帧打包(与 per-track `widthPct` **同一条** `vizPackFixed` 编码);`OutputProcessor` 从 `handles_.rawWidth` 取值,句柄未就绪 → NaN → 段内哨兵 |
| 读 | `VizPlane` 解码回 `VizSnapshot::globalWidthPct`;桥在哨兵时发 `undefined`;`vizGlobalWidthPct()` 回落 100 |

### 编码:`定点值 + 1`,`0` = 写方未提供

**为什么不能直接存定点值**:旧写方(本卡之前的 Output)覆盖式初始化把这一槽清成 `0`,而 `0`
在定点编码里是**合法宽度**(0% = 全收拢到中央)。直接存的话,新读方配旧写方会把「没这个字段」
读成「用户把最大角度调到了 0」——分布图把 15 根柱全挤到中线,**而那看起来像一张正常的图**。
`+1` 之后 `0` 成了不可能出现的真值,可以安全地当哨兵。

## 为什么不属 §5 意义上的「布局改动」

契约 §5 / §6.1 规定「布局改动必须 abi+1 且段名升 v2,新旧不互认」。本变更**不触发**它:

1. **尺寸零变化**:`sizeof(VizFrame)` 仍是 128。
2. **既有偏移零变化**:偏移 88 之前的每一个字段一个字节没动;唯一移动的是 `_reserved` 的
   **起点**(88 → 92),而它是留白、不承载任何语义。
3. **§6.1 的尾部留白本就是这个用途** —— 契约自己写着「尾部留白,**后续增补不改既有偏移**」,
   `VizTrackColors` 头注同样写着「字段先行落段…读写两侧无需改布局」。
4. **两向都优雅降级、不拒连、不半兼容**(见下),没有 J40 要禁的那种「半兼容惨案」风险。

**这个判定有机检,不只是散文**:三条 `static_assert` —— 新字段 `== 88`(落在原留白起点)、
`_reserved == 92`(剩余留白的新起点)、`sizeof == 128`。谁往前插字段或把它挪走,必有一条红。

**对照:走 abi+1 + 段名 v2 的代价**(本变更**没有**选它)。viz 段的 `abi` 挂的是**总 abi**、
段名是**全段共用前缀**,升级会让旧 Input 与新 Output 在 registry / ctrl / audio ring / feat ring
**全部拒连**,音频直接不通。为一个图表缩放对齐付这个代价不成比例。

## 兼容性影响

- **旧读方(Monitor)+ 新写方**:旧读方把该槽当填充忽略 ⇒ 分布图照 100 画,**与本变更之前
  逐字一致**。不拒连、无横幅、无行为差异。
- **新读方 + 旧写方**:槽为 `0` ⇒ 解码回哨兵 ⇒ `vizGlobalWidthPct()` 回落 **100**,同样与之前
  一致。**这一条有专门的判据**(见 PR 的 ipc 那一格)。
- **abi / 段名不变** ⇒ 所有既有跨版本组合的互认关系一个字节没变。
- **不影响** registry / ctrl / audio ring / feat ring 任何一段,也不影响自动化参数与 state schema。

## 审批

用户 2026-09-06 原话:**「monitor 也要加,下个版本加」**(统筹转述)。
挂 `status/frozen-contract` 标签,待用户批准后合入(06 §3.7)。
