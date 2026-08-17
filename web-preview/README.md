# web-preview/

**不装 DAW 也能在浏览器里看全部 UI 状态**(D3 通道 2)。画面 100% 来自真源
`web/output/index.html` / `web/input/index.html`(同源 `<iframe>` 引用),数据来自
`web-preview/mock/` 的 mock 后端。

> **06 §6.2 硬约束 1**:`web/` 是唯一真源,**web-preview/ 里没有一行复制来的 UI 代码**。
> 预览器只做两件事:把 mock 注入真源页面的 `window.__SCVB_MOCK__`,以及在顶部挂一条预览工具条。

---

## 1. 起服 · 入口

```powershell
pwsh web-preview/serve.ps1              # 固定端口 8823,打印可点 URL
pwsh web-preview/serve.ps1 -Open        # 起服并用默认浏览器打开导航页
pwsh web-preview/serve.ps1 -SmokeTest   # 起服 → 自发三条请求 → 打印结果 → 停服(自检用)
```

导航页:<http://127.0.0.1:8823/web-preview/index.html>

**必须经 HTTP 打开,`file://` 不行**,两条硬理由:

1. ES module 在 `file://` 下被 CORS 拒绝(真源页面的 `app.js` 就是 module script);
2. Chromium 把每个 `file://` 文档当**不透明源**,iframe 立刻变跨源,
   `frame.contentWindow.__SCVB_MOCK__ = …` 直接 SecurityError,注入这条路整个不成立。

站点根是**仓库根**而不是 `web-preview/` —— 壳页用 `../web/output/index.html` 引用真源,
两棵目录必须落在同一个源下,才能既解析得到路径、又保持同源。

服务器一律发 `Cache-Control: no-store`:改完灰模刷新即见,且让 iframe 每次装载都真的走网络,
把注入窗口保持在宽档(见第 4 节)。

---

## 2. 文件表

| 文件 | 职责 |
|---|---|
| `index.html` | 导航页:两插件 × 六 fixture × 关键变体的直达链接(朴素,零设计要求) |
| `output.html` | Output 薄壳页 —— 只有 `mountPreview({ role: "output" })` 一行逻辑 |
| `input.html` | Input 薄壳页 —— 同上,只差 `role` |
| `shell.js` | **注入引擎**:建 iframe、把 mock 送进 iframe 的 window、工具条与失败诊断(两张壳页共用,见第 4 节) |
| `serve.ps1` | 一键静态服务器(固定端口 / no-store / `-SmokeTest` 自检 / `-Inject` 兜底) |
| `mock/juce-bridge-mock.js` | mock 后端:契约全集函数与事件(Output 34/9、Input 7/5),数据一律经 `web/shared/mock-data.js` 生成器 |
| `mock/state-driver.js` | 场景/fixture 驱动:解析 `?fixture=`/`?scenario=`/`&loop=`,组装初始状态,按契约 §2 频率推周期事件 |

`shell.js` 是本卡在任务卡交付物清单之外新增的文件(统筹批准),目的是让两张壳页不各抄一份
注入逻辑;已记入 `scratchpad/t28/deviations.md`。

---

## 3. URL 参数

| 参数 | 取值 | 说明 |
|---|---|---|
| `fixture` | `empty` \| `fifteen-tracks` \| `misaligned` \| `channel-conflict` \| `second-output` \| `stereo-mixed` | 六个 fixture,见下表 |
| `scenario` | 05 §2.5(Output 16 个)/ §3(Input 7 个)的场景名 | 兼容口径;已实现的映射到 fixture,**未实现的回落 `fifteen-tracks` + console.warn「场景 {x} 待 T31–T36 接线」,不假装支持** |
| `loop` | `none` | 宿主**不提供**循环区:`setRange("daw_loop")` 回 `{ok:false, reason:"noLoop"}`。只对 Output 有意义 |

工具条上的 `fixture`/`scenario`/`loop` 三格走白名单显示,表外取值一律显示 `unknown` ——
既挡掉 query 注入面,也把「参数拼错了」变成肉眼可见的信号。

### 六个 fixture

