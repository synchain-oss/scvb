// SPDX-License-Identifier: GPL-3.0-or-later
// ======================================================================// SCVB — 界面文案字典(中 / EN / FR)
// ======================================================================// 真源:masterPlan/plan/05-ui-spec.md §5(状态词规范 512-524 / 术语表 530-618)、
// §5.1(分组词条组 619-633)、§5.2(设计定稿回流 635-658)、§2.6(tour 步骤表 421-429)。
// 全部词条逐字照 05 转写,占位符({a} {b} {g} {k} {l} {n} {t} {v} {x} {y} {X} {name})原样保留;
// 改文案先改 05,再回改本文件,不得只改一处。
//
// 纪律三条:
//  1. guide.rule1..9(九条硬约束红字)不在本文件手写——由 scripts/gen-hard-rules.mjs 从唯一真源
//     docs/USER_GUIDE.zh-CN.md#硬约束 生成写入本文件,任何位置禁止手抄(05 §0.1 / §5);
//     CI 跑 gen-hard-rules.mjs --check 比对哈希。设计稿里的 NINE 常量只是视觉参照,不是文案真源。
//     fr 红字必须经人工审校后方可发布(05 §5)。
//  2. 三词分工(R1 定案,禁止混用):range = 范围(作用范围,Tab1)、interval = 区段(分析产物,
//     「将影响 N 区段」)、selection = 选区(Tab3 工作选区,重采集/重分析/清除的对象)。
//  3. 连接类状态每态唯一用词,一律走 state.* 组,不再自由造「等待连接 / 离线」之类泛化说法。
//
// key 前缀沿 05 §5:common.* / out.master.* / out.tracks.* / out.wave.* / out.set.* / in.*;
// 但一律以表内印刷 key 逐字为准——05 §5.2 起多数条目印作 master.* / tracks.* / wave.* / set.*(无 out. 前缀),
// 本文件照印刷原文收录,不擅自补前缀(见 T27 差异清单)。
//
// T27 自译(05 未给三语)的条目:state.* 6 条 FR、state.waitingForOutput.group 的 EN/FR、
// (此处刻意写阿拉伯数字而非中文数词:12 §3.4 有一道零命中机检,专抓把九条红字的标题
//  误写成中文数词「6」的历史笔误,扫全仓文档面与本文件。本文件正是九条红字的落地面之一,
//  一个语义无关的中文数词会让那道安全门禁常年假红。)
// state.groupSuffix 三语、tour.step1..7 的 EN/FR —— 均待人工审校,逐条清单见 T27 差异清单
// 的「i18n.js」小节(05 §5 要求 fr 发布前人工审校,那份清单就是审校人的入口)。
// ======================================================================
export const T = {
    zh: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "未连接",
        "state.staleLink": "链接异常(心跳陈旧:仍在线但通信停更)",
        "state.connected": "已连接", // Output Tab2 轨「活跃」与 Input pill「健康接管」同词,共用一条
        "state.noChannel": "未选择通道",
        "state.outputOffline": "Output 未运行",
        "state.waitingForOutput": "等待 Output",
        "state.passthrough": "直通中", // FR 取术语表 passthrough 行;裸词条 passthrough 同义但仅用于术语解释
        "state.takenOver": "已接管(静音转发)", // FR 取术语表 takenOver 行;zh 括注为 05 原文,裸词条 takenOver 无括注、仅用于术语解释
        "state.waitingForOutput.group": "等待 Output · 组 {X}", // 05 §5.1 例外①:group-mismatch pill 不采稿内提案,唯一状态词在前;EN/FR 为 T27 自译
        // 等价于 state.waitingForOutput + state.groupSuffix;两处必须同时改,别只改一处
        "state.groupSuffix": " · 组 {X}", // T27 自定:05 §3 要求 Input pill 四态主文案统一带「· 组 {A-H}」后缀,
        // 拼法一律 t[stateKey] + t["state.groupSuffix"].replace("{X}", g) —— 不许在 JS 里硬写 " · 组 "(05 §5「正文引用 key 而非自由文案」),
        // 也不许拿 group.label 拼(那是首字母大写的「Group / Groupe」,与状态词的全大写 pill 不同款)

        // 术语表(05 §5,530-586 行):三语建议,写入字典前用户可改;EN 状态词沿 Bridge 全大写惯例。
        // 三词分工(R1 定案,不得混用):range=范围(作用范围,Tab1)、interval=区段(分析产物)、selection=选区(Tab3 操作目标)。
        pan: "声像",
        width: "宽度",
        track: "轨道",
        channel: "通道",
        group: "分组",
        segment: "分段",
        capture: "采集",
        tabs: "标签页",
        analyze: "分析",
        output: "输出",
        followHost: "跟随宿主",
        engineDrive: "引擎驱动",
        version: "版本",
        priority: "优先级",
        leadLock: "主唱锁中",
        leadSelect: "主唱选择",
        leadFollowAnalysis: "自动选择",
        msBalance: "MS 平衡",
        stereoBadge: "立体声",
        participateAutoPan: "参与自动声像",
        trackWidth: "轨道宽度",
        "tracks.monoWidthNoop": "mono 源无法调整宽度",
        "master.leadSelectHint": "该轨设置为 Lead 并强制居中",
        pair: "配对",
        threshold: "阈值",
        sensitivity: "灵敏度",
        range: "范围",
        follow: "全曲跟随",
        selection: "选区",
        interval: "区段",
        loopRegion: "循环区",
        gainCurve: "增益曲线",
        bell: "钟形",
        shelf: "搁架",
        cut: "切除",
        sideOut: "向外",
        sideLeft: "向左",
        sideRight: "向右",
        paddingPre: "前留白",
        paddingPost: "后留白",
        transition: "过渡",
        misaligned: "路由失准",
        capturing: "采集中",
        stale: "数据过期",
        manualMark: "手动编辑",
        locked: "已锁定",
        copyTo: "复制到",
        renameVersion: "重命名版本",
        // 下面两条是术语表裸词条(05 §5 印刷 key),只用于术语解释/图例/说明块;
        // 音频路径的实时状态一律用 state.passthrough / state.takenOver(纪律三),后者 zh 带「(静音转发)」括注。
        passthrough: "直通中",
        takenOver: "已接管",
        printing: "打印中",
        lowSample: "样本不足",
        // T33 Wave 4 用户 preview 裁定:短语「播完自动停」读不成句,改完整句(key 不变)
        autoStop: "播放结束自动停止",
        reidentify: "重新识别(含手动段)",
        applyToSegments: "应用到分段",
        setAsRange: "设为范围",
        clearCapture: "清除采集数据",
        scale: "界面缩放",
        language: "语言",
        armedWaiting: "已就绪·等待播放",
        outOfRange: "已离开采集范围",
        occupied: "占用",

        // 横幅、写入确认条、页脚与错误态长句(05 §5,587-605 行):占位符 {a}{b}{v}{x}{y}{n}{t}{k}{l} 原样保留。
        "banner.versionMismatch":
            "此工程由更新版本的 SCVB 保存,请升级插件(本机 abi {a} / 工程 abi {b})",
        "banner.abiMismatch":
            "两端 SCVB 版本不匹配,已拒绝连接(Output abi {a} / Input abi {b})——请把两个插件升到同一版本",
        "banner.printGuard": "输出开关处于引擎驱动状态(随工程恢复)",
        "banner.printGuard.confirm": "继续引擎驱动",
        "out.master.writeConfirm":
            "引擎驱动 {v} · 范围 {x}–{y} · 30 条轨道;若 DAW 侧已激活 Latch/Write,播放本范围将覆盖该范围已有自动化;未激活则仅试听、不保存",
        "footer.printing": "引擎驱动 {v} · {x}–{y}",
        "footer.printDone":
            "本次录制覆盖 {x}–{y};若在录制自动化,建议切回跟随宿主试听核对",
        "out.master.writeConfirm.follow":
            "引擎驱动 {v} · 范围 = 全部已分析区域(全曲跟随,共 {n} 段 · 合计 {t}) · 30 条轨道;若 DAW 侧已激活 Latch/Write,播放已分析区域将覆盖其已有自动化;未激活则仅试听、不保存",
        "footer.printing.follow": "引擎驱动 {v} · 全曲跟随(已分析区域内)",
        "footer.printDone.follow":
            "本次录制覆盖已分析区域;若在录制自动化,建议切回跟随宿主试听核对",
        "wave.diffKept": "{k} 处手动编辑/锁定段已保留",
        "tracks.manualOverwriteConfirm":
            "将以固定值替换该轨(当前版本)的全部分段结果,可撤销",
        "tracks.manualOverwriteConfirm.locked": "(含 {l} 个锁定段)",
        "in.pill.abiMismatch": "版本不匹配",
        "in.pill.srMismatch": "采样率不一致",
        "tracks.manualDrivenHint": "该轨仍由手动固定值驱动,是否重新识别该轨?",
        "in.releaseConfirm": "将释放通道 {n},不再对本轨做任何处理",
        "wave.recaptureArmed": "重采集已就绪 · 选区 {x}–{y} · {n} 轨",
        // 无占位符短式(T33 PR#64 评审【重要】1 / 【建议】2):Tab2 行首圆点 badge 的
        // tooltip 与 Tab3 badge 的静态兜底文案都只拿得到「在布防中」这一个布尔位,
        // 拿不到 {x}{y}{n} —— 灌长式会把字典原文连占位符一起写进 title / textContent。
        // 范围与轨数在两处 badge(Tab1 Range / Tab2 图例行)上由 fmt 灌完整串。
        "wave.recaptureArmedShort": "重采集已就绪",
        "footer.recaptureOutputWarn":
            "输出引擎仍按全局范围工作,与本次重采集选区无关",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.title 与 guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从
        // docs/USER_GUIDE.zh-CN.md#硬约束 生成写入(05 §5 / §0.1,禁止手抄)。下面这对标记之间
        // 的内容是**生成物** —— 手改会被下一次 --write 覆盖,并在此之前被 --check 判红。
        // BEGIN GENERATED hard-rules:zh
        "guide.title":
            "必读:SCVB 的九条使用规则,违反其中任何一条都会导致静音、错音或分析失效。",
        "guide.rule1":
            "人声轨必须保持 DAW 原有路由,指向 SCVB Output 所在的总线。不要把人声轨改成直接送主输出,也不要绕开总线。(ADR-002)",
        "guide.rule2":
            "SCVB Input 必须插在人声轨插件链的最后一格;SCVB Output 必须插在总线的第一格。位置不对会破坏 DAW 的处理顺序假设;各宿主对这一格的具体叫法见 docs/DAW_COMPATIBILITY.md。(ADR-002 / J45)",
        "guide.rule3":
            '只有在检测到健康的 SCVB Output 时,Input 才会向下游输出静音——这是设计行为,不是 bug。这条静音通路保住了 DAW 依赖图里"先人声轨、后总线"的排序,离线渲染与 REAPER 的预测性多线程下依然成立。检测不到健康 Output 时(未装、未连上、对端已退出),Input 自动切回直通,80ms ramp 过渡、5 秒滞回防抖(滞回只作用于"静音 → 直通"方向;"直通 → 静音"在确认健康后立即 80ms ramp),所以你不会因为只装了一个插件就得到一条没有声音的轨道。(ADR-002 / J12 + J32)',
        "guide.rule4":
            "人声轨与总线的宿主 pan 必须保持居中。SCVB 内部用 equal-power pan,与宿主 pan law 无关;宿主 pan 不居中会叠加出错误声像。(ADR-010)",
        "guide.rule5":
            '每个 channel id 在同一个组内唯一,而同一条人声轨只能属于一个组。同组内两个 Input 抢同一个 channel 时,后来者会看到"channel 冲突"警告并且不会生效;不同组的同号 channel 是两条互不相干的通路。(ADR-002 / J66)',
        "guide.rule6":
            "同一个组同一时间只能有一个生效的 Output 实例。同组的第二个实例进入只读观察模式并显示警告;八个组(A–H)各自是独立的总线域,互不影响。(ADR-002 / J66)",
        "guide.rule7":
            "stereo 人声轨默认不参与自动声像分配,需要它参与时必须手动打开。mono 源经 equal-power pan 摆位;stereo 源走 dual-pan + width 模型(pan = 弧中心,width = 张开度),默认保留你已有的声像宽度,不会被自动分配改写。(ADR-003 / J57 + J60)",
        "guide.rule8":
            'SCVB Output 不向 DAW 报告额外延迟。对齐靠时间线寻址完成,不要试图用 PDC(延迟补偿)去"修正"它。(ADR-002)',
        "guide.rule9":
            '看到"时间线缺口 / 重叠"警告时,不要继续导出。先按 docs/DAW_COMPATIBILITY.md 的通用坑清单排查路由,警告计数不归零就说明有轨的音频没被正确接管。',
        // END GENERATED hard-rules:zh
        "guide.dontShowAgain": "不再显示",
        "guide.start": "开始使用",
        "set.reopenGuide": "重看引导",
        "set.viewWorkflow": "查看工作流程",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "要不要花一分钟快速了解界面?",
        "tour.ask.start": "开始引导",
        "tour.ask.later": "暂不,以后可在设置页重看",
        "tour.clickAnywhere": "左键点击任意处继续",
        "tour.skip": "跳过",
        "tour.prev": "上一步",
        "tour.next": "下一步",
        "tour.done": "完成",
        "tour.demoBadge": "演示数据",
        // 首启语言选择卡(T36b 第四轮:独立 overlay,先于红字九条页;三语常显,各按钮用各自语言)。
        "lang-start.title":
            "请选择语言 / Choose your language / Choisissez votre langue",
        "lang-start.zh": "中文",
        "lang-start.en": "English",
        "lang-start.fr": "Français",

        // Input 首启轻量引导 mini tour(**[J80] / 07 T48**;5 步基线,词条 tour-in.*)。
        // 按钮行/交互提示复用 tour.* 同一份(措辞相同,不另立第二套)。
        // 第 4 步 = 九条硬约束第 3 条的场景化改写:用词与 12 §3.4 / guide.rule3 同源
        //(「设计行为,不是 bug」「检测不到健康 Output 时(未装、未连上、对端已退出)自动切回直通」
        // 「不会因为只装了一个插件就得到一条没有声音的轨道」),禁「永久静音 / 哑轨」类旧表述。
        "tour-in.help": "重看引导",
        "tour-in.step1.title": "欢迎使用 SCVB Input",
        "tour-in.step1.body":
            "这是 SCVB 的采集端：每条人声轨的最后一格插一个，负责把这条轨送给总线上的 SCVB Output。花二十秒认一下这几件东西。",
        "tour-in.step2.title": "组：A–H",
        "tour-in.step2.body":
            "组是一整套独立的工作区。本轨要和总线上接管它的那个 SCVB Output 选同一个组；只用一套时保持 A 即可。",
        "tour-in.step3.title": "通道号",
        "tour-in.step3.body":
            "同组内每条人声轨占一个号（01–15），号在组内唯一。点一张卡即认领；右上角标了「占用」的号，已经被同组别的轨占着。",
        "tour-in.step4.title": "连接状态",
        "tour-in.step4.body":
            "连接成功后，本轨会向下游输出静音、由总线上的 SCVB Output 接管发声——这是设计行为，不是 bug。检测不到健康的 Output 时（未装、未连上、对端已退出），本轨自动切回直通，所以你不会因为只装了一个插件就得到一条没有声音的轨道。",
        "tour-in.step5.title": "完整控制在 Output",
        "tour-in.step5.body":
            "分析、平衡、写自动化这些完整控制都在总线上的 SCVB Output 里；Input 这一页只管认领通道和看状态。想再看一遍这段引导，随时点这个「？」。",

        // tour 全参数导览 44 步(终稿 v2,以 drafts/tour-zh-copy-final-2026-08-24.md 为准;05 §2.6 待统筹勘误)。
        "tour.step1.title": "欢迎使用 SCVB",
        "tour.step1.body":
            "SCVB 是 Synchain 的开源多轨人声平衡工具。先花一分钟来了解一下界面吧。已注入 15 轨演示数据,结束后还原。",
        "tour.step4.title": "这是 SCVB Output 的主界面",
        "tour.step4.body":
            "顶部有四个页签:整体调整 / 轨道 / 波形 / 设置。这个引导会带你逐页了解。",
        "tour.step8.title": "组与它的作用",
        "tour.step8.body":
            "组把通道分成 A–H 八个独立工作区,每个组各自采集、分析、输出;单组使用保持 A 即可。",
        "tour.step5.title": "第一步:采集",
        "tour.step5.body":
            "打开采集开关并播放当前范围,插件会记录每条轨的响度特征。这一步不会写入任何自动化。",
        "tour.step6.title": "第二步:分析",
        "tour.step6.body":
            "离线计算每个乐句的声像 / 音量方案;点击前可以先预览采集范围,手动编辑或锁定的段落会被保留。",
        "tour.step7.title": "第三步:输出",
        "tour.step7.body":
            "切到「引擎驱动」后,参数由分析结果自动驱动;要把参数录成自动化,请在 DAW 侧打开 Latch 或 Write。",
        "tour.step12.title": "声像 / 音量分布",
        "tour.step12.body":
            "这是全体轨道的声像与音量的实时状态:柱高 = 音量,横位 = 声像;横向细线 = 立体声源的张开度与角度域曲线。",
        "tour.step9.title": "Width · 最大角度",
        "tour.step9.body": "整体宽度的上限:最大张开角 0–90°。",
        "tour.step10.title": "MS Balance",
        "tour.step10.body":
            "总线 M/S 平衡:−100 偏向 Mid,+100 偏向 Side;调整MS音量关系。",
        "tour.step11.title": "Lead Select",
        "tour.step11.body": "通过序号选择和实时强制居中Lead轨道,0=遵循分析。",
        "tour.step14.title": "Range · 范围",
        "tour.step14.body":
            "三档选项:全曲跟随 / 循环区 / 手动,决定采集和输出的作用范围。",
        "tour.step13.title": "过渡时间",
        "tour.step13.body":
            "段间过渡:20–300ms;决定区间之间音量和声像过渡的速度。",
        "tour.step15.title": "角度域曲线",
        "tour.step15.body":
            "每条声像曲线由控制点构成:双击任意位置添加点,拖动调整角度与增益,选中后双击删除;每个点可选钟形 / 搁架 / 切除(带 6–24 dB/oct 斜率),最多 16 点",
        "tour.step16.title": "本页:轨道",
        "tour.step16.body":
            "每个通道占一行,构成 15 轨矩阵;行内是每轨的微调控件,冻结后才会解锁为手动调节。",
        "tour.step17.title": "轨道行",
        "tour.step17.body":
            "状态灯 / CH+ST / 标签;每轨一行,未连接时整行会降低透明度。",
        "tour.step18.title": "pan 旋钮",
        "tour.step18.body":
            "声像 −100..+100;冻结后解锁为手动调节,未冻结时由分析曲线驱动。",
        "tour.step19.title": "width 旋钮",
        "tour.step19.body": "立体声轨的宽度 0–100%;对单声道轨无效。",
        "tour.step20.title": "音量 / 电平合成控件",
        "tour.step20.body":
            "液柱表示实时电平,卡箍表示音量推子;拖动卡箍可修改轨道音量,冻结后解锁为手动调节。",
        "tour.step21.title": "优先级",
        "tour.step21.body":
            "0..10,10 为最高优先级,优先级更高的轨道在声像上会更靠近中心。",
        "tour.step22.title": "主唱锁",
        "tour.step22.body":
            "把该轨标记为主唱:分析时它作为最高优先级轨道,中心槽策略(主唱独占)会优先居中它;多轨锁定时会给出提示。",
        "tour.step24.title": "音量豁免",
        "tour.step24.body":
            "独立开关:该轨不参与音量平衡计算,与主唱锁 / Lead Select 不联动。",
        "tour.step25.title": "参与自动声像",
        "tour.step25.body":
            "该轨是否参与声像重分布;立体声轨默认关闭,但仍参与音量平衡。",
        "tour.step23.title": "配对",
        "tour.step23.body":
            "把两条轨配成一对,配对的两轨声像联动、作为一个整体移动;同组两轨行首显示同色圆点。",
        "tour.step26.title": "冻结 PAN / VOL",
        "tour.step26.body":
            "两个冻结开关会 bypass 引擎对该维度的影响,旋钮与推子随之解锁为纯手动调节。",
        "tour.step27.title": "启用开关",
        "tour.step27.body":
            "关闭该轨:该轨不参与采集 / 分析 / 输出,不对轨道做任何调整。",
        "tour.step28.title": "本页:波形",
        "tour.step28.body":
            "查看分析结果、调整分段;上方是泳道曲线,下方是工具条;拖动、框选、缩放都在这页完成。",
        "tour.step34.title": "下方面板与操作",
        "tour.step34.body":
            "框选泳道建立选区;拖拽边界微调分段;用滚轮或底部缩放条缩放 / 平移;双击可分割,选中段可合并。",
        "tour.step32.title": "泳道区",
        "tour.step32.body":
            "已为你放大泳道:每条泳道叠着 pan / vol 阶梯曲线,播放头随走带移动;上排 pan(声像)、下排 vol(音量)",
        "tour.step33.title": "选区手柄",
        "tour.step33.body":
            "已为你创建一段示例选区:两端就是手柄,拖动手柄可改范围;『设为范围』会把选区写入 Range",
        "tour.step31.title": "框选后工具条",
        "tour.step31.body":
            "重采集 / 重分析 / 重新识别 / 清除,作用于选中的轨道 × 选区。",
        "tour.step35.title": "段落详情与编辑",
        "tour.step35.body":
            "已为你选中一段:检查器里可编辑该段的 pan / vol、查看 origin(E / C)、锁定该段。",
        "tour.step30.title": "分段工具条",
        "tour.step30.body":
            "SENSITIVITY / MIN SEG 分段滑杆;边界可拖拽,双击可分割,选中相邻两段按 Delete 合并。",
        "tour.step29.title": "VAD 滑杆(语音检测)",
        "tour.step29.body":
            "这五个滑杆决定「哪里算人声、哪里算静音」,是分段的基础:THRESHOLD 响度门限——比它响才算在唱;HYSTERESIS 回滞——关门比开门低一档,防止门限附近抖动把句子切碎;HOLD 静音保持——短暂停顿仍算在唱,避免字与字之间被断句;PAD PRE / PAD POST 前后留白——不咬掉起音与尾音。",
        "tour.step36.title": "本页:设置",
        "tour.step36.body":
            "说明、响度口径、中心槽策略、缩放、语言、存储、诊断;末步的「重看引导」也在这页。",
        "tour.step2.title": "工作流程与优先级",
        "tour.step2.body":
            "一次完整的工作流程:采集 → 分析 → 微调/冻结 → 写入自动化 → 手动调自动化。优先级:宿主自动化 > 冻结的手动值 > 手动微调 > 引擎分析曲线。之后随时可在设置页「查看工作流程」里回看。",
        "tour.step37.title": "使用说明",
        "tour.step37.body":
            "三步工作流 + 九条重要提示;请逐条读一遍,违反任何一条都会导致静音或错音。",
        "tour.step38.title": "第二响度指标",
        "tour.step38.body":
            "段响度采用哪个指标:K 加权段积分(接近人耳听感)/ RMS(平均能量)/ 峰值 dBFS(瞬时峰值);修改后需重新分析。",
        "tour.step39.title": "中心槽策略",
        "tour.step39.body":
            "多轨争抢中心位时如何处理:按优先级排队 / 主唱独占 / 均分微偏。",
        "tour.step40.title": "界面缩放",
        "tour.step40.body": "缩放整个界面UI大小;与顶部栏(header)缩放联动。",
        "tour.step41.title": "语言",
        "tour.step41.body": "中文 / English / Français 三语即时切换。",
        "tour.step42.title": "存储状态",
        "tour.step42.body":
            "特征数据可以内嵌在工程里,也可以存到外部;占用大小可在这里查看。",
        "tour.step43.title": "诊断",
        "tour.step43.body": "可复制每轨心跳与失准计数,用于排查连接问题。",
        "tour.step3.title": "版本与复制",
        "tour.step3.body":
            "右上角是版本区:V1 / V2 两套平衡方案可随时切换,「复制」按钮可把当前版本整套复制到另一个版本;插件版本号显示在右下角,遇到问题时请连同诊断信息一起反馈。",
        "tour.step44.title": "以后在这里可以再看一次",
        "tour.step44.body": "引导随时可以重看,红字九条也在同一块里展开。",

        // 工作流程图(tour 步 35;节点 + 优先级行,三语)。
        "workflow.capture": "采集",
        "workflow.analyze": "分析",
        "workflow.tweak": "微调 / 冻结",
        "workflow.write": "写入自动化",
        "workflow.manual": "手动调自动化",
        "workflow.priority":
            "优先级:宿主自动化 > 冻结的手动值 > 手动微调 > 引擎分析曲线",

        // tour demo 的 15 条轨名(J62)。真值在 web/shared/mock-data.js 的 DEMO_LABELS,
        // key 表在同文件的 DEMO_LABEL_KEYS(mock-data.js 不 import 本文件,依赖方向照 06 §6.2)。
        // 为什么它们要进词典:用户轨的 label 是用户数据、保持原样,而 **tour demo 是我们自己造的
        // UI 内容**,05 §5「切语言同步全部可见文案」对它成立 —— 否则 EN/FR 用户看到英/法文讲解框
        // 配 15 条中文轨名。渲染 label 还是 t(key) 由 T31 决定,mock-data.js 两者都给。
        // zh 逐字 = mock-data.js DEMO_TRACKS 的 label(设计稿 CH_LABELS + T27 补的 14/15);
        // EN/FR 为 T27 自译,与 tour.step* 同批待人工审校。L/R/C 的 FR 侧照 master.distAxis 取 G/D/C。
        "demo.ch1": "主唱1",
        "demo.ch2": "主唱2",
        "demo.ch3": "和声 L",
        "demo.ch4": "和声 R",
        "demo.ch5": "和声 C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "低八度",
        "demo.ch9": "高八度",
        "demo.ch10": "和唱 L",
        "demo.ch11": "和唱 R",
        "demo.ch12": "音效轨",
        "demo.ch13": "念白",
        "demo.ch14": "和唱 C",
        "demo.ch15": "outro",
        "demo.versionName": "基础平衡", // tour demo 的 V1 版本名(用户命名语义的演示值;deepseek-review 建议 5)

        // 分组词条组(05 §5.1,623-633 行;J71② 整组采设计稿三语提案表)。
        // 两插件共用的扁平 key 组,取代旧 in.groupSwitchConfirm / out.set.groupSwitchConfirm /
        // in.oneGroupPerTrack / in.groupHint.noOutputInGroup(旧 key 作废,不得并存)。
        "group.label": "组",
        "group.oneOnly":
            "同一条人声轨只能属于一个组——同轨插第二个 SCVB Input(无论哪个组)只会收到静音",
        "group.noOutput":
            "组 {g} 无 Output——请在目标总线插入 SCVB Output 并选择组 {g}",
        "group.switchConfirm": "切换到组 {x} 将断开当前连接(组 {y} · 通道 {n})",
        "group.switchConfirm.out": "切换到组 {x} 将断开本组全部 {n} 条连接",
        "group.switchConfirm.primary": "切换到 {x}",
        "ch.occupied": "通道 {n} 已被占用(组 {g})",
        "banner.secondOutput": "组 {X} 已有主 Output,本实例只读观察",
        "group.oneInstanceNote": "本实例只属于一个组",

        // [D1] Header 常驻撤销 / 重做(契约 §1.25/§1.26;Ctrl+Z 之外的显式入口)。
        // EN/FR 为本卡自译,**待 U17 人工审校**。置灰 tooltip 不写「不可用」这类
        // 状态词,直说栈空的事实(05 §5 状态词规范:每态唯一用词,不自由造泛化说法)。
        "header.history": "撤销与重做",
        "header.undo": "撤销",
        "header.redo": "重做",
        "header.undoEmpty": "没有可撤销的操作",
        "header.redoEmpty": "没有可重做的操作",

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "全曲",
        "master.rangeFollowHint":
            "跟随播放,自动扩展范围。已分析区域共 {n} 段 · 合计 {t}",
        "master.step1.desc": "开关打开后播放本范围以记录响度特征",
        "master.widthAngleHint": "最外层人声的角度。0–90°",
        "master.msHint": "−100 偏 Mid / +100 偏 Side",
        "master.distHint": "柱高 = 音量,横位 = 声像;横线 = 立体声源张开度",
        "master.distAxis": "左 L · −50 · 中 C · +50 · 右 R",

        // T43 / [J75] A+B:分布图双视图(分布 ↔ 轨迹)+ 15 色轨道配色。
        // EN/FR 为本卡自译,与 tour.step* 同批**待 U17 人工审校**。
        "chart.modeGroup": "分布图视图",
        "chart.modeDistribution": "分布",
        "chart.modeTrajectory": "轨迹",
        "chart.trajHint":
            "每轨一条线 = 该轨最终打印的声像;没有分段的区间不画线",
        // y 轴的三个方位词(上 / 中 / 下 = +100 / 0 / −100)。刻度**数字**由
        // trajectory-chart.js 按当前纵向缩放档算,方位词只贴在这三条锚刻度上。
        // **三条各自成 key,不合成一串按语序拆** —— 合成串的话,U17 审校时调一下
        // 词序就会把左右标反,而那是一眼看不出来的错(图照画,只是左右颠倒)。
        "chart.panSideR": "右 R",
        "chart.panSideC": "中 C",
        "chart.panSideL": "左 L",
        "chart.trajCanvasAria":
            "声像轨迹图:滚轮左右平移,Ctrl+滚轮横向缩放,Shift+滚轮上下平移,Alt+滚轮纵向缩放;也可拖拽或用方向键",
        "chart.zoomAria": "时间轴缩放档位",
        "chart.backToPlayhead": "回到播放头",
        "chart.resetPanZoom": "纵向复位",
        "chart.trajEmpty": "尚无分段结果——分析之后这里显示每轨的声像轨迹",
        "chart.legendAria": "轨道配色图例",
        "chart.legendHint": "色点对应轨号;悬停高亮该轨",

        // T41:Tab3「建议表」视图 + CSV 导出(11 §4.2.3 通路 B1/B2;U12 已答复进 v1 主线)。
        // 列头词条对应冻结列名(悬停列头可见原列名),EN/FR 为本卡自译、**待 U17 人工审校**。
        "suggest.entry": "建议表",
        "suggest.title": "建议表",
        "suggest.disclaimer":
            "这些是建议值——SCVB 不会替你在 DAW 里应用它们;请照表手工设置。",
        "suggest.backToLanes": "返回泳道",
        "suggest.export": "导出 CSV",
        "suggest.scope": "{v} · {t} 轨 · {n} 行",
        "suggest.empty": "尚无分段结果——分析之后这里列出每轨每段的建议值",
        "suggest.staleNote":
            "其中 {n} 条轨的采集数据已过期——建议先重新采集再照表设值。",
        "suggest.legend":
            "声像与音量是每段一个建议值;宽度是每轨一个当前设置值,所以同一轨每行相同。宽度只对立体声轨有值,mono 轨与取不到值时都留空(0 是「收成 mono」的有效值,与留空不是一回事);来源与锁定的取值与工程里存的逐字相同",
        "suggest.exporting": "正在导出…",
        "suggest.exportOk": "已导出 {n} 行 → {path}",
        "suggest.exportCancelled": "已取消导出",
        "suggest.exportFail": "导出失败:{reason}",
        "suggest.exportUnavailable":
            "本版本尚未接通导出——保存对话框待 native 侧落地",
        "suggest.tableAria": "建议表:每轨每段的声像 / 音量 / 宽度建议值",
        "suggest.rowsAria": "建议表数据区,方向键滚动",
        "suggest.col.trackIndex": "轨",
        "suggest.col.trackLabel": "轨名",
        "suggest.col.sourceChannels": "声道",
        "suggest.col.version": "版本",
        "suggest.col.versionName": "版本名",
        "suggest.col.segmentIndex": "段",
        "suggest.col.t0Sec": "起(秒)",
        "suggest.col.t1Sec": "止(秒)",
        "suggest.col.pan": "声像",
        "suggest.col.volDb": "音量 dB",
        "suggest.col.width": "宽度 %",
        "suggest.col.origin": "来源",
        "suggest.col.locked": "锁定",

        "master.transitionHint": "段与段之间参数切换的过渡时间",
        "master.copyConfirmWarn":
            "目标已有数据将被覆盖——{name} 的 15 轨 pan / vol、全部分段结果与手动编辑标记将被整体替换。可撤销(Ctrl+Z)。",
        "tracks.colLegend":
            "音量＝该轨是否进音量平衡计算(默认开)和音量调节· 声像＝该轨是否进声像重分布(stereo 轨默认关,仍参与音量平衡)和声像调节· 冻结P/V＝正常计入引擎计算,但不再被引擎驱动,旋钮解锁为纯手动控制(两开关共用一个每轨自动化参数)",
        "tracks.emptyGroup":
            "组 {g} 尚无输入——在人声轨插件链最后一格插入 SCVB Input 并选择组 {g}",
        // ---- T32 Wave 1 新增(Output Tab2 正式实现;05 §2.2 有语义无 key 的位置)----
        // 列头短名一律 mono 大写微标,三语同值或极简缩写(与 master.msEyebrow 同族纪律:
        // 这是视觉层的 mono 标签,不是句子;26px 列宽下任何长词都会被 overflow 截掉)。
        // EN/FR 待人工审校(05 §5),逐条见 T32 差异清单。
        "tracks.colCh": "CH",
        "tracks.labelPlaceholder": "轨 {n}",
        "tracks.colVolPart": "参与音量调节",
        "tracks.colState": "状态",
        "tracks.colPan": "PAN",
        "tracks.colW": "W",
        "tracks.colVolLevel": "音量",
        "tracks.colPrio": "PRIO",
        "tracks.colLead": "LEAD",
        // 参与性两列的列头短标签(回流⑬);其全称与语义靠 tracks.colLegend 长句说明,
        // 该长句里同名的三个词(设计稿 626 行的三处 <strong>)由 tab-tracks.js 的
        // legendSegments() 按「段首 → 首个等号」自行切出并加粗,不依赖本组 key。
        "tracks.colVolExempt": "音量",
        "tracks.colAutoPan": "声像",
        "tracks.colFreezePan": "冻结P",
        "tracks.colFreezeVol": "冻结V",
        "tracks.colOn": "ON",
        "tracks.footNote": "15 轨",
        "tracks.emptyRoute": "人声轨的输出路由须保持指向本总线",
        // 「样本不足」角标保短版(裸词条 lowSample),全句进 tooltip(统筹裁定 B12)
        "lowSample.full": "样本不足,分析结果可能不稳定",
        "tracks.panAutoHint": "自动模式:由分析曲线驱动",
        // Lead Select 选中轨的行首居中标记(05 §2.2 主唱锁行;全句走 master.leadSelectHint)
        "tracks.leadCenter": "居中",
        "tracks.multiLead": "多主唱居中",
        "tracks.misaligned": "失准 ×{n}",
        "tracks.srErr": "采样率不一致",
        "tracks.labelEdit": "轨道名称(≤24 字符)",
        "tracks.reidentifyOne": "重新识别轨 {n}",
        // 单轨重新识别的**二次确认**(05 §2.2「二次确认同 §2.3」;契约 §1.6 的
        // clearManual 分支:locked 段不受影响,须先逐段解锁)。T32 Wave 2 新增。
        "tracks.reidentifyConfirm":
            "将清除轨 {n} 的手动固定值并重新识别;已锁定段保持不变,是否继续?",
        "tracks.pairNone": "无",
        "tracks.pairFullSuffix": "(满)",
        "tracks.pairOverflow": "配对超员",
        "common.decrease": "减",
        "common.increase": "增",
        "wave.trackPickHint": "勾选左侧轨头选择目标轨(可多选,按 Shift 连选)",
        "wave.selChip": "上面四个操作作用于 {n} 轨",
        "wave.setRangeTip":
            "作用范围已改为选区 {x}–{y}——Range 档位已切到「手动」,工作选区本身不变",
        "wave.originLegend":
            "origin:E = 手动编辑,C = 手动创建,auto 段无角标。锁定段在重新识别时保持不变。",
        "set.loudnessMode.title": "段落响度分析算法",
        "set.loudnessMode.note":
            "影响分析时的段间响度归一化基准;改后需重分析。",
        "set.centerSlot.title": "多轨争抢中心位时的优先级",
        "set.centerSlot.note":
            "主唱锁与 Lead Select 之外的兜底规则;不影响音量豁免。",
        // 05 §3(463 行)以短名 `in.chHint` 引用同一条,§5.2(658 行)印作本长名;
        // 实施一律用本 key —— applyI18n 对未命中的 key 不报错也不回退,写成 in.chHint 会静默留占位原文。
        "in.chHint.groupEmpty": "该组尚无 Output,通道表为空",

        // ---- T35 新增(Output Tab4 设置;05 §2.4 全部行 + J69 两设置块)----
        // zh 逐字取 05 §2.4 / §5.2(选项正名「K 加权段积分」非 LUFS-S,05 §2.4);
        // EN/FR 为 T35 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "set.usage.eyebrow": "使用说明",
        "set.usage.workflow":
            "采集 → 分析 → 输出。开启采集并播放,插件记录每条轨的响度特征;分析在离线状态下算出每个乐句的声像/音量方案;输出切到「引擎驱动」后由插件驱动参数,在 DAW 侧开启 Latch 或 Write 模式以录制为自动化轨道。",
        "set.usage.docs": "文档",

        // ---- [J78] 「手动接管与自动化」说明块(05 §2.4;Tab4「使用说明」块之后)----
        // zh 逐字取 masterPlan plan/settings-automation-guide-2026-08-24.md,不得转述;
        // 优先级链的措辞与 workflow.priority(tour 步 35 流程图)同源同字。
        // EN/FR 为自译,已入 U17 人工审校清单。
        "set.automationGuide.eyebrow": "手动 / 自动化",
        "set.automationGuide.title": "手动接管与自动化,谁说了算?",
        "set.automationGuide.line1":
            "优先级从高到低:宿主自动化 > 冻结的手动值 > 手动微调 > 引擎分析曲线。",
        "set.automationGuide.line2":
            "冻结 PAN 或 VOL = 手动接管该维度;write 时会以一条平直线写入自动化,之后你可以在 DAW 里自由手绘。",
        "set.automationGuide.line3":
            "段检查器里的手动微调改的是引擎曲线本身,会随 write 一起写入(该维度已冻结时除外)。",
        "set.automationGuide.line4":
            "画好自动化后,插件会忠实跟随宿主,不再被引擎覆盖;只有重新 write 才会再次覆盖。",

        "set.loudnessMode.eyebrow": "第二响度指标",
        "set.loudnessMode.opt.kw_integrated": "K 加权段积分",
        "set.loudnessMode.opt.rms": "RMS",
        "set.loudnessMode.opt.peak_dbfs": "峰值 dBFS",
        "set.centerSlot.eyebrow": "中心槽策略",
        "set.centerSlot.opt.priority_queue": "按优先级排队",
        "set.centerSlot.opt.lead_exclusive": "主唱独占",
        "set.centerSlot.opt.even_spread": "均分微偏",
        "set.guide.showAll": "查看全部九条",
        "set.guide.collapse": "收起",
        "set.guide.rulesMissing": "九条约束全文将在发布版补齐",
        "set.storage.eyebrow": "存储状态",
        "set.storage.embedded": "内嵌于工程({mb} MB)",
        "set.storage.external": "已保存为外部文件(>8MB 自动)",
        "set.storage.sessionGuid": "session {guid}",
        "set.diag.eyebrow": "诊断",
        "set.diag.copy": "复制诊断信息",
        "set.diag.copied": "已复制",
        "set.diag.colCh": "CH",
        "set.diag.colHb": "HB",
        "set.diag.colMis": "MIS",
        "set.diag.colGen": "GEN",
        "set.diag.colSeq": "SEQ",
        "set.reanalyze": "改后需重分析",
        // ---- T36 新增(Input 单页正式实现;05 §3 语义,词条真源 05 §5/§3)。
        // EN/FR 为 T36 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "in.pillSub.passthrough": "直通中:未对本轨做任何处理",
        "in.pillSub.takenOver": "已接管:本轨静音,声音由插入 Output 的总线输出",
        "in.pillSub.hysteresis": "连接不稳定,即将切换直通",
        "in.pillSub.stereo": "本轨为立体声源,SCVB 将保留其宽度",
        "in.source.mono": "本轨为单声道源",
        "in.group.note": "须与 Output 同组才能连接;单组使用保持 A 即可",
        "in.channels.emptyHint": "选择本轨的通道编号(与 Output 轨道页一一对应)",
        "in.unassigned": "未分配",
        "in.releaseConfirm.primary": "释放",
        "in.channels.manual.confirm": "确认",
        "in.priority.note": "此设置保存在 Output",
        "in.priority.offline": "需 Output 在线",
        "in.footer.hint": "就绪",
        "in.footer.noBackend": "未接后端——请经 web-preview 预览入口打开",
        "in.priority.unassigned": "需先选择通道",
        "ch.occupied.group": "通道已被占用(组 {g})",

        // T31 新增(Output 外壳 + Tab1 正式实现,05 §2.0/§2.1 语义 + design-v2 定稿文案)。
        // 立项理由与逐条出处见 scratchpad/t31/deviations.md「新增词条」节;
        // EN/FR 为 T31 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "common.cancel": "取消",
        "tab.master": "整体调整",
        "tab.tracks": "轨道",
        "tab.wave": "波形与分段",
        "tab.settings": "设置",
        "version.emptyBadge": "空",
        "master.copyConfirmPrimary": "覆盖并复制",
        "master.captureOff": "采集 OFF",
        "master.step2.desc": "将影响 {n} 区段 / {m} 轨;{k} 处手动编辑将保留",
        "master.step2.desc.noData": "当前范围内无采集数据——调整范围或先采集",
        "master.step2.coverage": "范围内 {p}% 已覆盖,未覆盖部分将保持原状",
        "master.analyzing": "分析中…",
        "master.analyzeDone": "分析完成",
        "master.step3.desc": "请在 DAW 中打开自动化录入",
        "master.step3.descStrong":
            "如果可选,请使用 Latch(推荐)或 Write 模式;请勿使用 Touch 模式",
        "master.writeConfirm.ok": "知道了,开始",
        "master.writeConfirm.undo": "撤销(回溯到跟随宿主)",
        "master.groupEyebrow": "GROUP · 分组",
        "master.widthEyebrow": "WIDTH · 最大角度",
        "master.rangeEyebrow": "RANGE · 范围",
        "master.distEyebrow": "声像 / 音量分布",
        "master.curveEyebrow": "基于角度的音量调整 · ±12 dB",
        "master.curveLegendMs": "MS 等效增益",
        "master.curveAxisX": "左 L −100 · −50 · 中 C 0 · +50 · 右 R +100",
        "master.curveEmptyHint": "双击添加控制点",
        // ---- T34 曲线编辑器(curve.*;shape/side 术语逐字对拍 05 §5 术语表)----
        "curve.canvasLabel": "角度域增益曲线编辑器",
        "curve.maxPoints": "已达 16 点上限",
        "curve.centerSide": "中心点请选择方向",
        "curve.shape.bell": "钟形",
        "curve.shape.shelf": "搁架",
        "curve.shape.cut": "切除",
        "curve.side.out": "向外",
        "curve.side.left": "向左",
        "curve.side.right": "向右",
        "curve.qLabel": "Q",
        "curve.slopeLabel": "斜率",
        "curve.slope.opt6": "6 dB/oct",
        "curve.slope.opt12": "12 dB/oct",
        "curve.slope.opt18": "18 dB/oct",
        "curve.slope.opt24": "24 dB/oct",
        "curve.sideTooltip":
            "向外:以该点为界,切除远离中心的外侧;向左/向右:只切该侧",
        "curve.deleteLabel": "删除控制点",
        "curve.announcePoint": "点 {n}:角度 {angle},{gain} dB,{shape},Q {q}",
        "curve.announcePointDir":
            "点 {n}:角度 {angle},{gain} dB,{shape},Q {q} · 方向 {side}",
        "master.leadSelectDefaultNote": "0 = 自动选择(默认)",
        "range.manual": "手动",
        "master.rangeLoopStale": "循环区已失效,沿用上次范围",
        "master.rangeLoopMissing": "宿主未提供循环区——档位保留但不可选",
        "master.rangeStart": "起点",
        "master.rangeEnd": "终点",
        "master.setToPlayhead": "设为播放头",
        "master.barsEstimateNote": "小节为估算值,播放该区域后校准",
        "footer.defaultHint":
            "采集 → 分析 → 输出:在 Tab1 打开采集开关并播放本范围",
        "scale.current": "当前",
        "scale.overflow": "超出当前屏幕",

        // ---- T31 Wave 2 新增(统筹裁定「§E 九处 + Wave 2 增补」;zh 逐字取 05 §2.0/§2.1 正文)----
        // 建议 05 §5 收录;EN/FR 为 T31 自译,已入待审校清单(scratchpad/t31/deviations.md)。
        // 横幅①③(05 §2.0「持续横幅」行):正文原句里的「M 轨」「轨 N」两处计数改写成
        // 占位符 {m} / {n} —— 数字来自 scvb.conn(misalignCount / srMismatch),不占位就填不进去。
        "banner.misaligned":
            "路由失准:{m} 轨检测到时间线缺口——检查人声轨是否仍指向本总线",
        "banner.srMismatch": "轨 {n} 采样率不一致,已禁用",
        "banner.sidecarMissing": "采集数据缺失/过期,请重新采集",
        "banner.noTimeline": "宿主未提供时间线",
        // ARMED 轻确认(05 §2.1 ③ 版本 chip 行逐字):FOLLOW 直接切、PRINT 硬拒绝,只有 ARMED 弹这条
        "master.versionArmedConfirm": "引擎输出将平滑切至新版本,是否继续?",
        // Tab1 空态卡(A1;用户裁定 2026-08-18:五步制,前三步红字,
        // 先跳线后插 Output;步 1 强调插入位置——措辞按 J45 宿主中立口径)
        "master.empty.step1":
            "在每条人声轨插件链最后一格(所有处理之后)插入 SCVB Input",
        "master.empty.step2": "人声轨的输出路由保持指向共同总线",
        "master.empty.step3": "SCVB Output 插在该总线第一格",
        "master.empty.step4": "采集并播放",
        "master.empty.step5": "分析并开启输出",
        // 轻确认条的主按钮(与 common.cancel 成对;05 未给逐字,取最短肯定式)
        "common.continue": "继续",
        // eyebrow(视觉层 mono 大写元素,照设计稿 **三语同值不译**;统筹 Wave 2 增补①:
        // 立 key 而非静态文本,免得切语言时被 applyI18n 漏掉)
        "master.msEyebrow": "MS BALANCE",
        "master.leadEyebrow": "LEAD SELECT",
        // 过渡卡 eyebrow(统筹 Wave 2 增补②:05 §2.1 ④ 标题「过渡时间」与设计稿一致,
        // 取代 Wave 1 用的裸词条 `transition`)
        "master.transitionEyebrow": "过渡时间",

        // ---- T31 Wave 2 评审修订新增(对抗校验 P2-4 / P2-6;建议 05 §5 一并收录)----
        // Output toast ①②(05 §2.0 组件表「Output toast」行逐字;来源 04 §5.6 / §5.4)。
        // toast③「已重采集 …」归 T33(Tab3 重采集本波未接线),故不立。
        "toast.projectCopy": "检测到工程副本,已创建独立采集数据副本",
        "toast.sidecarSwitched":
            "采集数据已转存外部文件(>8MB),位置见设置页(路径管理与导入导出待功能卡)",
        // 缩放 10 秒防呆确认框(05 §1.2:立即预览 → 10 秒倒计时 → 取消/超时/关窗回退)。
        // 05 只给机制未给逐字正文;{s} = 剩余秒数,按钮「取消」复用 common.cancel。
        "scale.confirmBody": "缩放已应用,{s} 秒后回退",
        "scale.keep": "保存",
        // PRINT 态三处 disabled 的 tooltip(兑现 deviations A22;zh 逐字取 05 §2.1 ⓪/③)
        "master.printLock.group": "自动化写入中不可切换分组",
        "master.printLock.version": "自动化写入中不可切换版本",
        "master.printLock.copy": "自动化写入中不可复制",

        // ---- T33 Wave 1 新增(Output Tab3 正式实现;05 §2.3/§2.3a 有语义无 key 的位置,
        // 图谱 A-17/A-18/A-19 + 各件所需)。EN/FR 为 T33 自译,已入 U17 待人工审校清单。----
        // 7 滑杆短标:mono 大写微标族,三语同值(A-19 统筹裁定,同 tracks.colCh 纪律;
        // 顺序 = 05 §2.3 的 setVadParams 五字段 + setSegmentation 两字段,不可重排)
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        // 桥面字段是 `hangover_ms`(契约 §1.18,冻结不动);**显示名取 HOLD** ——
        // hangover 是 VAD/语音编码域的标准词(G.729B、AMR VAD),而本插件的用户
        // 是混音师,他们脑内的词是 gate 的 Attack/**Hold**/Release;两者在 02
        // §2.3 的状态机里功能完全等同(releaseCount ≥ hangoverFrames 才出段)。
        // 术语链靠 tip 里的括注不断。用户 preview 提出,统筹裁定(deviations)。
        "wave.sldHangover": "HOLD",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
        // 七条悬停说明(A-19 同族自造;`{d}` = 02 §0.3 的出厂默认档)。
        // 七个 mono 微标不看说明猜不出各自管什么 —— 用户 preview 逐个问过来了。
        "wave.tipThreshold": "门限阈值:比它响则被识别,比它小被忽略(默认 {d})",
        "wave.tipHysteresis":
            "回滞:开门用门限值,关门要再低这么多才关。防止音量在门限附近抖动、把一句话切成碎片(默认 {d})",
        "wave.tipHold":
            "静音保持(VAD hangover):跌破门限后仍被识别的时长,防止字与字之间的短停顿被判成句子结束(默认 {d})",
        "wave.tipPadPre":
            "前留白:识别区域之前的留白,用于保留音头、气口(默认 {d})",
        "wave.tipPadPost": "后留白:识别区域之后的留白,用于保留尾音(默认 {d})",
        // 02 §3.1/§3.2:谷切分只对**长于 maxSegment(默认 8s)**的段生效 ⇒ 典型
        // 乐句(1–4s)下拖动本杆看不到变化。已转 native 侧评估,tip 先据实说明。
        "wave.tipSensitivity":
            "分段灵敏度:越高越容易在能量谷处切段。仅对长于 8 秒的段生效,短乐句上不会有变化(默认 {d})",
        // 02 §2.3 后处理 P1 逐字:「core 段长 < minSegmentMs → **丢弃**」,且在
        // padding 之前判定。**不是**并进邻段(合并是内部常量 mergeGap,不暴露)。
        "wave.tipMinSeg":
            "最短段长:短于此长度的段会被直接丢弃(在前后留白之前判定),用于过滤杂音,但也会丢掉短促的 ad-lib 与单字和声(默认 {d})",
        // 泳道空态(05 §2.3 行 318 逐字;A-17)
        "wave.emptyMain": "尚无采集数据——开启采集开关并播放",
        "wave.emptyCta": "去 Tab1 打开采集",
        // 检查器顶部「跟随宿主」提示(05 §2.3a ADR-005 第 3 条逐字;A-18)
        "wave.followHostNote": "跟随宿主中:编辑已保存,开启输出后生效",
        // 工具条四钮(05 §2.3 行 300-303 表内名;「重新识别」复用裸词条 reidentify)
        "wave.btnRecapture": "重采集选区",
        "wave.btnReanalyze": "重分析选区",
        "wave.btnClearCoverage": "清除选区采集数据",
        // 两段式反馈(05 §2.3 行 298 三段式的 ①② 态;diff 首行走 wave.diffKept)
        "wave.applyCountdown": "300ms 后应用…",
        "wave.applying": "正在应用…",
        // 布防行的行内短说明(B-04 裁定:长句移 footer.recaptureOutputWarn,这里留短句)
        "wave.recaptureInlineNote":
            "本次重采集只针对选区进行,不影响输出引擎的工作范围",
        // 段落详情与编辑(05 §2.3a;标题 eyebrow + 只读字段名;origin 角标与标题
        // 同行,角标值 E/C 本身非词条 —— §17②,无独立字段行)
        "wave.inspectorTitle": "段落详情与编辑",
        // 面板开关 + 空态句(T33 Wave 5 用户裁定④:检查器改「常驻 + 显式开关」,
        // 覆盖 C-11 的条件渲染;建议 05 §2.3 行 286「可展开」同步改口径)
        "wave.inspectorToggle": "段落详情与编辑",
        // 标题栏 ✕ 的 aria-label(Wave 5 /code-review minor⑥):它折叠的是常驻面板,
        // 与工具条那枚开关同一个本地态 —— 不是「取消/放弃刚才的编辑」,
        // 因此不得复用确认框族的 common.cancel(EN/FR 自译,入 U17 待审校)
        "wave.inspectorClose": "收起",
        "wave.inspectorEmpty": "点选泳道内的段以编辑",
        "wave.segStart": "开始",
        "wave.segEnd": "结束",
        "wave.segLen": "时长",
        // 段响度标签正名(契约 §1.21:loudness_mode 默认 kw_integrated;
        // [J72a] C-12:该字段的旧显示名已废,不得回流)
        "wave.segLoudness": "K 加权段积分",
        "wave.volField": "VOL",
        "wave.lockSegment": "锁定本段",
        // 泳道内锁定小标(图例帧 778 文字;检查器行走 wave.lockSegment)
        "wave.lockBadge": "锁定",
        // 轨头件(B-08 裁定:覆盖率与段数并成一行;曲线可见压成眼睛图标钮)
        "wave.covSeg": "{p}% · {n} 段",
        "wave.curveVisible": "曲线可见",
        "wave.pickTrack": "选择轨 {n}",
        // 边界手柄 tooltip(A-14 裁定:Alt 关吸附写进 tooltip;双击分割一并说明)。
        // T33 Wave 4 用户 preview 反馈⑦:原句没说清「这个把手是干嘛的」——补主语
        // 「分段边界」,并把双击分割 / Delete 合并两条一并写进同一句(反馈⑧)。
        "wave.boundaryHandleTip":
            "分段边界:拖动改边界(自动吸附能量谷,按住 Alt 关闭吸附);双击段内=在此分割;选中相邻两段按 Delete=合并",
        // 吸附命中(金色)态的补充说明(反馈⑦③:金色是「已吸附」而不是别的意思)
        "wave.boundarySnapTip": "已吸附到能量谷(按住 Alt 关闭吸附)",
        // 缩放拖拽条两枚(T33 Wave 4 用户新件;05 无此件,J72 口径)
        "wave.hZoomBar": "横向缩放(拖动改视口跨度,方向键步进)",
        "wave.vZoomBar": "纵向缩放(拖动改泳道行高,方向键步进)",
        // 二次确认框两枚(05 §2.3 行 302/303 逐字)
        "wave.reidentifyConfirm":
            "将清除 {k} 个手动编辑标记并重算;{l} 个已锁定段保持不变,确定?",
        "wave.clearCoverageConfirm":
            "将删除选中轨×选区的采集特征数据,是否继续?",
        // ---- T33 Wave 2 交互反馈件(05 §2.3 行 300-313 / 契约 §5.5;
        //      EN/FR 入 U17 待审校)----
        // recaptureArm 拒绝态四值的行内说明(§5.5:出说明、不点亮 badge)
        "wave.armReason.noTracks": "未勾选目标轨,点击左侧轨头勾选",
        "wave.armReason.noSelection": "无有效选区,在时间标尺上拖出选区",
        "wave.armReason.readOnly": "只读观察态,写入未生效",
        "wave.armReason.noTimeline": "宿主未提供时间线,无法写入",
        // merge 的 notAdjacent 拒绝反馈(契约 §1.22)与工具条合并钮(05 行 313)
        "wave.notAdjacent": "只能合并相邻两段",
        "wave.btnMerge": "合并选中两段",
        // 重采集受覆盖提示(05 行 300:「将覆盖 K 段已有数据」)
        "wave.recaptureOverlap": "将覆盖 {k} 段已有数据",
        // diff 变更列表条目与增删摘要(A-02;首行走 wave.diffKept)
        "wave.diffItem": "轨 {ch} · 段 {i}:pan {pf}→{pt} · vol {vf}→{vt}",
        "wave.diffAddedRemoved": "新增 {a} 段 · 移除 {r} 段",
        // clearCoverage 回执反馈(契约 §1.24 的 clearedS)
        "wave.clearedCoverage": "已清除 {s} s 采集数据",
    },

    en: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "NOT CONNECTED",
        "state.staleLink":
            "Link anomaly (stale heartbeat: still online but updates stopped)",
        "state.connected": "CONNECTED",
        "state.noChannel": "NO CHANNEL",
        "state.outputOffline": "OUTPUT OFFLINE",
        "state.waitingForOutput": "WAITING FOR OUTPUT",
        "state.passthrough": "PASSTHROUGH",
        "state.takenOver": "TAKEN OVER",
        "state.waitingForOutput.group": "WAITING FOR OUTPUT · GROUP {X}",
        "state.groupSuffix": " · GROUP {X}",

        // 术语表(05 §5,530-586 行):三语建议,写入字典前用户可改;EN 状态词沿 Bridge 全大写惯例。
        // 三词分工(R1 定案,不得混用):range=范围(作用范围,Tab1)、interval=区段(分析产物)、selection=选区(Tab3 操作目标)。
        pan: "Pan",
        width: "Width",
        track: "Track",
        channel: "Channel",
        group: "Grouping",
        segment: "Segment",
        capture: "Capture",
        tabs: "Tabs",
        analyze: "Analyze",
        output: "Output",
        followHost: "FOLLOW HOST",
        engineDrive: "ENGINE DRIVE",
        version: "Version",
        priority: "Priority",
        leadLock: "Lead Lock",
        leadSelect: "Lead Select",
        leadFollowAnalysis: "Auto-select",
        msBalance: "MS Balance",
        stereoBadge: "ST",
        participateAutoPan: "Auto-Pan Participate",
        trackWidth: "Track Width",
        "tracks.monoWidthNoop": "Mono source cannot adjust width",
        "master.leadSelectHint":
            "This track is set as Lead and forced to center",
        pair: "Pair",
        threshold: "Threshold",
        sensitivity: "Sensitivity",
        range: "Range",
        follow: "Follow",
        selection: "Selection",
        interval: "Interval",
        loopRegion: "Loop",
        gainCurve: "Gain Curve",
        bell: "Bell",
        shelf: "Shelf",
        cut: "Cut",
        sideOut: "Out",
        sideLeft: "Left",
        sideRight: "Right",
        paddingPre: "Pre-pad",
        paddingPost: "Post-pad",
        transition: "Transition",
        misaligned: "ROUTE MISMATCH",
        capturing: "CAPTURING",
        stale: "STALE",
        manualMark: "Edited",
        locked: "Locked",
        copyTo: "Copy to",
        renameVersion: "Rename",
        // 术语表裸词条,只用于术语解释/图例;音频路径状态走 state.*(见 zh 同处注释)
        passthrough: "PASSTHROUGH",
        takenOver: "TAKEN OVER",
        printing: "PRINTING",
        lowSample: "LOW SAMPLE",
        // 完整句(zh 同处纪律);⚠ 长度受工具条动作行约束 —— 37 字符的
        // 「Stop automatically when playback ends」实测把 EN 动作行挤成两行
        // (30px → 48px,可见泳道少一条),修订轮压回 23 字符
        autoStop: "Stop when playback ends",
        reidentify: "Re-identify (incl. edited)",
        applyToSegments: "Apply",
        setAsRange: "Set as Range",
        clearCapture: "Clear capture",
        scale: "UI Scale",
        language: "Language",
        armedWaiting: "READY · WAITING",
        outOfRange: "OUT OF RANGE",
        occupied: "OCCUPIED",

        // 横幅、写入确认条、页脚与错误态长句(05 §5,587-605 行):占位符 {a}{b}{v}{x}{y}{n}{t}{k}{l} 原样保留。
        "banner.versionMismatch":
            "Saved by a newer SCVB. Please update the plug-in (local abi {a} / project abi {b}).",
        "banner.abiMismatch":
            "SCVB version mismatch — connection refused (Output abi {a} / Input abi {b}). Update both plug-ins to the same version.",
        "banner.printGuard":
            "Output is in ENGINE DRIVE (restored with project)",
        "banner.printGuard.confirm": "Continue engine drive",
        "out.master.writeConfirm":
            "Engine drive {v} · range {x}–{y} · 30 tracks. If Latch/Write is active in your DAW, playing this range will overwrite existing automation there; if not active, this is monitoring only.",
        "footer.printing": "ENGINE DRIVE {v} · {x}–{y}",
        "footer.printDone":
            "This pass covered {x}–{y}. If you were recording automation, switch back to Follow Host to check.",
        "out.master.writeConfirm.follow":
            "Engine drive {v} · range = all analyzed areas (follow, {n} segments · total {t}) · 30 tracks. If Latch/Write is active in your DAW, playing analyzed areas will overwrite existing automation there; if not active, this is monitoring only.",
        "footer.printing.follow": "ENGINE DRIVE {v} · FOLLOW (ANALYZED AREAS)",
        "footer.printDone.follow":
            "This pass covered the analyzed areas. If you were recording automation, switch back to Follow Host to check.",
        "wave.diffKept": "{k} edited/locked segments preserved",
        "tracks.manualOverwriteConfirm":
            "This replaces all analyzed segments of this track (current version) with a fixed value. Undoable.",
        "tracks.manualOverwriteConfirm.locked":
            "(includes {l} locked segments)",
        "in.pill.abiMismatch": "VERSION MISMATCH",
        "in.pill.srMismatch": "SR MISMATCH",
        "tracks.manualDrivenHint":
            "Track still driven by a manual fixed value — re-identify it?",
        "in.releaseConfirm":
            "Release channel {n} — no further processing on this track",
        "wave.recaptureArmed": "RECAPTURE READY · {x}–{y} · {n} TRACKS",
        "wave.recaptureArmedShort": "RECAPTURE READY",
        "footer.recaptureOutputWarn":
            "Engine still drives the global range — unaffected by the recapture selection",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.title 与 guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从
        // docs/USER_GUIDE.zh-CN.md#硬约束 + docs/hard-rules.i18n.json 生成写入(禁止手抄)。
        // BEGIN GENERATED hard-rules:en
        "guide.title":
            "Must read: SCVB's nine usage rules. Breaking any one of them causes silence, wrong panning, or failed analysis.",
        "guide.rule1":
            "Vocal tracks must keep their original DAW routing, pointing at the bus that hosts SCVB Output. Do not re-route a vocal track straight to the master output, and do not bypass the bus. (ADR-002)",
        "guide.rule2":
            "SCVB Input must sit in the last slot of the vocal track's plugin chain; SCVB Output must sit in the first slot of the bus. Any other position breaks the processing-order assumption SCVB relies on; for what each host calls that slot, see docs/DAW_COMPATIBILITY.md. (ADR-002 / J45)",
        "guide.rule3":
            'Input mutes its downstream output only while a healthy SCVB Output is detected — this is by design, not a bug. That mute path is what preserves the "vocal tracks first, bus second" ordering in the DAW\'s dependency graph, and it still holds under offline rendering and REAPER\'s anticipative multithreading. When no healthy Output is detected (not installed, not connected, peer has quit), Input falls back to passthrough automatically, over an 80 ms ramp with a 5-second hysteresis debounce (the hysteresis applies only to the "mute → passthrough" direction; "passthrough → mute" ramps over 80 ms as soon as health is confirmed), so installing only one of the two plugins will never leave you with a dead track. (ADR-002 / J12 + J32)',
        "guide.rule4":
            "Host pan must stay centred on both the vocal tracks and the bus. SCVB pans internally with an equal-power law, independently of the host's pan law; an off-centre host pan stacks on top of it and produces a wrong stereo image. (ADR-010)",
        "guide.rule5":
            'Each channel id is unique within one group, and a given vocal track may belong to only one group. When two Inputs in the same group claim the same channel, the late arrival shows a "channel conflict" warning and stays inactive; the same channel number in a different group is a separate, unrelated path. (ADR-002 / J66)',
        "guide.rule6":
            "Only one Output instance can be active in a group at any one time. A second instance in the same group drops into read-only observer mode and shows a warning; the eight groups (A–H) are independent bus domains and do not affect one another. (ADR-002 / J66)",
        "guide.rule7":
            "Stereo vocal tracks stay out of automatic pan assignment by default; switch one in by hand when you want it included. Mono sources are placed with equal-power pan; stereo sources use a dual-pan + width model (pan = centre of the arc, width = spread) which by default preserves the stereo width you already have, rather than letting automatic assignment overwrite it. (ADR-003 / J57 + J60)",
        "guide.rule8":
            'SCVB Output reports no additional latency to the DAW. Alignment is done by timeline addressing; do not try to "correct" it with PDC (plugin delay compensation). (ADR-002)',
        "guide.rule9":
            'Do not carry on exporting while a "timeline gap / overlap" warning is showing. Work through the common-pitfalls list in docs/DAW_COMPATIBILITY.md to check your routing first: for as long as the warning count refuses to fall back to zero, some track\'s audio is not being picked up correctly.',
        // END GENERATED hard-rules:en
        "guide.dontShowAgain": "Don't show again",
        "guide.start": "Get started",
        "set.reopenGuide": "Show guide again",
        "set.viewWorkflow": "View workflow",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "Want a one-minute tour of the interface?",
        "tour.ask.start": "Start tour",
        "tour.ask.later": "Not now — replay it any time in Settings",
        "tour.clickAnywhere": "Left-click anywhere to continue",
        "tour.skip": "Skip",
        "tour.prev": "Back",
        "tour.next": "Next",
        "tour.done": "Done",
        "tour.demoBadge": "DEMO DATA",
        // 首启语言选择卡(T36b 第四轮:独立 overlay,先于红字九条页;三语常显,各按钮用各自语言)。
        "lang-start.title":
            "请选择语言 / Choose your language / Choisissez votre langue",
        "lang-start.zh": "中文",
        "lang-start.en": "English",
        "lang-start.fr": "Français",

        // Input first-run mini tour ([J80] / T48). EN/FR 自译,入 U17 审校清单。
        "tour-in.help": "Show guide again",
        "tour-in.step1.title": "Welcome to SCVB Input",
        "tour-in.step1.body":
            "This is SCVB's capture end: one instance in the last slot of every vocal track, feeding that track to the SCVB Output on the bus. Take twenty seconds to meet the pieces.",
        "tour-in.step2.title": "Group A–H",
        "tour-in.step2.body":
            "A group is a self-contained workspace. This track has to sit in the same group as the SCVB Output that takes it over; with a single setup, just leave it on A.",
        "tour-in.step3.title": "Channel number",
        "tour-in.step3.body":
            "Each vocal track in a group takes one number (01–15), unique within that group. Click a card to claim it; a card marked “occupied” is already held by another track in this group.",
        "tour-in.step4.title": "Connection status",
        "tour-in.step4.body":
            "Once connected, this track outputs silence downstream and the SCVB Output on the bus takes over — this is by design, not a bug. When no healthy Output is detected (not installed, not connected, peer has quit), this track falls back to passthrough automatically, so installing only one of the two plugins will never leave you with a track that has no sound.",
        "tour-in.step5.title": "Full control lives in Output",
        "tour-in.step5.body":
            "Analysis, balancing and automation writing all live in the SCVB Output on the bus; this page only claims a channel and shows status. To walk through this again, click this “?” any time.",

        // tour full-parameter 44 steps (final copy; EN self-translated, pending U17 review).
        "tour.step1.title": "Welcome to SCVB",
        "tour.step1.body":
            "SCVB is Synchain's open-source multitrack vocal balancing tool. Take a minute to get to know the interface. A 15-track demo set is loaded and is cleared when the tour ends.",
        "tour.step4.title": "This is the SCVB Output main window",
        "tour.step4.body":
            "Four tabs at the top: Master / Tracks / Waveform / Settings. This tour walks you through each page.",
        "tour.step8.title": "Groups and their role",
        "tour.step8.body":
            "Groups split the channels into eight independent workspaces (A–H); each group runs its own capture, analysis, and output. For single-group use, keep A.",
        "tour.step5.title": "Step 1 — Capture",
        "tour.step5.body":
            "Turn on the capture switch and play the current range; the plug-in records the loudness features of every track. Nothing is written to automation in this step.",
        "tour.step6.title": "Step 2 — Analyze",
        "tour.step6.body":
            "Computes a pan/level plan for every phrase offline. Preview the capture range before you commit; edited or locked segments are preserved.",
        "tour.step7.title": "Step 3 — Output",
        "tour.step7.body":
            "Switch to ENGINE DRIVE and the parameters are driven by the analysis result. To record them as automation, arm Latch or Write on the DAW side.",
        "tour.step12.title": "Pan / level distribution",
        "tour.step12.body":
            "The live pan and level of all tracks: bar height = level, horizontal position = pan; the horizontal line = the stereo source spread and the angle-domain curve.",
        "tour.step9.title": "Width · max angle",
        "tour.step9.body": "The overall width limit: max spread angle 0–90°.",
        "tour.step10.title": "MS Balance",
        "tour.step10.body":
            "Bus M/S balance: −100 toward Mid, +100 toward Side; adjust the M/S level relationship.",
        "tour.step11.title": "Lead Select",
        "tour.step11.body":
            "Pick a track by number and force it to center in real time; 0 = follow analysis.",
        "tour.step14.title": "Range",
        "tour.step14.body":
            "Three modes: Follow / Loop / Manual; sets the capture and output scope.",
        "tour.step13.title": "Transition",
        "tour.step13.body":
            "Inter-segment transition: 20–300 ms; sets how fast level and pan ramp between intervals.",
        "tour.step15.title": "Angle-domain curve",
        "tour.step15.body":
            "Each pan curve is built from control points: double-click anywhere to add a point, drag to adjust angle and gain, and double-click to delete. Each point can be a bell / shelf / cut node (6–24 dB/oct slope); up to 16 points.",
        "tour.step16.title": "This page: Tracks",
        "tour.step16.body":
            "One row per channel, a 15-track matrix; the controls in each row tweak that track and only unlock to manual when frozen.",
        "tour.step17.title": "Track row",
        "tour.step17.body":
            "Status light / CH+ST / label; one row per track, dimmed when not connected.",
        "tour.step18.title": "Pan knob",
        "tour.step18.body":
            "Pan −100..+100; unlocks to manual when frozen, driven by the analysis curve otherwise.",
        "tour.step19.title": "Width knob",
        "tour.step19.body":
            "Stereo track width 0–100%; no effect on mono tracks.",
        "tour.step20.title": "Level / volume combo",
        "tour.step20.body":
            "The liquid shows the live level, the collar is the volume fader; drag the collar to change the track volume, unlocked to manual when frozen.",
        "tour.step21.title": "Priority",
        "tour.step21.body":
            "0..10, 10 = highest; higher-priority tracks sit closer to the center.",
        "tour.step22.title": "Lead Lock",
        "tour.step22.body":
            "Mark this track as the lead: it is the top-priority track during analysis, and the center-slot policy (lead exclusive) centers it first; warns if several are locked.",
        "tour.step24.title": "Vol exempt",
        "tour.step24.body":
            "Independent switch: this track is excluded from level balancing; not linked to Lead Lock / Lead Select.",
        "tour.step25.title": "Auto-Pan Participate",
        "tour.step25.body":
            "Whether this track joins pan redistribution; stereo tracks off by default, still level-balanced.",
        "tour.step23.title": "Pair",
        "tour.step23.body":
            "Pair two tracks; their pan is linked and the pair moves as one. Same pair shows the same colored dot at the row head.",
        "tour.step26.title": "Freeze PAN / VOL",
        "tour.step26.body":
            "The two freeze switches bypass the engine for that dimension; the knob and fader unlock to pure manual control.",
        "tour.step27.title": "Enable",
        "tour.step27.body":
            "Turn it off: the track skips capture / analyze / output — no adjustment is made to that track.",
        "tour.step28.title": "This page: Waveform",
        "tour.step28.body":
            "Review results and adjust segments; lane curves on top, toolbar below; drag, box-select and zoom all happen here.",
        "tour.step34.title": "Bottom panel and actions",
        "tour.step34.body":
            "Box-select lanes to make a selection; drag boundaries to fine-tune segments; wheel or bottom zoom bar to zoom/scroll; double-click to split, select two adjacent segments to merge.",
        "tour.step32.title": "Lane area",
        "tour.step32.body":
            "The lanes are zoomed in for you: each lane stacks the pan/vol step curves, and the playhead follows the transport; the top row is pan, the bottom row is vol.",
        "tour.step33.title": "Selection handles",
        "tour.step33.body":
            'A sample selection has been created for you: the two ends are the handles — drag them to change the range; "Set as Range" writes the selection to Range.',
        "tour.step31.title": "Post-selection toolbar",
        "tour.step31.body":
            "Re-capture / re-analyze / re-identify / clear, applied to the selected tracks × selection.",
        "tour.step35.title": "Segment details & editing",
        "tour.step35.body":
            "A segment has been selected for you: in the inspector you can edit its pan/vol, check its origin (E/C), and lock it.",
        "tour.step30.title": "Segmentation toolbar",
        "tour.step30.body":
            "SENSITIVITY / MIN SEG sliders; drag boundaries, double-click to split, select two adjacent segments and press Delete to merge.",
        "tour.step29.title": "VAD sliders (voice detection)",
        "tour.step29.body":
            "These five sliders decide what counts as voice versus silence — the basis of segmentation: THRESHOLD is the loudness gate — only louder than it counts as singing; HYSTERESIS closes the gate a step lower than it opens, so flutter near the threshold doesn't chop sentences; HOLD keeps short pauses counting as singing, so word-to-word gaps aren't split; PAD PRE / PAD POST add head and tail padding — so attacks and tails aren't clipped.",
        "tour.step36.title": "This page: Settings",
        "tour.step36.body":
            'Usage notes, loudness metric, center slot policy, scale, language, storage, diagnostics; the final "Show guide again" is also here.',
        "tour.step2.title": "Workflow and priority",
        "tour.step2.body":
            'A full workflow: capture → analyze → tweak/freeze → write automation → adjust automation manually. Priority: host automation > frozen manual value > manual tweak > engine analysis curve. You can review it anytime via the "View workflow" button in Settings.',
        "tour.step37.title": "Usage notes",
        "tour.step37.body":
            "Three-step workflow + nine important notes; please read each one. Breaking any of them causes silence or wrong panning.",
        "tour.step38.title": "Second loudness metric",
        "tour.step38.body":
            "Which metric for segment loudness: K-weighted integral (close to hearing) / RMS (average energy) / peak dBFS (instant peak); re-analyze after changing.",
        "tour.step39.title": "Center slot policy",
        "tour.step39.body":
            "What happens when tracks compete for center: priority queue / lead exclusive / even spread.",
        "tour.step40.title": "UI Scale",
        "tour.step40.body":
            "Scales the whole UI; synced with the header scale.",
        "tour.step41.title": "Language",
        "tour.step41.body":
            "Switch instantly between 中文 / English / Français.",
        "tour.step42.title": "Storage status",
        "tour.step42.body":
            "Features can be embedded in the project or stored externally; the size used is shown here.",
        "tour.step43.title": "Diagnostics",
        "tour.step43.body":
            "Copy per-track heartbeat and misalignment counts to troubleshoot connections.",
        "tour.step3.title": "Version & Copy",
        "tour.step3.body":
            "The top-right is the version area: two balance versions V1 / V2 can be switched anytime; the Copy button copies the current version to the other. The plug-in version number is shown at the bottom-right — send it together with diagnostics when you need help.",
        "tour.step44.title": "You can replay this tour here",
        "tour.step44.body":
            "The tour can be replayed at any time, and the nine hard rules open in the same block.",

        // Workflow diagram (tour step 35; nodes + priority row).
        "workflow.capture": "Capture",
        "workflow.analyze": "Analyze",
        "workflow.tweak": "Tweak / Freeze",
        "workflow.write": "Write automation",
        "workflow.manual": "Adjust automation manually",
        "workflow.priority":
            "Priority: host automation > frozen manual value > manual tweak > engine analysis curve",

        // tour demo 的 15 条轨名(J62;口径见 zh 侧同组注释)。EN 为 T27 自译,待人工审校。
        "demo.ch1": "Lead Vocal 1",
        "demo.ch2": "Lead Vocal 2",
        "demo.ch3": "Harmony L",
        "demo.ch4": "Harmony R",
        "demo.ch5": "Harmony C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "Octave Down",
        "demo.ch9": "Octave Up",
        "demo.ch10": "Harmony L",
        "demo.ch11": "Harmony R",
        "demo.ch12": "FX",
        "demo.ch13": "Spoken",
        "demo.ch14": "Harmony C",
        "demo.ch15": "Outro",
        "demo.versionName": "Base balance", // T27 自译,待人工审校

        // 分组词条组(05 §5.1,623-633 行;J71② 整组采设计稿三语提案表)。
        // 两插件共用的扁平 key 组,取代旧 in.groupSwitchConfirm / out.set.groupSwitchConfirm /
        // in.oneGroupPerTrack / in.groupHint.noOutputInGroup(旧 key 作废,不得并存)。
        "group.label": "Group",
        "group.oneOnly":
            "A vocal track can only belong to one group — a second SCVB Input on the same track (any group) receives silence only",
        "group.noOutput":
            "Group {g} has no Output — insert SCVB Output on the target bus and select group {g}",
        "group.switchConfirm":
            "Switching to group {x} will drop the current connection (group {y} · channel {n})",
        "group.switchConfirm.out":
            "Switching to group {x} will drop all {n} connections in this group",
        "group.switchConfirm.primary": "Switch to {x}",
        "ch.occupied": "Channel {n} is already taken (group {g})",
        "banner.secondOutput":
            "Group {X} already has a primary Output — this instance is read-only",
        "group.oneInstanceNote": "This instance belongs to one group only",

        // [D1] Header undo / redo (contract §1.25/§1.26; self-translated, U17 review pending).
        "header.history": "Undo and redo",
        "header.undo": "Undo",
        "header.redo": "Redo",
        "header.undoEmpty": "Nothing left to undo",
        "header.redoEmpty": "Nothing left to redo",

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "Follow",
        "master.rangeFollowHint":
            "Follows playback, range extends automatically. {n} analyzed segments · {t} total",
        "master.step1.desc":
            "Turn it on and play this range to record loudness features",
        "master.widthAngleHint": "The angle of the outermost vocals. 0–90°",
        "master.msHint": "−100 toward Mid / +100 toward Side",
        "master.distHint":
            "Bar height = level, horizontal position = pan; the line = stereo source width",
        "master.distAxis": "L · −50 · C · +50 · R",

        // T43 / [J75] A+B —— 自译,**待 U17 人工审校**。
        "chart.modeGroup": "Distribution chart view",
        "chart.modeDistribution": "Distribution",
        "chart.modeTrajectory": "Trajectory",
        "chart.trajHint":
            "One line per track = that track's final printed pan; no line where there are no segments",
        "chart.panSideR": "R",
        "chart.panSideC": "C",
        "chart.panSideL": "L",
        "chart.trajCanvasAria":
            "Pan trajectory chart: scroll to move sideways, Ctrl+scroll to zoom horizontally, Shift+scroll to move up and down, Alt+scroll to zoom vertically; drag or arrow keys also work",
        "chart.zoomAria": "Timeline zoom level",
        "chart.backToPlayhead": "Back to playhead",
        "chart.resetPanZoom": "Reset Y zoom",
        "chart.trajEmpty":
            "No segments yet — each track's pan trajectory appears here after analysis",
        "chart.legendAria": "Track colour legend",
        "chart.legendHint":
            "The colour dot matches the track number; hover to highlight that track",

        // T41 —— 自译,**待 U17 人工审校**。
        "suggest.entry": "Suggestions",
        "suggest.title": "Suggestion table",
        "suggest.disclaimer":
            "These are suggested values — SCVB does not apply them in your DAW for you; set them by hand from this table.",
        "suggest.backToLanes": "Back to lanes",
        "suggest.export": "Export CSV",
        "suggest.scope": "{v} · {t} tracks · {n} rows",
        "suggest.empty":
            "No segments yet — the per-track, per-segment suggestions appear here after analysis",
        "suggest.staleNote":
            "{n} of these tracks have stale capture data — recapture them before transferring these values by hand.",
        "suggest.legend":
            "Pan and volume are one suggested value per segment; width is one current setting per track, so it repeats on every row of that track. Width only has a value on stereo tracks and stays blank on mono ones and when it is unknown (0 means collapsed to mono, which is not the same as blank); origin and locked read exactly as stored in the project",
        "suggest.exporting": "Exporting…",
        "suggest.exportOk": "Exported {n} rows → {path}",
        "suggest.exportCancelled": "Export cancelled",
        "suggest.exportFail": "Export failed: {reason}",
        "suggest.exportUnavailable":
            "Export is not wired up in this build yet — the save dialog is pending on the native side",
        "suggest.tableAria":
            "Suggestion table: pan / volume / width suggestions per track and segment",
        "suggest.rowsAria": "Suggestion rows, arrow keys to scroll",
        "suggest.col.trackIndex": "Trk",
        "suggest.col.trackLabel": "Track name",
        "suggest.col.sourceChannels": "Ch",
        "suggest.col.version": "Ver",
        "suggest.col.versionName": "Version name",
        "suggest.col.segmentIndex": "Seg",
        "suggest.col.t0Sec": "Start (s)",
        "suggest.col.t1Sec": "End (s)",
        "suggest.col.pan": "Pan",
        "suggest.col.volDb": "Vol dB",
        "suggest.col.width": "Width %",
        "suggest.col.origin": "Origin",
        "suggest.col.locked": "Locked",

        "master.transitionHint":
            "How fast parameters transition between segments.",
        "master.copyConfirmWarn":
            "Existing data will be overwritten — all 15 tracks' pan/vol, segment results and manual-edit marks of {name} are replaced. Undoable (Ctrl+Z).",
        "tracks.colLegend":
            "Vol = whether this track joins volume balancing (on by default) and the volume control · Pan = whether this track joins pan redistribution (stereo off by default, still volume-balanced) and the pan control · Freeze P/V = still counted by the engine but no longer driven; the knob unlocks to pure manual control (both switches share one per-track automation parameter)",
        "tracks.emptyGroup":
            "Group {g} has no inputs yet — insert SCVB Input in the last slot of each vocal track and select group {g}",
        // ---- T32 Wave 1 新增(EN 为 T32 自译,待人工审校)----
        "tracks.colCh": "CH",
        "tracks.labelPlaceholder": "Track {n}",
        "tracks.colVolPart": "Volume participation",
        "tracks.colState": "State",
        "tracks.colPan": "PAN",
        "tracks.colW": "W",
        "tracks.colVolLevel": "VOLUME",
        "tracks.colPrio": "PRIO",
        "tracks.colLead": "LEAD",
        "tracks.colVolExempt": "Vol",
        "tracks.colAutoPan": "Pan",
        "tracks.colFreezePan": "FRZ P",
        "tracks.colFreezeVol": "FRZ V",
        "tracks.colOn": "ON",
        "tracks.footNote": "15 tracks",
        "tracks.emptyRoute":
            "Keep each vocal track's output routing pointed at this bus",
        "lowSample.full": "Low sample — analysis may be unstable",
        "tracks.panAutoHint": "Auto mode: driven by the analysis curve",
        "tracks.leadCenter": "CTR",
        "tracks.multiLead": "Multiple leads centred",
        "tracks.misaligned": "Misaligned ×{n}",
        "tracks.srErr": "Sample rate mismatch",
        "tracks.labelEdit": "Track name (24 characters max)",
        "tracks.reidentifyOne": "Re-identify track {n}",
        "tracks.reidentifyConfirm":
            "This clears the manual fixed value on track {n} and re-identifies it. Locked segments are left untouched. Continue?",
        "tracks.pairNone": "None",
        "tracks.pairFullSuffix": " (full)",
        "tracks.pairOverflow": "Pair over capacity",
        "common.decrease": "Decrease",
        "common.increase": "Increase",
        "wave.trackPickHint":
            "Tick the lane headers on the left to choose target tracks (multi-select, Shift for a range)",
        "wave.selChip": "The four actions above apply to {n} tracks",
        "wave.setRangeTip":
            "Working range set to selection {x}–{y} — Range switched to Manual; the selection itself is unchanged",
        "wave.originLegend":
            "origin: E = edited, C = created by hand; auto segments carry no badge. Locked segments survive re-identification.",
        "set.loudnessMode.title": "Segment loudness analysis algorithm",
        "set.loudnessMode.note":
            "Sets the reference for segment-to-segment loudness normalization; changing it requires re-analysis.",
        "set.centerSlot.title":
            "Priority when tracks compete for the center slot",
        "set.centerSlot.note":
            "Fallback rule beyond Lead Lock and Lead Select; it does not affect Vol Exempt.",
        "in.chHint.groupEmpty":
            "This group has no Output yet — the channel table is empty",

        // ---- T35 新增(EN 自译,待人工审校)----
        "set.usage.eyebrow": "USAGE",
        "set.usage.workflow":
            "Capture → Analyze → Output. Turn on capture and play; the plug-in records each track's loudness features. Analysis computes a pan/level plan for every phrase offline. Switch Output to ENGINE DRIVE and the plug-in drives the parameters; enable Latch or Write mode in your DAW to record them as an automation track.",
        "set.usage.docs": "Docs",

        // ---- [J78] 手动接管与自动化说明块(EN 自译,待人工审校;口径见 zh 侧同组注释)----
        "set.automationGuide.eyebrow": "MANUAL / AUTOMATION",
        "set.automationGuide.title":
            "Manual takeover and automation — who has the final say?",
        "set.automationGuide.line1":
            "Priority, highest to lowest: host automation > frozen manual value > manual tweak > engine analysis curve.",
        "set.automationGuide.line2":
            "Freezing PAN or VOL means you take that dimension over by hand; on write the plug-in prints a flat line into the automation lane, and you are then free to draw it yourself in the DAW.",
        "set.automationGuide.line3":
            "A manual tweak in the segment inspector edits the engine curve itself and goes out with the write (unless that dimension is already frozen).",
        "set.automationGuide.line4":
            "Once the automation is drawn, the plug-in follows the host faithfully and the engine no longer overrides it; only another write overwrites it again.",

        "set.loudnessMode.eyebrow": "SECOND LOUDNESS METRIC",
        "set.loudnessMode.opt.kw_integrated": "K-weighted integral",
        "set.loudnessMode.opt.rms": "RMS",
        "set.loudnessMode.opt.peak_dbfs": "Peak dBFS",
        "set.centerSlot.eyebrow": "CENTER SLOT POLICY",
        "set.centerSlot.opt.priority_queue": "Priority queue",
        "set.centerSlot.opt.lead_exclusive": "Lead exclusive",
        "set.centerSlot.opt.even_spread": "Even spread",
        "set.guide.showAll": "Show all nine",
        "set.guide.collapse": "Collapse",
        "set.guide.rulesMissing":
            "The full nine rules will be included in the release build",
        "set.storage.eyebrow": "STORAGE",
        "set.storage.embedded": "Embedded in project ({mb} MB)",
        "set.storage.external": "Saved as an external file (>8 MB automatic)",
        "set.storage.sessionGuid": "session {guid}",
        "set.diag.eyebrow": "DIAGNOSTICS",
        "set.diag.copy": "Copy diagnostics",
        "set.diag.copied": "Copied",
        "set.diag.colCh": "CH",
        "set.diag.colHb": "HB",
        "set.diag.colMis": "MIS",
        "set.diag.colGen": "GEN",
        "set.diag.colSeq": "SEQ",
        "set.reanalyze": "Re-analysis required",
        // ---- T36 新增(Input 单页正式实现;05 §3 语义,词条真源 05 §5/§3)。
        // EN/FR 为 T36 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "in.pillSub.passthrough": "Passthrough: no processing on this track",
        "in.pillSub.takenOver":
            "Taken over: this track is muted, and audio is output by the bus inserted into Output",
        "in.pillSub.hysteresis":
            "Connection unstable — switching to passthrough",
        "in.pillSub.stereo":
            "This track is a stereo source — SCVB preserves its width",
        "in.source.mono": "This track is a mono source",
        "in.group.note":
            "Must share a group with Output to connect; keep A for single-group use",
        "in.channels.emptyHint":
            "Choose this track's channel number (matches the Output Tracks page)",
        "in.unassigned": "Unassigned",
        "in.releaseConfirm.primary": "Release",
        "in.channels.manual.confirm": "Confirm",
        "in.priority.note": "This setting is stored on Output",
        "in.priority.offline": "Requires Output online",
        "in.footer.hint": "Ready",
        "in.footer.noBackend": "No backend attached — open via web-preview",
        "in.priority.unassigned": "Select a channel first",
        "ch.occupied.group": "A channel is already taken (group {g})",

        // T31 新增(Output 外壳 + Tab1 正式实现,05 §2.0/§2.1 语义 + design-v2 定稿文案)。
        // 立项理由与逐条出处见 scratchpad/t31/deviations.md「新增词条」节;
        // EN/FR 为 T31 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "common.cancel": "Cancel",
        "tab.master": "Master",
        "tab.tracks": "Tracks",
        "tab.wave": "Waveform & Segments",
        "tab.settings": "Settings",
        "version.emptyBadge": "EMPTY",
        "master.copyConfirmPrimary": "Overwrite & copy",
        "master.captureOff": "CAPTURE OFF",
        "master.step2.desc":
            "Affects {n} intervals / {m} tracks; {k} manual edits will be kept",
        "master.step2.desc.noData":
            "No captured data in the current range — adjust the range or capture first",
        "master.step2.coverage":
            "{p}% of the range is covered; uncovered parts stay as they are",
        "master.analyzing": "Analyzing…",
        "master.analyzeDone": "Analysis done",
        "master.step3.desc": "Enable automation recording in your DAW",
        "master.step3.descStrong":
            "If available, use Latch (recommended) or Write mode; never use Touch mode",
        "master.writeConfirm.ok": "Got it, start",
        "master.writeConfirm.undo": "Undo (back to Follow Host)",
        "master.groupEyebrow": "GROUP",
        "master.widthEyebrow": "WIDTH · MAX ANGLE",
        "master.rangeEyebrow": "RANGE",
        "master.distEyebrow": "PAN / VOLUME DISTRIBUTION",
        "master.curveEyebrow": "ANGLE-BASED LEVEL ADJUST · ±12 dB",
        "master.curveLegendMs": "MS equivalent gain",
        "master.curveAxisX":
            "Left L −100 · −50 · Center C 0 · +50 · Right R +100",
        "master.curveEmptyHint": "Double-click to add a control point",
        // ---- T34 曲线编辑器(curve.*;shape/side 术语逐字对拍 05 §5 术语表)----
        "curve.canvasLabel": "Angle-domain gain curve editor",
        "curve.maxPoints": "Maximum of 16 points reached",
        "curve.centerSide": "Center point — choose a direction",
        "curve.shape.bell": "Bell",
        "curve.shape.shelf": "Shelf",
        "curve.shape.cut": "Cut",
        "curve.side.out": "Out",
        "curve.side.left": "Left",
        "curve.side.right": "Right",
        "curve.qLabel": "Q",
        "curve.slopeLabel": "Slope",
        "curve.slope.opt6": "6 dB/oct",
        "curve.slope.opt12": "12 dB/oct",
        "curve.slope.opt18": "18 dB/oct",
        "curve.slope.opt24": "24 dB/oct",
        "curve.sideTooltip":
            "Out: cuts the outer side away from center, bounded by this point. Left/Right: cuts only that side",
        "curve.deleteLabel": "Delete point",
        "curve.announcePoint":
            "Point {n}: angle {angle}, {gain} dB, {shape}, Q {q}",
        "curve.announcePointDir":
            "Point {n}: angle {angle}, {gain} dB, {shape}, Q {q} · direction {side}",
        "master.leadSelectDefaultNote": "0 = auto-select (default)",
        "range.manual": "Manual",
        "master.rangeLoopStale":
            "Loop region is no longer valid — keeping the previous range",
        "master.rangeLoopMissing":
            "Host provides no loop region — the mode stays listed but cannot be selected",
        "master.rangeStart": "Range start",
        "master.rangeEnd": "Range end",
        "master.setToPlayhead": "Set to playhead",
        "master.barsEstimateNote":
            "Bar numbers are estimates; they calibrate after playing this area",
        "footer.defaultHint":
            "Capture → Analyze → Output: turn on the capture switch in Tab 1 and play this range",
        "scale.current": "Current",
        "scale.overflow": "Exceeds current screen",

        // ---- T31 Wave 2 新增(EN 自译,待人工审校)----
        "banner.misaligned":
            "Route mismatch: timeline gaps detected on {m} track(s) — check that the vocal tracks still route to this bus",
        "banner.srMismatch": "Track {n} sample rate mismatch — disabled",
        "banner.sidecarMissing":
            "Capture data missing or outdated — please re-capture",
        "banner.noTimeline": "Host provides no timeline",
        "master.versionArmedConfirm":
            "Engine output will fade smoothly to the new version. Continue?",
        "master.empty.step1":
            "Insert SCVB Input in the last slot of each vocal track's plugin chain (after all processing)",
        "master.empty.step2":
            "Keep each vocal track's output routed to the shared bus",
        "master.empty.step3":
            "Insert SCVB Output in the first slot of that bus",
        "master.empty.step4": "Capture and play",
        "master.empty.step5": "Analyze and switch output on",
        "common.continue": "Continue",
        // eyebrow:三语同值(视觉层 mono 大写元素,照设计稿不译)
        "master.msEyebrow": "MS BALANCE",
        "master.leadEyebrow": "LEAD SELECT",
        "master.transitionEyebrow": "TRANSITION TIME",

        // ---- T31 Wave 2 评审修订新增(EN 自译,待人工审校)----
        "toast.projectCopy":
            "Project copy detected — a separate capture data copy was created",
        "toast.sidecarSwitched":
            "Capture data was moved to an external file (>8 MB); see Settings (path management and import/export are future work)",
        "scale.confirmBody": "Scale applied, reverting in {s} s",
        "scale.keep": "Save",
        "master.printLock.group": "Can't switch group while writing automation",
        "master.printLock.version":
            "Can't switch version while writing automation",
        "master.printLock.copy": "Can't copy while writing automation",

        // ---- T33 Wave 1 新增(EN 为 T33 自译,待 U17 人工审校;
        // 7 滑杆短标 / ORIGIN 为 mono 微标,三语同值)----
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        "wave.sldHangover": "HOLD",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
        "wave.tipThreshold":
            "Threshold: louder than this is recognized, quieter is ignored (default {d})",
        "wave.tipHysteresis":
            "Hysteresis: opens at the threshold but only closes this far below it — stops level jitter near the threshold from shredding a phrase (default {d})",
        "wave.tipHold":
            "Hold (VAD hangover): how long to keep treating the signal as voiced after it drops below the threshold, so short pauses between words aren't split (default {d})",
        "wave.tipPadPre":
            "Pre-pad: padding before the detected region, to keep the attack and breath (default {d})",
        "wave.tipPadPost":
            "Post-pad: padding after the detected region, to keep the tail (default {d})",
        "wave.tipSensitivity":
            "Segmentation sensitivity: higher splits more readily at energy valleys. Only applies to segments longer than 8 s, so short phrases will not change (default {d})",
        "wave.tipMinSeg":
            "Minimum segment length: shorter segments are discarded (judged before padding) to filter noise, but it also drops brief ad-libs and single-note harmonies (default {d})",
        "wave.emptyMain":
            "No captured data yet — turn on the capture switch and play",
        "wave.emptyCta": "Open capture in Tab 1",
        "wave.followHostNote":
            "Following host: edits are saved and take effect once output is on",
        "wave.btnRecapture": "Re-capture selection",
        "wave.btnReanalyze": "Re-analyze selection",
        "wave.btnClearCoverage": "Clear selection capture data",
        "wave.applyCountdown": "Applying in 300 ms…",
        "wave.applying": "Applying…",
        "wave.recaptureInlineNote":
            "This recapture applies only to the selection and does not affect the output engine's range",
        "wave.inspectorTitle": "Segment details & editing",
        "wave.inspectorToggle": "Segment details & editing",
        "wave.inspectorClose": "Collapse",
        "wave.inspectorEmpty": "Click a segment in a lane to edit it",
        "wave.segStart": "Start",
        "wave.segEnd": "End",
        "wave.segLen": "Length",
        "wave.segLoudness": "K-weighted segment integral",
        "wave.volField": "VOL",
        "wave.lockSegment": "Lock this segment",
        "wave.lockBadge": "LOCKED",
        "wave.covSeg": "{p}% · {n} seg",
        "wave.curveVisible": "Curves visible",
        "wave.pickTrack": "Select track {n}",
        "wave.boundaryHandleTip":
            "Segment boundary: drag to move it (snaps to energy valleys, hold Alt to disable); double-click inside a segment to split there; select two adjacent segments and press Delete to merge",
        "wave.boundarySnapTip":
            "Snapped to an energy valley (hold Alt to disable)",
        "wave.hZoomBar":
            "Horizontal zoom (drag to change the viewport span, arrow keys to step)",
        "wave.vZoomBar":
            "Vertical zoom (drag to change the lane height, arrow keys to step)",
        "wave.reidentifyConfirm":
            "This clears {k} manual edit marks and recomputes; {l} locked segments stay unchanged. Continue?",
        "wave.clearCoverageConfirm":
            "This deletes the captured feature data for the selected tracks × selection. Continue?",
        // ---- T33 Wave 2 interaction feedback (05 §2.3 / contract §5.5; U17 review pending) ----
        "wave.armReason.noTracks":
            "No target tracks — tick the track heads on the left",
        "wave.armReason.noSelection":
            "No valid selection — drag one on the time ruler",
        "wave.armReason.readOnly":
            "Read-only observer — writing did not take effect",
        "wave.armReason.noTimeline": "Host provides no timeline — cannot write",
        "wave.notAdjacent": "Only two adjacent segments can be merged",
        "wave.btnMerge": "Merge selected pair",
        "wave.recaptureOverlap": "Will overwrite {k} segments of existing data",
        "wave.diffItem": "Track {ch} · seg {i}: pan {pf}→{pt} · vol {vf}→{vt}",
        "wave.diffAddedRemoved": "{a} added · {r} removed",
        "wave.clearedCoverage": "Cleared {s} s of captured data",
    },

    fr: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "NON CONNECTÉ",
        "state.staleLink":
            "Lien anormal (heartbeat obsolète : toujours en ligne mais plus de mises à jour)",
        "state.connected": "CONNECTÉ",
        "state.noChannel": "AUCUN CANAL",
        "state.outputOffline": "OUTPUT HORS LIGNE",
        "state.waitingForOutput": "EN ATTENTE D'OUTPUT",
        "state.passthrough": "DIRECT",
        "state.takenOver": "PRIS EN CHARGE",
        "state.waitingForOutput.group": "EN ATTENTE D'OUTPUT · GROUPE {X}",
        "state.groupSuffix": " · GROUPE {X}",

        // 术语表(05 §5,530-586 行):三语建议,写入字典前用户可改;EN 状态词沿 Bridge 全大写惯例。
        // 三词分工(R1 定案,不得混用):range=范围(作用范围,Tab1)、interval=区段(分析产物)、selection=选区(Tab3 操作目标)。
        pan: "Panoramique",
        width: "Largeur",
        track: "Piste",
        channel: "Canal",
        group: "Groupement",
        segment: "Segment",
        capture: "Capture",
        tabs: "Onglets",
        analyze: "Analyser",
        output: "Sortie",
        followHost: "SUIVI HÔTE",
        engineDrive: "PILOTAGE MOTEUR",
        version: "Version",
        priority: "Priorité",
        leadLock: "Verrou lead",
        leadSelect: "Sélection lead",
        leadFollowAnalysis: "Sélection auto",
        msBalance: "Balance M/S",
        stereoBadge: "ST",
        participateAutoPan: "Participation pan auto",
        trackWidth: "Largeur de piste",
        "tracks.monoWidthNoop":
            "Une source mono ne peut pas ajuster la largeur",
        "master.leadSelectHint":
            "Cette piste est définie comme Lead et forcée au centre",
        pair: "Paire",
        threshold: "Seuil",
        sensitivity: "Sensibilité",
        range: "Plage",
        follow: "Suivi",
        selection: "Sélection",
        interval: "Intervalle",
        loopRegion: "Boucle",
        gainCurve: "Courbe de gain",
        bell: "Cloche",
        shelf: "Plateau",
        cut: "Coupe",
        sideOut: "Extérieur",
        sideLeft: "Gauche",
        sideRight: "Droite",
        paddingPre: "Marge avant",
        paddingPost: "Marge après",
        transition: "Transition",
        misaligned: "ROUTAGE DÉSALIGNÉ",
        capturing: "CAPTURE EN COURS",
        stale: "OBSOLÈTE",
        manualMark: "Modifié",
        locked: "Verrouillé",
        copyTo: "Copier vers",
        renameVersion: "Renommer",
        // 术语表裸词条,只用于术语解释/图例;音频路径状态走 state.*(见 zh 同处注释)
        passthrough: "DIRECT",
        takenOver: "PRIS EN CHARGE",
        printing: "IMPRESSION",
        lowSample: "ÉCHANTILLON INSUFFISANT",
        // 同 EN:整句但压短,免得动作行换行(FR 最长态是工具条的宽度基准)
        autoStop: "Arrêter en fin de lecture",
        reidentify: "Ré-identifier (incl. modifiés)",
        applyToSegments: "Appliquer",
        setAsRange: "Définir comme plage",
        clearCapture: "Effacer la capture",
        scale: "Échelle",
        language: "Langue",
        armedWaiting: "PRÊT · EN ATTENTE",
        outOfRange: "HORS PLAGE",
        occupied: "OCCUPÉ",

        // 横幅、写入确认条、页脚与错误态长句(05 §5,587-605 行):占位符 {a}{b}{v}{x}{y}{n}{t}{k}{l} 原样保留。
        "banner.versionMismatch":
            "Enregistré par une version plus récente de SCVB. Veuillez mettre à jour le plug-in (abi local {a} / abi du projet {b}).",
        "banner.abiMismatch":
            "Versions SCVB incompatibles — connexion refusée (abi Output {a} / abi Input {b}). Mettez les deux plug-ins à la même version.",
        "banner.printGuard":
            "La sortie est en PILOTAGE MOTEUR (restauré avec le projet)",
        "banner.printGuard.confirm": "Continuer le pilotage moteur",
        "out.master.writeConfirm":
            "Pilotage moteur {v} · plage {x}–{y} · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture de cette plage écrasera l'automation existante ; sinon, écoute seule.",
        "footer.printing": "PILOTAGE MOTEUR {v} · {x}–{y}",
        "footer.printDone":
            "Cette passe a couvert {x}–{y}. Si vous enregistriez l'automation, repassez en Suivi hôte pour vérifier.",
        "out.master.writeConfirm.follow":
            "Pilotage moteur {v} · plage = toutes les zones analysées (suivi, {n} segments · total {t}) · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture des zones analysées écrasera l'automation existante ; sinon, écoute seule.",
        "footer.printing.follow":
            "PILOTAGE MOTEUR {v} · SUIVI (ZONES ANALYSÉES)",
        "footer.printDone.follow":
            "Cette passe a couvert les zones analysées. Si vous enregistriez l'automation, repassez en Suivi hôte pour vérifier.",
        "wave.diffKept": "{k} segments modifiés/verrouillés préservés",
        "tracks.manualOverwriteConfirm":
            "Remplace tous les segments analysés de cette piste (version actuelle) par une valeur fixe. Annulable.",
        "tracks.manualOverwriteConfirm.locked":
            "(dont {l} segments verrouillés)",
        "in.pill.abiMismatch": "VERSION INCOMPATIBLE",
        "in.pill.srMismatch": "FRÉQ. INCOHÉRENTE",
        "tracks.manualDrivenHint":
            "Piste encore pilotée par une valeur fixe manuelle — la ré-identifier ?",
        "in.releaseConfirm":
            "Libérer le canal {n} — plus aucun traitement sur cette piste",
        "wave.recaptureArmed": "RÉ-CAPTURE PRÊTE · {x}–{y} · {n} PISTES",
        "wave.recaptureArmedShort": "RÉ-CAPTURE PRÊTE",
        "footer.recaptureOutputWarn":
            "Le moteur pilote toujours la plage globale — indépendamment de la sélection de ré-capture",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.title 与 guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从
        // docs/USER_GUIDE.zh-CN.md#硬约束 + docs/hard-rules.i18n.json 生成写入(禁止手抄)。
        // fr 红字发布前必须经人工审校(05 §5),审校状态见 docs/hard-rules.i18n.json 的 frReview。
        // BEGIN GENERATED hard-rules:fr
        "guide.title":
            "À lire : les neuf règles d'utilisation de SCVB. En enfreindre une seule entraîne silence, panoramique erroné ou analyse échouée.",
        "guide.rule1":
            "Les pistes de voix doivent conserver leur routage DAW d'origine, vers le bus qui héberge SCVB Output. Ne redirigez pas une piste de voix directement vers la sortie principale et ne contournez pas le bus. (ADR-002)",
        "guide.rule2":
            "SCVB Input doit occuper la dernière case de la chaîne d'effets de la piste de voix ; SCVB Output doit occuper la première case du bus. Toute autre position casse l'hypothèse d'ordre de traitement sur laquelle SCVB repose ; pour le nom de cette case dans chaque hôte, voir docs/DAW_COMPATIBILITY.md. (ADR-002 / J45)",
        "guide.rule3":
            "Input ne coupe sa sortie aval que tant qu'un SCVB Output sain est détecté — c'est le comportement voulu, pas un bug. Ce chemin de coupure préserve l'ordre « pistes de voix d'abord, bus ensuite » dans le graphe de dépendances du DAW, et il reste valable en rendu hors ligne comme sous le multithreading anticipatif de REAPER. Si aucun Output sain n'est détecté (non installé, non connecté, pair quitté), Input repasse automatiquement en direct, via une rampe de 80 ms avec anti-rebond à hystérésis de 5 secondes (l'hystérésis ne s'applique qu'au sens « coupure → direct » ; « direct → coupure » suit une rampe de 80 ms dès la santé confirmée) : n'installer qu'un seul des deux plugins ne vous laissera donc jamais une piste muette. (ADR-002 / J12 + J32)",
        "guide.rule4":
            "Le panoramique hôte doit rester centré, sur les pistes de voix comme sur le bus. SCVB applique en interne un panoramique à puissance constante, indépendant de la loi de panoramique de l'hôte ; un panoramique hôte décentré s'y ajoute et produit une image stéréo erronée. (ADR-010)",
        "guide.rule5":
            "Chaque identifiant de canal est unique au sein d'un groupe, et une même piste de voix ne peut appartenir qu'à un seul groupe. Lorsque deux Input du même groupe réclament le même canal, le second affiche un avertissement « conflit de canal » et reste inactif ; le même numéro de canal dans un autre groupe correspond à un chemin distinct et sans rapport. (ADR-002 / J66)",
        "guide.rule6":
            "Un seul Output peut être actif à la fois au sein d'un groupe. Une seconde instance du même groupe passe en mode observation en lecture seule et affiche un avertissement ; les huit groupes (A–H) sont des domaines de bus indépendants, sans influence mutuelle. (ADR-002 / J66)",
        "guide.rule7":
            "Les pistes de voix stéréo sont exclues par défaut de la répartition automatique du panoramique ; activez-les manuellement pour les y inclure. Les sources mono sont placées par panoramique à puissance constante ; les sources stéréo suivent un modèle dual-pan + largeur (pan = centre de l'arc, largeur = ouverture) qui conserve par défaut la largeur stéréo existante au lieu de la laisser écraser par la répartition automatique. (ADR-003 / J57 + J60)",
        "guide.rule8":
            "SCVB Output ne déclare aucune latence supplémentaire au DAW. L'alignement repose sur l'adressage temporel ; n'essayez pas de le « corriger » avec la PDC (compensation du retard des plugins). (ADR-002)",
        "guide.rule9":
            "Ne poursuivez pas l'export tant qu'un avertissement « trou / chevauchement de timeline » est affiché. Vérifiez d'abord votre routage à l'aide de la liste des pièges courants de docs/DAW_COMPATIBILITY.md : tant que le compteur d'avertissements ne retombe pas à zéro, l'audio d'une piste n'est pas correctement pris en charge.",
        // END GENERATED hard-rules:fr
        "guide.dontShowAgain": "Ne plus afficher",
        "guide.start": "Commencer",
        "set.reopenGuide": "Revoir le guide",
        "set.viewWorkflow": "Voir le workflow",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "Envie d'une visite guidée d'une minute ?",
        "tour.ask.start": "Commencer la visite",
        "tour.ask.later": "Pas maintenant — à revoir dans Réglages",
        "tour.clickAnywhere": "Clic gauche n'importe où pour continuer",
        "tour.skip": "Passer",
        "tour.prev": "Précédent",
        "tour.next": "Suivant",
        "tour.done": "Terminé",
        "tour.demoBadge": "DONNÉES DÉMO",
        // 首启语言选择卡(T36b 第四轮:独立 overlay,先于红字九条页;三语常显,各按钮用各自语言)。
        "lang-start.title":
            "请选择语言 / Choose your language / Choisissez votre langue",
        "lang-start.zh": "中文",
        "lang-start.en": "English",
        "lang-start.fr": "Français",

        // Guide léger de premier lancement côté Input ([J80] / T48). EN/FR 自译,入 U17 审校清单。
        "tour-in.help": "Revoir le guide",
        "tour-in.step1.title": "Bienvenue dans SCVB Input",
        "tour-in.step1.body":
            "Voici l’extrémité de capture de SCVB : une instance dans le dernier emplacement de chaque piste de voix, qui alimente le SCVB Output placé sur le bus. Prenez vingt secondes pour faire connaissance.",
        "tour-in.step2.title": "Groupe A–H",
        "tour-in.step2.body":
            "Un groupe est un espace de travail autonome. Cette piste doit se trouver dans le même groupe que le SCVB Output qui la prend en charge ; avec une seule configuration, laissez simplement A.",
        "tour-in.step3.title": "Numéro de canal",
        "tour-in.step3.body":
            "Chaque piste de voix d’un groupe occupe un numéro (01–15), unique au sein de ce groupe. Cliquez sur une carte pour la revendiquer ; une carte marquée « occupé » est déjà tenue par une autre piste du groupe.",
        "tour-in.step4.title": "État de la connexion",
        "tour-in.step4.body":
            "Une fois connectée, cette piste envoie du silence en aval et le SCVB Output du bus prend le relais — c’est le comportement voulu, pas un bug. Si aucun Output sain n’est détecté (non installé, non connecté, pair quitté), cette piste repasse automatiquement en direct : n’installer qu’un seul des deux plugins ne vous laissera donc jamais une piste sans aucun son.",
        "tour-in.step5.title": "Le contrôle complet est dans Output",
        "tour-in.step5.body":
            "L’analyse, l’équilibrage et l’écriture d’automation se trouvent tous dans le SCVB Output du bus ; cette page ne sert qu’à revendiquer un canal et à afficher l’état. Pour revoir ce guide, cliquez sur ce « ? » à tout moment.",

        // tour complet 44 étapes (copie finale ; FR auto-traduite, à relire).
        "tour.step1.title": "Bienvenue dans SCVB",
        "tour.step1.body":
            "SCVB est l'outil open-source de Synchain pour équilibrer les voix multipistes. Prenez une minute pour découvrir l'interface. Un jeu de démonstration de 15 pistes est chargé, puis retiré à la fin.",
        "tour.step4.title": "Voici la fenêtre principale de SCVB Output",
        "tour.step4.body":
            "Quatre onglets en haut : Général / Pistes / Formes d'onde / Réglages. Cette visite vous guide page par page.",
        "tour.step8.title": "Les groupes et leur rôle",
        "tour.step8.body":
            "Les groupes répartissent les canaux en huit espaces de travail indépendants (A–H) ; chaque groupe gère sa propre capture, son analyse et sa sortie. Pour un usage avec un seul groupe, gardez A.",
        "tour.step5.title": "Étape 1 — Capture",
        "tour.step5.body":
            "Activez l'interrupteur de capture et lisez la plage actuelle ; le plug-in enregistre les caractéristiques de loudness de chaque piste. Rien n'est écrit dans l'automation à cette étape.",
        "tour.step6.title": "Étape 2 — Analyse",
        "tour.step6.body":
            "Calcule hors ligne un plan pan/volume pour chaque phrase. Prévisualisez la plage de capture avant de valider ; les segments modifiés ou verrouillés sont préservés.",
        "tour.step7.title": "Étape 3 — Sortie",
        "tour.step7.body":
            "Passez en PILOTAGE MOTEUR et les paramètres sont pilotés par le résultat de l'analyse. Pour les enregistrer en automation, armez Latch ou Write côté DAW.",
        "tour.step12.title": "Répartition pan / volume",
        "tour.step12.body":
            "L'état temps réel du pan et du volume de toutes les pistes : hauteur = volume, position horizontale = pan ; la fine ligne horizontale = l'ouverture de la source stéréo et la courbe angulaire.",
        "tour.step9.title": "Width · angle max",
        "tour.step9.body":
            "La limite de largeur globale : angle d'ouverture max 0–90°.",
        "tour.step10.title": "MS Balance",
        "tour.step10.body":
            "Balance M/S du bus : −100 vers Mid, +100 vers Side ; ajustez le rapport de volume M/S.",
        "tour.step11.title": "Lead Select",
        "tour.step11.body":
            "Choisissez une piste par numéro et forcez-la au centre en temps réel ; 0 = suivre l'analyse.",
        "tour.step14.title": "Plage",
        "tour.step14.body":
            "Trois modes : Suivi / Boucle / Manuel ; définit la portée de la capture et de la sortie.",
        "tour.step13.title": "Transition",
        "tour.step13.body":
            "Transition inter-segments : 20–300 ms ; définit la vitesse de rampe du volume et du pan entre les intervalles.",
        "tour.step15.title": "Courbe du domaine angulaire",
        "tour.step15.body":
            "Chaque courbe de panoramique est constituée de points de contrôle : double-cliquez n'importe où pour ajouter un point, faites glisser pour ajuster l'angle et le gain, et double-cliquez pour supprimer. Chaque point peut être cloche / plateau / coupe (pente 6–24 dB/oct), jusqu'à 16 points.",
        "tour.step16.title": "Cette page : Pistes",
        "tour.step16.body":
            "Une ligne par canal, une matrice de 15 pistes ; les commandes de chaque ligne règlent la piste et ne se déverrouillent en manuel qu'une fois gelées.",
        "tour.step17.title": "Ligne de piste",
        "tour.step17.body":
            "Voyant / CH+ST / libellé ; une ligne par piste, estompée si non connectée.",
        "tour.step18.title": "Potentiomètre pan",
        "tour.step18.body":
            "Pan −100..+100 ; déverrouillé en manuel une fois gelé, piloté par la courbe d'analyse sinon.",
        "tour.step19.title": "Potentiomètre width",
        "tour.step19.body":
            "Largeur de piste stéréo 0–100 % ; sans effet sur les pistes mono.",
        "tour.step20.title": "Bloc niveau / volume",
        "tour.step20.body":
            "Le liquide montre le niveau temps réel, le collier est le fader de volume ; glissez le collier pour changer le volume, déverrouillé en manuel une fois gelé.",
        "tour.step21.title": "Priorité",
        "tour.step21.body":
            "0..10, 10 = la plus haute ; les pistes de priorité plus élevée se placent plus près du centre.",
        "tour.step22.title": "Verrou lead",
        "tour.step22.body":
            "Marquez cette piste comme lead : elle devient la piste de priorité maximale à l'analyse, et la stratégie de créneau central (exclusivité lead) la centre d'abord ; avertit si plusieurs sont verrouillées.",
        "tour.step24.title": "Exemption volume",
        "tour.step24.body":
            "Interrupteur indépendant : la piste est exclue de l'équilibrage du volume ; non lié au verrou lead / Lead Select.",
        "tour.step25.title": "Participation pan auto",
        "tour.step25.body":
            "Si la piste entre dans la redistribution du pan ; stéréo désactivé par défaut, équilibrage conservé.",
        "tour.step23.title": "Paire",
        "tour.step23.body":
            "Appairez deux pistes ; leur panoramique est lié et la paire se déplace comme un tout. Même paire = même point coloré en tête de ligne.",
        "tour.step26.title": "Gel PAN / VOL",
        "tour.step26.body":
            "Les deux interrupteurs de gel contournent le moteur pour cette dimension ; le potentiomètre et le fader se déverrouillent en pur manuel.",
        "tour.step27.title": "Activer",
        "tour.step27.body":
            "Désactivez-le : la piste saute capture / analyse / sortie — aucun ajustement n'est appliqué à cette piste.",
        "tour.step28.title": "Cette page : Formes d'onde",
        "tour.step28.body":
            "Vérifiez les résultats et ajustez les segments ; courbes des pistes en haut, barre d'outils en bas ; glisser, sélectionner par rectangle, zoomer se font ici.",
        "tour.step34.title": "Panneau inférieur et actions",
        "tour.step34.body":
            "Tracez une sélection sur les pistes ; glissez les limites pour ajuster les segments ; molette ou barre de zoom pour zoomer/défiler ; double-clic pour diviser, sélectionnez deux segments adjacents pour fusionner.",
        "tour.step32.title": "Zone de pistes",
        "tour.step32.body":
            "Les pistes sont agrandies pour vous : chaque piste superpose les courbes en escalier pan/vol, la tête de lecture suit le transport ; rangée du haut = pan, du bas = vol.",
        "tour.step33.title": "Poignées de sélection",
        "tour.step33.body":
            "Une sélection d'exemple a été créée pour vous : les deux extrémités sont les poignées — faites-les glisser pour changer la plage ; « Définir comme plage » écrit la sélection dans la plage.",
        "tour.step31.title": "Barre post-sélection",
        "tour.step31.body":
            "Re-capturer / ré-analyser / ré-identifier / effacer, appliqués aux pistes sélectionnées × la sélection.",
        "tour.step35.title": "Détails et édition du segment",
        "tour.step35.body":
            "Un segment a été sélectionné pour vous : dans l'inspecteur, modifiez son pan/vol, consultez son origine (E/C) et verrouillez-le.",
        "tour.step30.title": "Barre de segmentation",
        "tour.step30.body":
            "Curseurs SENSITIVITY / MIN SEG ; glissez les limites, double-clic pour diviser, sélectionnez deux segments adjacents et appuyez sur Suppr pour fusionner.",
        "tour.step29.title": "Curseurs VAD (détection vocale)",
        "tour.step29.body":
            "Ces cinq curseurs décident ce qui compte comme voix ou silence — la base de la segmentation : THRESHOLD est le seuil de niveau — seul ce qui est plus fort compte comme chant ; HYSTERESIS ferme la porte un cran plus bas qu'elle ne l'ouvre, pour éviter que le flottement près du seuil ne hache les phrases ; HOLD garde les pauses courtes comme chant, pour ne pas couper entre les mots ; PAD PRE / PAD POST ajoutent une marge avant/après — pour ne pas tronquer l'attaque ni la chute.",
        "tour.step36.title": "Cette page : Réglages",
        "tour.step36.body":
            "Mode d'emploi, indicateur de loudness, stratégie de créneau central, échelle, langue, stockage, diagnostic ; le bouton « Revoir le guide » de la dernière étape se trouve aussi ici.",
        "tour.step2.title": "Flux de travail et priorité",
        "tour.step2.body":
            "Un flux complet : capture → analyse → ajuster/geler → écrire l'automation → ajuster l'automation manuellement. Priorité : automation de l'hôte > valeur manuelle gelée > ajustement manuel > courbe d'analyse du moteur. Vous pouvez le revoir à tout moment via le bouton « Voir le workflow » dans Réglages.",
        "tour.step37.title": "Mode d'emploi",
        "tour.step37.body":
            "Flux en trois étapes + neuf notes importantes ; lisez-les une par une. En enfreindre une seule cause silence ou panoramique erroné.",
        "tour.step38.title": "Second indicateur de loudness",
        "tour.step38.body":
            "Quelle mesure pour le loudness de segment : intégrale pondérée K (proche de l'oreille) / RMS (énergie moyenne) / crête dBFS (crête instantanée) ; ré-analyser après changement.",
        "tour.step39.title": "Stratégie de créneau central",
        "tour.step39.body":
            "Que faire quand plusieurs pistes se disputent le centre : file de priorité / exclusivité lead / répartition égale.",
        "tour.step40.title": "Échelle",
        "tour.step40.body":
            "Met à l'échelle toute l'UI ; synchronisée avec l'échelle de l'en-tête.",
        "tour.step41.title": "Langue",
        "tour.step41.body":
            "Basculez instantanément entre 中文 / English / Français.",
        "tour.step42.title": "État du stockage",
        "tour.step42.body":
            "Les caractéristiques peuvent être intégrées au projet ou stockées à l'extérieur ; la taille utilisée s'affiche ici.",
        "tour.step43.title": "Diagnostic",
        "tour.step43.body":
            "Copiez le heartbeat et les compteurs de désalignement par piste pour diagnostiquer les connexions.",
        "tour.step3.title": "Versions et copie",
        "tour.step3.body":
            "En haut à droite se trouve la zone des versions : deux versions V1 / V2 commutables à tout moment ; le bouton Copier copie la version actuelle vers l'autre. Le numéro de version du plug-in s'affiche en bas à droite — envoyez-le avec les diagnostics si vous demandez de l'aide.",
        "tour.step44.title": "Vous pourrez revoir cette visite ici",
        "tour.step44.body":
            "La visite peut être rejouée à tout moment, et les neuf règles strictes s'ouvrent dans le même bloc.",

        // Diagramme de flux (étape 35 ; nœuds + ligne de priorité).
        "workflow.capture": "Capture",
        "workflow.analyze": "Analyser",
        "workflow.tweak": "Ajuster / Geler",
        "workflow.write": "Écrire l'automation",
        "workflow.manual": "Ajuster l'automation manuellement",
        "workflow.priority":
            "Priorité : automation de l'hôte > valeur manuelle gelée > ajustement manuel > courbe d'analyse du moteur",

        // tour demo 的 15 条轨名(J62;口径见 zh 侧同组注释)。FR 为 T27 自译,待人工审校;
        // L/R/C 照 master.distAxis 的 fr 侧取 G/D/C。
        "demo.ch1": "Voix principale 1",
        "demo.ch2": "Voix principale 2",
        "demo.ch3": "Harmonie G",
        "demo.ch4": "Harmonie D",
        "demo.ch5": "Harmonie C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "Octave inférieure",
        "demo.ch9": "Octave supérieure",
        "demo.ch10": "Harmonie G",
        "demo.ch11": "Harmonie D",
        "demo.ch12": "Effets",
        "demo.ch13": "Parlé",
        "demo.ch14": "Harmonie C",
        "demo.ch15": "Outro",
        "demo.versionName": "Équilibre de base", // T27 自译,待人工审校

        // 分组词条组(05 §5.1,623-633 行;J71② 整组采设计稿三语提案表)。
        // 两插件共用的扁平 key 组,取代旧 in.groupSwitchConfirm / out.set.groupSwitchConfirm /
        // in.oneGroupPerTrack / in.groupHint.noOutputInGroup(旧 key 作废,不得并存)。
        "group.label": "Groupe",
        "group.oneOnly":
            "Une piste vocale ne peut appartenir qu'à un seul groupe — un second SCVB Input sur la même piste (quel que soit le groupe) ne reçoit que du silence",
        "group.noOutput":
            "Le groupe {g} n'a pas d'Output — insérez SCVB Output sur le bus cible et sélectionnez le groupe {g}",
        "group.switchConfirm":
            "Passer au groupe {x} coupera la connexion actuelle (groupe {y} · canal {n})",
        "group.switchConfirm.out":
            "Passer au groupe {x} coupera les {n} connexions de ce groupe",
        "group.switchConfirm.primary": "Passer à {x}",
        "ch.occupied": "Le canal {n} est déjà occupé (groupe {g})",
        "banner.secondOutput":
            "Le groupe {X} a déjà un Output principal — cette instance est en lecture seule",
        "group.oneInstanceNote":
            "Cette instance n'appartient qu'à un seul groupe",

        // [D1] Annuler / Rétablir dans l'en-tête (contrat §1.25/§1.26 ;
        // auto-traduit, relecture U17 en attente).
        "header.history": "Annuler et rétablir",
        "header.undo": "Annuler",
        "header.redo": "Rétablir",
        "header.undoEmpty": "Plus rien à annuler",
        "header.redoEmpty": "Plus rien à rétablir",

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "Suivi",
        "master.rangeFollowHint":
            "Suit la lecture, la plage s'étend automatiquement. {n} segments analysés · {t} au total",
        "master.step1.desc":
            "Activez et lisez cette plage pour enregistrer les caractéristiques de loudness",
        "master.widthAngleHint": "L'angle des voix les plus externes. 0–90°",
        "master.msHint": "−100 vers Mid / +100 vers Side",
        "master.distHint":
            "Hauteur = volume, position horizontale = panoramique ; le trait = largeur de la source stéréo",
        "master.distAxis": "G · −50 · C · +50 · D",

        // T43 / [J75] A+B —— 自译,**待 U17 人工审校**。
        "chart.modeGroup": "Vue du graphique de répartition",
        "chart.modeDistribution": "Répartition",
        "chart.modeTrajectory": "Trajectoire",
        "chart.trajHint":
            "Une ligne par piste = le panoramique final imprimé de cette piste ; aucune ligne là où il n'y a pas de segment",
        "chart.panSideR": "D",
        "chart.panSideC": "C",
        "chart.panSideL": "G",
        "chart.trajCanvasAria":
            "Graphique de trajectoire du panoramique : molette pour défiler latéralement, Ctrl+molette pour zoomer horizontalement, Maj+molette pour défiler verticalement, Alt+molette pour zoomer verticalement ; le glisser et les flèches fonctionnent aussi",
        "chart.zoomAria": "Niveau de zoom de la timeline",
        "chart.backToPlayhead": "Revenir à la tête de lecture",
        "chart.resetPanZoom": "Réinit. zoom Y",
        "chart.trajEmpty":
            "Aucun segment pour l'instant — la trajectoire de panoramique de chaque piste apparaît ici après l'analyse",
        "chart.legendAria": "Légende des couleurs de piste",
        "chart.legendHint":
            "La pastille de couleur correspond au numéro de piste ; survolez pour mettre la piste en évidence",

        // T41 —— 自译,**待 U17 人工审校**。
        "suggest.entry": "Suggestions",
        "suggest.title": "Tableau de suggestions",
        "suggest.disclaimer":
            "Ce sont des valeurs suggérées — SCVB ne les applique pas à votre place dans le DAW ; reportez-les à la main depuis ce tableau.",
        "suggest.backToLanes": "Retour aux couloirs",
        "suggest.export": "Exporter en CSV",
        "suggest.scope": "{v} · {t} pistes · {n} lignes",
        "suggest.empty":
            "Aucun segment pour l'instant — les suggestions par piste et par segment apparaissent ici après l'analyse",
        "suggest.staleNote":
            "{n} de ces pistes ont des données de capture périmées — recapturez-les avant de reporter ces valeurs à la main.",
        "suggest.legend":
            "Le panoramique et le volume sont une valeur suggérée par segment ; la largeur est un réglage courant par piste, elle se répète donc sur chaque ligne de cette piste. La largeur n'a de valeur que sur les pistes stéréo et reste vide sur les pistes mono comme lorsqu'elle est inconnue (0 signifie « repliée en mono », ce qui n'est pas la même chose que vide) ; origine et verrou reprennent exactement les valeurs enregistrées dans le projet",
        "suggest.exporting": "Export en cours…",
        "suggest.exportOk": "{n} lignes exportées → {path}",
        "suggest.exportCancelled": "Export annulé",
        "suggest.exportFail": "Échec de l'export : {reason}",
        "suggest.exportUnavailable":
            "L'export n'est pas encore raccordé dans cette version — la boîte de dialogue d'enregistrement est en attente côté natif",
        "suggest.tableAria":
            "Tableau de suggestions : suggestions de panoramique / volume / largeur par piste et par segment",
        "suggest.rowsAria": "Lignes de suggestions, flèches pour faire défiler",
        "suggest.col.trackIndex": "Pst",
        "suggest.col.trackLabel": "Nom de piste",
        "suggest.col.sourceChannels": "Can",
        "suggest.col.version": "Ver",
        "suggest.col.versionName": "Nom de version",
        "suggest.col.segmentIndex": "Seg",
        "suggest.col.t0Sec": "Début (s)",
        "suggest.col.t1Sec": "Fin (s)",
        "suggest.col.pan": "Pan",
        "suggest.col.volDb": "Vol dB",
        "suggest.col.width": "Largeur %",
        "suggest.col.origin": "Origine",
        "suggest.col.locked": "Verrou",

        "master.transitionHint":
            "Durée de transition des paramètres entre segments.",
        "master.copyConfirmWarn":
            "Les données existantes seront écrasées — pan/vol des 15 pistes, résultats de segmentation et marques d'édition manuelle de {name} sont remplacés. Annulable (Ctrl+Z).",
        "tracks.colLegend":
            "Vol = si la piste entre dans l'équilibrage du volume (activé par défaut) et le réglage du volume · Pan = si la piste entre dans la redistribution du pan (stéréo désactivé par défaut, équilibrage du volume conservé) et le réglage du pan · Gel P/V = toujours pris en compte par le moteur mais plus piloté ; le potentiomètre se déverrouille en contrôle manuel pur (les deux interrupteurs partagent un même paramètre d'automation par piste)",
        "tracks.emptyGroup":
            "Le groupe {g} n'a encore aucune entrée — insérez SCVB Input dans le dernier emplacement de chaque piste vocale et sélectionnez le groupe {g}",
        // ---- T32 Wave 1 新增(FR 为 T32 自译,发布前必须人工审校,05 §5)----
        "tracks.colCh": "CH",
        "tracks.labelPlaceholder": "Piste {n}",
        "tracks.colVolPart": "Participation volume",
        "tracks.colState": "État",
        "tracks.colPan": "PAN",
        "tracks.colW": "W",
        "tracks.colVolLevel": "VOLUME",
        "tracks.colPrio": "PRIO",
        "tracks.colLead": "LEAD",
        "tracks.colVolExempt": "Vol",
        "tracks.colAutoPan": "Pan",
        "tracks.colFreezePan": "GEL P",
        "tracks.colFreezeVol": "GEL V",
        "tracks.colOn": "ON",
        "tracks.footNote": "15 pistes",
        "tracks.emptyRoute":
            "Le routage de sortie des pistes vocales doit rester dirigé vers ce bus",
        "lowSample.full":
            "Échantillon insuffisant — l'analyse peut être instable",
        "tracks.panAutoHint": "Mode auto : piloté par la courbe d'analyse",
        "tracks.leadCenter": "CTR",
        "tracks.multiLead": "Plusieurs voix principales centrées",
        "tracks.misaligned": "Désalignement ×{n}",
        "tracks.srErr": "Fréquence d'échantillonnage incompatible",
        "tracks.labelEdit": "Nom de piste (24 caractères max)",
        "tracks.reidentifyOne": "Ré-identifier la piste {n}",
        "tracks.reidentifyConfirm":
            "La valeur fixe manuelle de la piste {n} sera effacée puis ré-identifiée ; les segments verrouillés restent inchangés. Continuer ?",
        "tracks.pairNone": "Aucun",
        "tracks.pairFullSuffix": " (complet)",
        "tracks.pairOverflow": "Paire en surnombre",
        "common.decrease": "Diminuer",
        "common.increase": "Augmenter",
        "wave.trackPickHint":
            "Cochez les en-têtes de piste à gauche pour choisir les cibles (multi-sélection, Maj pour une plage)",
        "wave.selChip":
            "Les quatre actions ci-dessus s'appliquent à {n} pistes",
        "wave.setRangeTip":
            "Plage de travail définie sur la sélection {x}–{y} — Range est passé en « Manuel » ; la sélection reste inchangée",
        "wave.originLegend":
            "origin : E = modifié, C = créé manuellement ; les segments auto n'ont pas de badge. Les segments verrouillés survivent à la ré-identification.",
        "set.loudnessMode.title":
            "Algorithme d'analyse de loudness des segments",
        "set.loudnessMode.note":
            "Définit la référence de normalisation du loudness entre segments ; tout changement impose une ré-analyse.",
        "set.centerSlot.title":
            "Priorité quand plusieurs pistes se disputent le centre",
        "set.centerSlot.note":
            "Règle de repli au-delà du verrou lead et de Lead Select ; sans effet sur l'exemption de volume.",
        "in.chHint.groupEmpty":
            "Ce groupe n'a pas encore d'Output — la table des canaux est vide",

        // ---- T35 新增(FR 自译,待人工审校)----
        "set.usage.eyebrow": "MODE D'EMPLOI",
        "set.usage.workflow":
            "Capture → Analyser → Sortie. Activez la capture et lancez la lecture ; le plug-in enregistre les caractéristiques de loudness de chaque piste. L'analyse calcule un plan pan/volume pour chaque phrase hors ligne. Passez la sortie en PILOTAGE MOTEUR pour que le plug-in pilote les paramètres ; activez le mode Latch ou Write dans votre DAW pour les enregistrer comme piste d'automation.",
        "set.usage.docs": "Documentation",

        // ---- [J78] 手动接管与自动化说明块(FR 自译,待人工审校;口径见 zh 侧同组注释)----
        "set.automationGuide.eyebrow": "MANUEL / AUTOMATION",
        "set.automationGuide.title":
            "Reprise manuelle et automation : qui décide ?",
        "set.automationGuide.line1":
            "Priorité, de la plus forte à la plus faible : automation de l'hôte > valeur manuelle gelée > ajustement manuel > courbe d'analyse du moteur.",
        "set.automationGuide.line2":
            "Geler PAN ou VOL revient à reprendre cette dimension à la main ; au write, le plug-in inscrit une ligne plate dans l'automation, que vous pouvez ensuite dessiner librement dans la DAW.",
        "set.automationGuide.line3":
            "Un ajustement manuel dans l'inspecteur de segment modifie la courbe du moteur elle-même et part avec le write (sauf si cette dimension est déjà gelée).",
        "set.automationGuide.line4":
            "Une fois l'automation dessinée, le plug-in suit fidèlement l'hôte et le moteur ne l'écrase plus ; seul un nouveau write la réécrit.",

        "set.loudnessMode.eyebrow": "SECOND INDICATEUR DE LOUDNESS",
        "set.loudnessMode.opt.kw_integrated": "Intégrale pondérée K",
        "set.loudnessMode.opt.rms": "RMS",
        "set.loudnessMode.opt.peak_dbfs": "Crête dBFS",
        "set.centerSlot.eyebrow": "STRATÉGIE DE CRÉNEAU CENTRAL",
        "set.centerSlot.opt.priority_queue": "File de priorité",
        "set.centerSlot.opt.lead_exclusive": "Exclusivité lead",
        "set.centerSlot.opt.even_spread": "Répartition égale",
        "set.guide.showAll": "Voir les neuf règles",
        "set.guide.collapse": "Replier",
        "set.guide.rulesMissing":
            "Les neuf règles complètes seront incluses dans la version publiée",
        "set.storage.eyebrow": "STOCKAGE",
        "set.storage.embedded": "Intégré au projet ({mb} Mo)",
        "set.storage.external":
            "Enregistré comme fichier externe (>8 Mo automatique)",
        "set.storage.sessionGuid": "session {guid}",
        "set.diag.eyebrow": "DIAGNOSTIC",
        "set.diag.copy": "Copier les diagnostics",
        "set.diag.copied": "Copié",
        "set.diag.colCh": "CH",
        "set.diag.colHb": "HB",
        "set.diag.colMis": "MIS",
        "set.diag.colGen": "GEN",
        "set.diag.colSeq": "SEQ",
        "set.reanalyze": "Ré-analyse requise",
        // ---- T36 新增(Input 单页正式实现;05 §3 语义,词条真源 05 §5/§3)。
        // EN/FR 为 T36 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "in.pillSub.passthrough": "Direct : aucun traitement sur cette piste",
        "in.pillSub.takenOver":
            "Pris en charge : cette piste est coupée, et le son sort par le bus inséré dans Output",
        "in.pillSub.hysteresis":
            "Connexion instable — passage en direct imminent",
        "in.pillSub.stereo":
            "Cette piste est une source stéréo — SCVB préserve sa largeur",
        "in.source.mono": "Cette piste est une source mono",
        "in.group.note":
            "Doit partager un groupe avec Output pour se connecter ; gardez A en usage mono-groupe",
        "in.channels.emptyHint":
            "Choisissez le numéro de canal de cette piste (correspond à la page Pistes de l'Output)",
        "in.unassigned": "Non assigné",
        "in.releaseConfirm.primary": "Libérer",
        "in.channels.manual.confirm": "Confirmer",
        "in.priority.note": "Ce réglage est enregistré côté Output",
        "in.priority.offline": "Nécessite Output en ligne",
        "in.footer.hint": "Prêt",
        "in.footer.noBackend": "Aucun backend — ouvrez via web-preview",
        "in.priority.unassigned": "Sélectionnez d'abord un canal",
        "ch.occupied.group": "Un canal est déjà occupé (groupe {g})",

        // T31 新增(Output 外壳 + Tab1 正式实现,05 §2.0/§2.1 语义 + design-v2 定稿文案)。
        // 立项理由与逐条出处见 scratchpad/t31/deviations.md「新增词条」节;
        // EN/FR 为 T31 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "common.cancel": "Annuler",
        "tab.master": "Général",
        "tab.tracks": "Pistes",
        "tab.wave": "Ondes & segments",
        "tab.settings": "Réglages",
        "version.emptyBadge": "VIDE",
        "master.copyConfirmPrimary": "Écraser et copier",
        "master.captureOff": "CAPTURE DÉSACTIVÉE",
        "master.step2.desc":
            "Affecte {n} intervalles / {m} pistes ; {k} modifications manuelles seront conservées",
        "master.step2.desc.noData":
            "Aucune donnée capturée dans la plage actuelle — ajustez la plage ou capturez d'abord",
        "master.step2.coverage":
            "{p}% de la plage est couverte ; les parties non couvertes restent inchangées",
        "master.analyzing": "Analyse en cours…",
        "master.analyzeDone": "Analyse terminée",
        "master.step3.desc":
            "Activez l'enregistrement d'automation dans votre DAW",
        "master.step3.descStrong":
            "Si disponible, utilisez le mode Latch (recommandé) ou Write ; n'utilisez jamais le mode Touch",
        "master.writeConfirm.ok": "Compris, démarrer",
        "master.writeConfirm.undo": "Annuler (revenir au suivi hôte)",
        "master.groupEyebrow": "GROUPE",
        "master.widthEyebrow": "LARGEUR · ANGLE MAX",
        "master.rangeEyebrow": "PLAGE",
        "master.distEyebrow": "RÉPARTITION PAN / VOLUME",
        "master.curveEyebrow": "RÉGLAGE DE NIVEAU PAR ANGLE · ±12 dB",
        "master.curveLegendMs": "Gain équivalent M/S",
        "master.curveAxisX":
            "Gauche G −100 · −50 · Centre C 0 · +50 · Droite D +100",
        "master.curveEmptyHint":
            "Double-cliquez pour ajouter un point de contrôle",
        // ---- T34 曲线编辑器(curve.*;shape/side 术语逐字对拍 05 §5 术语表)----
        "curve.canvasLabel": "Éditeur de courbe de gain angulaire",
        "curve.maxPoints": "Limite de 16 points atteinte",
        "curve.centerSide": "Point central — choisissez une direction",
        "curve.shape.bell": "Cloche",
        "curve.shape.shelf": "Plateau",
        "curve.shape.cut": "Coupe",
        "curve.side.out": "Extérieur",
        "curve.side.left": "Gauche",
        "curve.side.right": "Droite",
        "curve.qLabel": "Q",
        "curve.slopeLabel": "Pente",
        "curve.slope.opt6": "6 dB/oct",
        "curve.slope.opt12": "12 dB/oct",
        "curve.slope.opt18": "18 dB/oct",
        "curve.slope.opt24": "24 dB/oct",
        "curve.sideTooltip":
            "Extérieur: coupe le côté extérieur, loin du centre, délimité par ce point. Gauche/Droite: coupe uniquement ce côté",
        "curve.deleteLabel": "Supprimer le point",
        "curve.announcePoint":
            "Point {n} : angle {angle}, {gain} dB, {shape}, Q {q}",
        "curve.announcePointDir":
            "Point {n} : angle {angle}, {gain} dB, {shape}, Q {q} · direction {side}",
        "master.leadSelectDefaultNote": "0 = sélection auto (par défaut)",
        "range.manual": "Manuel",
        "master.rangeLoopStale":
            "La boucle n'est plus valide — la plage précédente est conservée",
        "master.rangeLoopMissing":
            "L'hôte ne fournit pas de boucle — le mode reste affiché mais non sélectionnable",
        "master.rangeStart": "Début de plage",
        "master.rangeEnd": "Fin de plage",
        "master.setToPlayhead": "Définir sur la tête de lecture",
        "master.barsEstimateNote":
            "Les numéros de mesure sont estimés ; ils se calibrent après lecture de cette zone",
        "footer.defaultHint":
            "Capture → Analyse → Sortie : activez la capture dans l'onglet 1 et lisez cette plage",
        "scale.current": "Actuel",
        "scale.overflow": "Dépasse l'écran actuel",

        // ---- T31 Wave 2 新增(FR 自译,**发布前必须人工审校**,05 §5)----
        "banner.misaligned":
            "Routage désaligné : trous de timeline détectés sur {m} piste(s) — vérifiez que les pistes voix pointent toujours vers ce bus",
        "banner.srMismatch":
            "Piste {n} : fréquence d'échantillonnage incohérente — désactivée",
        "banner.sidecarMissing":
            "Données de capture manquantes ou périmées — veuillez recapturer",
        "banner.noTimeline": "L'hôte ne fournit aucune timeline",
        "master.versionArmedConfirm":
            "La sortie du moteur passera progressivement à la nouvelle version. Continuer ?",
        "master.empty.step1":
            "Insérez SCVB Input dans le dernier emplacement de la chaîne d'effets de chaque piste voix (après tout traitement)",
        "master.empty.step2":
            "Conservez le routage de sortie de chaque piste voix vers le bus commun",
        "master.empty.step3":
            "Insérez SCVB Output dans le premier emplacement de ce bus",
        "master.empty.step4": "Capturez et lisez",
        "master.empty.step5": "Analysez et activez la sortie",
        "common.continue": "Continuer",
        // eyebrow:三语同值(视觉层 mono 大写元素,照设计稿不译)
        "master.msEyebrow": "MS BALANCE",
        "master.leadEyebrow": "LEAD SELECT",
        "master.transitionEyebrow": "TEMPS DE TRANSITION",

        // ---- T31 Wave 2 评审修订新增(FR 自译,**发布前必须人工审校**,05 §5)----
        "toast.projectCopy":
            "Copie de projet détectée — une copie indépendante des données de capture a été créée",
        "toast.sidecarSwitched":
            "Les données de capture ont été déplacées dans un fichier externe (>8 Mo) ; voir les réglages (gestion des chemins et import/export à venir)",
        "scale.confirmBody": "Échelle appliquée, retour dans {s} s",
        "scale.keep": "Enregistrer",
        "master.printLock.group":
            "Changement de groupe impossible pendant l'écriture d'automation",
        "master.printLock.version":
            "Changement de version impossible pendant l'écriture d'automation",
        "master.printLock.copy":
            "Copie impossible pendant l'écriture d'automation",

        // ---- T33 Wave 1 新增(FR 为 T33 自译,**发布前必须人工审校**,05 §5 / U17;
        // 7 滑杆短标 / ORIGIN 为 mono 微标,三语同值)----
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        "wave.sldHangover": "HOLD",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
        "wave.tipThreshold":
            "Seuil : ce qui est plus fort est identifié, ce qui est plus faible est ignoré (défaut {d})",
        "wave.tipHysteresis":
            "Hystérésis : ouverture au seuil, fermeture seulement à cette distance en dessous — évite qu\u2019un niveau instable près du seuil ne hache une phrase (défaut {d})",
        "wave.tipHold":
            "Maintien (hangover VAD) : durée pendant laquelle le signal reste identifié après être passé sous le seuil, pour éviter de couper entre les mots (défaut {d})",
        "wave.tipPadPre":
            "Marge avant : marge avant la région identifiée, pour conserver l'attaque et la respiration (défaut {d})",
        "wave.tipPadPost":
            "Marge après : marge après la région identifiée, pour conserver la queue de son (défaut {d})",
        "wave.tipSensitivity":
            "Sensibilité de segmentation : plus elle est haute, plus on coupe aux creux d\u2019énergie. N\u2019agit que sur les segments de plus de 8 s, donc sans effet sur les phrases courtes (défaut {d})",
        "wave.tipMinSeg":
            "Longueur minimale de segment : les segments plus courts sont supprimés (jugé avant les marges) pour filtrer le bruit, mais cela supprime aussi les ad-libs brefs et les harmonies d'une note (défaut {d})",
        "wave.emptyMain":
            "Aucune donnée capturée — activez l'interrupteur de capture puis lancez la lecture",
        "wave.emptyCta": "Ouvrir la capture dans l'onglet 1",
        "wave.followHostNote":
            "Suivi hôte : les modifications sont enregistrées et prendront effet une fois la sortie activée",
        "wave.btnRecapture": "Re-capturer la sélection",
        "wave.btnReanalyze": "Ré-analyser la sélection",
        "wave.btnClearCoverage":
            "Effacer les données de capture de la sélection",
        "wave.applyCountdown": "Application dans 300 ms…",
        "wave.applying": "Application…",
        "wave.recaptureInlineNote":
            "Cette ré-capture ne concerne que la sélection et n'affecte pas la plage du moteur de sortie",
        "wave.inspectorTitle": "Détails et édition du segment",
        "wave.inspectorToggle": "Détails et édition du segment",
        "wave.inspectorClose": "Réduire",
        "wave.inspectorEmpty":
            "Cliquez sur un segment dans une piste pour le modifier",
        "wave.segStart": "Début",
        "wave.segEnd": "Fin",
        "wave.segLen": "Durée",
        "wave.segLoudness": "Intégrale de segment pondérée K",
        "wave.volField": "VOL",
        "wave.lockSegment": "Verrouiller ce segment",
        "wave.lockBadge": "VERROUILLÉ",
        "wave.covSeg": "{p}% · {n} seg",
        "wave.curveVisible": "Courbes visibles",
        "wave.pickTrack": "Sélectionner la piste {n}",
        "wave.boundaryHandleTip":
            "Limite de segment : glissez pour la déplacer (aimantée aux creux d'énergie, Alt pour désactiver) ; double-clic dans un segment pour scinder ici ; sélectionnez deux segments adjacents et appuyez sur Suppr pour fusionner",
        "wave.boundarySnapTip":
            "Aimantée à un creux d'énergie (Alt pour désactiver)",
        "wave.hZoomBar":
            "Zoom horizontal (glissez pour changer l'étendue de la vue, flèches pour avancer pas à pas)",
        "wave.vZoomBar":
            "Zoom vertical (glissez pour changer la hauteur des pistes, flèches pour avancer pas à pas)",
        "wave.reidentifyConfirm":
            "Efface {k} marques d'édition manuelle et recalcule ; {l} segments verrouillés restent inchangés. Continuer ?",
        "wave.clearCoverageConfirm":
            "Supprime les données de caractéristiques capturées pour les pistes sélectionnées × la sélection. Continuer ?",
        // ---- Retours d'interaction T33 Wave 2(05 §2.3 / contrat §5.5;relecture U17 en attente)----
        "wave.armReason.noTracks":
            "Aucune piste cible — cochez les en-têtes de piste à gauche",
        "wave.armReason.noSelection":
            "Aucune sélection valide — tracez-en une sur la règle temporelle",
        "wave.armReason.readOnly":
            "Observateur en lecture seule — l'écriture n'a pas pris effet",
        "wave.armReason.noTimeline":
            "L'hôte ne fournit pas de ligne temporelle — écriture impossible",
        "wave.notAdjacent":
            "Seuls deux segments adjacents peuvent être fusionnés",
        "wave.btnMerge": "Fusionner la paire sélectionnée",
        "wave.recaptureOverlap": "Écrasera {k} segments de données existantes",
        "wave.diffItem": "Piste {ch} · seg {i} : pan {pf}→{pt} · vol {vf}→{vt}",
        "wave.diffAddedRemoved": "{a} ajoutés · {r} supprimés",
        "wave.clearedCoverage": "{s} s de données capturées effacées",
    },
};

