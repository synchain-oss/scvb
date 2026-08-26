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
//   ④ index.html 里的 boot 守卫存在,且事件名与 C++ 侧 kBootErrorEventId 逐字一致。
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
// ⚠ `checkRole("monitor")` 还差一步才能开:见下面的 TODO。
//
// 本套 #82 引入时只写了 output / input —— Monitor 页那会儿还在 #90 上没合入,第三个 target
// 漏了。本套的闭包逻辑本来就够,少的只是那一行:补上它**当场**报出 v5.2 那四个缺失模块
// (`web/shared/trajectory-chart.js` -> `../output/canvas/{timeline,hidpi,layers,playhead}.js`)。
//
// TODO(monitor):开这一行需要**两个**修复同时在树上,少任何一个都会红:
//   ① 打包侧 —— 本 PR 的 `EXTRA_DIRS output/canvas`(已在,上面的 EXTRA_DIRS 表已同步);
//   ② 页面侧 —— `web/monitor/index.html` 的 `__scvbReportBootError` boot 守卫(T46 的 #103,
//      同样是「第三个 target 漏了」:#82 给 Output/Input 加守卫时 Monitor 页没合入)。
// 本地实测:只有 ① 时红的是三条**守卫**断言,闭包那部分已全绿(33 个打包文件、24 个可达
// URL 全命中)。故这一行归**后落地的那个 PR** 开,由它同时验到两半。

console.log(`\n=== 结果:${fail === 0 ? "全部通过" : fail + " 项失败"} ===`);
process.exit(fail === 0 ? 0 : 1);
