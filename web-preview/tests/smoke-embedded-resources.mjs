// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB web-preview —— 嵌入资源可解析性回归(node,无 DOM)
// =============================================================================
// 守的是「插件窗口空白 → 5s 看门狗 → 兜底面板」这一类故障的静态面。
//
// 【背景】插件里的 web 资源不是按目录服务的:cmake/ScvbWebAssets.cmake 把 web/ 下的文件
// 扁平编进 BinaryData,ResourceProvider 再按 **basename** 反查(见其头注)。于是:
//   • 目录层级不进包,页面里的 ../shared/x.js 靠 URL 归一 + basename 命中,能用;
//   • 但只要有一个引用的 basename 不在打包集合里,插件里就取不到那个文件 —— 而浏览器
//     预览(按真实目录服务)照样全绿,故障只在真机上现形,且症状是「加载太慢」这种
//     完全不指向真因的文案。本套把这条差异搬到 CI 上。
//
// 断言:
//   ① 每侧打包集合内 basename 全局唯一(重名会让其中一个永远取不到);
//   ② 从 index.html 出发,模块图 / 样式表 / 图片 / 字体的每个引用都能按 basename 命中;
//   ③ 同一个文件不会被两个不同的服务 URL 取到(ES module 按 URL 定身份,会被实例化两次);
//   ④ index.html 里的 boot 守卫存在,且事件名与 C++ 侧 kBootErrorEventId 逐字一致;
//   ⑤ 该 boot 守卫是 ES5、且落在非 module 的 <script> 里(解析期错误才接得住)。
//   ⑥ [SL-355] index.html 里**恰好一条**根元素底色、内联、排在外链 css 之前;[SL-402] 起
//      它是「linear-gradient(<deg>, #rrggbb <n>% …)」的字面量,且与 C++ 侧
//      kShellBackdropStops 色标数组逐项相同(开窗白闪的第三段)。这一族是**占位**:
//      遮挡闸期间铺满整个窗口的那块底。
//   ⑥b [SL-377] tokens.css 的 `--page-backdrop` 是**外圈色**(外壳圆角之外那一圈),
//      与占位**是两个角色**:钉它 == 设计稿 body 底色,且 **!=** 占位那一族(任何一个
//      停靠点、以及占位渐变的轴中点色都不行)。
//      (SL-355→SL-370 期间这一格对拍的是「两者同值」,SL-377 用户裁定拆开后已整格重写;
//      [SL-402] 占位升成渐变后「!=」一支随之改形。)
//   ⑥c [SL-370→SL-402] 占位的 C++ 真源 == tokens.css 的 --page-gradient —— SL-370 当时
//      对拍的是「单色常量 == 渐变轴中点色」,[SL-402] 占位本身升成渐变,本格随之升级为
//      **整张色标表逐项对拍**(真源方向不变:tokens 是真源,C++ 数组与三份内联逐字照抄它)。
//   ⑦ [SL-370 / SL-429 / SL-437] index.html 里「首帧已绘」上行信号在场:事件名与 C++ 的
//      kFirstFrameEventId 逐字一致、武装是**嵌套两层** requestAnimationFrame、且**确实由
//      paint 记录接线过来**(断的是生效不是在场,见 checkFirstFrameSignal 的 (c));
//      回落路与保险定时器都在,**且保险的回调直接发信号、不绕两层 rAF**(见 (d));
//      [SL-430 前半] 载荷里那个诊断字段的**字段名与 C++ 真源逐字一致、算式真的接上了**(见 (e));
//      [SL-437] 撤网(sent=true / clearTimeout(guard))**排在 postMessage 之后**(见 (f))。
//
// 用法:node web-preview/tests/smoke-embedded-resources.mjs [仓库根绝对路径]
// 退出码:0 = 全绿;1 = 有失败项。
// =============================================================================

import { readFileSync, readdirSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve, basename, relative } from "node:path";

const ROOT =
    process.argv[2] ||
    resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

let fail = 0;
const bad = (msg) => {
    fail++;
    console.error(`  [FAIL] ${msg}`);
};

/**
 * 列目录下匹配后缀的文件(相对仓库根的 POSIX 路径)。
 *
 * `recursive` 必须逐条对上 cmake 那边用的是 `GLOB` 还是 `GLOB_RECURSE` —— 见 packagedFiles()。
 * 这个参数原先不存在(一律递归),于是 `web/shared` 这一条与 cmake 的**非递归** `GLOB` 是
 * 漂的。今天没出事只因为 `web/shared/` 底下还没有子目录;可一旦有人往里加一层
 * (计划中的 `web/shared/canvas/` 就是),本套会**照样全绿而文件根本没进包** ——
 * 正是本文件头注警告的那种「两边漂了就白守了」。
 */
function listFiles(dir, exts, recursive = false) {
    const out = [];
    const walk = (d) => {
        for (const name of readdirSync(d)) {
            const p = join(d, name);
            if (statSync(p).isDirectory()) {
                if (recursive) walk(p);
            } else if (exts.some((e) => name.endsWith(e)))
                out.push(relative(ROOT, p).split("\\").join("/"));
        }
    };
    walk(join(ROOT, dir));
    return out;
}

/**
 * 角色 -> `scvb_add_web_assets` 的 `EXTRA_DIRS`(相对 web/ 的目录名,非递归)。
 *
 * **登记制,与 src/<role>/CMakeLists.txt 逐字对应**;smoke-monitor.mjs §9 会断言那边不许
 * 出现未登记的跨角色目录。现状只有一条:`web/shared/trajectory-chart.js` 反过来 import
 * `../output/canvas/{timeline,hidpi,layers,playhead}.js`,四个文件不在 monitor 的四个目录里。
 * 终局是把 canvas/ 提到 `web/shared/canvas/`;**搬的时候注意**:`web/shared` 那条是**非递归**
 * glob,搬过去仍然不进包,必须同时把 cmake 那条改成 GLOB_RECURSE(或显式加子目录),
 * 否则这里和 cmake 一起绿、真机一起黑。
 */
const EXTRA_DIRS = {
    monitor: ["output/canvas"],
};

/**
 * 打包集合 —— 必须与 cmake/ScvbWebAssets.cmake 的 glob 逐条同口径,**包括递归与否**。
 * 两边漂了这套就白守了,故此处只有一份注释指路,没有第二份「聪明」的推导。
 */
function packagedFiles(role) {
    return [
        // role_files 是 GLOB_RECURSE(output/canvas/ 就靠这一条进 Output 的包)
        ...listFiles(`web/${role}`, [".html", ".js", ".css"], true),
        // 以下四条 cmake 用的都是非递归 GLOB
        ...listFiles("web/shared", [".js", ".css", ".png"]),
        ...listFiles("web/js/juce", [".js"]),
        ...listFiles("web/fonts", [".woff2"]),
        ...(EXTRA_DIRS[role] ?? []).flatMap((d) =>
            listFiles(`web/${d}`, [".js", ".css", ".png"]),
        ),
    ];
}

/** 抽出一个文件里的全部资源引用(与 ResourceProvider 无关,纯文本扫描)。 */
function referencesIn(file) {
    const text = readFileSync(join(ROOT, file), "utf8");
    const refs = [];
    const push = (re, group) => {
        for (const m of text.matchAll(re)) refs.push(m[group]);
    };
    push(/\bfrom\s+"([^"]+)"/g, 1); // ES import
    push(/\bimport\s+"([^"]+)"/g, 1); // 副作用 import(JUCE helper 就靠这条拉 check_native_interop)
    push(/\bimport\(\s*"([^"]+)"\s*\)/g, 1); // 动态 import
    push(/<(?:script|img)\b[^>]*\bsrc="([^"]+)"/g, 1);
    push(/<link\b[^>]*\bhref="([^"]+)"/g, 1);
    push(/url\(\s*"([^"]+)"\s*\)/g, 1); // CSS @font-face / background
    // 相对引用才归 resource provider 管;data:/http(s):/# 一律跳过。
    return refs.filter((r) => /^\.{0,2}\//.test(r) || !/^[a-z]+:|^#/i.test(r));
}

