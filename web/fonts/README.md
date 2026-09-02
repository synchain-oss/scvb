# web/fonts — 离线子集字体

WebView UI 用的字体,**离线打包**进 VST3(DAW 联网敏感,运行期绝不拉 Google Fonts;
`web/` 与 `web-preview/` 全域禁止出现 Google Fonts 域名,CI 用 grep 断言零命中)。
每个 `.woff2` 都是 **`text=` 子集**:只含 UI 实际用到的字形。

| 文件                 | 分发家族名(`@font-face`) | 来源家族      | 字重            | 用途(tokens.css 变量)                                   |
| -------------------- | ------------------------- | ------------- | --------------- | -------------------------------------------------------- |
| `SpaceGrotesk.woff2` | `Space Grotesk`           | Space Grotesk | 600             | `--ff-grotesk`:标题 / 大数字 / CTA                       |
| `ScvbSans.woff2`     | `SCVB Sans`               | IBM Plex Sans | 400             | `--ff-sans`:正文默认                                     |
| `ScvbMono.woff2`     | `SCVB Mono`               | IBM Plex Mono | 400             | `--ff-mono`:mono 标签 / eyebrow / dB 读数 / tabular-nums |
| `NotoSansSC.woff2`   | `Noto Sans SC`            | Noto Sans SC  | 可变(见下) | 全部 CJK(经三条字体栈的逐字回退命中);字重跟随元素      |

中间两行「分发家族名」与「来源家族」不同,是 OFL-1.1 §3 的改名结果,见下文「保留字体名(RFN)」。

许可证 OFL-1.1(`REUSE.toml` 的 `web/fonts/**` 特例块声明),版权行见 `THIRD-PARTY-NOTICES.md`
(四款的家族 / 版本 / 上游 URL 已登记在那张表里)。

## 子集怎么定的

**不手写汉字常量。** `scripts/fetch_fonts.py` 扫描 **`web/` 下全部 `.js` 的字符串字面量**
(跳过注释与正则字面量,只取真正可能上屏的文本)与全部 `.html` 的可见文本,
再并上兜底集:可打印 ASCII + 法语重音(`À Â Ä Ç È É Ê Ë Î Ï Ô Œ Ù Û Ü` 及小写)+ 符号 `· — … ± ° ⚠`。
扫描结果按「是否只有 Noto 有该字形」拆两路:CJK(含 `、。「」` 等 CJK 标点与全角形式)进 Noto 子集,
其余进拉丁三款;Noto 子集额外含全部拉丁字符,作最后一道回退。

> 扫描面不是只有 `i18n.js`。词条真源确实是 `i18n.js`(其上游是 `05-ui-spec.md` §5),
> 但**上屏的字不止来自词条**:各页 `app.js` / `tab-*.js` / `viz.js` 里有直接拼进 DOM 的
> 单位、分隔符、兜底文案与 monitor 侧未走 `data-t` 的标签。只扫 `i18n.js` 就会漏掉它们。

本次生成(2026-08-25)的规模:拉丁 **152** 字符,CJK **768** 字符,合计 920。
三语文案上千条,靠手抄字表必然漏字,故文案一改就重跑脚本,不再维护第二份字符清单。

> **这份 README 从前那句「不重跑就会上屏方块,而 CI 查不出」在 2026-08 被兑现了。**
> 2026-08-17 之后 i18n.js 连改四批词条,子集一次没重跑,`feature/v1` 主线带着 200 多个
> 无字形字符(含「卡箍」这种正经词条)一路合入,八道门禁没有一道看得见 —— 因为当时
> 根本没有任何一道门禁在看字体。现在有了:`scripts/check-font-coverage.py`(gates **3h**)
> 逐字比对「重算出的所需字符集」与「四个 woff2 的 cmap」,缺一个字就红并列出缺字。
> 改完文案忘了重跑,提 PR 前的 gates 就会拦下。

> `⚠`(U+26A0)与 `⇒ ① ② ④ ⓘ`:Space Grotesk 与 IBM Plex 两族(即 `SCVB Sans` / `SCVB Mono`
> 的来源)本身没有这些字形,
> 子集里自然也没有,由字体栈回退到 `Noto Sans SC`(它有)。这正是三条栈都必须把
> `'Noto Sans SC'` 排在拉丁字体之后、系统字体之前的原因——回退目标是内嵌字体,
> 不依赖宿主系统字体。gates 3h 把这一档记为 `[INFO]`,不判红。

### 四款上游都没有的两个字形

`⇕`(U+21D5,Wave 纵向缩放读数)与 `✕`(U+2715,选区 chip / 面板标题栏关闭键)在
**Space Grotesk 2.000 / IBM Plex Sans 3.201 / IBM Plex Mono 2.3 / Noto Sans SC 2.004
四款全量字体的 cmap 里都不存在**,重跑 `fetch_fonts.py` 也补不上。它们现在靠宿主系统字体
上屏(Windows/WebView2 = Segoe UI Symbol,画得出,但不是我们的字面)。

