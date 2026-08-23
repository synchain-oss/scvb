// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB Output · 泳道波形渲染(分块拉取 LRU + 静态层位图;T33 Wave 1 交付)。
// -----------------------------------------------------------------------------
// 真源:
//   • 契约 §1.27 `requestWaveform(ch, startS, endS, cols)` —— **request/response,
//     一次调用恰好一次 completion,绝不进事件流**;cols 1..4096;未覆盖列
//     `covered=0` 且 `minDb=maxDb=-160` 哨兵;`valleys[]` 秒、升序(边界吸附用);
//   • 05 §6.3:拉取式分块,块粒度 = 视口宽,LRU 缓存 **8 块/轨**;拖动平移先 blit
//     旧位图,**静止 120ms** 后取新块;每轨一条离屏位图;VAD 罩、覆盖条、stale
//     条纹画在同层(边界/手柄/选区/播放头在共享动态层,归 tab-wave.js);
//   • 05 §2.3(311):kw_ms→dB 的 min/max 包络柱;未覆盖 = 空白底纹;stale =
//     琥珀斜条纹叠加(⚠ 角标是 DOM 件,Wave 2);不同 passId 底色微差;
//   • 05 §2.3(316)+ 统筹裁定 B-07:底部覆盖条 **2px + accent 薰衣草**。
//
// 本文件零 DOM 查询:画到调用方给的 2D ctx 上;取数走注入的 request 函数。
// 颜色经 palette 参注入;**Wave 1 调用方不传 palette,恒用 DEFAULT_PALETTE
// 字面镜像**(canvas 读不到 CSS 变量)—— 改 tokens.css 对应色须同步这里;
// [Wave 2] tab-wave.js 在 mount 时 getComputedStyle 换算真值注入(TODO 已挂)。
// =============================================================================

/** LRU 容量:8 块/轨(05 §6.3 逐字)。 */
export const TILE_LRU_CAP = 8;

/** 视口静止多久才取新块(ms;05 §6.3「静止 120ms 后取新块」;brief §0.8)。 */
export const IDLE_REFETCH_MS = 120;

/** 契约 §1.27:cols 上限。 */
export const MAX_COLS = 4096;

/** 契约 §1.27:未覆盖列哨兵 dB。 */
export const UNCOVERED_DB = -160;

/** 包络显示地板(dB):低于此值画最细柱(特征流值域,mock 同口径 -80)。 */
export const ENV_FLOOR_DB = -80;

/**
 * VAD 标注带透明度(**唯一真源**)。
 *
 * 档位史:图谱 §12 ② 给两档「实景 .07 / 放大态 .13」→ Wave 2 亲验 .07 在深底上
 * 读不出绿,取 .13 → Wave 4 用户 preview 判「太灰太淡」,提到 .20 并换更鲜亮的绿
 * (tokens.css §20a `--wave-vad`)+ 上下 1.5px 亮线 → **Wave 5 用户 preview 第二轮
 * 判「像绿底放了个波」**:罩把包络之外的上下空白**全部**填满,绿面积比波形还大,
 * 喧宾夺主。
 *
 * **统筹裁定(J72 用户口径,Wave 5)**:形制从「满泳道半透明罩」改为
 * **泳道顶部一条标注带**(`VAD_BAND_PX`,含最上沿的 1.5px 亮线),带下不再填。
 * 带窄了就可以更实 —— alpha .20 → **.55**:与深底(≈44,40,54)合成出
 * (68,132,105) 的明确绿,一眼分得出有声/静音,而波形本体所在的中带一寸不染。
 * 偏离 05「半透明绿罩」的字面(那句预设的是满高罩),deviations §S 登记。
 *
 * ⚠ tab-wave.js 的 computedPalette() 只把 rgb 换成 tokens 真值,**alpha 从这里
 * import**——两处各写一份字面量正是本波踩过的坑(smoke-tab3 有断言钉住)。
 */
export const VAD_ALPHA = 0.62;

/**
 * VAD 标注带高(CSS px;Wave 5 裁定「顶部一条 5–6px,含 1.5px 亮线」取 5)。
 *
 * 5 而不是 6 的理由是几何咬合:`envelopeHalfPx` 的上限是 `h/2 − 4`,故**任何**
 * 行高下包络能顶到的最高点恒为 y=4 —— 带取 5 时只与最高的那几根柱尖重叠 1px,
 * 且包络在带**之后**画(层序 ③→④),那 1px 由粉柱盖回去,「粉白不被染」成立。
 * 22px 最矮档下带占 23% 高度仍读得出,88px 最高档下也不至于糊成一条边。
 */
export const VAD_BAND_PX = 3;

/**
 * 默认调色(= tokens.css 字面镜像;键名即语义)。
 * env/vad/coverage 的出处见图谱 §12,stale 配方见图谱 A-08 统筹建议;
 * env/envCore/vad/vadEdge 四色在 Wave 4 按用户 preview 裁定换成「粉 + 白」波形 +
 * 鲜亮绿罩(tokens.css §20a),其余键不动。
 */