| fixture | 内容 |
|---|---|
| `empty` | 0 轨连接、无 coverage、`guide_seen=true`(不弹引导);Output Tab1 空态引导卡语境;`range.mode=follow` |
| `fifteen-tracks` | 健康满配:15 轨全连、4 stereo、327 段、覆盖 84–92%。**`range.mode=follow`(默认档代表)** |
| `misaligned` | 路由失准:2–3 轨 `misalignCount>0` → 琥珀横幅① + Tab2 对应轨 ⚠ 计数(横幅①由 UI 按 `scvb.conn` 渲染,无独立 error code) |
| `channel-conflict` | Input 侧:`claim="conflict"` + `channelConflict` 错误 + `occupiedMask` 含目标位;Output 侧不受影响 |
| `second-output` | `outputReadOnly=true` + `secondOutput` 错误 + 全写控件 disabled。**`range.mode=daw_loop` 且宿主提供 loop(daw_loop 代表档)** |
| `stereo-mixed` | mono+stereo 混存:stereo 轨带 ST、`participate_in_auto_pan=false` 默认、每轨 width 旋钮可用。**降级变体 `&loop=none`** 见上 |

**[J04] `range.mode` 一律三值枚举 `follow` / `daw_loop` / `manual`,默认 `follow`**;
v0 那套「`manual` 配一对零值端点表示全曲」的哨兵约定已废除,mock 与 fixture 里不得残留。

---

## 4. 注入机制(shell.js)

真源页面里的那句是全部前提:

```js
// web/output/app.js(T27b 交付,只读)
bridge = createBridge({ role: "output", mockBackend: window.__SCVB_MOCK__ });
```

`app.js` 是 `<script type="module">`,module script 恒为 **deferred** —— 文档解析完成后才求值。
于是「注入窗口」= 新文档 commit ~ `app.js` 求值,中间隔着 `index.html` 解析 + `app.js` 及其
4 个 import 的模块图抓取(至少两轮网络往返)。同源 iframe 与壳页共用一个 event loop,
壳页在这段时间里能拿到大量 task tick。

### 三条路(A/B 已实现且默认生效,C 默认关闭)

| | 做法 | 状态 |
|---|---|---|
| **A** | 赋 `frame.src` **之前**先 `frame.contentWindow.__SCVB_MOCK__ = mock` | **实测不生效**,与规范推定一致(导航 commit 为新文档新建 global object,WindowProxy 复用但属性不继承)。保留为零成本先手,不作依赖 |
| **B** | **重注入泵**:赋 src 后立刻开一条 `MessageChannel` 任务链(无 `setTimeout(0)` 的 4ms 钳位,几乎逐 task 触发),每 tick 检查 `frame.contentWindow.__SCVB_MOCK__`,缺了就补;一旦观测到注入进的是**目标文档**(`contentDocument.URL !== "about:blank"`)即停泵 | **默认生效的那一条,实测通过** |
| **C** | `pwsh web-preview/serve.ps1 -Inject`:服务器在**传输途中**往真源 HTML 的 `</head>` 前插一段 classic script,从 `window.parent.__SCVB_PREVIEW_MOCK__` 取件。classic script 解析期同步执行,恒早于 deferred 的 `app.js`,**时序确定不靠泵** | 默认**关闭**,但已单独实测可用(见下);A/B 失灵时才开 |

#### 实测记录(T28 写手,Chromium 140 / 本机 Chrome,`http://127.0.0.1:8823`)

| 探针 | 结果 |
|---|---|
| **A** 单测:导航前写 `contentWindow.__PROBE_A__='kept'` → 赋 src → `load` 后读回 | 写入成功、`load` 后读回 **`null`** ⇒ **A 不生效** |
| **B** 单测:`injectAndMount()` 配桩 session 跑 Output/Input 各一次 | `wired` **9/9**、**5/5**;`injectCount=1`、`committedInject=true`、**注入时 `readyState="loading"`**(目标文档还在解析,远早于 deferred 的 `app.js`);`retries=0`;导航间隙内泵拿到 **2700–3000 个 tick**(耗时 38–60ms)⇒ 余量极大 |
| **C** 单测:服务器开 `-Inject`,**绕开壳页与泵**,裸建 iframe 只赋 src | `window.__SCVB_MOCK__` **已就位**,灰模 footer 提示保持原文(未落到「未接后端」分支)⇒ **C 独立可用** |
| 端到端:六 fixture × 两插件 + 三个 `loop` 降级变体,共 15 条 URL(默认档,服务器未开 `-Inject`) | **15/15** 工具条转绿(Output 9/9、Input 5/5 事件已接线),壳页零 console error |