/**
 * 插件里的服务 URL 归一 —— 复刻真机行为:
 *   • origin = https://juce.backend;**入口 = /<role>/index.html**,与 C++ 侧
 *     WebViewHost::entryUrl() 同口径(真源在那里,本文件下面会对拍)。入口带上角色目录,
 *     服务 URL 空间才与 web/ 的磁盘布局逐段对齐;
 *   • 相对路径按标准 URL 解析;
 *   • ResourceProvider 只看最后一段 basename,所以**同一个文件在多个 URL 下都取得到** ——
 *     正因如此 URL 必须唯一,否则 ES module 会按 URL 各实例化一份(见 ③)。
 */
const ORIGIN = "https://juce.backend";
const servedUrl = (fromUrl, ref) => new URL(ref, ORIGIN + fromUrl).pathname;

/**
 * 持有模块级状态的 canvas 绘制模块。被实例化两份时,两个 tab 各拿一份,状态各走各的
 * (实证:Tab1 轨迹图与 Tab3 波形页的播放头对不上)。单列一条命名断言,让回归时的失败
 * 信息直指「这几个模块不是单例了」,而不是淹在通用的 URL 去重报错里。
 */
const STATEFUL_CANVAS_MODULES = [
    "timeline.js",
    "hidpi.js",
    "layers.js",
    "playhead.js",
];

/**
 * 暂缓 boot 守卫断言的角色 —— **自我删除式豁免**。
 *
 * `checkRole` 里其实是两组互不相干的断言:①②③ 打包/闭包/单例,④⑤ index.html 的 boot 守卫。
 * 它们分属两个不同的修复,绑在一起就会死锁:Monitor 的打包修复(EXTRA_DIRS,#102)让 ①②③
 * 绿而 ④⑤ 仍红;Monitor 的守卫修复(#103)反过来。于是**两个 PR 谁都不能单方面开
 * `checkRole("monitor")`**,而「断言与修复同批落地」这条原则要求各自带上各自的断言。
 * 拆开之后各走各的:#102 开 ①②③,#103 删掉这里的 "monitor" 顺带开 ④⑤。
 *
 * **豁免会自己消失**:下面 checkRole 会验证「豁免仍然必要」—— 一旦 Monitor 页真加上了守卫,
 * 这条豁免立刻变成失败项并要求删除。所以它不可能被忘在这里长期挂着(这是一条**惰性**豁免
 * 最常见的失败形态:加了 TODO,然后没人再看)。
 */
// #103 给 Monitor 页补上守卫后按自我删除条件清空;空集合保留,供未来新 role 暂缓用。
const BOOT_GUARD_PENDING = new Set([]);

function checkRole(role) {
    console.log(`\n--- ${role} ---`);
    const files = packagedFiles(role);

    // ① basename 唯一
    const byBase = new Map();
    for (const f of files) {
        const b = basename(f);
        if (byBase.has(b))
            bad(`${role}:basename 重名 ${b}(${byBase.get(b)} / ${f})`);
        else byBase.set(b, f);
    }
    console.log(`  打包 ${files.length} 个文件,basename 唯一`);

    // ②③ 从 index.html 出发遍历引用图。入口 URL 与 C++ 的 entryUrl() 同口径。
    const entry = `web/${role}/index.html`;
    const entryUrl = `/${role}/index.html`;
    const seenUrls = new Map(); // servedUrl -> 实际文件
    const urlsPerFile = new Map(); // 文件 -> 取到它的 servedUrl 集合
    const queue = [[entry, entryUrl]];
    const visited = new Set([entryUrl]);

    while (queue.length > 0) {
        const [file, url] = queue.shift();
        seenUrls.set(url, file);
        if (!urlsPerFile.has(file)) urlsPerFile.set(file, new Set());
        urlsPerFile.get(file).add(url);

        for (const ref of referencesIn(file)) {
            const nextUrl = servedUrl(url, ref);
            const target = byBase.get(basename(nextUrl));
            if (!target) {
                bad(
                    `${role}:${file} 引用 ${ref} → ${nextUrl},打包集合里没有这个 basename`,
                );
                continue;
            }
            if (visited.has(nextUrl)) continue;
            visited.add(nextUrl);
            queue.push([target, nextUrl]);
        }
    }

    for (const [file, urls] of urlsPerFile) {
        if (urls.size > 1)
            bad(
                `${role}:${file} 被两个 URL 取到(${[...urls].join(" / ")}),模块会被实例化两次`,
            );
    }
    console.log(`  从 index.html 可达 ${visited.size} 个 URL,引用全部命中`);

    // ③b canvas 模块单例(命名断言,防回归)。
    // 通用的「一个文件不许被两个 URL 取到」已经覆盖它,但那条报错太泛;这几个模块持有
    // 播放头/图层状态,分裂后的症状是「两个 tab 显示不一致」而非报错,值得单独点名。
    for (const name of STATEFUL_CANVAS_MODULES) {
        const file = byBase.get(name);
        if (!file) continue; // 该侧没打包这个模块(Input 不含 canvas)
        const urls = urlsPerFile.get(file);
        if (!urls || urls.size === 0) continue; // 打包了但没被引用,不算问题
        if (urls.size > 1)
            bad(
                `${role}:canvas 模块 ${name} 不是单例 —— 被 ${urls.size} 个 URL 取到` +
                    `(${[...urls].join(" / ")});两个 tab 会各持一份播放头/图层状态`,
            );
    }
    const canvasChecked = STATEFUL_CANVAS_MODULES.filter((n) =>
        byBase.has(n),
    ).length;
    if (canvasChecked > 0)
        console.log(
            `  canvas 模块单例:${canvasChecked} 个受检模块各只有一个 URL`,
        );

    // ③c 入口 URL 与 C++ 的 entryUrl() 同口径 —— 两边漂了,这套模拟的就不是真机行为。
    const hostCpp = readFileSync(
        join(ROOT, "src/plugin-common/WebViewHost.cpp"),
        "utf8",
    );
    if (
        !/getResourceProviderRoot\(\)\s*\+\s*config_\.role\s*\+\s*"\/index\.html"/.test(
            hostCpp,
        )
    )
        bad(
            "WebViewHost::entryUrl() 不再是 <root>/<role>/index.html —— " +
                "本套的 URL 模型已与真机不符,请同批更新",
        );

    // ④⑤ boot 守卫 —— 与①②③是**两组独立的断言**,故拆成单独函数按角色分别开关。
    // 拆的理由见 BOOT_GUARD_PENDING 的注释:两组分属两个不同的修复,绑在一起会造成
    // 「两个 PR 谁都不能单方面开这一行」的死锁。
    if (!BOOT_GUARD_PENDING.has(role)) {
        checkBootGuard(role, entry);
    } else if (
        readFileSync(join(ROOT, entry), "utf8").includes(
            "__scvbReportBootError",
        )
    ) {
        // 豁免的自我删除条件:守卫已经在场了,豁免就是错的,必须删掉否则白豁免一场。
        bad(
            `${role}:index.html 已经有 boot 守卫了 —— ` +
                `请把 "${role}" 从 BOOT_GUARD_PENDING 里删掉,让 ④⑤ 两组断言真正开起来`,
        );
    } else {
        console.log(
            `  boot 守卫断言暂缓(BOOT_GUARD_PENDING:等 ${role} 页补上守卫的那个 PR 删掉本条)`,
        );
    }

    checkShellBackdropInline(role, entry);
    checkFirstFrameSignal(role, entry);
}

/**
 * 开窗占位渐变的 C++ 真源(`kShellBackdropStops` 色标数组;解析不出返回 null)。
 *
 * [SL-402] 占位从单色升级为渐变后,真源是 PlatformWebView.h 里那个色标数组
 * (ShellBackdropStop { pos, argb })。**锚到定义块**(`inline constexpr … = { … };`,
 * 以 `\n};` 收尾):`kShellBackdropStops` 这个标识符在同文件注释里出现好几次,不锚块
 * 就会拿注释里的举例当真源。数组元素一律 `{ <float>f, 0xAARRGGBB }`,注释随写都行 ——
 * 正则只认花括号初始化器本身;解析不出 / 少于 2 个停靠点按「解析不出」处理(fail-closed)。
 */
