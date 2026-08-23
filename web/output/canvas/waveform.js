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
 * 过渡帧最多拼几块(每帧 × 每条可见泳道各跑一次,必须封顶)。
 * 3 = 一块垫底(最宽,补两侧)+ 两块压顶(最新鲜、细节最贴当前视口)。
 * 不封顶时 LRU 8 + 影子 8 一帧要画十几块 × 每块上千列 ⇒ 帧率崩、整页闪白。
 */
export const TRANSIENT_BLOCK_CAP = 3;

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

    return {
        getTile,
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
            // 陈旧块先(垫底),新鲜块后(压在上面)—— 两组各自宽到窄。
            const all = pick(perChStale.get(ch)).concat(pick(perCh.get(ch)));
            if (all.length <= TRANSIENT_BLOCK_CAP) return all;
            // **封顶**:过渡帧是每帧 × 每条可见泳道跑一次,块数不设上限时
            // (LRU 8 + 影子 8)一帧要画十几块 × 每块上千列 ⇒ 帧率崩、整页
            // 卡片闪白(用户实测)。留最宽的一块垫底 + 最后几块(= 最新鲜、
            // 细节最贴当前视口的)压顶,视觉上与全画等价。
            return [all[0]].concat(all.slice(-(TRANSIENT_BLOCK_CAP - 1)));
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
    const iFrom = mapped
        ? Math.max(0, Math.floor((0 - originX) / colW) - 1)
        : 0;
    const iTo = mapped
        ? Math.min(cols, Math.ceil((w - originX) / colW) + 1)
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
    ctx.fillStyle = pal.envCore;
    const coreW = Math.max(colW - 0.6, 0.4);
    for (let i = iFrom; i < iTo; i++) {
        if (!tile.covered[i]) continue;
        const lo = envelopeHalfPx(tile.minDb[i], h);
        ctx.fillRect(x0(i), mid - lo, coreW, lo * 2);
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
