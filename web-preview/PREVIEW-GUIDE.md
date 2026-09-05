# SCVB 预览通道使用指南

> 谁该读:①想自己点开看一眼 UI 的**用户**;②要出图给用户过目的 **Claude / DeepSeek 会话**。
> 一句话:预览 = `serve.ps1` 起的本地静态服务 + 壳页把 mock 注进真源页的 iframe;
> 看图有三条通路 —— **人眼(浏览器)/ Claude(真机扩展)/ 任何会话(无头截图 `shot.mjs`)**。
>
> **路径占位符**:`<repo>` = 本仓 checkout 根目录。

---

## 0. 底座:预览是怎么跑起来的

```
pwsh web-preview/serve.ps1              # 站点根 = 仓库根,固定端口 8823
→ http://127.0.0.1:8823/web-preview/output.html   ← Output 插件(4 tab)
→ http://127.0.0.1:8823/web-preview/input.html    ← Input 插件(单页)
→ http://127.0.0.1:8823/web-preview/index.html    ← 导航页(列出全部入口)
```

**页面结构是两层**,这一点决定了所有自动化怎么写:

| 层 | 是什么 | 里面有什么 |
|---|---|---|
| 顶层 = 壳页 `web-preview/output.html` | 预览外壳 | 顶部那条工具条(`role=… fixture=… mock 已注入 · 9/9 事件已接线`)+ 一个**同源 iframe** |
| iframe = 真源页 `web/output/index.html` | **真正的 UI** | 全部界面节点(`[data-tab-btn]`、`[data-gb=…]`…) |

壳页在 iframe 里挂 `window.__SCVB_MOCK__`(mock 后端),真源页的 `app.js` 用它替代 JUCE 桥。
所以:**任何选择器都要落到 iframe 文档里**,顶层文档只有工具条。页内取真源文档的写法:

```js
const D = document.querySelector("iframe").contentDocument;
D.querySelector('[data-tab-btn="wave"]').click();
```

**为什么必须经 serve.ps1,不能 `file://` 直开**:ES module 在 `file://` 下被 CORS 拒;
且 Chromium 把每个 `file://` 文档当不透明源,iframe 立刻跨源,`contentWindow.__SCVB_MOCK__`
直接 SecurityError。**预览只有这一条入口。**

服务器要点:`Cache-Control: no-store`(永远拿最新的 `web/` 真源,改完立刻可见);端口被占直接
报错退出,不静默换端口(换了端口就没法把 URL 写进文档当固定入口)。

---

## 1. 通路 A —— 用户自己看(最省事)

```powershell
# 1) 起服(独立窗口留着,别关)
pwsh '<repo>\web-preview\serve.ps1'

# 2) 浏览器打开(或加 -Open 让脚本自己开导航页)
#    http://127.0.0.1:8823/web-preview/output.html?fixture=fifteen-tracks
```

会话侧要让用户看某个具体状态时,**直接把带参数的 URL 贴给用户**即可 —— 这是最可靠的一条路,
不依赖任何扩展或无头浏览器。

---

## 2. 通路 B —— Claude 会话用真机 Chrome 看

需要 Chrome 扩展(claude-in-chrome MCP)。工具:`navigate` / `computer`(screenshot / click /
zoom / wait)/ `read_console_messages` / `javascript_tool`,尽量用 `browser_batch` 一次发多步。

**血泪纪律(踩过才写下来的)**:

1. **URL 别写错**:是 `/web-preview/output.html`,不是 `/output/`(后者 404)。
2. **等待要用宿主侧 `computer.wait`**,不要在页内写 `sleep` 循环 —— 后台标签页的
   `setTimeout` 会被 Chrome 节流,页内等待会假死。
3. **预览服务器要用独立进程起**(`Start-Process pwsh … -WindowStyle Hidden`),
   会话的后台任务会被回收,服务器跟着死。
4. **看颜色/1px 级细节必须 `zoom` 裁切放大**,整屏缩略图会把差异糊掉。
5. **自定义控件的每条交互路径都要真点一遍**(选项、键盘、取消路径)—— 只走主路径漏过两次 bug。
6. **扩展/标签页会坏**:若截图与注入连续超时,先做**对照实验**(换个已知good的 commit 或换 tab)
   再怀疑代码。曾经花很久追一个「Tab3 冻结」,最后发现同一 tab 上连已合并基线也冻 = 扩展坏了。

---

## 3. 通路 C —— 任何会话(含 DeepSeek)用无头截图

**这是给没有浏览器扩展的会话准备的通路**,零 npm 依赖(node 内置 `fetch` + `WebSocket` 驱动
Chrome DevTools 协议),Windows 上自动找 Chrome/Edge。

```powershell
# 前置:服务器已在跑(通路 A 第 1 步)
cd '<repo>'

# 最常用:切到某个 tab 抓整屏
node web-preview/shot.mjs --tab=wave  --out=web-preview/.shots/tab3.png
node web-preview/shot.mjs --tab=tracks --out=web-preview/.shots/tab2.png

# 指定场景(布防态、打印守卫、首次运行…)
node web-preview/shot.mjs --tab=wave --scenario=recapture-armed --out=shots/armed.png

# 看细节:裁一块放大 3 倍(x,y,w,h 用整屏图上量的 CSS 坐标)
node web-preview/shot.mjs --tab=wave --clip=180,320,560,180 --scale=3 --out=shots/zoom.png

# 点开某个东西再拍(选择器落在真源页文档里,脚本已自动跨进 iframe)
node web-preview/shot.mjs --tab=wave --click='[data-gb="wave-lane-3"]' --wait=800

# 排障:把页面 console 打出来
node web-preview/shot.mjs --tab=wave --console

# 换语言 / 换角色 / 整页长图
node web-preview/shot.mjs --tab=settings --lang=en
node web-preview/shot.mjs --role=input --out=shots/input.png
node web-preview/shot.mjs --tab=wave --full
```