function shellBackdropStops() {
    const hdr = readFileSync(
        join(ROOT, "src/plugin-common/PlatformWebView.h"),
        "utf8",
    );
    const m = hdr.match(
        /inline\s+constexpr\s+ShellBackdropStop\s+kShellBackdropStops\s*\[\s*\]\s*=\s*\{([\s\S]*?)\n\};/,
    );
    if (!m) return null;
    const stops = [
        ...m[1].matchAll(/\{\s*(\d(?:\.\d+)?)f\s*,\s*0x([0-9a-fA-F]{8})\s*\}/g),
    ].map((mm) => ({
        pos: parseFloat(mm[1]),
        hex: "#" + mm[2].slice(2).toLowerCase(), // 低 24 位 = css 的 #rrggbb
    }));
    return stops.length >= 2 ? stops : null;
}

/**
 * C++ 渐变**角度**常量(度;读不到返回 null)。
 * [第 1 推] `kShellBackdropAngleDeg` 与色标数组并列的命名空间级常量;锚同一形态的定义行。
 */
function shellBackdropAngleDeg() {
    const hdr = readFileSync(
        join(ROOT, "src/plugin-common/PlatformWebView.h"),
        "utf8",
    );
    const m = hdr.match(
        /inline\s+constexpr\s+float\s+kShellBackdropAngleDeg\s*=\s*(\d+(?:\.\d+)?)f/,
    );
    return m ? parseFloat(m[1]) : null;
}

/** 色标表的人话格式(报错里用):`#b5acc9@0% → …`。pos 统一存 0..1,显示按 % 折回。 */
const stopsFmt = (stops) =>
    stops.map((s) => `${s.hex}@${Math.round(s.pos * 100)}%`).join(" → ");

/**
 * 解析一段 CSS `linear-gradient(<deg>, <stop>…)` 的**停靠点表**(解析不出返回 null)。
 *
 * [SL-402] 占位的三个落点(C++ 数组 / 三份 index.html 内联 / tokens.css)都必须是
 * 「角度 + `#rrggbb <n>%` 停靠点」的完整形态,所以解析器**fail-closed**:
 *   · 必须显式写 deg 角度(CSS 允许省方向,省了就无法与 C++ 的几何对齐,宁可红);
 *   · 每个停靠点必须 `#rrggbb <n>%`(`rgb()` / `#fff` / 缺百分号都算没认出来);
 *   · 按顶层逗号切开实参逐段数(见 splitTopLevel):任何一段没被认成停靠点 ⇒ null,
 *     不静默丢弃 —— 静默丢一段算出来的仍是「少一段的渐变」,对拍会红在别处、指错真因;
 *   · 首尾必须 0% / 100%,中间严格递增。
 */
