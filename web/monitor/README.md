<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# web/monitor/ —— SCVB Monitor 页面资源目录

**页面实现归 T46(PR #90)**,本目录在 T45(PR #92)只落两件事:目录约定,与 native 侧的接口真源指路。

T45 早期在这里放过一个占位页(`index.html` + `app.js`),用途是开发期证明
「Output → viz 段 → Monitor → web」整条链通。T46 的真实现落地后它就只剩害处了 ——
两个 PR 各带一份同名文件,合并时必冲突,而占位版没有任何值得保留的内容。**已删除。**

## 目录约定(T45 交付的部分)

- 页面资源放本目录,与 `web/input/`、`web/output/` 同构;
- 复用 `../shared/` 的 `tokens.css` / `base.css` / `i18n.js` / `track-colors.js` / `trajectory-chart.js`
  ——T43 已把轨迹图组件与 15 色轨道调色板落进 `web/shared/`,零重复实现;
- 词条走 `monitor.*` 组三语,入 `web/shared/i18n.js`;
- 设计盒 **960×720、七档缩放**,真源 = `web/shared/design-box.js` 的 `DESIGN.monitor`
  (C++ 侧 `src/core/DesignBox.h` 由 `scripts/gen-design-box.py` 从它生成,gate 3d 逐值对拍)。

## 资源嵌入

本目录的文件经 `cmake/ScvbWebAssets.cmake`(`juce_add_binary_data`)编进 Monitor 插件二进制,
`MonitorEditor` 的 `resourceSource` 从生成的 `ScvbMonitorWebData` 取;`web-preview` 也直接消费同一份文件。
「页面真会去取的每个文件都进了包」由 ctest 的 `test_web_assets_embedded` 兜。

## native 侧接口真源

| 面 | 真源 |
|---|---|
| 桥函数 / 事件名 | `src/monitor/MonitorBridgeApi.h`(1 专属函数 `setObservedGroup` + 4 事件) |
| `scvb.viz` / `scvb.state` 载荷字段 | `src/monitor/MonitorEditor.cpp` 的 `buildVizPayload` / `buildStatePayload` |
| viz 段布局与降采样口径 | `src/core/ipc/VizPlane.h` + `docs/contract-changes/20260825-viz-segment.md` |
| 段布局逐行冻结 | `tests/golden/ipc-layout.txt` |

**两侧不是靠信件同步**:T46 的 `web/monitor/viz-contract.js` 是段契约的 JS 侧镜像,`web-preview/tests/smoke-monitor.mjs`
把它同时对拍上表的 golden(段字段名 + 偏移升序)与 `MonitorEditor.cpp` 的 `setProperty` 名
(按函数切开比,并有反向断言:桥送出而镜像表没有的字段当场红)。
**改名字会在对方的机检上立刻现形;但改语义不会** —— 同一个字段名换含义两面都是绿的,那种改动必须在信里明说。

## 桥面已进冻结契约

Monitor 桥面已随 [J81] 转正:`docs/SCVB_CONTRACT.md §7` 有 `manifest.monitor`(正文 §10),
`web/shared/bridge.js` 有 `BRIDGE_FUNCTIONS.monitor` / `BRIDGE_EVENTS.monitor`,
`scripts/check-bridge-parity.mjs` 的 [M] 块把 manifest、`bridge.js` 与 `src/monitor/MonitorBridgeApi.h` 三向比对。
`monitor-bridge.js` 里的两张名表已不是比对面,以 `bridge.js` 为准;它按文件头自陈应退化成薄封装或删除(T46 后续小项)。