`node web-preview/shot.mjs --help` 有完整选项表。

**DeepSeek 的视觉回路**(与 Claude 的真机回路等价):

```
改代码 → node web-preview/shot.mjs --tab=… --out=shots/x.png
       → 把 PNG 喂给 deepseek-v4-flash-vision-exp 判图
       → 按判定修 → 再拍 → 收敛
最后:PR 里附截图 + 把 URL 贴给用户过目(J72 口径:无设计稿依据的件必须由用户 preview 把关)
```

**无头 ≠ 真机**:无头 Chrome 与真机的光栅路径不完全相同(字体抗锯齿、亚像素、GPU 合成),
**颜色与像素级验收仍以真机为准**;无头图用于「有没有画出来 / 布局对不对 / 回归有没有崩」。

---

## 4. 参数速查

| 参数 | 取值 | 说明 |
|---|---|---|
| `fixture` | `empty` / `fifteen-tracks`(默认) / `misaligned` / `channel-conflict` / `second-output` / `stereo-mixed` | 世界初始状态;表外值回落默认并 warn |
| `scenario` | `empty` `connected` `misaligned` `conflict` `occupied` `group-switch` `no-output` `print-guard` `first-run` `recapture-armed` `first-run-tour` `input-first-run` `stale` | 场景 → fixture 的映射 + 快照覆写;表外值回落 `fifteen-tracks` 并 warn「待接线」 |
| `loop` | `host`(默认) / `none` | 宿主是否提供循环区(`daw_loop` 档可用性) |
| `--tab`(shot.mjs) | `master` `tracks` `wave` `settings` | 对应 整体调整 / 轨道 / 波形与分段 / 设置 |
| `--lang`(shot.mjs) | `zh` `en` `fr` | 走 UI 语言胶囊切换 |

DOM 定位约定:一切可测节点都带 `data-gb="…"` 锚点(如 `wave-lane-3`、`segment-inspector`、
`wave-toolbar`),状态一律写 `data-*` 属性 —— 自动化选择器优先用它们,别用 class 或文本。

---

## 5. 排障表

| 症状 | 原因 | 处置 |
|---|---|---|
| 页面 404 | URL 写成了 `/output/` | 用 `/web-preview/output.html` |
| 起服报「端口被占」 | 8823 已有服务器 | 那就是它,直接用;要重启先关掉旧进程 |
| 页面空白 / 工具条显示 mock 未注入 | 走了 `file://`,或 iframe 跨源 | 必须经 8823 打开 |
| `shot.mjs` 说「找不到 tab 按钮」 | 选择器落在了壳页顶层 | 脚本已自动跨 iframe;若自写脚本,取 `iframe.contentDocument` |
| `shot.mjs` 说找不到 Chrome | 非默认安装路径 | `--chrome=<chrome.exe 路径>` |
| `shot.mjs` **退 2**,报「导航失败:net::ERR_CONNECTION_REFUSED」 | 预览服务没起 | 先 `pwsh web-preview/serve.ps1` |
| `shot.mjs` **退 2**,报「主文档 HTTP 404」 | URL 拼错(同本表第一行) | 用 `/web-preview/<role>.html` |
| `shot.mjs` **退 2**,报「壳页报注入失败」 | 主文档 200,但 iframe 里的真源页没起来(`web/<role>/index.html` 挪走/改名,或 mock 后端抛错、driver.start() 抛错) | 按消息里壳页带出的**原话**查;`--console` 看页面 console |
| `shot.mjs` **退 2**,报「壳页 16s 内没离开『未就绪』态(data-ok=wait)」 | 壳页的 `.pv-status` 一直停在 `wait`(初始值就是它):注入还在重试、或壳页从头到尾没起来。**拍下去只会是中间态**,所以判负而不是放行 | 看消息里壳页带出的**原话**;`--console` 看页面 console。若确是慢而不是坏,`shell.js` 的重试预算(`PUMP_DEADLINE_MS` / `maxRetries`)才是该调的地方 |
| `shot.mjs` **退 2**,报「注入 `__D`(真源文档)失败 / 落地页无法求值 / 落地页读不到壳页状态 / 页内点击 … 失败」(括号里带页内异常原文) | 落地之后本脚本自己的探针求不出值 —— 页面多半没真的起来(执行上下文没了、被换成错误页) | 消息前半句说的就是哪一步;`--console` 看页面 console,再确认服务与 `web/<role>/index.html` 都在 |
| `shot.mjs` **退 1**,报「--eval 表达式抛错」或「选择器语法错:…」 | 抛错源自你给的输入(`--eval` 的表达式、`--click`/`--lang`/`--tab` 的选择器),与页面起没起无关 | 照括号里的页内异常改那个参数;`--lang` / `--tab` 的值是**原样插进选择器**的,带引号会拼出非法选择器。退 1 不退 2 就是为了把「你写错了」和「页面没起来」分开 |
| 截图是白屏/半截 | 首帧未画完 | 加 `--wait=3000`,或 `--settle` 调大 |
| 真机截图/注入连续超时 | 扩展或该标签页坏了 | 先用已知好的 commit 做对照实验,再怀疑代码 |

---

## 6. 红线

- `web/` 是 UI 的**唯一真源**;预览层(`web-preview/`)只准注入 mock 与包壳,不得复制 UI 代码。
- `docs/design/` 的设计稿**一字节不动**(J69 字节一致校验)。
- 截图产物写 `web-preview/.shots/`(已 gitignore),不入库。
- 无设计稿依据的件按 J72:实现后 **PR 附截图 + 用户 preview 把关**。