function parseLinearGradient(value) {
    const m = /^\s*linear-gradient\(\s*([\d.]+)deg\s*,([\s\S]*)\)\s*$/i.exec(
        value,
    );
    if (!m) return null;
    const stops = [
        ...m[2].matchAll(/#([0-9a-fA-F]{6})\s+(\d+(?:\.\d+)?)\s*%/g),
    ].map((mm) => ({
        pos: parseFloat(mm[2]) / 100, // **统一存 0..1**(与 C++ 数组同单位;CSS 写的 n% / 100)
        hex: "#" + mm[1].toLowerCase(),
    }));
    if (stops.length < 2) return null;
    if (splitTopLevel(m[2]).length !== stops.length) return null;
    if (stops[0].pos !== 0 || stops[stops.length - 1].pos !== 1) return null;
    for (let i = 0; i + 1 < stops.length; i++)
        if (stops[i + 1].pos <= stops[i].pos) return null;
    return { deg: parseFloat(m[1]), stops };
}

/** 两张色标表是否逐项相同(位置用数值等号,色值逐字节)。 */
function stopsEqual(a, b) {
    return (
        a.length === b.length &&
        a.every(
            (s, i) => Math.abs(s.pos - b[i].pos) < 1e-9 && s.hex === b[i].hex,
        )
    );
}

/**
 * 色标表沿轴 **50% 处的插值色**(`#rrggbb`;解析不出返回 null)。
 * 与 C++ `shellBackdropMid()` 同一条公式(50% 落在 [pos_i, pos_{i+1}] 段内线性插值);
 * ⑥b 拿它与外圈色对拍 —— 外圈色与「占位渐变的中点」同值时,两个角色又被焊死了。
 */
function gradientMidHex(stops) {
    for (let i = 0; i + 1 < stops.length; i++) {
        const a = stops[i];
        const b = stops[i + 1];
        if (a.pos <= 0.5 && 0.5 <= b.pos) {
            const t = (0.5 - a.pos) / (b.pos - a.pos);
            const rgb = [0, 2, 4].map((k) => {
                const av = parseInt(a.hex.slice(1 + k, 3 + k), 16);
                const bv = parseInt(b.hex.slice(1 + k, 3 + k), 16);
                return Math.round(av + t * (bv - av));
            });
            return (
                "#" + rgb.map((v) => v.toString(16).padStart(2, "0")).join("")
            );
        }
    }
    return null;
}

/**
 * ⑥ [SL-355] 开窗底色内联在外链 css 之前;[SL-402] 起它是**渐变字面量**、且与 C++ 色标数组
 * 逐项相同。
 *
 * 守的是「开窗先灰、再全白、才出内容」里的**白**那一段最后一节:文档已经提交、
 * `../shared/tokens.css` 与 `../shared/base.css` 还没经 ResourceProvider 取回来时,
 * 页面自己没有任何底色。此时露出来的是 WebView2 的 DefaultBackgroundColor;而那一层
 * **可能整层不在** —— JUCE 是 QueryInterface 取 ICoreWebView2Controller2、取不到就静默
 * 跳过(juce 的 WebView2::setWebViewPreferences),取不到时露的就是白。
 * 分层全貌与各段证据只写在 src/plugin-common/WebViewHost.cpp 的 HostWebView::paint 头注 ——
 * **包括「排在外链之前」只是排序事实、不是时序保证**那一条,别从本函数的 (d) 反推出
 * 「白闪已经堵住」。
 *
 * 四条断言各自独立,报错文案互不相同(拆任一条都只红它自己那句):
 *   (a) **恰好一条**作用在根元素上的 background / background-color。要求「恰好一条」而不是
 *       「至少一条」,是因为 smoke-monitor.mjs 的「零裸 hex」豁免按同一形态**全局**剥除:
 *       多出来的第二条会被那边一并剥掉,又不是这里取的 hits[0],两道门就都看不见它;
 *   (b) 取值是**字面量** —— 写成 var() 单列一句:自定义属性定义在 tokens.css 里,那等于
 *       又回到「等外链」;[SL-377] 起写成 var(--page-backdrop) 还**连颜色都是错的**
 *       (那是外圈色,不再是占位);
 *   (c) 取值能按「linear-gradient(<deg>, #rrggbb <n>% …)」完整解析([SL-402]:占位是
 *       渐变,单色字面量同样红 —— 那是 SL-402 修掉的「占位没有深浅」回归);
 *   (d) 停靠点与 C++ 真源 kShellBackdropStops **逐项相同** —— 两边同表才谈得上「盖住的
 *       和露出来的是同一张渐变」;改一边不改另一边即红;
 *   (e) [第 1 推] `min-height: 100%` 与 `background-attachment: fixed` 两条声明与 background
 *       **同块**在场(兜路的一半:background 简写把 background-color 重置成 transparent、
 *       根元素背景图按根元素自身盒定尺寸;缺任一条,外链缺席那一段不铺满甚至露白);
 *       渲染层的 applied cascade 由 smoke-ui-layout-page.mjs 的 A9 单独钉;
 *   (f) [第 1 推] 角度与 C++ 真源 kShellBackdropAngleDeg 逐字相同(「同形」包括走向)。
 *   另有一条老规矩不变:这条声明排在第一个 <link rel="stylesheet"> **之前**(它的**生效**
 *   不依赖任何外链请求的结果)。
 * 颜色**不透明**这一条不在这里重复:tests/webview/test_plugin_common.cpp 已经
 * CHECK(bg.isOpaque()),同一件事只留一份判据。
 */
function checkShellBackdropInline(role, entry) {
    const cppStops = shellBackdropStops();
    if (cppStops === null) {
        bad(
            "PlatformWebView.h 里找不到 kShellBackdropStops 的 inline constexpr 定义(开窗占位渐变的 C++ 真源)",
        );
        return;
    }

    // **先剥 HTML 注释再匹配**:这三个 index.html 的 <!-- --> 里写满中文说明,其中就有一段
    // 在解释本条声明 —— 不剥的话注释里的一句话就足以顶替真声明,判据当场失去牙齿。
    const html = readFileSync(join(ROOT, entry), "utf8").replace(
        /<!--[\s\S]*?-->/g,
        "",
    );

    // 选择器必须以 `html` 起头(前面只允许行首或 `}` / `;` / `>` 这三种边界),这样
    // `.foo html { … }` 那种后代选择器不会被当成根元素底色。**不钉排版**:`m` 标志下 `^`
    // 认的是行首,`[};>]` 认的是同一行里紧挨着的边界,prettier 怎么折行都命中。
    const hits = [
        ...html.matchAll(/(?:^|[};>])[ \t]*html\s*\{([^}]*)\}/gm),
    ].filter((h) => /background(?:-color)?\s*:/.test(h[1]));
    if (hits.length === 0) {
        bad(
            `${role}:index.html 里没有作用在根元素上的内联底色(html { background: … })` +
                ` —— 外链 css 到达之前这一页没有任何底色,开窗会露白`,
        );
        return;
    }
    if (hits.length > 1) {
        bad(
            `${role}:根元素底色声明有 ${hits.length} 条,只允许一条 ——` +
                ` 多出来的那条会被 smoke-monitor.mjs 的「零裸 hex」豁免一并剥掉,两道门都看不见它`,
        );
        return;
    }
    const hit = hits[0];
    const val = /background(?:-color)?\s*:\s*([^;}]+)/.exec(hit[1])[1].trim();

    if (val.includes("var(")) {
        bad(
            `${role}:内联底色写成了 ${val} —— 自定义属性定义在 web/shared/tokens.css 里,` +
                `用 var() 等于又回到「等外链」那条路上,本声明就白写了` +
                `([SL-377] 起 var(--page-backdrop) 还**连颜色都是错的**:那是外圈色)`,
        );
    } else if (parseLinearGradient(val) === null) {
        bad(
            `${role}:内联根元素底不是「linear-gradient(<deg>, #rrggbb <n>% …)」的完整字面量` +
                `(实得 ${val})—— [SL-402] 占位是**渐变**:写成单色是退回「占位没有深浅」的旧病,` +
                `缺角度 / 缺百分号 / 写成 rgb() 都无法与 C++ 色标数组对拍,一律判负`,
        );
    } else {
        const inline = parseLinearGradient(val);
        if (!stopsEqual(inline.stops, cppStops)) {
            bad(
                `${role}:内联渐变停靠点(${stopsFmt(inline.stops)})与 C++ 真源 ` +
                    `kShellBackdropStops(${stopsFmt(cppStops)})不一致 —— 两边同表才谈得上` +
                    `「盖住的和露出来的是同一张渐变」(改一边不改另一边即红)`,
            );
        }
        // (f) [第 1 推] 角度逐字对拍:「与成品同形」包括**走向**。C++ 常量被改(比如 156)
        // 或内联把 157deg 写成别的值,这里当场红 —— 只对拍色标表时这条漂移是静默的。
        const cppDeg = shellBackdropAngleDeg();
        if (cppDeg === null) {
            bad(
                "PlatformWebView.h 里找不到 kShellBackdropAngleDeg 的 inline constexpr 定义(占位渐变角度的 C++ 真源)",
            );
        } else if (inline.deg !== cppDeg) {
            bad(
                `${role}:内联渐变角度 ${inline.deg}deg 与 C++ 真源 kShellBackdropAngleDeg` +
                    `(${cppDeg}deg)不一致 —— 「与成品同形」包括走向,角度漂了占位与内容的` +
                    `明暗走向就岔开`,
            );
        }
        if (
            stopsEqual(inline.stops, cppStops) &&
            cppDeg !== null &&
            inline.deg === cppDeg
        ) {
            console.log(
                `  开窗占位渐变内联在场:html{background:…} = C++ 色标数组(${cppStops.length} 停靠点)@ ${cppDeg}deg`,
            );
        }
    }

    // (e) [第 1 推] 兜路两行必须与 background **同块**在场(fail-closed,缺一条即红、
    // 写在别的块不算 —— hit[1] 就是含根元素 background 的那一个块的整个声明体):
    //   · `min-height: 100%` —— background 简写把 background-color 重置成 transparent,
    //     根元素背景**图**按根元素自身盒定尺寸;外链 base.css(带 height:100%)缺席时
    //     没有这行,图的尺寸塌成内容高、甚至不绘制,兜路反而露白;
    //   · `background-attachment: fixed` —— 图按视口铺、不随内容平铺成色带。
    // 渲染层(cascade 真的生效)由 smoke-ui-layout-page.mjs 的 A9 钉,这里管源码层。
    // **先剥块内 CSS 注释再测**:删式注入或谁在块里写句说明,都不许用「注释里提了一句」
    // 顶替真声明(本仓 #188 同族的教训)。
    const blockBody = hit[1].replace(/\/\*[\s\S]*?\*\//g, "");
    if (!/min-height\s*:\s*100%/.test(blockBody)) {
        bad(
            `${role}:内联占位块里没有 min-height: 100% —— background 简写已把 background-color` +
                ` 重置成 transparent,根元素背景图按根元素自身盒定尺寸,外链缺席时图会塌成内容高` +
                `甚至不绘制,兜路反而露白(必须与 background 同块,写在别的块不算)`,
        );
    }
    if (!/background-attachment\s*:\s*fixed/.test(blockBody)) {
        bad(
            `${role}:内联占位块里没有 background-attachment: fixed —— 没有它,背景图随内容` +
                `平铺成色带,兜路那一段铺出来的不是整张渐变(必须与 background 同块)`,
        );
    }
    // (e2) [第 2 推] **顺序钉**:`background-attachment: fixed` 必须排在 `background` 简写
    // **之后** —— 简写会把 attachment 重置回 scroll,排在简写前面的 fixed 在渲染层被覆盖
    // (A9/monitor-page 那类渲染层格会红,但 output 侧没有渲染层兜底格,源码层必须钉死)。
    // 下标取自剥过 CSS 注释的块体,`background\s*:` 不会误命中 `background-attachment:`
    // (后者 "background" 后面跟的是 "-",不是冒号)。
    const bgIdx = blockBody.search(/background\s*:\s*linear-gradient/);
    const attIdx = blockBody.search(/background-attachment\s*:\s*fixed/);
    if (bgIdx >= 0 && attIdx >= 0 && attIdx < bgIdx) {
        bad(
            `${role}:background-attachment: fixed 排在 background 简写之前 —— 简写会把 ` +
                `attachment 重置回 scroll,渲染层拿到的不是 fixed(必须排在 background 之后)`,
        );
    }

    const firstLink = html.search(/<link\b[^>]*\brel="stylesheet"/i);
    if (firstLink >= 0 && hit.index > firstLink) {
        bad(
            `${role}:内联底色声明排在第一个外链 <link rel="stylesheet"> 之后 ——` +
                ` 这条声明的生效不该依赖任何外链请求的结果`,
        );
    }
}

/**
 * 设计稿 `body` 的底色(`#rrggbb`;读不到返回 null)。
 *
 * `docs/design/SCVB 设计稿.dc.html` 是**外圈色的视觉真源**(tokens.css 头注:05 未列的值
 * 取自设计稿)。全文件只有一条 `body { … }` 规则,且没有任何 HTML 注释;真要多出第二条,
 * 下面按「恰好一条」判负,不去猜该拿哪一条。
 */
function designBodyBackgroundHex() {
    const html = readFileSync(
        join(ROOT, "docs/design/SCVB 设计稿.dc.html"),
        "utf8",
    ).replace(/<!--[\s\S]*?-->/g, "");
    const rules = [
        ...html.matchAll(/(?:^|[};>])[ \t]*body\s*\{([^}]*)\}/gm),
    ].filter((h) => /background(?:-color)?\s*:/.test(h[1]));
    if (rules.length !== 1) return null;
    const m = /background(?:-color)?\s*:\s*(#[0-9a-fA-F]{3,8})\b/.exec(
        rules[0][1],
    );
    return m ? m[1].toLowerCase() : null;
}