export const DEFAULT_PALETTE = Object.freeze({
    env: "rgba(216, 186, 216, 0.52)", // --wave-env-pink(外柱 = maxDb 峰包络)
    envCore: "rgba(255, 250, 255, 0.6)", // --wave-env-core(内柱 = minDb 亮芯)
    vad: `rgba(122, 205, 178, ${VAD_ALPHA})`, // rgba(var(--wave-vad), VAD_ALPHA)
    vadEdge: "rgba(154, 226, 202, 0.72)", // --wave-vad-edge(顶缘 1.5px 亮线)
    uncovered: "rgba(255, 255, 255, 0.05)", // 空白底纹(斜纹亮线)
    stale: "rgba(212, 176, 118, 0.22)", // rgba(var(--sem-amber), .22)
    passTint: "rgba(255, 255, 255, 0.03)", // passId 偶数轮次的底色微差
    coverage: "rgba(181, 172, 201, 0.85)", // rgba(var(--acc), .85)(B-07 accent)
});

/** 覆盖条高(px;B-07 裁定 2px)。 */
export const COVERAGE_BAR_PX = 2;

/**
 * 画内柱亮芯的**列宽上限**(CSS px);列宽超过它就整层不画。
 *
 * 亮芯是柱子**内部的一道纹理**(图例帧 756 的条纹感),颜色近白
 * (`--wave-env-core` α=.6),语义是「这一列的 min 包络」。它成立的前提是
 * 一列≈1px —— 稳态恒成立(cols 按视口像素宽请求)。过渡帧拿粗块垫底时列宽
 * 会涨到十几二十 px,这个前提就塌了,而两轮 preview 各暴露一种塌法:
 *
 *   · 宽度随列宽走(`colW − 0.6`):亮芯成近白**实心板**并连成一片 ——
 *     实测放大过程近白像素占画布 17.2%(稳态 0%),即「波形区整片闪白」;
 *   · 只封宽度(本常量的前身 `ENV_CORE_MAX_W`):白面积降到 2%,但每根宽粉柱
 *     中间留下一道孤立细白线 —— 用户判「有亮的竖线,比刚才还严重」。
 *
 * 病根是**粗块上这层没有信息可表达**:一列跨零点几秒,其 min 包络画成一根线
 * 只是噪声。所以不封宽度、直接跳过 —— 粗块渲染成连续的粉色轮廓(④外柱本就
 * 铺满列宽、列列相邻),过渡帧比稳态**略欠纹理**,而不是多出白板或白线。
 * 2.5 的取法:稳态 colW≈1,视口刚动、块还没换时最多到 2 出头,恰好都在门内。
 */
export const ENV_CORE_MAX_COL_PX = 2.5;

/**
 * 画亮芯的**列宽下限**(CSS px);列宽低于它同样整层不画。
 *
 * 是 `ENV_CORE_MAX_COL_PX` 的镜像病:缩小时视口变宽,细块被压到一列不足 1px,
 * 而亮芯有 0.4px 的最小宽度(不给下限就会细到消失)—— 于是列列重叠,
 * 一个像素里叠进两三道 α=.6 的近白,合成出一条**实心白带**。
 * 实测缩小过程近白像素占画布 5.5%(稳态 0%),正是用户看到的另一半闪白。
 *
 * 0.8 而不是 1.0:稳态 colW 名义为 1,但 `cols` 取整、HiDPI 下 `k` 非整数时
 * 实际会落在 0.9x,卡在 1.0 会把稳态自己关在门外(那就成了「永远不画」)。
 */
export const ENV_CORE_MIN_COL_PX = 0.8;

/**
 * 亮芯宽度(CSS px);**恒定**,不随列宽走。
 *
 * 0.4 = 稳态那一档的实际值(colW≈1 时旧式 `colW − 0.6` 正好给出 0.4),所以
 * 这个常量对稳态画面是逐像素等价的,只改过渡帧。
 *
 * 为什么必须恒定:亮芯颜色近白(α=.6),0.4px 宽时被抗锯齿摊成一层柔光,
 * 而门内上沿(colW→2.5)按旧式算法会长到 1.9px —— **实心细线**,亮度陡增。
 * 逐像素采样过:拖动缩放条到最缩小档时近白像素占画布 14.6%(稳态恒 0),
 * 主色 `245,234,245 α=207` 正是「单层外柱 + 单层亮芯」的精确合成值
 * (α = .6 + .52×.4 = .808 = 207)—— 不是叠加,就是这一层自己变粗了。
 * 用户报的「一缩放就发白」在门内这一段仍由它贡献。
 *
 * ⚠ 别再试图「按列宽比例缩放亮芯」:那正是 `ENV_CORE_MAX_COL_PX` 头注里
 * 记的两次弯路(实心白板 / 孤立白竖线)的同一个错误换个写法。
 */
export const ENV_CORE_W = 0.4;

/**
 * 过渡帧最多拼几块(每帧 × 每条可见泳道各跑一次,必须封顶)。
 * 3 = 一块垫底(最宽,补两侧)+ 两块压顶(最新鲜、细节最贴当前视口)。
 * 不封顶时 LRU 8 + 影子 8 一帧要画十几块 × 每块上千列 ⇒ 帧率崩、整页闪白。
 */
export const TRANSIENT_BLOCK_CAP = 3;

/**
 * 全曲概览块的列数(见 `perChOverview` 头注)。
 *
 * 512 而不是更多,两条理由都指向同一个数:
 *   · **成本**:它每帧每轨都要参与拼接,列数直接进循环次数。1024 时实测拖动
 *     缩放条超 32ms 的长帧从 3 涨到 13(brief §4.5 红线);
 *   · **观感**:5 分钟曲子上 512 列 ≈ 0.59s/列,整曲档下列宽 ≈ 1.3px —— 正落在
 *     亮芯的 `ENV_CORE_MIN/MAX_COL_PX` 门内,有纹理;1024 时列宽 0.64px 反而
 *     掉到下限之外,连亮芯都不画了。
 * 它只是垫底件,细节由 LRU 里跨度贴合视口的正常块压在上面。
 */
