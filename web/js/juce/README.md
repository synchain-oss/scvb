# web/js/juce/

JUCE 官方 web JS helper(`index.js` / `check_native_interop.js`),随 JUCE 许可 AGPL-3.0-or-later,
版权归 Raw Material Software Limited(不加 Synchain 版权头,由 `REUSE.toml` 声明)。

**已于 T27 第二波填充**(JUCE 8.0.8 原样副本,仅行尾按本仓 `.gitattributes` 取 LF,内容零改写;
与 Bridge 仓 `vst-plugin/web/js/juce/` 同名文件 byte-identical)。由 `web/shared/bridge.js` 在
检测到真 JUCE 宿主时**惰性** `import("../js/juce/index.js")` 消费。

> **别在预览页 import 这两个文件**:`check_native_interop.js` 在没有 JUCE 宿主的普通浏览器里
> 会自己造一个占位 `window.__JUCE__`(空的 `initialisationData.__juce__functions` + 纯本地
> `backend`)。`createBridge()` 已按「`backend` 存在且 `__juce__functions` 非空」判定真宿主、
> 并让显式注入的 `mockBackend` 优先,所以不会再被骗过去;但页面自己直接调 `getNativeFunction()`
> 仍会得到一个永不 resolve 的 Promise。预览一律走 `createBridge({ role, mockBackend })`。