/**
 * ⑥b [SL-377] `tokens.css` 的 `--page-backdrop` = **外圈色**,与占位那一族各钉各的。
 *
 * 【为什么这一格被整个重写】SL-355→SL-370 期间 `--page-backdrop` 与开窗预绘底是同一个值,
 * 本格当时对拍的就是「它 == C++ 真源(当时的单色常量)」。SL-377 用户裁定把两者拆成两个
 * 角色(窗口四角改回深色、占位仍是粉),那条等式**从此是错的** —— 继续留着它就是把两个角色
 * 焊死,谁也改不动其中一个。
 *
 * [SL-402] 占位从单色升成**渐变**后,「与占位不同值」的判据随之改形:外圈色不得等于占位
 * 渐变的**任何一个停靠点**,也不得等于占位渐变的**轴中点色**(= C++ shellBackdropMid() 取
 * 的那块纯色;DefaultBackgroundColor 与 FallbackPanel 的中点锚都在这条边界之外)。只查
 * 中点不够 —— 渐变是四个停靠点,只对中点时把某个停靠点写成 #191820 不会红。
 *
 * 两个角色现在各有各的边:
 *   · **占位**(kShellBackdropStops + 三份 index.html <head> 内联)由 ⑥ 钉彼此同表、
 *     由 ⑥c 钉它等于 tokens 的 --page-gradient;
 *   · **外圈色**(本格)钉两条,**都不能少**:
 *     (a) == 设计稿 `body` 的底色 —— 没有这一条,`--page-backdrop` 就成了一个谁都能随手改的
 *         自由值,(b) 那条 `!=` 照样绿;
 *     (b) != 占位那一族 —— 没有这一条,「两个角色」这件事就没有任何东西守着:把 tokens 与
 *         占位一起改回同值时 (a) 会红,但只把**占位**改成 #191820 时 (a) 是绿的,而那正是
 *         SL-377 要治的「四角与占位分不开」。
 * 只跑一次(不进 checkRole 的角色循环)—— 它与角色无关。
 */