export const OVERVIEW_COLS = 512;

/** 概览块重取节流(ms);只在视口静止时触发,采集中不至于每 500ms 重拉一次。 */
export const OVERVIEW_REFRESH_MS = 3000;

/**
 * 概览块垫底时整体透明度系数(见 `dimPalette`)。
 *
 * 为什么必须压:概览一列跨 0.59s(512 列 / 5 分钟),外柱取的是区间 **max**
 * —— 粗列把柱子铺得又满又高,几乎连成实心带;而它旁边由细块画的部分逐列
 * 有起伏、露得出底色。于是「垫底的那截**比真数据还亮**」,接缝一眼可见,
 * 用户 preview 圈出的正是这一块。
 *
 * 画得更细治标不治本:取数成本上去了,缩放比一大(放到 ×13 时概览一列铺
 * 16px)照样露馅。垫底件的语义本就是「真数据还没到,先给个轮廓」,让它
 * **退让**才是对的 —— 0.55 下它读作一层淡影,补住了空白又不抢戏。
 */
export const OVERVIEW_DIM = 0.55;

/**
 * 按系数缩放调色板里每个 `rgba()` 的 alpha(非 rgba 值原样带过)。
 * 只在过渡帧给概览块用;`DEFAULT_PALETTE` 是冻结对象,这里恒返回新对象。
 */
export function dimPalette(palette, k) {
    const f = Number.isFinite(k) ? Math.min(Math.max(k, 0), 1) : 1;
    const src = { ...DEFAULT_PALETTE, ...(palette || {}) };
    const out = {};
    for (const key of Object.keys(src)) {
        const v = src[key];
        const m =
            typeof v === "string"
                ? v.match(
                      /^rgba\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*\)$/,
                  )
                : null;
        out[key] = m
            ? `rgba(${m[1]}, ${m[2]}, ${m[3]}, ${Number(m[4]) * f})`
            : v;
    }
    return out;
}

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 块缓存键(同一轨内唯一;起止量化到 ms,cols 原样)。 */
export function tileKey(startS, endS, cols) {
    return `${Math.round(num(startS, 0) * 1000)}:${Math.round(
        num(endS, 0) * 1000,
    )}:${Math.trunc(num(cols, 0))}`;
}

/**
 * 包络对比度整形指数。dB 是对数量,在 -80..0 上做**线性**映射会把
 * 「有声 −7..−16」与「静默 −44..−54」压成同高一排栅栏(Wave 2 亲验第 4 条);
 * 取 γ=2.2 的幂整形后二者半高比 ≈ 4:1,乐句起伏才读得出来。
 * 两端不动(0 dB → 满高、−80 dB → 1px),线性档的验收点不受影响。
 */
export const ENV_GAMMA = 2.2;

/**
 * dB → 包络半高(px)。地板(≤ -80)画 1px 最细柱,0 dB 顶到
 * `laneH/2 − 4`(上下各留 4px 呼吸,34px 泳道时 = 13px,与设计稿
 * 「半高 4..12px」带一致);中间按 ENV_GAMMA 幂整形拉开动态。
 * 未覆盖哨兵(-160)也会落在最细柱 —— 调用方先按 covered 位裁掉,
 * 不依赖本函数分辨。
 */
export function envelopeHalfPx(db, laneH) {
    const h = Math.max(num(laneH, 34), 8);
    const max = h / 2 - 4;
    const d = Math.min(Math.max(num(db, ENV_FLOOR_DB), ENV_FLOOR_DB), 0);
    const u = (d - ENV_FLOOR_DB) / -ENV_FLOOR_DB;
    return 1 + Math.pow(u, ENV_GAMMA) * (max - 1);
}

/**
 * 契约 §1.27 返回形状的**最小守卫**:绘制循环真正下标的三条
 * (`minDb` / `maxDb` / `covered`)必须都在、且等长。
 *
 * 为什么不能只校验 `minDb`(PR#64 评审【建议】4):`paintWaveTile` 的④包络循环写
 * `tile.covered[i]` 与 `tile.maxDb[i]`,②passId 分支的谓词也读 `tile.covered[i]`
 * —— 缺 `covered` 的畸形响应会在 rAF 绘制里抛 TypeError,炸掉的不是这一条泳道
 * 而是整帧静态层重绘(且 `drawStatic` 里没有 try/catch,脏位已被清掉,重试都不会
 * 再来一次)。长度不等同样致命:`covered` 比 `minDb` 短一列就读到 undefined、
 * 长一列则整块画歪。可选的 `vad`/`stale`/`passId` 各自有 `Array.isArray` 分支,
 * 短于 cols 时 `runsOf` 只扫到自己的长度,不需要进本守卫。
 */
export function isTileShape(tile) {
    if (!tile) return false;
    const { minDb, maxDb, covered } = tile;
    return (
        Array.isArray(minDb) &&
        Array.isArray(maxDb) &&
        Array.isArray(covered) &&
        minDb.length === maxDb.length &&
        minDb.length === covered.length
    );
}

/**
 * 单轨 LRU(Map 的插入序即访问序;get 时重插保鲜)。
 * 存的值可以是 tile 本体或在途 Promise(去重:同键并发只发一次请求)。
 */
