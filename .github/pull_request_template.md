<!--
SCVB 版 PR 模板(12 §1.7)。「冻结契约自查」三项为 SCVB 冻结面。
-->

## 这个 PR 做了什么

<!-- 一段话。关联 issue: Closes #___ -->

## 变更类型

- [ ] fix - [ ] feat - [ ] docs - [ ] refactor - [ ] test - [ ] ci - [ ] chore

## ★ 冻结契约自查(三项都必须勾"未触碰",否则本 PR 会被直接关闭)

- [ ] 未新增/删除/改名任何自动化参数,未改 ParamID 或 index 顺序(params-v0)
- [ ] 未改动 IPC 共享内存段名或结构体布局;若改动,已 `abi+1` 且段名升 v2,并附迁移说明(ipc-contract-v0 §5)
- [ ] 未在不写迁移函数的情况下改动 state schema(params-v0 §四)

## 本地 gates(逐项贴结果)

- [ ] `cmake --build build --config Release` 成功
- [ ] `ctest --test-dir build --output-on-failure`(Catch2)全绿
- [ ] `clang-format --dry-run --Werror` 无输出
- [ ] 本地 **全量** `pluginval --strictness-level 5`(含 GUI,真机 Windows 11)对两个 .vst3 均通过
- [ ] 若触碰音频线程:已确认 processBlock 内无分配/锁/IO/日志/异常

## DAW 实测(触碰路由/自动化/state 时必填)

| DAW          | 实时 | 离线渲染 | 备注 |
| ------------ | ---- | -------- | ---- |
| REAPER       | ☐    | ☐        |      |
| Cubase       | ☐    | ☐        |      |
| Ableton Live | ☐    | ☐        |      |
| Studio One   | ☐    | ☐        |      |

## 截图 / 录屏

## DCO

- [ ] 所有 commit 均已 `git commit -s`(Signed-off-by)
