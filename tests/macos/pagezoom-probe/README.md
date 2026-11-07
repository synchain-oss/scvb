# scvb_pagezoom_probe:真 WebKit 上量 `WKWebView.pageZoom`

mac 上缩放走 `WKWebView.pageZoom`(CSS zoom 恒 1),全部理由是「`getBoundingClientRect` 和事件坐标落在同一个坐标系」。这一条只有系统 WKWebView 自己答得了:`web-preview` 的冒烟跑在 Chromium 上,没有 pageZoom,旧 WebKit 的 rect 语义也只能模拟。背景见 [KI-16](../../../docs/KNOWN_ISSUES.md)。

独立 CMake 工程:根工程和 `tests/CMakeLists.txt` 都不引用本目录,Windows 构建与 `gates.ps1` 不受影响;不链 JUCE。CI 入口是 `.github/workflows/build-macos.yml` 的 `WKWebView pageZoom probe (real WebKit)` 步骤(在构建之前,一分钟出结论;失败不打断后面的门,由 job 末尾的判决步骤把 job 判红)。

## 页面里跑的是什么

产品代码本身:经自定义 scheme `scvb-probe` 从 `--web-root` 直接取 `web/shared/hit.js`、`shell-fit.js`、`design-box.js`。seed 的形状与 JUCE 注入的一样(`window.__JUCE__.initialisationData.nativeZoom = [true]`)。设置顺序也与插件相同:先设 `pageZoom`、发导航,再把视图放进窗口。

## 跑法

```bash
cmake -S tests/macos/pagezoom-probe -B build-pagezoom -DCMAKE_BUILD_TYPE=Release
cmake --build build-pagezoom --parallel
./build-pagezoom/scvb_pagezoom_probe --web-root web --summary pagezoom.md
./build-pagezoom/scvb_pagezoom_probe --web-root web --negative-control    # 必须退 1
```

## 判据

Output 的七个档位逐档开窗(窗口 = 设计盒 × 档位):

| 判据 | 容差 |
| ---- | ---- |
| `documentElement.clientWidth/Height` ≈ 窗口 / 档位 | ±1.5 |
| 页面里的 `shell-fit` 倍率 = 1,`style.zoom` = `"1"` | 精确 |
| `zoomLocked()` = false,`scalePresets` 七档全在 | 精确 |
| 三个靶子(左上、中间、右下)的 `getBoundingClientRect` 落在设计盒坐标上 | ±0.51 |
| `elementFromPoint` 取到各自的靶子 | 精确 |
| 真 `NSEvent` 点击进了页面时,命中元素对,`event.clientX/Y` 偏差 < 1/档位 + 1 | 见下 |

靶子分散摆放,是因为比例算错时偏差随坐标增大,右下那个一定露馅。

真点击的容差随档位走:WebKit 把事件位置取整到 view point,一个 view point = 1/档位 个 CSS px,`clientX/Y` 再取整一次。0.65 档实测靶心 (1050, 695) 落在 (1049, 693),正是这两次取整的结果。比例真算错时,右下靶子会差几十 px。

**只记录、不判红**:

- 真点击一个事件都没进页面(发不出来不等于坐标不对)。**进了页面但落点错,判红。**
- CSS zoom 0.8 对照臂:同一页面不设 pageZoom、走 CSS zoom,打印 `zoomRectMode` 与 rect,用来看这台 WebKit 是新语义还是旧语义。
- 重新导航后 `pageZoom` 还在不在。
- 每档的 `devicePixelRatio`、后备存储倍率、`screen.availWidth`。

**负对照** `--negative-control`:不设 `pageZoom` 再跑。视口不等于窗口 / 档位,必须退 1,否则这个探针没有牙。

## 退出码

| 码 | 含义 |
| --- | ---- |
| `0` | 判据全过 |
| `1` | 有判据不过 |
| `2` | **没测成**(页面没起来),不是通过;CI 记 warning、不判红 |

## 输出

stdout 上 `PAGEZOOM-PROBE <key>=<value>` 打版本(`macos` / `safari` / `webkit-bundle` / `user-agent`)、`zoomRectMode`、`real-clicks`、`reload`、`css-zoom-arm`、`negative-control` 与 `RESULT`。`--summary <file>` 再写一份 markdown,CI 把它贴进 step summary。

## 局限

- 测的是 runner 这一台的 WebKit。旧语义(WebKit 26.4 之前)是否测到,看日志里的 `zoomRectMode`。
- 用独立窗口,不是插件编辑器也不在 LUNA 里。宿主里的表现以 [LUNA 验收](../../../docs/validation/luna-acceptance.md)为准。