export function createTileCache(cap = TILE_LRU_CAP) {
    const map = new Map();
    return {
        get(key) {
            if (!map.has(key)) return undefined;
            const v = map.get(key);
            map.delete(key);
            map.set(key, v);
            return v;
        },
        set(key, value) {
            if (map.has(key)) map.delete(key);
            map.set(key, value);
            while (map.size > cap) {
                map.delete(map.keys().next().value);
            }
        },
        delete: (key) => map.delete(key),
        clear: () => map.clear(),
        size: () => map.size,
        /** 只看不保鲜(身份比对用;走 get 会重排 LRU 序,把将死的块又救活)。 */
        peek: (key) => map.get(key),
        // 区间级失效要按键遍历(键里编着起止时间);拷成数组再遍历,
        // 免得边删边迭代
        keys: () => [...map.keys()],
    };
}

/**
 * 拉取源:按 (ch, 视口, cols) 取块,LRU 8 块/轨 + 在途去重。
 * @param {object} opts
 * @param {(ch:number, startS:number, endS:number, cols:number) => Promise<object>} opts.request
 *        契约 §1.27 的桥函数(tab-wave 里包一层 bridge 容错)
 * @returns {{getTile:(ch,startS,endS,cols)=>Promise<object|null>,
 *            peek:(ch,startS,endS,cols)=>object|null,
 *            invalidate:(ch?:number)=>void}}
 */
