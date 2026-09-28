# DAW 支持等级登记

> 状态:演进中
> 最后更新:2026-09-28
> 来源:masterPlan 10 §3.4(判定规则)、J124(Live / Studio One 标未验证,U14 修订)

## 0. 这份文件和 DAW_COMPATIBILITY 的关系

- 10 §3.4 原文写「Tier 表写进本文件,README 从此文件同步」。实施中对外的真源定在了 [DAW_COMPATIBILITY.md](../DAW_COMPATIBILITY.md) §4(12 §3.3),两份 README 的支持 DAW 表都写明转贴自那一节。
- 所以本文件**不另立真源**:它登记每个 DAW 的等级**是按什么定的、要什么才能上调**,等级本身必须与 DAW_COMPATIBILITY §4 逐行一致。两边不一致时以 §4 为准,回改本文件(发版清单 F3 每版核一次)。
- 改等级一律在同一个 PR 里同时改 DAW_COMPATIBILITY §4、`README.md`、`README.zh-CN.md` 与本文件。

## 1. 判定规则(10 §3.4,冻结)

| Tier | 条件 |
| --- | --- |
| **Tier 1 完全支持** | RT / OFF / LOOP / BLK / SR / SEEK / LIFE / AUTO / STATE / UI 全绿;FRZ / MUTE 的限制已写进文档 |
| **Tier 2 有限制** | 上述任一项有已知限制,但有用户自己能做的规避(如 Cubase 隐藏车道、REAPER 要改 VST 兼容设置、FL 不可写自动化) |
| **Tier 3 未验证 / 不支持** | 拿不到该 DAW,或存在没有规避手段的阻断问题 |

**降 Tier 3 不是「不支持」,而是「未验证」**(10 §3.0):README 的等级表要用「未验证」的措辞,不能暗示不可用。

场景代号见 [daw-matrix.md](daw-matrix.md)「场景代号」。

## 2. 当前等级(v0.9.0-rc.1 发版前)

| DAW | 版本 | 等级 | 依据 | 上调 / 维持的条件 |
| --- | --- | --- | --- | --- |
| Cubase | 14 / 15 | **Tier 1(主测)** | S1 路由 spike 的 C-1..C-14 全过(`docs/spikes/S1-daw-checklist.md` §3);成品测试包 v5.6 – v5.6.19 在 Cubase 15 Pro 上多轮实测(自动化写入、存工程重开、离线导出);已知限制有规避(自动化在 Ins 隐藏车道、Input 须在 pre-fader 区最后一格) | **维持**要两件事:① RC 包在 [daw-matrix.md](daw-matrix.md) v0.9.0-rc.1 一节的 Cubase 行全绿;② MUTE 的限制写进文档(发版清单 H3,目前没写) |
| REAPER | 7 | **Tier 2(部分验证)** | 只有 S1 spike 层:R-2 / R-3 / R-4 / R-5 / R-12 与并入 C 系列的场景通过;成品插件与自动化写入没在 REAPER 上测过;RD-04(关 GUI 不写自动化)有宿主端规避 | 成品在 REAPER 上跑完矩阵(见下面「待统筹核」) |
| Ableton Live | 12 | **Tier 3(未验证)** | 两层都没有上机(U27 让 S1 跳过 Live;J124 首发标未验证) | 真机跑完矩阵;L-5(设备停用)必测 |
| Studio One | 6 | **Tier 3(未验证)** | 两层都没有上机(U27 仅作可选对照、未执行;J124 首发标未验证);版本按 U9 为 6 | 真机跑完矩阵;Dropout Protection 异 block size 必测 |
| FL Studio | 21 | 不在 v1 支持矩阵 | DAW_COMPATIBILITY §4 注明 | — |

**待统筹核**:REAPER 的 Tier 2 取的是 DAW_COMPATIBILITY §4 的现状(#298)。按 §1 的字面,Tier 2 说的是「有限制但有规避」,不是「部分验证」;REAPER 的成品层没测过,严格读更接近 Tier 3「未验证」。维持还是下调由统筹定,改的话按 §0 四处一起改。

## 3. U14 的修订(J124)

- **U14 原裁**:每次 release 跑全矩阵(含 patch,用户接受上机成本),10 §6 的清单不裁剪。
- **J124(2026-09-28)**:Live / Studio One 没上机,README 与 DAW_COMPATIBILITY 标未验证(Tier 3);U14 按此修订为**首发只验 Cubase**。
- **对 v0.9.0-rc.1 的含义**:发版清单 F1 只跑 Cubase 一行;REAPER 本版不复测、等级沿用;Live / Studio One 在矩阵里记 N/A。
- **以后**:某个 DAW 真机跑完 [daw-matrix.md](daw-matrix.md) 那一行的全部格子才上调等级;U9 记录的用户手头 DAW 是 Cubase(主用)、Studio One 6、Ableton Live,REAPER 需另装试用版。

## 4. 变更记录

| 日期 | 变化 | 依据 |
| --- | --- | --- |
| 2026-09-28 | Cubase 维持 Tier 1;REAPER 定为 Tier 2(部分验证);Ableton Live 12 与 Studio One 6 定为 Tier 3(未验证) | #298(DAW_COMPATIBILITY 按真机实测收正)、J124 |