逐 fixture 的**真浏览器目视验收**另由统筹会话完成 —— 与上面这份时序/接线实测是两件事。

方案 C 的红线自查:磁盘上的 `web/` **一个字节没动**(改的只是这一次响应体),插入的是 6 行引导代码
而非任何 UI 代码,`web/` 仍是唯一真源。代价是开着它时预览只能经 `serve.ps1 -Inject` 打开。

### 成功判据(不是纸面的)

壳页注入的不是 `session.mock` 本体,而是 `Object.create(session.mock)` 派生的**探针对象**,
只覆盖 `addEventListener` 做计数(其余函数走原型链,`bridge.js` 的
`typeof mock[name] === "function"` 照样成立,driver 手里的对象毫发无伤)。
`bridge.js` 的 `wireEvents()` 会为本侧每个事件名调一次 `addEventListener`
(Output 9 / Input 5),所以 iframe `load` 之后:

- `wired > 0` ⇔ `createBridge` **真的**接上了注入的 mock → 工具条转绿「mock 已注入 · n/N 事件已接线」,并调 `session.start()`;
- `wired === 0` ⇔ 注入迟到/失败 → 自动重试一次(带 cache-bust query,配合 `no-store` 把窗口重新拉宽);
- 重试后仍为 0 → 工具条转红 + `console.error` 打出完整诊断(注入次数 / 是否 commit 后注入 / 注入时的 `readyState` / 泵 tick 数 / 异常)。

**此时的纪律**:先用方案 C 兜底;仍不行才 append `scratchpad/t28/deviations.md`
提「需改 `web/` 一行」的申请给统筹 —— **预览器不得自行改 `web/`**。

### 与 `mock/state-driver.js` 的接口(统筹裁定)

```js
export function createPreviewSession({ role, params }) { … }
//   role   : "output" | "input"
//   params : URLSearchParams(location.search)
// 返回:{ mock, start(), stop(), info: { fixture, scenario, loop, warnings: string[] } }
```

壳页先取具名导出 `createPreviewSession`,取不到再退 `default`,**不猜第三个名字** ——
猜名字会把「driver 还没写好」伪装成「注入失败」,排查成本翻倍。

---

## 5. axe-core 跑法(dev-only,不 vendored、不进运行时)

验收口径「axe-core 可跑」= 页面结构不阻碍扫描(无 CSP / 无 shadow 屏障)。跑法:

```powershell
# 终端 1:起服
pwsh web-preview/serve.ps1

# 终端 2:扫壳页(默认会一并扫同源 iframe 内的真源页面)
npx @axe-core/cli "http://127.0.0.1:8823/web-preview/output.html?fixture=fifteen-tracks"
npx @axe-core/cli "http://127.0.0.1:8823/web-preview/input.html?fixture=channel-conflict"
npx @axe-core/cli "http://127.0.0.1:8823/web-preview/index.html"
```

要单独看真源页面本身(不含预览工具条),直接扫裸页:

```powershell
npx @axe-core/cli "http://127.0.0.1:8823/web/output/index.html"
```

`@axe-core/cli` 需要本机 Chrome + chromedriver;只在开发机跑,**不加进依赖、不进 CI 运行时**。
本卡只**记录** serious/critical 结果(进 `scratchpad/t28/deviations.md`),修复归 T31–T36。

---

## 6. 纪律

- `web/` **只读**:预览器只以 `<iframe src="../web/…">` 与 `import "../web/shared/…"` 引用,不复制、不改写;
- mock 数据一律经 `web/shared/mock-data.js` 的生成器造,**禁止自造形状**;
- 机器断言:**J59 废除的那个 10 轨口径旧 fixture 名**在 `web-preview/` 与 `web/` 全仓
  `grep -rn` 零命中(模式见 07 卡 T28 验收行)。这条断言连注释与说明文字都算命中,
  所以文档里指代它一律用「10 轨口径的旧 fixture 名」,**不把那个字面量写出来**;
  **[J04] 哨兵式全曲判断**(v0 那套「`manual` + 一对零值端点 = 全曲」)在 `web-preview/` 零命中 ——
  这条断言的 grep 模式同样**连文档里的示例都算命中**,所以此处只描述、不抄模式原文,
  模式以 07 卡 T28 验收行为准;
- 门禁:`node scripts/check-bridge-parity.mjs` + `npx prettier@3 --check "web-preview/**"`。