export function createWaveformSource(opts) {
    const request = (opts || {}).request;
    /** ch → LRU(**新鲜**块;权威绘制只认这一份)。 */
    const perCh = new Map();
    /**
     * ch → LRU(**陈旧影子**;只供过渡帧垫底,`peek` 永不命中它)。
     *
     * 为什么要它:`scvb.captureProgress` 是 2Hz 增量事件,每帧的 `addedRanges`
     * 只有半秒宽 —— 但**跨度大的块和几乎每个新增区间都相交**,硬删的话,
     * 采集中「整曲概览」那块每 500ms 就被清一次,缩小视口时两侧永远没东西
     * 垫底、露出空白(用户实测)。软失效把它挪到影子里:数据是旧的,但对
     * 「过渡半秒钟别露白」这件事足够用,新块一到就被盖掉。
     *
     * ⚠ `clearCoverage` 那类**数据被真删掉**的失效必须走硬删(keepStale:false):
     * 那时候旧块画出来的是「已经不存在的波形」,垫底就是骗人。
     */
    const perChStale = new Map();

    /**
     * ch → **全曲概览块**(`{want, have, at, dirty, inflight}`);LRU 之外单独存,
     * 永不被淘汰,只供过渡帧**兜底垫满**。
     *
     * 为什么非要它:过渡帧原来只能拿 LRU 里恰好与视口相交的块来拼,而
     *   ① LRU 只有 8 块/轨,块的跨度都 ≈ 当时的视口宽,**盖不满**变宽后的视口;
     *   ② `TRANSIENT_BLOCK_CAP` 还要再砍到 3 块;
     *   ③ 采集中 2Hz 的 `captureProgress` 不断把块挪进影子。
     * 三条叠起来的结果是:视口一动,**清空画布后补不满**,大片留白;而下一帧
     * 若一块都不相交就走「原样留着」分支,画面又整个回来 —— 两种状态逐帧交替,
     * 就是用户实测的「运动时波形显示不全 + 闪烁」。
     *
     * 概览块跨整首曲子,**必然覆盖任何视口**,排在拼接序最后补空隙,于是
     * 「补不满」这件事在几何上不再可能发生,闪烁也就没了源头。
     * 代价是每轨多一次 `requestWaveform`(OVERVIEW_COLS 列),且只在视口静止时
     * 按 `OVERVIEW_REFRESH_MS` 节流重取 —— 不进 LRU,不挤占正常块。
     */
    const perChOverview = new Map();

    function cacheOf(ch) {
        if (!perCh.has(ch)) perCh.set(ch, createTileCache());
        return perCh.get(ch);
    }

    function staleOf(ch) {
        if (!perChStale.has(ch)) perChStale.set(ch, createTileCache());
        return perChStale.get(ch);
    }

    async function getTile(ch, startS, endS, cols) {
        if (typeof request !== "function") return null;
        const n = Math.min(Math.max(Math.trunc(num(cols, 0)), 1), MAX_COLS);
        if (!(num(endS, 0) > num(startS, 0))) return null;
        const cache = cacheOf(ch);
        const key = tileKey(startS, endS, n);
        const hit = cache.get(key);
        if (hit) return hit.then ? hit : hit;
        // 「这一笔还是不是当前有效的那一笔」的判据:resolve 时缓存里挂的仍是
        // **本 promise** 才算数。否则 —— invalidate 在请求在途时删了键(区间级
        // 失效走的正是这条路:采集中 2Hz 的 addedRanges 一边失效、用户一边
        // 平移/缩放取数),这笔迟到的 resolve 会把**失效前**算出来的块重新塞
        // 回缓存,后续 peek 命中它 ⇒ 新采/新清的区域一直显示旧图,直到下一次
        // 失效或手动平移重取才纠正(pr-agent)。
        // 用 promise 身份比对而不是代次计数:同一个键被 invalidate 后又发起
        // 新请求时,新笔会覆盖 cache[key],旧笔自然判出局,不需要额外账。
        let p;
        const settle = (tile) => {
            const cur = cache.peek ? cache.peek(key) : undefined;
            if (cur !== p) return tile || null; // 已被失效/被新笔取代:不回写
            if (!isTileShape(tile)) {
                cache.delete(key);
                return null;
            }
            cache.set(key, tile);
            return tile;
        };
        p = Promise.resolve()
            .then(() => request(ch, startS, endS, n))
            // 契约 §5.5 风格的拒绝载荷({reason}/{observer})与畸形响应
            // 一律当无数据(形状守卫见 isTileShape 头注)
            .then(settle)
            .catch(() => {
                if ((cache.peek ? cache.peek(key) : undefined) === p) {
                    cache.delete(key);
                }
                return null;
            });
        cache.set(key, p);
        return p;
    }

    /**
     * 确保该轨有一份可用的全曲概览块;自带节流,可以每帧无脑调。
     * **只在视口静止时调**(动的时候一律不取数,见 §1.27「静止 120ms 才取新块」)。
     * 返回当前手上那份(可能是上一轮的旧数据)——取新的是后台事,不 await。
     */
    function ensureOverview(ch, startS, endS, cols) {
        if (typeof request !== "function") return null;
        const a = num(startS, 0);
        const b = num(endS, 0);
        if (!(b > a)) return null;
        const n = Math.min(
            Math.max(Math.trunc(num(cols, OVERVIEW_COLS)), 1),
            MAX_COLS,
        );
        let rec = perChOverview.get(ch);
        if (!rec) {
            rec = {
                want: null,
                have: null,
                at: 0,
                dirty: true,
                inflight: null,
            };
            perChOverview.set(ch, rec);
        }
        const want = rec.want;
        const spanChanged =
            !want || want.a !== a || want.b !== b || want.n !== n;
        if (spanChanged) {
            rec.want = { a, b, n };
            rec.dirty = true;
        }
        const now = Date.now();
        const due = rec.dirty && now - rec.at >= OVERVIEW_REFRESH_MS;
        // 曲长/列数变了要立刻重取(旧那份的几何已经对不上了),其余情况按节流走
        if (rec.inflight || !(spanChanged || due)) {
            return rec.have;
        }
        rec.at = now;
        const p = Promise.resolve()
            .then(() => request(ch, a, b, n))
            .then((tile) => {
                if (rec.inflight !== p) return; // 已被更新的一笔取代
                rec.inflight = null;
                if (!isTileShape(tile)) return;
                // have 自带几何:want 可能已经被下一笔改掉,画的时候必须用
                // **这份数据自己的**起止,否则曲长一变就画歪
                rec.have = { tile, startS: a, endS: b };
                rec.dirty = false;
                rec.at = Date.now();
            })
            .catch(() => {
                if (rec.inflight === p) rec.inflight = null;
            });
        rec.inflight = p;
        return rec.have;
    }

    return {
        getTile,
        ensureOverview,
        /** 概览块(只读,不触发取数);没有就 null。 */
        peekOverview(ch) {
            const rec = perChOverview.get(ch);
            return rec && rec.have ? rec.have : null;
        },
        /**
         * 取该轨缓存里**所有与 [startS,endS) 相交**的成品块(不含在途 Promise),
         * 附各自的时间范围;**按跨度从宽到窄**排序,调用方顺序画上去即可 ——
         * 宽块打底、窄块(细节多)压在上面。
         *
         * 用途:视口变化中的过渡帧。只画「当前视口那一块」时,缩小档会因为老块
         * 比新视口窄而在两侧留白(用户实测);把几块老块按时间拼上去就补满了。
         * 不走 `get` 是刻意的:过渡帧的读取不该重排 LRU、把将淘汰的块救活。
         */
        peekOverlapping(ch, startS, endS) {
            const a = num(startS, 0);
            const b = num(endS, 0);
            if (!(b > a)) return [];
            const pick = (cache) => {
                const out = [];
                if (!cache || !cache.keys) return out;
                for (const key of cache.keys()) {
                    const parts = String(key).split(":");
                    const t0 = Number(parts[0]) / 1000;
                    const t1 = Number(parts[1]) / 1000;
                    if (!(t1 > t0) || t1 <= a || t0 >= b) continue;
                    const v = cache.peek ? cache.peek(key) : undefined;
                    if (!v || v.then) continue; // 在途 Promise 不算
                    out.push({ tile: v, startS: t0, endS: t1 });
                }
                return out.sort(
                    (x, y) => y.endS - y.startS - (x.endS - x.startS),
                );
            };
            // **窄块在前、宽块在后**(新鲜组优先于陈旧组):调用方按此序画,
            // 每块只画**前面没画过的空隙** —— 细节最好的先占位,宽块只补两侧。
            //
            // ⚠ 顺序不能反过来「宽块垫底、窄块压顶」:波形是半透明的
            // (外柱 α=.52 / 内柱 α=.6),重叠区被画两三遍就把 α 叠成
            // 0.89 / 0.94 —— **几乎纯白**,用户实测「一动视口波形区域就闪白」
            // 正是这条。空隙裁剪由调用方做(它才知道像素坐标),这里只保证序。
            // pick 给的是宽→窄;各自反过来成窄→宽,再让**新鲜组整体排在前**
            // (同样宽度下新鲜的先占位,陈旧的只补它够不着的地方)。
            const fresh = pick(perCh.get(ch)).reverse();
            const stale = pick(perChStale.get(ch)).reverse();
            const all = fresh.concat(stale);
            // 封顶:过渡帧每帧 × 每条可见泳道跑一次,LRU 8 + 影子 8 不设上限时
            // 一帧要画十几块 × 每块上千列。窄→宽取前 N 块即可(细节优先)。
            return all.slice(0, TRANSIENT_BLOCK_CAP);
        },
        /** 只查缓存(在途 Promise 不算命中)——渲染帧禁止 await。 */
        peek(ch, startS, endS, cols) {
            const cache = perCh.get(ch);
            if (!cache) return null;
            const v = cache.get(tileKey(startS, endS, cols));
            return v && !v.then ? v : null;
        },
        /**
         * 数据失效(clearCoverage / 重采集完成后调;缺 ch = 全轨)。
         *
         * `ranges`(可选,`[{startS,endS}…]`)= **只失效与这些区间相交的块**。
         * `scvb.captureProgress` 是 2Hz 增量事件(§2.7),载荷通常只新增很小
         * 一段;不给区间就整轨 8 块全丢 ⇒ 采集中反复整轨重取(pr-agent)。
         * 缺省仍是整轨清 —— clearCoverage 那类「整轨语义变了」的场合要的就是它。
         *
         * `keepStale`(默认 **true**)= 失效的块挪进影子缓存供过渡帧垫底,
         * 而不是直接扔。采集中跨度大的块与每个 `addedRanges` 都相交,硬删会
         * 让「整曲概览」每 500ms 消失一次、缩小时两侧露白(用户实测)。
         * **`clearCoverage` 必须传 `keepStale:false`** —— 数据真被删了,
         * 拿旧块垫底就是画一段已经不存在的波形。
         */
        invalidate(ch, ranges, o) {
            const keepStale = !(o && o.keepStale === false);
            // 概览块:软失效只**标脏**(手上那份继续垫底,静止后按节流换新);
            // 硬删才真丢 —— 数据没了还拿它垫底就是画不存在的波形。
            // 不按 ranges 细分:它跨整首曲子,和任何区间都相交,细分等于必删。
            for (const [c, rec] of perChOverview) {
                if (ch != null && c !== ch) continue;
                if (keepStale) rec.dirty = true;
                else perChOverview.delete(c);
            }
            const retire = (cache, stale, key) => {
                const v = cache.peek ? cache.peek(key) : undefined;
                cache.delete(key);
                if (keepStale && v && !v.then) stale.set(key, v);
            };
            if (ch == null) {
                if (keepStale) {
                    for (const [c, cache] of perCh) {
                        const stale = staleOf(c);
                        for (const key of cache.keys())
                            retire(cache, stale, key);
                    }
                }
                perCh.clear();
                if (!keepStale) perChStale.clear();
                return;
            }
            const cache = perCh.get(ch);
            if (!cache) return;
            if (!Array.isArray(ranges) || !ranges.length) {
                const stale = staleOf(ch);
                for (const key of cache.keys()) retire(cache, stale, key);
                perCh.delete(ch);
                if (!keepStale) perChStale.delete(ch);
                return;
            }
            const stale = staleOf(ch);
            for (const key of cache.keys()) {
                // 键形 = `${起 ms}:${止 ms}:${cols}`(tileKey)
                const parts = String(key).split(":");
                const t0 = Number(parts[0]) / 1000;
                const t1 = Number(parts[1]) / 1000;
                if (!(t1 > t0)) {
                    cache.delete(key); // 键形不认识就按失效处理,不留脏块
                    continue;
                }
                const hit = ranges.some(
                    (r) => r && Number(r.endS) > t0 && Number(r.startS) < t1,
                );
                if (hit) retire(cache, stale, key);
            }
            // 硬删场景:影子里与该区间相交的也要一起清(否则垫底的还是旧数据)
            if (!keepStale && perChStale.has(ch)) {
                const sc = perChStale.get(ch);
                for (const key of sc.keys()) {
                    const parts = String(key).split(":");
                    const t0 = Number(parts[0]) / 1000;
                    const t1 = Number(parts[1]) / 1000;
                    if (
                        !(t1 > t0) ||
                        ranges.some(
                            (r) =>
                                r &&
                                Number(r.endS) > t0 &&
                                Number(r.startS) < t1,
                        )
                    ) {
                        sc.delete(key);
                    }
                }
            }
        },
    };
}