/** 支持的语言(05 §5:UI 三语)。用白名单而不是 `T[lang]` 真值判断 —— 后者会把
 *  "constructor"/"toString" 一类原型链属性当成合法语言,dict 于是返回一个函数,
 *  页面全部 [data-t] 因 t[key] == null 静默不刷新,documentElement.lang 也被写坏。 */
export const LANGS = ["zh", "en", "fr"];

/** 归一化语言码:不在白名单一律回落中文(05 §5)。 */
function normLang(lang) {
    return LANGS.includes(lang) ? lang : "zh";
}

/** 取某语言字典,未知语言回落中文(05 §5)。 */
export function dict(lang) {
    return T[normLang(lang)];
}

/**
 * 把 root 里所有 `data-t="key"` 元素的文本换成当前语言,
 * 并把 `data-t-aria="key"` 元素的 aria-label 一并同步(05 §5:切语言同步全部 aria-label)。
 * 同时同步 documentElement.lang(照 Bridge index.html 的 a11y 做法),返回该语言字典供动态文案渲染。
 * 占位符替换由调用方自己做——本模块只发字典,不做模板求值。
 */
export function applyI18n(root, lang) {
    const t = dict(lang);
    // own-property 判定与 normLang 同口径(PR #32 claude-review 建议 1):
    // data-t="toString" 一类原型链键不得命中(check-i18n 会拦,运行期同样要硬)
    const has = (k) => Object.prototype.hasOwnProperty.call(t, k);
    root.querySelectorAll("[data-t]").forEach((el) => {
        const key = el.getAttribute("data-t");
        if (has(key)) el.textContent = t[key];
    });
    root.querySelectorAll("[data-t-aria]").forEach((el) => {
        const key = el.getAttribute("data-t-aria");
        if (has(key)) el.setAttribute("aria-label", t[key]);
    });
    if (typeof document !== "undefined" && document.documentElement) {
        document.documentElement.lang = normLang(lang);
    }
    return t;
}