真正的修法是把词条换成四款都有的字形:`✕` → `×`(U+00D7),`⇕` → `↕`(U+2195)——属 UI 侧
改动,未在本次字体卡内做。在那之前,这两个字记在 `check-font-coverage.py` 的
`KNOWN_ABSENT_UPSTREAM` 白名单里,gates 3h 打 `[INFO]` 放行。**这张白名单会自己保鲜**:
条目一旦真有了字形、或已不再被任何文案使用,门禁立刻判红要求删条目 —— 它不会烂成
「加进去就永远绿」。

## 字重纪律

每族子集只内嵌**单一字重**(Noto 例外,是可变字体)。`tokens.css` 的 `@font-face`
声明的 `font-weight` 区间比子集实际宽,是给字体栈回退留口子;配 `base.css` 里 body 的
`font-synthesis: none`——缺字重时宁可整字回退到系统字体,也不合成伪粗/伪斜(小字会发糊)。

> **NotoSansSC.woff2 的 `font-weight: 300 700` 描述符不得删除或改成单值。**
> 上游 Noto Sans SC 可变字体的 `fvar` 轴是 `wght 100..900`,**默认实例是 Thin(100)**
> (name ID 1 = 「Noto Sans SC Thin」);本地子集化保留整条 wght 轴,这一点与旧的
> `text=` 产物完全一致。
> 现在中文能正常显示,靠的正是 `tokens.css` 那条 `font-weight: 300 700` 描述符把计算出的
> 字重打到 wght 轴并夹在 300–700;删掉或改成单值,默认 Thin 实例会接管,全部中文变成发丝细体,
> 而三道门禁一条都查不出来。

## 保留字体名(RFN)

OFL-1.1 **§3** 规定:Modified Version **不得使用**上游声明的 Reserved Font Name(RFN)。
**子集化就是修改**,所以本目录四款全部受这一条约束,逐款核验结论(登记在
`scripts/check-font-names.py` 的 `RESERVED`,与 `THIRD-PARTY-NOTICES.md` 的字体一节同源):

| 文件                 | 上游 RFN | 处置                                          |
| -------------------- | -------- | --------------------------------------------- |
| `SpaceGrotesk.woff2` | 无       | 上游版权行没有 RFN 声明,原样分发              |
| `ScvbSans.woff2`     | `Plex`   | 已改名:`SCVB Sans` / `ScvbSans-Regular`      |
| `ScvbMono.woff2`     | `Plex`   | 已改名:`SCVB Mono` / `ScvbMono-Regular`      |
| `NotoSansSC.woff2`   | `Source` | 分发名本就不含它,不必改名;登记在册守住不回归 |

> **改名改的是字体 `name` 表,不是文件名。** §3 管的是「呈现给用户的字体名」——它存在
> woff2 的 `name` 表里(nameID 1 家族 / 3 唯一 ID / 4 全名 / 6 PostScript 名 / 16·17 排版家族)。
> 文件改叫 `ScvbSans.woff2`、CSS 里也写了 `'SCVB Sans'`,但 `name` 表仍是 "IBM Plex Sans" 的话,
> 装进系统字体册、被 DevTools 字体面板或 PDF 导出读到的**依然是上游名**,违规照旧,
> 而 diff 看着已经改完了。改名由 `fetch_fonts.py` 的 `rename_font()` 在生成期做,
> `cmap` 与字形一字未动 —— **视觉零变化**。

版权(nameID 0)、商标(7)、许可证声明与 URL(13/14)四条署名**永不改写**,随分发逐字保留
(OFL-1.1 §2 的随附义务),因此也不参与 RFN 断言 —— OFL 惯例的版权行本身就写着
"with Reserved Font Name 'X'",`NotoSansSC.woff2` 的 nameID 7 逐字是
"Source is a trademark of Adobe…";把它们算进断言只会制造恒红。

回归由 `scripts/check-font-names.py` 守住(gates **3k** + `.github/workflows/compliance.yml` 两步),
两个面都扫:

1. **每款 woff2 的 `name` 表**逐条比对 —— **woff2 是 brotli 压缩的,grep 二进制不命中
   不等于名字已清除**,只有解表才算证据。目录里出现未登记的 `.woff2` 同样判红:新增家族
   必须先核 RFN 再进包,没有默认放行。
2. **随分发进包的文本资源**(`web/` 下的 `.css`/`.js`/`.html`,vendored 的 `web/js/juce/` 除外)
   里的字体名。违规面共三处 —— 文件名、`@font-face` family 与字体栈字面量、`name` 表 ——
   只守两头的话,把 family 改回 `"IBM Plex Sans"` 而 woff2 一字不动,解表照样全绿,而进了
   `.vst3` 的 CSS 又在向用户呈现 RFN。判据**只落在字体名上下文**(`font-family:` 声明、
   `--ff-*` 字体栈变量、含 CSS 通用族关键字的字符串字面量),不是整文件 grep:RFN "Source"
   是个常用词,整文件扫会在 `source_channels` 这类标识符上刷出几十条假红,而假红的结局
   通常是整道扫描被关掉。