// -----------------------------------------------------------------------------
// 静态层画笔 —— 一块 tile → 一条泳道的静态位图(在 ctx 上按 CSS px 作画,
// hidpi.js 已 setTransform)。层序:
// 未覆盖底纹 → passId 微差 → **VAD 顶部标注带** → 包络柱 →
// stale 斜条纹 → 覆盖条。
// ⚠ 图谱 §12 按设计稿 DOM 序(852 包络 → 853-855 VAD)要求「VAD 压在包络之上」;
// T33 Wave 4 用户裁定⑤⑥把绿提亮并把波形改成粉+白后,压在上面会把粉柱整体染绿
// (与裁定⑤自带的验收句「不能盖住波形」直接冲突)→ 两者对调;Wave 5 用户第二轮
// 判「像绿底放了个波」→ 绿收成**泳道顶部一条标注带**,带下不填。
// deviations §S 登记(见下面 ③ 处的长注)。
// -----------------------------------------------------------------------------

/**
 * 逐列位数组 → [start, end) 连续段表(减少 fillRect 次数的预算意识,05 §6.3)。
 * `i0`/`i1` 可选,只扫这一段 —— 时间映射下块常常只有一小截落在画布内,
 * 全扫等于给画布外的列白算一遍(多块拼合时是数量级差别)。
 */
