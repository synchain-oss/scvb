# web/fonts — 离线子集字体

WebView UI 用的字体,**离线打包**进 VST3(DAW 联网敏感,运行期绝不拉 Google Fonts;
`web/` 与 `web-preview/` 全域禁止出现 Google Fonts 域名,CI 用 grep 断言零命中)。
每个 `.woff2` 都是 **`text=` 子集**:只含 UI 实际用到的字形。

| 文件                 | 来源家族      | 字重            | 用途(tokens.css 变量)                                   |
| -------------------- | ------------- | --------------- | -------------------------------------------------------- |
| `SpaceGrotesk.woff2` | Space Grotesk | 600             | `--ff-grotesk`:标题 / 大数字 / CTA                       |
| `IBMPlexSans.woff2`  | IBM Plex Sans | 400             | `--ff-sans`:正文默认                                     |
| `IBMPlexMono.woff2`  | IBM Plex Mono | 400             | `--ff-mono`:mono 标签 / eyebrow / dB 读数 / tabular-nums |
| `NotoSansSC.woff2`   | Noto Sans SC  | 可变(见下) | 全部 CJK(经三条字体栈的逐字回退命中);字重跟随元素      |

许可证 OFL-1.1(`REUSE.toml` 的 `web/fonts/**` 特例块声明),版权行见 `THIRD-PARTY-NOTICES.md`
(四款的家族 / 版本 / 上游 URL 已登记在那张表里)。

## 子集怎么定的

**不手写汉字常量。** `scripts/fetch_fonts.py` 扫描 `web/shared/i18n.js`(界面文案唯一真源,
其上游真源是 `05-ui-spec.md` §5)的全部字符串字面量——跳过注释,只取真正会上屏的文案——
再并上兜底集:可打印 ASCII + 法语重音(`À Â Ä Ç È É Ê Ë Î Ï Ô Œ Ù Û Ü` 及小写)+ 符号 `· — … ± ° ⚠`。
扫描结果按「是否只有 Noto 有该字形」拆两路:CJK(含 `、。「」` 等 CJK 标点与全角形式)进 Noto 子集,
其余进拉丁三款;Noto 子集额外含全部拉丁字符,作最后一道回退。

本次生成(2026-08-16)的规模:拉丁 136 字符,CJK 396 字符。三语文案上千条,靠手抄字表必然漏字,
故文案一改就重跑脚本,不再维护第二份字符清单。

> `⚠`(U+26A0):Space Grotesk / IBM Plex 两族本身没有这个字形,子集里自然也没有,
> 由字体栈回退到 `Noto Sans SC`(它有)。这正是三条栈都必须把 `'Noto Sans SC'` 排在
> 拉丁字体之后、系统字体之前的原因——回退目标是内嵌字体,不依赖宿主系统字体。

## 字重纪律

每族子集只内嵌**单一字重**(Noto 例外,是可变字体)。`tokens.css` 的 `@font-face`
声明的 `font-weight` 区间比子集实际宽,是给字体栈回退留口子;配 `base.css` 里 body 的
`font-synthesis: none`——缺字重时宁可整字回退到系统字体,也不合成伪粗/伪斜(小字会发糊)。

> **NotoSansSC.woff2 的 `font-weight: 300 700` 描述符不得删除或改成单值。**
> 请求参数虽写 `wght@400..700`,但 Google 的 `text=` 子集接口不 pin 轴:实测产物的
> `fvar` 轴是 `wght 100..900`,**默认实例是 Thin(100)**(name ID 1 = 「Noto Sans SC Thin」)。
> 现在中文能正常显示,靠的正是 `tokens.css` 那条 `font-weight: 300 700` 描述符把计算出的
> 字重打到 wght 轴并夹在 300–700;删掉或改成单值,默认 Thin 实例会接管,全部中文变成发丝细体,
> 而三道门禁一条都查不出来。

## 重新生成

新增/改动 UI 文案或语言后必须重跑(否则新字上屏是方块):

```bash
python scripts/fetch_fonts.py                  # 输出到本目录
python scripts/fetch_fonts.py --print-charset  # 只看扫出来的两个子集(拉丁 + CJK),不联网
python scripts/fetch_fonts.py --help           # 用法
```

法语重音是否真被扫进来,只能看 `--print-charset` 打印的 **LATIN** 那一行(CJK 行看不出)。

脚本联网取 Google Fonts CSS2 `text=` 接口,直接拿到子集好的 WOFF2,本地无需装 `fonttools`/`brotli`。
**只有这个脚本联网,产物是离线资产。** 换文件名要同步改 `web/shared/tokens.css` 的四条 `@font-face`。

## 离线回退(当前未启用)

若构建/开发机取不到 Google Fonts,应急口径:把 Bridge 仓 `vst-plugin/web/fonts/*.woff2`
复制过来占位,并在本节顶部醒目注明「占位子集,字形不全」。占位子集只含 Bridge 那 40 个汉字,
SCVB 的 396 字会大面积缺字,**T31 前必须联网重跑 `fetch_fonts.py` 替换**。

当前四款均为**联网实跑生成的 SCVB 真子集**,未走回退;已用 `fontTools` 校验字符覆盖:
Noto 对 532 个请求字符缺 0,拉丁三款仅缺 `⚠`(见上,按设计走栈回退)。