function checkTokensBackdrop() {
    console.log("\n--- tokens.css 外圈色(⑥b)---");
    const tok = readFileSync(join(ROOT, "web/shared/tokens.css"), "utf8");
    const m = tok.match(/--page-backdrop:\s*(#[0-9a-fA-F]{3,8})\b/);
    if (!m) {
        bad(
            "web/shared/tokens.css 里找不到 --page-backdrop 的字面量取值" +
                "(它是外壳圆角之外那一圈的颜色,拿不到就无从核)",
        );
        return;
    }
    const ring = m[1].toLowerCase();

    const design = designBodyBackgroundHex();
    if (design === null) {
        bad(
            "docs/design/SCVB 设计稿.dc.html 里读不到唯一一条 body { background: #rrggbb }" +
                "(外圈色的视觉真源;设计稿改了写法就把 designBodyBackgroundHex() 一起改,这里宁可红也不跳过)",
        );
    } else if (ring !== design) {
        bad(
            `tokens.css 的 --page-backdrop ${ring} 与设计稿 body 底色 ${design} 不一致 ——` +
                ` 外圈色(外壳圆角之外那一圈)的视觉真源是设计稿,[SL-377] 用户裁定照它取深色`,
        );
    }

    const cppStops = shellBackdropStops();
    if (cppStops === null) {
        bad(
            "PlatformWebView.h 里找不到 kShellBackdropStops 的 inline constexpr 定义(开窗占位渐变的 C++ 真源)",
        );
        return;
    }
    const mid = gradientMidHex(cppStops);
    if (ring === mid || cppStops.some((s) => s.hex === ring)) {
        bad(
            `tokens.css 的 --page-backdrop(${ring})又落进了开窗占位那一族` +
                `(占位渐变 = ${stopsFmt(cppStops)},轴中点 = ${mid})—— [SL-377] 起这是**两个角色**:` +
                ` 占位铺满整窗、必须与成品外壳渐变同形(⑥/⑥c),外圈色只在外壳圆角之外那一圈可见、` +
                ` 照设计稿取深色。两者同值 = 四角跟着占位跑,用户裁掉的正是这个`,
        );
        return;
    }
    console.log(
        `  --page-backdrop ${ring} = 设计稿 body 底色, != 占位那一族(${stopsFmt(cppStops)})`,
    );
}

/**
 * 按**顶层**逗号切开一段 CSS 实参列表(括号内的逗号不算)。
 * 只为 parseLinearGradient 数「渐变里到底写了几个停靠点」用 —— 不做别的 CSS 解析。
 */
function splitTopLevel(text) {
    const out = [];
    let depth = 0;
    let cur = "";
    for (const ch of text) {
        if (ch === "(") depth++;
        else if (ch === ")") depth--;
        if (ch === "," && depth === 0) {
            out.push(cur);
            cur = "";
            continue;
        }
        cur += ch;
    }
    out.push(cur);
    return out.map((x) => x.trim()).filter((x) => x.length > 0);
}

/**
 * `--page-gradient` 的**停靠点表**(解析不出返回 null,fail-closed)。
 *
 * [SL-402] 这是「成品首屏真正可见的底」的完整形态:--page-gradient 是 .sc-shell 的底
 * (web/shared/base.css 是它的唯一消费者),而外壳几乎铺满整个插件窗口。⑥c 拿它与 C++ 的
 * kShellBackdropStops **逐项对拍**(色值 + 位置)—— 对拍整张表,不再只对中点:
 * 中点相同而走向不同(比如把首末对调)的两张渐变,占位与成品照样对不上。
 * **解析不出就 fail-closed**,不悄悄跳过:一条「算不出来所以不判」的判据和没有这条判据
 * 是一回事,而它还会顶着「有判据」的名义。
 */
function pageGradientStops() {
    const tok = readFileSync(join(ROOT, "web/shared/tokens.css"), "utf8");
    // 不钉排版:`[^;]+` 跨行,prettier 把 linear-gradient 折成几行都命中。
    const decl = tok.match(/--page-gradient:\s*([^;]+);/);
    if (!decl) return null;
    return parseLinearGradient(decl[1]); // { deg, stops }
}

/**
 * ⑥c [SL-370] 占位必须与**成品首屏真正可见的底**是同一个,而不只是几处彼此同值。
 *
 * ⑥ 对拍的是「占位那一族彼此一致」—— 它们一起写成深色时照样全绿,而用户看到的
 * 正是这一段黑(v5.6.8:「先白然后黑然后再白,最后内容」)。少的那一条就是本格:把占位
 * 钉到**外壳渐变**上,让「占位 ≠ 成品」这件事有东西会红。
 *
 * [SL-370] 当时对拍的是「C++ 单色常量 == --page-gradient 的轴中点色」;[SL-402] 占位本身
 * 升成了渐变,本格随之升级为**整张色标表的对拍**:C++ 真源 kShellBackdropStops ==
 * tokens.css 的 --page-gradient(逐项:停靠点色值 + 位置 + 顺序)。配合 ⑥ 的
 * 「内联 == C++ 数组」,三条边合成一张完整的同源链:
 *   tokens(真源) == C++ 色标数组 == 三份 index.html 内联 —— 改任何一处不同批改其余即红。
 * ⚠ [SL-377] `--page-backdrop` **不在**本格的对拍链上了 —— 它现在是外圈色,由 ⑥b 单独钉。
 */
function checkBackdropMatchesShell() {
    console.log("\n--- 开窗占位渐变 vs 外壳渐变(⑥c)---");
    const cppStops = shellBackdropStops();
    if (cppStops === null) {
        bad(
            "PlatformWebView.h 里找不到 kShellBackdropStops 的 inline constexpr 定义(开窗占位渐变的 C++ 真源)",
        );
        return;
    }
    const tok = pageGradientStops();
    if (tok === null) {
        bad(
            "web/shared/tokens.css 的 --page-gradient 解析不出「linear-gradient(<deg>, #rrggbb <n>% × ≥2,首尾 0%/100%」的停靠点表" +
                "(改了渐变写法就把 parseLinearGradient()/pageGradientStops() 一起改;这里宁可红也不跳过)",
        );
        return;
    }
    if (!stopsEqual(cppStops, tok.stops)) {
        bad(
            `开窗占位渐变(${stopsFmt(cppStops)})与成品外壳渐变 --page-gradient ` +
                `(${stopsFmt(tok.stops)})的色标不一致 —— 成品首屏铺满窗口的是 .sc-shell 的 ` +
                `--page-gradient,占位与它差一个明暗走向,用户开窗就会看见多出来的一段` +
                `(SL-370:「白 → 黑 → 白 → 内容」;SL-402:「占位没有深浅」)。` +
                `把 kShellBackdropStops 改成与 tokens 同表,并同步三份 index.html 的 <head> 内联` +
                `(⑥ 会跟着核)。**不要**顺手去改 tokens.css 的 --page-backdrop:` +
                `[SL-377] 起那是外圈色,另一个角色`,
        );
        return;
    }
    // [第 1 推] 角度对拍:「同形」包括**走向**。tokens 的角度 == C++ 的
    // kShellBackdropAngleDeg(内联那一条在 ⑥(f) 里钉,C++↔tokens 在这里钉)。
    const cppDeg = shellBackdropAngleDeg();
    if (cppDeg === null) {
        bad(
            "PlatformWebView.h 里找不到 kShellBackdropAngleDeg 的 inline constexpr 定义(占位渐变角度的 C++ 真源)",
        );
        return;
    }
    if (tok.deg !== cppDeg) {
        bad(
            `tokens.css --page-gradient 的角度 ${tok.deg}deg 与 C++ 真源 kShellBackdropAngleDeg` +
                `(${cppDeg}deg)不一致 —— 「与成品同形」包括走向,角度漂了占位与内容的明暗走向就岔开` +
                `(内联那一条由 ⑥(f) 核)`,
        );
        return;
    }
    console.log(
        `  C++ 色标数组 = --page-gradient(${stopsFmt(tok.stops)}) @ ${cppDeg}deg`,
    );
}

/**
 * ⑦ [SL-370 / SL-429] 「首帧已绘」上行信号在场且形态正确。
 *
 * C++ 侧在导航开始后把 WebView 子窗口挪出宿主可视区、由 WebViewHost::paint 铺占位底色,
 * 靠这条信号放回来(**[SL-376] 起它是唯一的正常放行路**;pageFinishedLoading 不再放行,
 * 只剩 3s 超时兜底)。机理只写在
 * src/plugin-common/WebViewRevealGate.h 一处,这里不复述,只守六条形态:
 *   (a) 事件名与 C++ 真源 WebViewHost.h 的 kFirstFrameEventId 逐字一致;
 *   (b) 那句 postMessage 的武装是**嵌套两层** requestAnimationFrame —— 单层 rAF 的回调跑在
 *       本帧提交**之前**,比嵌套两层更早,信号更不可能落在已绘之后;
 *   (c) [SL-429] 武装**确实由 paint 记录接线过来**(PerformanceObserver 在场、buffered 落在
 *       `.observe({...})` 的实参里、paint 回调体里真的调 armOnce、DOMContentLoaded 全块
 *       只出现一次且在 catch 里)—— 断的是**生效**不是在场,理由见该处;
 *   (d) [SL-429] 回落路(readyState + DOMContentLoaded)与保险定时器都在场,**且保险的回调
 *       直接发信号、不绕两层 rAF**。⚠ 它**不**断「保险排在 try 之前」与「撤网落在 signal()
 *       里(而不是 armOnce() 里)」—— 那两件事才是「信号不会永不发出」的前提,而这一格
 *       守不到,见该处。**「撤网在 signal() 里之后,具体排在哪一行」由 (f) 另断**。
 *   (e) [SL-430 前半] 载荷里的诊断字段**接上了**:字段名与 C++ 真源
 *       `kFirstFramePaintDeltaKey` 逐字一致(与 (a) 同一个 shape),且算式真的是
 *       `Math.round(performance.now() - paintStartMs)`、基线取自 paint 记录的 startTime。
 *   (f) [SL-437] 撤网(`sent = true` + `clearTimeout(guard)`)在 `signal()` 里**排在
 *       postMessage 之后**,不在 `__JUCE__` 存在性检查之前 —— 否则通道未就绪时网已撤、
 *       消息却从没真正发出去,(d) 那个保险因此空转。源码级判据,不依赖浏览器;
 *       页面级黑盒见 smoke-first-frame-page.mjs 的「B. [SL-437]」节,两条都留
 *       (判例「源码正则 ≠ 可执行」—— 互为补充,不是替代)。
 *
 * 扫描面**先剥 HTML 注释**:紧邻上方那段说明里逐字写着事件名、requestAnimationFrame 与
 * PerformanceObserver,不剥的话注释自己就能把这几条断言全顶替掉(#188 同族,连撞过三次)。
 * 各条报错文案互不相同,拆任一条只红它自己那句。
 */
function checkFirstFrameSignal(role, entry) {
    const header = readFileSync(
        join(ROOT, "src/plugin-common/WebViewHost.h"),
        "utf8",
    );
    const idMatch = header.match(/kFirstFrameEventId\s*=\s*"([^"]+)"/);
    if (!idMatch) {
        bad(
            "WebViewHost.h 里找不到 kFirstFrameEventId(开窗遮挡闸放行信号的 C++ 真源)",
        );
        return;
    }
    const eventId = idMatch[1];
    // (e) [SL-430 前半] 载荷字段名的真源同样在 WebViewHost.h,取法与上面的事件名**逐字同源**。
    const keyMatch = header.match(/kFirstFramePaintDeltaKey\s*=\s*"([^"]+)"/);
    if (!keyMatch) {
        bad(
            "WebViewHost.h 里找不到 kFirstFramePaintDeltaKey([SL-430 前半] 载荷字段名的 C++ 真源)",
        );
        return;
    }
    const paintDeltaKey = keyMatch[1];

    const html = readFileSync(join(ROOT, entry), "utf8").replace(
        /<!--[\s\S]*?-->/g,
        "",
    );

    // 只在**含该事件名的那个 <script> 块**里判形态:整页扫会把别处的 rAF 算进来。
    const rawBlock = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)]
        .map((mm) => mm[1])
        .find((body) => body.includes('"' + eventId + '"'));
    // ⚠ [SL-429] 上面剥的是 **HTML** 注释,**JS 注释还在**。块里那几行 `// buffered:true ——`
    // 之类的说明逐字写着下面要断言的关键词,不剥的话注释自己就给实现发了合格证 ——
    // 本卡的删除式实测:去掉 `buffered: true` 这个**实参**,判据照样全绿(#188 同族)。
    // ⚠ [SL-429 第 2 轮] 第一版只剥**整行** `//`,留着行尾 `//` 不动(怕误伤 `https://`)——
    // 复审当场指出那个洞还在:`po.observe({ type: "paint" }); // buffered: true` 同样能顶替
    // 下面的 buffered 断言,只是从「整行注释」挪到了「行尾注释」那一侧。现在行尾也剥,
    // `https://` 一族由模式本身避开 —— **确切条件看下面那条正则,别照这句话去改它**
    // ([SL-429 第 3 轮] 上一版在这里写了一句「靠『`//` 前一个字符不是 `:`』」,而实现里
    // 那个字符是**必须存在**的一个字符,行首就是 `//` 的那种根本匹配不到、靠上一条兜住 ——
    // 结果对、说法偏,按「消歧优先删句」把那半句删掉,只留指路)。
    // ⚠ 这条规则的已知边界(写明,不假装没有):字符串字面量里的裸 `//`(如 `"a//b"`)
    // 会被误剥。本块里没有这种写法,而且真要有,后果是**判据更严**(把实现文本剥掉 ⇒ 判负),
    // 不是更松 —— fail-closed 这一侧可以接受。
    // 光剥注释还不够,所以下面 (c) 的 buffered 断言**同时**改成了「必须落在 `.observe({...})`
    // 的实参对象里」的结构形态:两道各自独立,任一道单独失效都不会让那一格空过。
    const block =
        rawBlock === undefined
            ? undefined
            : rawBlock
                  .replace(/\/\*[\s\S]*?\*\//g, "")
                  .replace(/^[ \t]*\/\/.*$/gm, "")
                  .replace(/([^:])\/\/.*$/gm, "$1");
    if (block === undefined) {
        bad(
            `${role}:index.html 里没有发 ${eventId} 的内联脚本 —— 开窗遮挡闸就只剩` +
                ` 3s 超时这一条兜底([SL-376] 起 pageFinishedLoading 不再放行),` +
                ` 每次开窗都要等满 3 秒才放回来`,
        );
        return;
    }

    // (b) 嵌套:外层 rAF 的回调体里直接再要一帧。`[^{}]*` 限定「同一层、无嵌套块」,
    // 空白与 `window.` 前缀随便写都命中,不钉换行。
    const nested =
        /requestAnimationFrame\(\s*function\s*\([^)]*\)\s*{[^{}]*requestAnimationFrame\(/;
    if (!nested.test(block))
        bad(
            `${role}:${eventId} 的武装不是**嵌套两层** requestAnimationFrame ——` +
                ` 单层 rAF 的回调跑在本帧提交之前,信号会早于首帧,` +
                `C++ 放回来的仍是一块没画上东西的 WebView`,
        );

    // (c) [SL-429] 武装的触发条件必须是「**已经画过一帧**」—— 观察 paint 记录,而不是
    // DOMContentLoaded。SL-370 当时按 DCL 触发,信号时刻 ≈ DCL + 两个 rAF,**与 first-paint
    // 之间没有任何约束**(本机 12 轮实测 input 页上早 200~370 ms;完整数表与「哪一页被解释了、
    // 哪一页没有」的三档结论在 src/plugin-common/WebViewRevealGate.h)。
    // ⚠ [SL-429 第 2 轮] 第一版这一格断的是「三个片段**在场**」,而不是「**生效的**是 paint
    // 那条路」—— 复审给出的绕法很自然:**把武装改回 DOMContentLoaded、同时把这段
    // PerformanceObserver 留成死代码**(「怕某些浏览器不报 paint 记录,两边都挂上」),
    // 三个片段全在 ⇒ 全绿。而页面级 A 兜不住它:output 在 DCL 触发下 Δms ≈ +99 ms、Δ帧 ≥ 2
    // 本来就是绿的(它是三页里唯一天然免疫的那页),monitor 十有八九也绿 ⇒ **那两页上唯一
    // 的网就是这一格**。所以现在断的是**接线**,不是在场:
    //   · `new PerformanceObserver` 在场;
    //   · `buffered: true` 必须落在 `.observe({...})` 的**实参对象**里(不是块里任意位置 ——
    //     行尾注释那个洞见上面剥注释那段);
    //   · **paint 回调体里真的调了 `armOnce`**(死代码那条路就是死在这一条上);
    //   · `DOMContentLoaded` 在整块里**恰好出现一次,且在 catch 里** —— 多出来的那一次
    //     正是「把武装改回 DCL」的形态,回落路只许待在 catch 那条兜底路上。
    // 判据名与它守的东西现在对得上了。删除式见 PR 描述的 D5(保留 PO、武装改回 DCL ⇒ 必红)。
    const observeCallHasBuffered =
        /\.observe\s*\(\s*\{[^}]*type\s*:\s*"paint"[^}]*buffered\s*:\s*true[^}]*\}\s*\)/.test(
            block,
        );
    const paintCallbackArms =
        /new\s+PerformanceObserver\s*\(\s*function[\s\S]{0,400}?armOnce\s*\(/.test(
            block,
        );
    const dclOnlyInCatch =
        (block.match(/DOMContentLoaded/g) || []).length === 1 &&
        /catch\s*\([^)]*\)\s*\{[^}]*DOMContentLoaded/.test(block);
    const paintGated =
        /new\s+PerformanceObserver\s*\(/.test(block) &&
        observeCallHasBuffered &&
        paintCallbackArms &&
        dclOnlyInCatch;
    if (!paintGated)
        bad(
            `${role}:${eventId} 的武装不是**由 paint 记录接线过来**的 ——` +
                ` 需要 new PerformanceObserver + observe({type:"paint", buffered:true})(buffered 必须在` +
                ` observe 的实参对象里)+ paint 回调体里真的调 armOnce +` +
                ` DOMContentLoaded 全块只出现一次且在 catch 里;` +
                ` 实得 observe(buffered)=${observeCallHasBuffered}、回调接线=${paintCallbackArms}、` +
                `DCL 只在 catch=${dclOnlyInCatch}。` +
                ` 「三个片段都在场、但生效的是 DOMContentLoaded」同样会让两层 rAF 早于首帧发信号,` +
                `C++ 放回来的仍是一块没画上东西的 WebView([SL-429] 的第二段白)`,
        );

    // (d) 回落路与保险在场,且保险的回调**直接发信号**。
    //
    // ⚠ [SL-429 第 3 轮] **这一格守得住什么、守不住什么,按字面读,别读成更强的句子。**
    // 本卡在这句话上连栽两轮,两次都是「注释宣称的保证,代码给不到」:
    //   · 第 1 轮写的是 `setTimeout(armOnce, 2500)` ⇒ 保险到点还要过两层 rAF,而保险存在的
    //     唯一理由就是「paint 记录不来」,那一档最可能的成因正是 BeginFrame 停摆 ——
    //     **停了 rAF 也不回调** ⇒ 保险在它自己的头号场景里等于不存在;
    //   · 第 2 轮修了回调体,却**把保险排在 `try` 里、`po.observe()` 之后** ⇒
    //     `new PerformanceObserver` 抛错时那一行根本没执行过,**回落路压根没有保险**。
    //   两次这四条断言都是全绿的。
    //
    // 【现在这一格确实守得住】`readyState` / `DOMContentLoaded` 的回落路在场;有一个
    //   `setTimeout` 保险;**它的回调直接 `signal()`,不绕 `arm()`/`armOnce()`**。
    // 【它守不住、今天靠代码自己对的】(写明,不留一句听起来更强的话):
    //   · **保险排在 `try` 之前**(⇒ 回落路也被兜住)—— 位置关系这里**没有断言**;
    //   · **撤网(`clearTimeout`)落在 `signal()` 里、不在 `armOnce()` 里**(⇒ 网只在信号
    //     真发出去时才撤)—— 这里也**没有断言**。
    //   两条都是 [SL-429 第 3 轮] 的修法本体,删掉它们这四条照样全绿。**这是一次明知的取舍**:
    //   统筹裁定本轮不再往这一族加正则(同一处连出三轮 ⇒ 整族转卡,见 SL-431),
    //   所以宁可把缺口写在这里,也不写一句「信号不会永不发出」这种给不到的保证。
    const guardSendsDirectly =
        /setTimeout\s*\(\s*function[\s\S]{0,300}?signal\s*\(\s*\)/.test(
            block,
        ) && !/setTimeout\s*\(\s*(arm|armOnce)\s*[,)]/.test(block);
    const hasFallback =
        /DOMContentLoaded/.test(block) &&
        /readyState/.test(block) &&
        /setTimeout\s*\(/.test(block) &&
        guardSendsDirectly;
    if (!hasFallback)
        bad(
            `${role}:${eventId} 少了回落 / 保险,或保险绕了 rAF ——` +
                ` 需要 readyState + DOMContentLoaded 的回落路、一个 setTimeout 保险,` +
                `且保险的回调**直接发信号**(实得 直接发=${guardSendsDirectly});` +
                ` 绕 arm() 的两层 rAF 时,BeginFrame 停摆那一档下 rAF 根本不回调,` +
                `保险等于不存在,信号仍会永不发出`,
        );

    // (e) [SL-430 前半] 载荷里那个诊断字段:**字段名与 C++ 真源逐字一致 + 算式真的接上了**。
    //
    // 为什么非要在**这一套**里钉(它不需要浏览器、永远会跑):这一格此前的**唯一**判据是
    // 页面级 A3,而 A3 在没有浏览器时记 `[SKIP]`、CI 上 rc=3 只打 `::warning::` ——
    // 按本仓既定政策,那一层**被允许静默消失**(判例 skip-swallows-the-guard)。
    // 而字段名一旦漂,失败形态是 C++ 打 `(no paint record)`,**与「页面确实走了回落路」在
    // 用户日志里逐字同形** ⇒ 我们分不出是哪一种,SL-430 前半整件事就失去意义。
    // 所以这里照 (a) 给事件名做对拍的**同一个 shape**:从 WebViewHost.h 抓常量再与页面比。
    //
    // 两条各守一件:①`p.<字段名> = Math.round(performance.now() - paintStartMs)` 的算式形态
    //   (顺带把「减号写反 / 拿别的当基线」变成源码可判);② 基线真的取自 paint 记录的
    //   `startTime`。都落在**剥完注释**的 block 上,注释顶替不了。
    const carriesPaintDelta = new RegExp(
        "p\\." +
            paintDeltaKey +
            "\\s*=\\s*Math\\.round\\(\\s*performance\\.now\\(\\)\\s*-\\s*paintStartMs",
    ).test(block);
    const paintBaselineWired =
        /paintStartMs\s*=\s*list\.getEntries\(\)\[0\]\.startTime/.test(block);
    if (!carriesPaintDelta || !paintBaselineWired)
        bad(
            `${role}:[SL-430 前半] 载荷里的 ${paintDeltaKey} 没接上 ——` +
                ` 需要 p.${paintDeltaKey} = Math.round(performance.now() - paintStartMs)` +
                `(字段名取自 WebViewHost.h 的 kFirstFramePaintDeltaKey,逐字一致)` +
                ` 与 paintStartMs = list.getEntries()[0].startTime 两者都在;` +
                ` 实得 算式=${carriesPaintDelta}、基线接线=${paintBaselineWired}。` +
                ` 字段名两侧任一边打错一个字母,C++ 会打 (no paint record) ——` +
                `**那与「页面确实走了回落路」在日志里逐字同形**,用户那份日志就分不出是哪一种`,
        );

    // (f) [SL-437] 撤网(`sent = true` + `clearTimeout(guard)`)必须落在 `postMessage` **之后**,
    // 不能落在 `__JUCE__` 存在性检查之前 —— 否则通道未就绪时网已撤、消息却从没真正发出去,
    // 2.5s 保险(上面 (d) 那个 `setTimeout`)因此空转。
    //
    // 这是**源码级**顺序判据,不依赖浏览器(判例「源码正则 ≠ 可执行」—— 源码形态对、时序
    // 生效错,正则照样全绿;所以这一格是**补充**,不是替代:页面级黑盒见
    // smoke-first-frame-page.mjs 的「B. [SL-437]」节,两条都留)。
    //
    // 用文本位置判序,不做完整的花括号配平解析:`block` 已经是**只含这一个事件的那个
    // <script> 块**(见上面 rawBlock 的筛选),`postMessage(` / `sent = true;` /
    // `clearTimeout(guard)` 在 **firstFrame 这个块里**各自只出现一次(都在 signal() 函数
    // 体内),用 indexOf 取位置足够、不需要更重的解析。
    // ⚠ **这条前提只对 firstFrame 块成立,(f) 也只该跑在这个块上**:同一文件里的
    // bootError 块(boot 守卫那段)没有 `clearTimeout(guard)`(它没有保险定时器这一层),
    // 把 (f) 复用到那个块会因为 `clearGuardIdx` 恒为 -1 而**恒红**。想把这条判据复用到
    // 别的事件之前,先确认那个事件的块里也有同名的三段。
    const postMessageIdx = block.indexOf("postMessage(");
    const sentTrueIdx = block.indexOf("sent = true;");
    const clearGuardIdx = block.indexOf("clearTimeout(guard)");
    const disarmAfterSend =
        postMessageIdx >= 0 &&
        sentTrueIdx > postMessageIdx &&
        clearGuardIdx > postMessageIdx;
    if (!disarmAfterSend)
        bad(
            `${role}:${eventId} 的撤网(sent = true / clearTimeout(guard))没有排在 ` +
                `postMessage 之后([SL-437])—— __JUCE__ 缺席 / postMessage 不是函数时,` +
                `两行会在消息从未真正发出的情况下执行,2.5s 保险因此空转;` +
                `实得 postMessage@${postMessageIdx}、sent=true@${sentTrueIdx}、` +
                `clearTimeout(guard)@${clearGuardIdx}(-1 = 没找到)`,
        );

    // PASS 行的条件必须与上面几处判负**逐项同源**:少一项就会出现「同一套里既红又绿」——
    // 断言已经判负,而这行还在写「在场且形态正确」。(#241 复审:收紧 (c) 时漏了 readyState;
    // A/B 实测 —— 把 output 页收敛成纯 DOMContentLoaded 之后,修前打 3 行「在场」、修后打 2 行。)
    if (
        nested.test(block) &&
        paintGated &&
        hasFallback &&
        carriesPaintDelta &&
        paintBaselineWired &&
        disarmAfterSend
    )
        console.log(
            `  ${eventId} 在场:paint 记录到达后再嵌套两层 rAF 才发(带回落 + 保险,` +
                `撤网排在 postMessage 之后)`,
        );
}

function checkBootGuard(role, entry) {
    // ④ boot 守卫在场且事件名与 C++ 真源一致
    const header = readFileSync(
        join(ROOT, "src/plugin-common/WebViewHost.h"),
        "utf8",
    );
    const m = header.match(/kBootErrorEventId\s*=\s*"([^"]+)"/);
    if (!m) {
        bad("WebViewHost.h 里找不到 kBootErrorEventId(C++ 侧真源)");
    } else {
        const html = readFileSync(join(ROOT, entry), "utf8");
        if (!html.includes("__scvbReportBootError"))
            bad(`${role}:index.html 缺 boot 守卫(__scvbReportBootError)`);
        if (!html.includes(`"${m[1]}"`))
            bad(
                `${role}:index.html 的 boot 守卫事件名与 C++ 的 ${m[1]} 不一致`,
            );
        else console.log(`  boot 守卫在场,事件名 ${m[1]} 与 C++ 真源一致`);

        // ⑤ boot 守卫本身必须是 ES5 —— 它是「前端炸了」时唯一还活着的东西,
        // 自己用了新语法就会跟着一起被解析期 SyntaxError 带走,那就完全失去意义了。
        // 它也必须是**非 module 的 <script>**:module 的解析失败同样发生在执行之前。
        const guard = (html.match(/<script>([\s\S]*?)<\/script>/) || [, ""])[1];
        if (!guard.includes("__scvbReportBootError")) {
            bad(
                `${role}:boot 守卫不在第一个普通 <script> 块里(module 里接不住解析期错误)`,
            );
        }
        const es6 = [
            [/=>/, "箭头函数"],
            [/\bconst\b|\blet\b/, "const / let"],
            [/`/, "模板字符串"],
            [/\?\?|\?\./, "?? / ?."],
            [/\.\.\./, "展开运算符"],
            [/\bclass\b/, "class"],
        ];
        const offenders = es6
            .filter(([re]) => re.test(guard))
            .map(([, n]) => n);
        if (offenders.length > 0)
            bad(
                `${role}:boot 守卫用了非 ES5 语法(${offenders.join(", ")}),旧引擎上会跟着一起炸`,
            );
        else console.log("  boot 守卫是 ES5、且在非 module 的 <script> 里");
    }
}

checkRole("output");
checkRole("input");
// monitor 这一行原先不在 —— #82 引入本套时 Monitor 页还在 #90 上没合入,第三个 target 漏了。
// 本套的闭包逻辑本来就够,少的只是这一行:它当场报出 v5.2 那四个缺失模块
// (`web/shared/trajectory-chart.js` -> `../output/canvas/{timeline,hidpi,layers,playhead}.js`)。
// ④⑤ 那组守卫断言按 BOOT_GUARD_PENDING 暂缓,理由与自我删除条件见那里。
checkRole("monitor");
checkTokensBackdrop(); // ⑥b 与角色无关,只跑一次
checkBackdropMatchesShell(); // ⑥c 同上

console.log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