export function runsOf(flags, predicate, i0, i1) {
    const runs = [];
    let s = -1;
    const len = flags ? flags.length : 0;
    const from = Math.max(0, Math.trunc(num(i0, 0)));
    const n = Math.min(len, i1 === undefined ? len : Math.trunc(num(i1, len)));
    for (let i = from; i <= n; i++) {
        const on = i < n && predicate(flags[i], i);
        if (on && s < 0) s = i;
        if (!on && s >= 0) {
            runs.push([s, i]);
            s = -1;
        }
    }
    return runs;
}

/** 45° 斜条纹填充(3px 线 / 8px 周期;A-08 统筹建议配方,stale 与未覆盖共用画法)。 */
function paintStripes(ctx, x, y, w, h, color, period, lineW) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    ctx.strokeStyle = color;
    ctx.lineWidth = lineW;
    ctx.beginPath();
    for (let d = x - h; d < x + w + h; d += period) {
        ctx.moveTo(d, y + h);
        ctx.lineTo(d + h, y);
    }
    ctx.stroke();
    ctx.restore();
}

/**
 * 画一块静态层。tile 为契约 §1.27 形状(六个 cols 长数组);w/h = 舞台 CSS px。
 * @param {CanvasRenderingContext2D} ctx
 * @param {object} tile
 * @param {number} w
 * @param {number} h
 * @param {object} [palette] 覆盖 DEFAULT_PALETTE 的键
 * @param {object} [opts] 时间映射与清屏控制:
 *   `{tileStartS, tileEndS, viewStartS, viewEndS}` —— 把这块**按时间**画进当前
 *   视口(块只覆盖视口的一段时就只占那一段);缺省 = 块铺满 [0,w] 的老口径。
 *   `{clear:false}` —— 不清屏,供多块拼合(缩小时用几块老块补齐视口)。
 *   `{xFrom, xTo}` —— **只画这段 x**(画布坐标);只收**循环**窗口,像素级
 *   裁剪仍要调用方自己 `ctx.clip()`(列宽可能跨过边界,循环只能取到整列)。
 *
 * ⚠ 这是「视口变化中」那条路径的正解,不要退回位图拉伸,也不要用 `ctx.scale`:
 *   · `ctx.scale` 会把 45° 斜纹剪切成缓坡、把 lineWidth 横向拉粗(前两轮的病根);
 *   · `drawImage` 位图拉伸则是放大糊、缩小两侧露白(第三轮用户实测)。
 *   按本映射重画时,**列宽变而画笔不变** —— 斜纹、线宽、覆盖条都画在目标
 *   坐标系里,几何恒正确;代价只是横向细节随源列数走(放大档变粗但清晰),
 *   静止 120ms 取到新块后自然补满(契约 §1.27)。
 */