门禁自身另有 `--self-test`(与隐私门禁 3j 同款,排在扫描之前),用仓内真字体与临时样例
就地合成坏样例,验证「漏改的呈现名必红(含版本/设计者/描述这些非署名槽)/ CSS 与 JS 字体栈里的
RFN 必红 / 署名记录与常用词不误伤 / vendored 目录不误伤 / 未登记字体必红」。

## 重新生成

新增/改动 UI 文案或语言后必须重跑(否则新字上屏是方块):

```bash
python scripts/fetch_fonts.py                  # 输出到本目录(联网)
python scripts/fetch_fonts.py --print-charset  # 只看扫出来的两个子集(拉丁 + CJK),不联网
python scripts/fetch_fonts.py --help           # 用法
python scripts/check-font-coverage.py          # 校验产物覆盖(gates 3h 跑的就是它,不联网)
python scripts/check-font-names.py             # 校验保留字体名(gates 3k 跑的就是它,不联网)
```

法语重音是否真被扫进来,只能看 `--print-charset` 打印的 **LATIN** 那一行(CJK 行看不出)。
**只有 `fetch_fonts.py` 联网,产物是离线资产。** 输出文件名的真源是 `fetch_fonts.py` 的
`LATIN_OUTPUTS` / `CJK_OUTPUT`:`check-font-coverage.py` 的 `LATIN_FONTS` 从它派生,
`check-font-names.py` 的 `RESERVED`(逐款人工核过的 RFN 结论,不可派生)在导入期与它对拍键集,
所以换文件名改真源那一处即可,漏登记会直接判红。另需手工同步的只剩 web 侧两处引用:
`web/shared/tokens.css` 的四条 `@font-face`(family 与 `url()`)与 `web/shared/trajectory-chart.js`
的 `STYLE_FALLBACK.mono` —— 这两处若把上游名写回去,由上文第 2 条的文本资源扫描判红。
改**家族名**还要同步 `fetch_fonts.py` 的 `RENAME`(它与 `RESERVED` 同样在导入期对拍)。
`cmake/ScvbWebAssets.cmake` 用 `file(GLOB)` 扫 `web/fonts/*.woff2`,不绑文件名。

### 两条取字体路线(按家族分工)

| 家族                    | 路线                                                | 为什么                                                                          |
| ----------------------- | --------------------------------------------------- | ------------------------------------------------------------------------------- |
| 拉丁三款                | Google Fonts CSS2 `text=`,Google 直接返回子集 WOFF2 | 拉丁集有上界(约 150 字符),URL 恒在 1KB 内;且 `text=` 返回**所请求字重的静态实例** |
| `Noto Sans SC`          | 下载上游全量可变字体,本地 `fontTools` 子集化        | CJK 集已 768 字且只增不减,`text=` 的 GET URL 会撞上限                           |

**为什么 CJK 侧必须离开 `text=`:**`text=` 走 GET,实测 URL 约 7.1KB 尚可,7.5KB 起
Google **不报 414,而是静默忽略 `text=`**,改返回 101 段 `unicode-range` 分片 CSS。老脚本的正则
只取第一段 `src:`,于是下载到一个 **12 字形的碎片**当成完整子集写进仓库,全程退出码 0。
这次重跑第一遍就踩中了这个坑(920 字符 → URL 7561 字节),是 gates 3h 当场拦下的。
现在 `fetch_fonts.py` 对这条路加了两道硬失败:URL 超 7000 字节直接退出;
返回的 `@font-face` 不止一段直接退出;并且每款下载完都用 `fontTools` 回读 cmap 断言覆盖率。

因此本脚本需要 `fontTools` + `brotli`(`pip install fonttools brotli`);
`--print-charset` / `--help` 不碰这两个包。

## 离线回退(当前未启用)

若构建/开发机取不到上游,应急口径:把 Bridge 仓 `web/fonts/*.woff2`
复制过来占位,并在本节顶部醒目注明「占位子集,字形不全」。占位子集只含 Bridge 那 40 个汉字,
SCVB 的 768 个 CJK 字会大面积缺字 —— 走回退期间 **gates 3h 会一直红**,这是刻意的:
占位子集就是缺字状态,不该看起来像正常态。

> 走回退时拉丁两款**必须改成本仓的文件名**:Bridge 那边按 §3 改名后叫 `BridgeSans.woff2` /
> `BridgeMono.woff2`,直接拷进来是未登记文件,gates 3k 会判红。改完文件名即可 —— 它们的
> `name` 表写的是 `Bridge Sans` / `Bridge Mono`(已核上游产物),不含 RFN;而 `@font-face`
> 的 family 由 `tokens.css` 自己声明,与字体内部家族名无关,不必跟着改。

当前四款均为**联网实跑生成的 SCVB 真子集**,未走回退。`check-font-coverage.py` 实测:
920 个所需字符里 918 个有字形,拉丁三款按设计缺 `⇒ ① ② ④ ⓘ ⚠`(逐字回退到 Noto),
另 2 个(`⇕` `✕`)四款上游都没有,见上文白名单一节。