export function paintWaveTile(ctx, tile, w, h, palette, opts) {
    // 画笔自守:本函数是导出件(T43 复用面 + smoke 直调),不能只依赖
    // createWaveformSource 那一道闸
    if (!ctx || !isTileShape(tile)) return;
    const pal = { ...DEFAULT_PALETTE, ...(palette || {}) };
    const cols = tile.minDb.length;
    if (!cols || !(w > 0) || !(h > 0)) return;
    const o = opts || {};
    const viewSpan = num(o.viewEndS, 0) - num(o.viewStartS, 0);
    const tileSpan = num(o.tileEndS, 0) - num(o.tileStartS, 0);
    // 映射齐备才走时间映射;否则退回「整块铺满 [0,w]」的老口径
    const mapped = viewSpan > 0 && tileSpan > 0;
    const colW = mapped ? (tileSpan / cols / viewSpan) * w : w / cols;
    const originX = mapped
        ? ((num(o.tileStartS, 0) - num(o.viewStartS, 0)) / viewSpan) * w
        : 0;
    const mid = h / 2;
    const x0 = (i) => originX + i * colW;

    // **可见列裁剪**:映射后块往往只有一小段落在画布内(整曲概览块放大到 30s
    // 视口时 90% 的列在画布外)。不裁的话那些列照样逐列 fillRect —— 多块拼合下
    // 每帧十几万次绘制调用,帧率直接崩,表现为**整页卡片闪白**(用户实测)。
    // 越界 canvas 自己会剪,但调用开销省不掉,必须在循环层面剪。
    //
    // `xFrom/xTo` 把窗口进一步收到「这次真要填的那段空隙」:多块拼合时每块
    // 常常只补两侧一点点,按整幅可见范围跑循环纯属白算 —— 概览块接进来后
    // 实测超 32ms 长帧从 3 涨到 13(brief §4.5 红线),收窄后回到 3。
    // 各留一列余量:列宽可能跨过边界,少算一列就会在接缝处露出一条缝。
    const xLo = Math.max(num(o.xFrom, 0), 0);
    const xHi = Math.min(num(o.xTo, w), w);
    const iFrom = mapped
        ? Math.max(0, Math.floor((xLo - originX) / colW) - 1)
        : 0;
    const iTo = mapped
        ? Math.min(cols, Math.ceil((xHi - originX) / colW) + 1)
        : cols;
    if (!(iTo > iFrom)) {
        if (o.clear !== false) ctx.clearRect(0, 0, w, h);
        return; // 整块在画布外
    }

    if (o.clear !== false) ctx.clearRect(0, 0, w, h);

    // ① 未覆盖 = 空白底纹(细斜纹,不是纯掏空 —— [J72a] C-03)
    for (const [a, b] of runsOf(tile.covered, (v) => !v, iFrom, iTo)) {
        paintStripes(ctx, x0(a), 0, (b - a) * colW, h, pal.uncovered, 7, 1);
    }

    // ② passId 底色微差:偶数轮次整段加一层极淡白(不同采集轮次可辨即可)
    if (Array.isArray(tile.passId)) {
        ctx.fillStyle = pal.passTint;
        for (const [a, b] of runsOf(
            tile.passId,
            (v, i) => tile.covered[i] && v > 0 && v % 2 === 0,
            iFrom,
            iTo,
        )) {
            ctx.fillRect(x0(a), 0, (b - a) * colW, h);
        }
    }

    // ③ VAD **顶部标注带**(泳道最上沿一条 VAD_BAND_PX,最上 1.5px 是亮线;
    //    阈值拖动重取重绘,05 §2.3 行 312)。
    //    形制史(全部由 J72 用户 preview 推动,deviations §S):
    //      · Wave 2:满高半透明罩 .13,压在包络之上 → 太灰太淡;
    //      · Wave 4:更鲜亮的绿 + .20,并按列裁掉包络带(与波形零重叠)——
    //        绿仍铺满上下两片空白;
    //      · **Wave 5(本轮)**:用户判「像绿底放了个波」—— 罩面积比波形还大,
    //        喧宾夺主。裁定改成**只画顶部一条**,带下一寸不填;带窄了就画实
    //        (VAD_ALPHA .20 → .55),有声区照样一眼可辨,而波形所在的中带干净。
    //    画法一并简化成**每 run 两次 fillRect**(旧版逐列 rect 是 929 列 × 15 轨
    //    静态层 <8ms 预算的大头,05 §6.3)。带与包络的 1px 咬合见 VAD_BAND_PX 头注:
    //    包络在本层**之后**画(③→④),柱尖盖回那 1px,粉白不被染。
    if (Array.isArray(tile.vad)) {
        const runs = runsOf(
            tile.vad,
            (v, i) => tile.covered[i] && v > 0,
            iFrom,
            iTo,
        );
        const band = Math.min(VAD_BAND_PX, h);
        ctx.fillStyle = pal.vad;
        for (const [a, b] of runs) {
            ctx.fillRect(x0(a), 0, (b - a) * colW, band);
        }
        ctx.fillStyle = pal.vadEdge;
        for (const [a, b] of runs) {
            ctx.fillRect(x0(a), 0, (b - a) * colW, 1.5);
        }
    }

    // ④ min/max 包络柱(05 §2.3 行 311):外柱 = max 峰包络(半透明晕),
    //    内柱 = min 亮芯,纵向对称于泳道中线(实景帧的居中几何,图谱 §12 ①)。
    //    外柱铺满列宽成连续包络,亮芯留 0.6px 缝 = 图例帧 756 的条纹感。
    //    Wave 4 配色:外柱淡粉紫 / 内柱近白(用户裁定⑥的「粉 + 白」半透明)。
    ctx.fillStyle = pal.env;
    for (let i = iFrom; i < iTo; i++) {
        if (!tile.covered[i]) continue;
        const hi = envelopeHalfPx(tile.maxDb[i], h);
        ctx.fillRect(x0(i), mid - hi, Math.max(colW, 0.6), hi * 2);
    }
    // 亮芯只在「一列≈一像素」时画(见 ENV_CORE_MIN/MAX_COL_PX 两条头注):
    // 列太宽 → 白板或白竖线;列太窄 → 逐列重叠堆成白带。两种都被 preview 判过,
    // 而两种在稳态都不会发生 —— 这层的前提本就是 cols 按视口像素宽请求。
    if (colW >= ENV_CORE_MIN_COL_PX && colW <= ENV_CORE_MAX_COL_PX) {
        // 宽度恒定(见 ENV_CORE_W):门内上沿按旧式 `colW − 0.6` 会长到 1.9px、
        // 由柔光变实心细线,近白像素 0 → 14.6%。稳态那一档值不变。
        ctx.fillStyle = pal.envCore;
        for (let i = iFrom; i < iTo; i++) {
            if (!tile.covered[i]) continue;
            const lo = envelopeHalfPx(tile.minDb[i], h);
            ctx.fillRect(x0(i), mid - lo, ENV_CORE_W, lo * 2);
        }
    }

    // ⑤ stale = 琥珀斜条纹叠加(05 §2.3 行 311;⚠ 角标是 DOM 件,Wave 2)
    if (Array.isArray(tile.stale)) {
        for (const [a, b] of runsOf(
            tile.stale,
            (v, i) => tile.covered[i] && v > 0,
            iFrom,
            iTo,
        )) {
            paintStripes(ctx, x0(a), 0, (b - a) * colW, h, pal.stale, 8, 3);
        }
    }

    // ⑥ 底部 2px 覆盖条(已覆盖 = accent 薰衣草;B-07 裁定)
    ctx.fillStyle = pal.coverage;
    for (const [a, b] of runsOf(tile.covered, (v) => !!v, iFrom, iTo)) {
        ctx.fillRect(
            x0(a),
            h - COVERAGE_BAR_PX,
            (b - a) * colW,
            COVERAGE_BAR_PX,
        );
    }
}
