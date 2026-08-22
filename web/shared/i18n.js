// SPDX-License-Identifier: GPL-3.0-or-later
// =============================================================================
// SCVB — 界面文案字典(中 / EN / FR)
// =============================================================================
// 真源:masterPlan/plan/05-ui-spec.md §5(状态词规范 512-524 / 术语表 530-618)、
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
// T27 自译(05 未给三语)的条目:state.* 六条 FR、state.waitingForOutput.group 的 EN/FR、
// state.groupSuffix 三语、tour.step1..7 的 EN/FR —— 均待人工审校,逐条清单见 T27 差异清单
// 的「i18n.js」小节(05 §5 要求 fr 发布前人工审校,那份清单就是审校人的入口)。
// =============================================================================

export const T = {
    zh: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "未连接",
        "state.staleLink": "失联",
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
        group: "组",
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
        leadFollowAnalysis: "遵循分析",
        msBalance: "MS 平衡",
        stereoBadge: "立体声",
        participateAutoPan: "参与自动声像",
        trackWidth: "轨道宽度",
        "tracks.monoWidthNoop": "mono 源:宽度在 v1 无效",
        "master.leadSelectHint":
            "该轨强制居中;是否参与音量调节为独立开关,不随此联动",
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
        autoStop: "播完自动停",
        reidentify: "重新识别(含手动段)",
        applyToSegments: "应用到分段",
        setAsRange: "设为范围",
        clearCapture: "清除采集数据",
        scale: "界面缩放",
        language: "语言",
        armedWaiting: "已布防·等待播放",
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
            "引擎驱动 {v} · 范围 {x}–{y} · 30 条车道;若 DAW 侧已 arm Latch/Write,播放本范围将覆盖该范围已有自动化;未 arm 则仅试听、不落盘",
        "footer.printing": "引擎驱动 {v} · {x}–{y}",
        "footer.printDone":
            "本次打印覆盖 {x}–{y};若在录制自动化,建议切回跟随宿主试听核对",
        "out.master.writeConfirm.follow":
            "引擎驱动 {v} · 范围 = 全部已分析区域(全曲跟随,共 {n} 段 · 合计 {t}) · 30 条车道;若 DAW 侧已 arm Latch/Write,播放已分析区域将覆盖其已有自动化;未 arm 则仅试听、不落盘",
        "footer.printing.follow": "引擎驱动 {v} · 全曲跟随(已分析区域内)",
        "footer.printDone.follow":
            "本次打印覆盖已分析区域;若在录制自动化,建议切回跟随宿主试听核对",
        "wave.diffKept": "{k} 处手动编辑/锁定段已保留",
        "tracks.manualOverwriteConfirm":
            "将以固定值替换该轨(当前版本)的全部分段结果,可撤销",
        "tracks.manualOverwriteConfirm.locked": "(含 {l} 个锁定段)",
        "in.pill.abiMismatch": "版本不匹配",
        "in.pill.srMismatch": "采样率不一致",
        "tracks.manualDrivenHint": "该轨仍由手动固定值驱动——重新识别该轨?",
        "in.releaseConfirm": "将释放通道 {n},本轨回到直通",
        "wave.recaptureArmed": "重采集布防中 · 选区 {x}–{y} · {n} 轨",
        "footer.recaptureOutputWarn":
            "输出引擎仍按全局范围工作,与本次重采集选区无关",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从 docs/USER_GUIDE.zh-CN.md#硬约束 生成写入(05 §5 / §0.1,禁止手抄)。
        "guide.title":
            "必读:SCVB 的九条硬约束,违反其中任何一条都会导致静音、错音或分析失效。",
        "guide.dontShowAgain": "不再显示",
        "guide.start": "开始使用",
        "set.reopenGuide": "重看引导",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "要不要花一分钟快速了解界面?",
        "tour.ask.start": "开始引导",
        "tour.ask.later": "暂不,以后可在设置页重看",
        "tour.clickAnywhere": "左键点击任意处继续;说明框上的按钮除外",
        "tour.skip": "跳过",
        "tour.prev": "上一步",
        "tour.next": "下一步",
        "tour.done": "完成",
        "tour.demoBadge": "演示数据",

        // tour 七步(05 §2.6 步骤表,421-429 行;J71① 定稿 7 步,增删步须回 05 改)。
        // zh 逐字取 §2.6「标题」「要点」列(去掉表内编辑注记);EN/FR 为 T27 自译,待人工审校。
        // 第 4 步措辞纪律:只说 DAW 侧开 Latch / Write,不得出现「写入完成」类表述(05 §2.6)。
        "tour.step1.title": "这是 SCVB Output 的主界面",
        "tour.step1.body":
            "流程从左到右:采集→分析→输出;已注入 15 轨演示数据,引导结束后还原",
        "tour.step2.title": "第一步:采集",
        "tour.step2.body":
            "打开采集开关并播放本范围,插件记录每条轨的响度特征。这一步不写任何自动化",
        "tour.step3.title": "第二步:分析",
        "tour.step3.body":
            "离线算出每个乐句的声像/音量方案;点击前有影响预览,手动编辑/锁定段会被保留",
        "tour.step4.title": "第三步:输出",
        "tour.step4.body":
            "切到「引擎驱动」后参数由分析结果驱动;要录成自动化在 DAW 侧开 Latch 或 Write",
        "tour.step5.title": "轨道页:每轨的微调",
        "tour.step5.body":
            "两枚冻结开关把该维度从引擎手里接管过来,旋钮与推子随之解锁为手动",
        "tour.step6.title": "波形页:看结果、改分段",
        "tour.step6.body":
            "每条泳道叠着 pan/vol 阶梯曲线;框选一段可以只重采集或只重分析这一段",
        "tour.step7.title": "以后在这里可以再看一次",
        "tour.step7.body": "引导随时可重看,红字九条也在同一块里展开",

        // tour demo 的 15 条轨名(J62)。真值在 web/shared/mock-data.js 的 DEMO_LABELS,
        // key 表在同文件的 DEMO_LABEL_KEYS(mock-data.js 不 import 本文件,依赖方向照 06 §6.2)。
        // 为什么它们要进词典:用户轨的 label 是用户数据、保持原样,而 **tour demo 是我们自己造的
        // UI 内容**,05 §5「切语言同步全部可见文案」对它成立 —— 否则 EN/FR 用户看到英/法文讲解框
        // 配 15 条中文轨名。渲染 label 还是 t(key) 由 T31 决定,mock-data.js 两者都给。
        // zh 逐字 = mock-data.js DEMO_TRACKS 的 label(设计稿 CH_LABELS + T27 补的 14/15);
        // EN/FR 为 T27 自译,与 tour.step* 同批待人工审校。L/R/C 的 FR 侧照 master.distAxis 取 G/D/C。
        "demo.ch1": "主唱",
        "demo.ch2": "主唱双",
        "demo.ch3": "和声 L",
        "demo.ch4": "和声 R",
        "demo.ch5": "和声 C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "低八度",
        "demo.ch9": "高八度",
        "demo.ch10": "群唱 L",
        "demo.ch11": "群唱 R",
        "demo.ch12": "呼吸轨",
        "demo.ch13": "念白",
        "demo.ch14": "群唱 C",
        "demo.ch15": "尾音",
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

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "全曲",
        "master.rangeFollowHint":
            "跟随播放,自动扩展范围。已分析区域共 {n} 段 · 合计 {t}",
        "master.step1.desc":
            "开关打开后播放本范围,插件记录响度特征——这一步不写任何自动化",
        "master.widthAngleHint":
            "左右各摆到最外时的角度;0–90°(自动化参数 width 0–150%)",
        "master.msHint": "−100 偏 Mid / +100 偏 Side,双击回 0——拖动看下方曲线",
        "master.distHint": "柱高 = 音量,横位 = 声像;横线 = 立体声源张开度",
        "master.distAxis": "左 L · −50 · 中 C · +50 · 右 R",
        "master.transitionHint":
            "段与段之间参数不瞬切,走完这段斜坡才到下一段的值。太短交界会「跳」,太长声像糊在两处之间。",
        "master.copyConfirmWarn":
            "目标已有数据将被覆盖——{name} 的 15 轨 pan / vol、全部分段结果与手动编辑标记将被整体替换。可撤销(Ctrl+Z)。",
        "tracks.colLegend":
            "音量＝参与音量调节,该轨是否进音量平衡计算(默认开)· 声像＝参与自动声像,该轨是否进声像重分布(stereo 轨默认关,仍参与音量平衡)· 冻结P/V＝结果照算但不再驱动,旋钮解锁为手动(两开关共用一个每轨自动化参数)",
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
        "tracks.colVolLevel": "音量 / 电平",
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
        "tracks.footNote": "15 轨 · 滚动 + 表头 sticky",
        "tracks.emptyRoute": "人声轨的输出路由须保持指向本总线(不要改)",
        // 「样本不足」角标保短版(裸词条 lowSample),全句进 tooltip(统筹裁定 B12)
        "lowSample.full": "样本不足,结果可能不稳",
        "tracks.panAutoHint": "自动模式——由分析曲线驱动",
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
            "将清除轨 {n} 的手动固定值并重新识别;已锁定段保持不变,确定?",
        "tracks.pairNone": "无",
        "tracks.pairFullSuffix": "(满)",
        "tracks.pairOverflow": "配对超员",
        "common.decrease": "减",
        "common.increase": "增",
        "wave.trackPickHint": "勾选左侧轨头选择目标轨(可多选,shift 连选)",
        "wave.selChip": "上面四个操作作用于 {n} 轨",
        "wave.setRangeTip":
            "作用范围已改为选区 {x}–{y}——Range 档位已切到「手动」,工作选区本身不变",
        "wave.originLegend":
            "origin:E = 手动编辑,C = 手动创建,auto 段无角标。锁定段在重新识别时保持不变。",
        "set.loudnessMode.title": "段响度用哪个口径",
        "set.loudnessMode.note":
            "影响分析时的段间响度归一化基准;改后需重分析。",
        "set.centerSlot.title": "多轨争抢中心位时怎么办",
        "set.centerSlot.note":
            "主唱锁与 Lead Select 之外的兜底规则;不影响参与音量调节开关。",
        // 05 §3(463 行)以短名 `in.chHint` 引用同一条,§5.2(658 行)印作本长名;
        // 实施一律用本 key —— applyI18n 对未命中的 key 不报错也不回退,写成 in.chHint 会静默留占位原文。
        "in.chHint.groupEmpty": "该组尚无 Output,通道表为空",

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
        "master.step3.desc": "要录成自动化:在 DAW 侧对本插件参数开",
        "master.step3.descStrong": "Latch(推荐)或 Write;请勿使用 Touch",
        "master.writeConfirm.ok": "知道了,开始",
        "master.writeConfirm.undo": "撤销(关回跟随宿主)",
        "master.groupEyebrow": "组 · GROUP",
        "master.widthEyebrow": "WIDTH · 最大角度",
        "master.rangeEyebrow": "RANGE · 范围",
        "master.distEyebrow": "声像 / 音量分布",
        "master.curveEyebrow": "角度域曲线 · ±12 dB",
        "master.curveLegendMs": "MS 等效增益",
        "master.curveAxisX": "左 L −100 · −50 · 中 C 0 · +50 · 右 R +100",
        "master.curveEmptyHint": "双击添加控制点",
        "master.leadSelectDefaultNote": "0 = 遵循分析(默认)",
        "range.manual": "手动",
        "master.rangeLoopStale": "循环区已失效,沿用上次范围",
        "master.rangeLoopMissing": "宿主未提供循环区——档位保留但不可选",
        "master.rangeStart": "起点",
        "master.rangeEnd": "终点",
        "master.setToPlayhead": "设为播放头",
        "master.barsEstimateNote": "小节为估算值,播放该区域后精确",
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
        "master.versionArmedConfirm": "引擎输出将平滑切至新版本,继续?",
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
            "采集数据已超过 8MB,已转存外部文件——发给他人需重新采集",
        // 缩放 10 秒防呆确认框(05 §1.2:立即预览 → 10 秒倒计时 → 取消/超时/关窗回退)。
        // 05 只给机制未给逐字正文;{s} = 剩余秒数,按钮「取消」复用 common.cancel。
        "scale.confirmBody": "缩放已应用,{s} 秒后回退",
        "scale.keep": "保持",
        // PRINT 态三处 disabled 的 tooltip(兑现 deviations A22;zh 逐字取 05 §2.1 ⓪/③)
        "master.printLock.group": "打印中不可切组",
        "master.printLock.version": "打印中不可切换版本,请先停止走带",
        "master.printLock.copy": "打印中不可复制版本",

        // ---- T33 Wave 1 新增(Output Tab3 正式实现;05 §2.3/§2.3a 有语义无 key 的位置,
        // 图谱 A-17/A-18/A-19 + 各件所需)。EN/FR 为 T33 自译,已入 U17 待人工审校清单。----
        // 7 滑杆短标:mono 大写微标族,三语同值(A-19 统筹裁定,同 tracks.colCh 纪律;
        // 顺序 = 05 §2.3 的 setVadParams 五字段 + setSegmentation 两字段,不可重排)
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        "wave.sldHangover": "HANGOVER",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
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
        "wave.recaptureInlineNote": "本次布防只门控采集,不改变输出范围",
        // 段检查器(05 §2.3a;标题 eyebrow + 只读字段名;origin 角标与标题
        // 同行,角标值 E/C 本身非词条 —— §17②,无独立字段行)
        "wave.inspectorTitle": "段检查器",
        "wave.segStart": "起",
        "wave.segEnd": "止",
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
        // 边界手柄 tooltip(A-14 裁定:Alt 关吸附写进 tooltip;双击分割一并说明)
        "wave.boundaryHandleTip":
            "拖动移动边界(自动吸附能量谷,按住 Alt 关闭);双击在此分割",
        // 二次确认框两枚(05 §2.3 行 302/303 逐字)
        "wave.reidentifyConfirm":
            "将清除 {k} 个手动编辑标记并重算;{l} 个已锁定段保持不变,确定?",
        "wave.clearCoverageConfirm": "将删除选中轨×选区的采集特征数据,确定?",
    },

    en: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "NOT CONNECTED",
        "state.staleLink": "STALE LINK",
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
        group: "Group",
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
        leadFollowAnalysis: "Follow analysis",
        msBalance: "MS Balance",
        stereoBadge: "ST",
        participateAutoPan: "Auto-Pan Participate",
        trackWidth: "Track Width",
        "tracks.monoWidthNoop": "Mono source: width has no effect in v1",
        "master.leadSelectHint":
            "Track forced to center; the volume participation switch is independent and unaffected",
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
        autoStop: "Auto-stop",
        reidentify: "Re-identify (incl. edited)",
        applyToSegments: "Apply",
        setAsRange: "Set as Range",
        clearCapture: "Clear capture",
        scale: "UI Scale",
        language: "Language",
        armedWaiting: "ARMED · WAITING",
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
            "Engine drive {v} · range {x}–{y} · 30 lanes. If Latch/Write is armed in your DAW, playing this range will overwrite existing automation there; if not armed, this is monitoring only.",
        "footer.printing": "ENGINE DRIVE {v} · {x}–{y}",
        "footer.printDone":
            "Pass covered {x}–{y}. If you were recording automation, switch back to Follow Host to check.",
        "out.master.writeConfirm.follow":
            "Engine drive {v} · range = all analyzed areas (follow, {n} segments · total {t}) · 30 lanes. If Latch/Write is armed in your DAW, playing analyzed areas will overwrite existing automation there; if not armed, this is monitoring only.",
        "footer.printing.follow": "ENGINE DRIVE {v} · FOLLOW (ANALYZED AREAS)",
        "footer.printDone.follow":
            "Pass covered the analyzed areas. If you were recording automation, switch back to Follow Host to check.",
        "wave.diffKept": "{k} edited/locked segments preserved",
        "tracks.manualOverwriteConfirm":
            "This replaces all analyzed segments of this track (current version) with a fixed value. Undoable.",
        "tracks.manualOverwriteConfirm.locked":
            "(includes {l} locked segments)",
        "in.pill.abiMismatch": "VERSION MISMATCH",
        "in.pill.srMismatch": "SR MISMATCH",
        "tracks.manualDrivenHint":
            "Track still driven by a manual fixed value — re-identify this track?",
        "in.releaseConfirm":
            "Release channel {n} — this track returns to passthrough",
        "wave.recaptureArmed": "RECAPTURE ARMED · {x}–{y} · {n} TRACKS",
        "footer.recaptureOutputWarn":
            "Engine still drives the global range — unaffected by the recapture selection",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从 docs/USER_GUIDE.zh-CN.md#硬约束 生成写入(05 §5 / §0.1,禁止手抄)。
        "guide.title":
            "Must read: SCVB's nine hard rules. Breaking any one of them causes silence, wrong panning, or failed analysis.",
        "guide.dontShowAgain": "Don't show again",
        "guide.start": "Get started",
        "set.reopenGuide": "Show guide again",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "Want a one-minute tour of the interface?",
        "tour.ask.start": "Start tour",
        "tour.ask.later": "Not now — replay it any time in Settings",
        "tour.clickAnywhere":
            "Left-click anywhere to continue (except the buttons in this box)",
        "tour.skip": "Skip",
        "tour.prev": "Back",
        "tour.next": "Next",
        "tour.done": "Done",
        "tour.demoBadge": "DEMO DATA",

        // tour 七步(05 §2.6 步骤表,421-429 行;J71① 定稿 7 步,增删步须回 05 改)。
        // zh 逐字取 §2.6「标题」「要点」列(去掉表内编辑注记);EN/FR 为 T27 自译,待人工审校。
        // 第 4 步措辞纪律:只说 DAW 侧开 Latch / Write,不得出现「写入完成」类表述(05 §2.6)。
        "tour.step1.title": "This is the SCVB Output main window",
        "tour.step1.body":
            "The flow runs left to right: capture → analyze → output. A 15-track demo set is loaded and is cleared when the tour ends.",
        "tour.step2.title": "Step 1 — Capture",
        "tour.step2.body":
            "Turn on the capture switch and play this range; the plug-in records the loudness features of every track. Nothing is written to automation in this step.",
        "tour.step3.title": "Step 2 — Analyze",
        "tour.step3.body":
            "Computes a pan/level plan for every phrase offline. An impact preview appears before you commit, and edited or locked segments are preserved.",
        "tour.step4.title": "Step 3 — Output",
        "tour.step4.body":
            "Switch to ENGINE DRIVE and the parameters are driven by the analysis result. To record it as automation, arm Latch or Write on the DAW side.",
        "tour.step5.title": "Tracks page — per-track tweaks",
        "tour.step5.body":
            "The two freeze switches take that dimension back from the engine; the knob and fader unlock to manual.",
        "tour.step6.title": "Waveform page — review results, edit segments",
        "tour.step6.body":
            "Each lane carries the pan/vol step curves on top; drag a selection to recapture or re-analyze just that part.",
        "tour.step7.title": "You can replay this tour here",
        "tour.step7.body":
            "The tour can be replayed at any time, and the nine hard rules open in the same block.",

        // tour demo 的 15 条轨名(J62;口径见 zh 侧同组注释)。EN 为 T27 自译,待人工审校。
        "demo.ch1": "Lead Vocal",
        "demo.ch2": "Lead Double",
        "demo.ch3": "Harmony L",
        "demo.ch4": "Harmony R",
        "demo.ch5": "Harmony C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "Octave Down",
        "demo.ch9": "Octave Up",
        "demo.ch10": "Group Vocal L",
        "demo.ch11": "Group Vocal R",
        "demo.ch12": "Breath",
        "demo.ch13": "Spoken",
        "demo.ch14": "Group Vocal C",
        "demo.ch15": "Tail",
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

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "Follow",
        "master.rangeFollowHint":
            "Follows playback, range extends automatically. {n} analyzed segments · {t} total",
        "master.step1.desc":
            "Turn it on and play this range; the plug-in records loudness features — nothing is written to automation",
        "master.widthAngleHint":
            "Angle at the outermost position on each side; 0–90° (automation parameter width 0–150%)",
        "master.msHint":
            "−100 toward Mid / +100 toward Side; double-click to reset — drag to preview on the curve below",
        "master.distHint":
            "Bar height = level, horizontal position = pan; the line = stereo source width",
        "master.distAxis": "L · −50 · C · +50 · R",
        "master.transitionHint":
            "Values ramp between segments instead of jumping. Too short and the seam clicks; too long and the image smears between two spots.",
        "master.copyConfirmWarn":
            "Existing data will be overwritten — all 15 tracks' pan/vol, segment results and manual-edit marks of {name} are replaced. Undoable (Ctrl+Z).",
        "tracks.colLegend":
            "Vol = volume participation, whether this track joins level balancing (on by default) · Pan = auto-pan participation, whether it joins pan redistribution (stereo off by default, still level-balanced) · Freeze P/V = still analyzed but no longer driven; knob/fader unlock to manual (both switches share one per-track parameter)",
        "tracks.emptyGroup":
            "Group {g} has no inputs yet — insert SCVB Input in the last slot of each vocal track and select group {g}",
        // ---- T32 Wave 1 新增(EN 为 T32 自译,待人工审校)----
        "tracks.colCh": "CH",
        "tracks.labelPlaceholder": "Track {n}",
        "tracks.colVolPart": "Volume participation",
        "tracks.colState": "State",
        "tracks.colPan": "PAN",
        "tracks.colW": "W",
        "tracks.colVolLevel": "VOL / LEVEL",
        "tracks.colPrio": "PRIO",
        "tracks.colLead": "LEAD",
        "tracks.colVolExempt": "Vol",
        "tracks.colAutoPan": "Pan",
        "tracks.colFreezePan": "FRZ P",
        "tracks.colFreezeVol": "FRZ V",
        "tracks.colOn": "ON",
        "tracks.footNote": "15 tracks · scroll + sticky header",
        "tracks.emptyRoute":
            "Keep each vocal track's output routing pointed at this bus (do not change it)",
        "lowSample.full": "Low sample — the result may be unstable",
        "tracks.panAutoHint": "Auto mode — driven by the analysis curve",
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
            "Tick the lane headers on the left to choose target tracks (multi-select, shift for a range)",
        "wave.selChip": "The four actions above apply to {n} tracks",
        "wave.setRangeTip":
            "Working range set to selection {x}–{y} — Range switched to Manual; the selection itself is unchanged",
        "wave.originLegend":
            "origin: E = edited, C = created by hand; auto segments carry no badge. Locked segments survive re-identification.",
        "set.loudnessMode.title": "Which metric for segment loudness",
        "set.loudnessMode.note":
            "Sets the reference for segment-to-segment loudness normalization; changing it requires re-analysis.",
        "set.centerSlot.title":
            "What happens when tracks compete for the center slot",
        "set.centerSlot.note":
            "Fallback rule beyond Lead Lock and Lead Select; it does not affect the volume participation switch.",
        "in.chHint.groupEmpty":
            "This group has no Output yet — the channel table is empty",

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
        "master.step3.desc":
            "To record it as automation, arm this plug-in's parameters in the DAW with",
        "master.step3.descStrong":
            "Latch (recommended) or Write; never use Touch",
        "master.writeConfirm.ok": "Got it, start",
        "master.writeConfirm.undo": "Undo (back to Follow Host)",
        "master.groupEyebrow": "GROUP",
        "master.widthEyebrow": "WIDTH · MAX ANGLE",
        "master.rangeEyebrow": "RANGE",
        "master.distEyebrow": "PAN / VOLUME DISTRIBUTION",
        "master.curveEyebrow": "ANGLE-DOMAIN CURVE · ±12 dB",
        "master.curveLegendMs": "MS equivalent gain",
        "master.curveAxisX": "L −100 · −50 · C 0 · +50 · R +100",
        "master.curveEmptyHint": "Double-click to add a control point",
        "master.leadSelectDefaultNote": "0 = follow analysis (default)",
        "range.manual": "Manual",
        "master.rangeLoopStale":
            "Loop region is no longer valid — keeping the previous range",
        "master.rangeLoopMissing":
            "Host provides no loop region — the mode stays listed but cannot be selected",
        "master.rangeStart": "Range start",
        "master.rangeEnd": "Range end",
        "master.setToPlayhead": "Set to playhead",
        "master.barsEstimateNote":
            "Bar numbers are estimates; they become exact after playing this area",
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
            "Capture data exceeded 8 MB and was moved to an external file — sharing this project requires re-capturing",
        "scale.confirmBody": "Scale applied, reverting in {s} s",
        "scale.keep": "Keep",
        "master.printLock.group": "Can't switch group while printing",
        "master.printLock.version":
            "Can't switch version while printing — stop the transport first",
        "master.printLock.copy": "Can't copy a version while printing",

        // ---- T33 Wave 1 新增(EN 为 T33 自译,待 U17 人工审校;
        // 7 滑杆短标 / ORIGIN 为 mono 微标,三语同值)----
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        "wave.sldHangover": "HANGOVER",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
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
            "Arming only gates capture; the output range is unchanged",
        "wave.inspectorTitle": "Segment inspector",
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
            "Drag to move the boundary (snaps to energy valleys, hold Alt to disable); double-click to split here",
        "wave.reidentifyConfirm":
            "This clears {k} manual edit marks and recomputes; {l} locked segments stay unchanged. Continue?",
        "wave.clearCoverageConfirm":
            "This deletes the captured feature data for the selected tracks × selection. Continue?",
    },

    fr: {
        // 状态词规范(05 §5,512-524 行):连接类状态每态唯一用词,正文一律引用 key。
        // state.* 前缀为 T27 自定(05 只给表格未给 key);FR 除 passthrough/takenOver 取自术语表外为 T27 自译。
        "state.notConnected": "NON CONNECTÉ",
        "state.staleLink": "LIEN OBSOLÈTE",
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
        group: "Groupe",
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
        leadFollowAnalysis: "Suivre l'analyse",
        msBalance: "Balance M/S",
        stereoBadge: "ST",
        participateAutoPan: "Participation pan auto",
        trackWidth: "Largeur de piste",
        "tracks.monoWidthNoop": "Source mono : la largeur est sans effet en v1",
        "master.leadSelectHint":
            "Piste forcée au centre ; l'interrupteur de participation au volume est indépendant",
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
        autoStop: "Arrêt auto",
        reidentify: "Ré-identifier (incl. modifiés)",
        applyToSegments: "Appliquer",
        setAsRange: "Définir comme plage",
        clearCapture: "Effacer la capture",
        scale: "Échelle",
        language: "Langue",
        armedWaiting: "ARMÉ · EN ATTENTE",
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
            "Pilotage moteur {v} · plage {x}–{y} · 30 voies. Si Latch/Write est armé dans votre DAW, la lecture de cette plage écrasera l'automation existante ; sinon, écoute seule.",
        "footer.printing": "PILOTAGE MOTEUR {v} · {x}–{y}",
        "footer.printDone":
            "Passe couvrant {x}–{y}. Si vous enregistriez l'automation, repassez en Suivi hôte pour vérifier.",
        "out.master.writeConfirm.follow":
            "Pilotage moteur {v} · plage = toutes les zones analysées (suivi, {n} segments · total {t}) · 30 voies. Si Latch/Write est armé dans votre DAW, la lecture des zones analysées écrasera l'automation existante ; sinon, écoute seule.",
        "footer.printing.follow":
            "PILOTAGE MOTEUR {v} · SUIVI (ZONES ANALYSÉES)",
        "footer.printDone.follow":
            "Passe couvrant les zones analysées. Si vous enregistriez l'automation, repassez en Suivi hôte pour vérifier.",
        "wave.diffKept": "{k} segments modifiés/verrouillés préservés",
        "tracks.manualOverwriteConfirm":
            "Remplace tous les segments analysés de cette piste (version actuelle) par une valeur fixe. Annulable.",
        "tracks.manualOverwriteConfirm.locked":
            "(dont {l} segments verrouillés)",
        "in.pill.abiMismatch": "VERSION INCOMPATIBLE",
        "in.pill.srMismatch": "FRÉQ. INCOHÉRENTE",
        "tracks.manualDrivenHint":
            "Piste encore pilotée par une valeur fixe manuelle — ré-identifier cette piste ?",
        "in.releaseConfirm":
            "Libérer le canal {n} — cette piste repasse en direct",
        "wave.recaptureArmed": "RÉ-CAPTURE ARMÉE · {x}–{y} · {n} PISTES",
        "footer.recaptureOutputWarn":
            "Le moteur pilote toujours la plage globale — indépendamment de la sélection de ré-capture",

        // 首次启动引导页(05 §5,606-610 行)。
        // guide.rule1..9 不在此文件手写:由 scripts/gen-hard-rules.mjs 从 docs/USER_GUIDE.zh-CN.md#硬约束 生成写入(05 §5 / §0.1,禁止手抄)。
        "guide.title":
            "À lire : les neuf règles strictes de SCVB. En enfreindre une seule entraîne silence, panoramique erroné ou analyse échouée.",
        "guide.dontShowAgain": "Ne plus afficher",
        "guide.start": "Commencer",
        "set.reopenGuide": "Revoir le guide",

        // tour 询问步与通用控件(05 §5,611-616 行)。
        "tour.ask": "Envie d'une visite guidée d'une minute ?",
        "tour.ask.start": "Commencer la visite",
        "tour.ask.later": "Pas maintenant — à revoir dans Réglages",
        "tour.clickAnywhere":
            "Cliquez n'importe où pour continuer (sauf les boutons de cette bulle)",
        "tour.skip": "Passer",
        "tour.prev": "Précédent",
        "tour.next": "Suivant",
        "tour.done": "Terminé",
        "tour.demoBadge": "DONNÉES DÉMO",

        // tour 七步(05 §2.6 步骤表,421-429 行;J71① 定稿 7 步,增删步须回 05 改)。
        // zh 逐字取 §2.6「标题」「要点」列(去掉表内编辑注记);EN/FR 为 T27 自译,待人工审校。
        // 第 4 步措辞纪律:只说 DAW 侧开 Latch / Write,不得出现「写入完成」类表述(05 §2.6)。
        "tour.step1.title": "Voici la fenêtre principale de SCVB Output",
        "tour.step1.body":
            "Le déroulé va de gauche à droite : capture → analyse → sortie. Un jeu de démonstration de 15 pistes est chargé, puis retiré à la fin de la visite.",
        "tour.step2.title": "Étape 1 — Capture",
        "tour.step2.body":
            "Activez l'interrupteur de capture et lisez cette plage ; le plug-in enregistre les caractéristiques de loudness de chaque piste. Rien n'est écrit dans l'automation à cette étape.",
        "tour.step3.title": "Étape 2 — Analyse",
        "tour.step3.body":
            "Calcule hors ligne un plan de panoramique/volume pour chaque phrase. Un aperçu de l'impact s'affiche avant validation, et les segments modifiés ou verrouillés sont préservés.",
        "tour.step4.title": "Étape 3 — Sortie",
        "tour.step4.body":
            "Passez en PILOTAGE MOTEUR et les paramètres sont pilotés par le résultat de l'analyse. Pour l'enregistrer en automation, armez Latch ou Write côté DAW.",
        "tour.step5.title": "Page Pistes — réglages par piste",
        "tour.step5.body":
            "Les deux interrupteurs de gel reprennent cette dimension au moteur ; le potentiomètre et le fader se déverrouillent en manuel.",
        "tour.step6.title":
            "Page Formes d'onde — vérifier les résultats, modifier les segments",
        "tour.step6.body":
            "Chaque piste superpose les courbes en escalier pan/vol ; tracez une sélection pour ré-capturer ou ré-analyser uniquement cette partie.",
        "tour.step7.title": "Vous pourrez revoir cette visite ici",
        "tour.step7.body":
            "La visite peut être rejouée à tout moment, et les neuf règles strictes s'ouvrent dans le même bloc.",

        // tour demo 的 15 条轨名(J62;口径见 zh 侧同组注释)。FR 为 T27 自译,待人工审校;
        // L/R/C 照 master.distAxis 的 fr 侧取 G/D/C。
        "demo.ch1": "Voix principale",
        "demo.ch2": "Double voix principale",
        "demo.ch3": "Harmonie G",
        "demo.ch4": "Harmonie D",
        "demo.ch5": "Harmonie C",
        "demo.ch6": "Ad-lib 1",
        "demo.ch7": "Ad-lib 2",
        "demo.ch8": "Octave inférieure",
        "demo.ch9": "Octave supérieure",
        "demo.ch10": "Chœur G",
        "demo.ch11": "Chœur D",
        "demo.ch12": "Souffle",
        "demo.ch13": "Parlé",
        "demo.ch14": "Chœur C",
        "demo.ch15": "Fin de note",
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

        // 设计定稿回流新增词条(05 §5.2,639-658 行;design-v1/v2 定稿文案,C4 批量入表)。
        "range.followShort": "Suivi",
        "master.rangeFollowHint":
            "Suit la lecture, la plage s'étend automatiquement. {n} segments analysés · {t} au total",
        "master.step1.desc":
            "Activez et lisez cette plage ; le plug-in enregistre les caractéristiques de loudness — rien n'est écrit dans l'automation",
        "master.widthAngleHint":
            "Angle à la position la plus extérieure de chaque côté ; 0–90° (paramètre d'automation width 0–150 %)",
        "master.msHint":
            "−100 vers Mid / +100 vers Side ; double-clic pour réinitialiser — glissez pour voir la courbe ci-dessous",
        "master.distHint":
            "Hauteur = volume, position horizontale = panoramique ; le trait = largeur de la source stéréo",
        "master.distAxis": "G · −50 · C · +50 · D",
        "master.transitionHint":
            "Les valeurs suivent une rampe entre segments au lieu de sauter. Trop court, la jointure « saute » ; trop long, l'image se brouille entre deux positions.",
        "master.copyConfirmWarn":
            "Les données existantes seront écrasées — pan/vol des 15 pistes, résultats de segmentation et marques d'édition manuelle de {name} sont remplacés. Annulable (Ctrl+Z).",
        "tracks.colLegend":
            "Vol = participation au volume, si la piste entre dans l'équilibrage (activé par défaut) · Pan = participation au pan auto, si la piste entre dans la redistribution (stéréo désactivé par défaut, équilibrage conservé) · Gel P/V = toujours analysé mais plus piloté ; potentiomètre/fader déverrouillés en manuel (les deux interrupteurs partagent un même paramètre par piste)",
        "tracks.emptyGroup":
            "Le groupe {g} n'a encore aucune entrée — insérez SCVB Input dans le dernier emplacement de chaque piste vocale et sélectionnez le groupe {g}",
        // ---- T32 Wave 1 新增(FR 为 T32 自译,发布前必须人工审校,05 §5)----
        "tracks.colCh": "CH",
        "tracks.labelPlaceholder": "Piste {n}",
        "tracks.colVolPart": "Participation volume",
        "tracks.colState": "État",
        "tracks.colPan": "PAN",
        "tracks.colW": "W",
        "tracks.colVolLevel": "VOL / NIVEAU",
        "tracks.colPrio": "PRIO",
        "tracks.colLead": "LEAD",
        "tracks.colVolExempt": "Vol",
        "tracks.colAutoPan": "Pan",
        "tracks.colFreezePan": "GEL P",
        "tracks.colFreezeVol": "GEL V",
        "tracks.colOn": "ON",
        "tracks.footNote": "15 pistes · défilement + en-tête fixe",
        "tracks.emptyRoute":
            "Le routage de sortie des pistes vocales doit rester dirigé vers ce bus (ne pas le modifier)",
        "lowSample.full":
            "Échantillon insuffisant — le résultat peut être instable",
        "tracks.panAutoHint": "Mode auto — piloté par la courbe d'analyse",
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
            "Cochez les en-têtes de piste à gauche pour choisir les cibles (multi-sélection, maj pour une plage)",
        "wave.selChip":
            "Les quatre actions ci-dessus s'appliquent à {n} pistes",
        "wave.setRangeTip":
            "Plage de travail définie sur la sélection {x}–{y} — Range est passé en « Manuel » ; la sélection reste inchangée",
        "wave.originLegend":
            "origin : E = modifié, C = créé manuellement ; les segments auto n'ont pas de badge. Les segments verrouillés survivent à la ré-identification.",
        "set.loudnessMode.title": "Quelle mesure pour le loudness de segment",
        "set.loudnessMode.note":
            "Définit la référence de normalisation du loudness entre segments ; tout changement impose une ré-analyse.",
        "set.centerSlot.title":
            "Que faire quand plusieurs pistes se disputent le centre",
        "set.centerSlot.note":
            "Règle de repli au-delà du verrou lead et de Lead Select ; sans effet sur l'interrupteur de participation au volume.",
        "in.chHint.groupEmpty":
            "Ce groupe n'a pas encore d'Output — la table des canaux est vide",

        // T31 新增(Output 外壳 + Tab1 正式实现,05 §2.0/§2.1 语义 + design-v2 定稿文案)。
        // 立项理由与逐条出处见 scratchpad/t31/deviations.md「新增词条」节;
        // EN/FR 为 T31 自译,已入待人工审校清单(05 §5:fr 发布前须人工审校)。
        "common.cancel": "Annuler",
        "tab.master": "Général",
        "tab.tracks": "Pistes",
        "tab.wave": "Formes d'onde et segments",
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
            "Pour l'enregistrer en automation, armez les paramètres de ce plug-in dans le DAW en",
        "master.step3.descStrong":
            "Latch (recommandé) ou Write ; n'utilisez jamais Touch",
        "master.writeConfirm.ok": "Compris, démarrer",
        "master.writeConfirm.undo": "Annuler (revenir au suivi hôte)",
        "master.groupEyebrow": "GROUPE",
        "master.widthEyebrow": "LARGEUR · ANGLE MAX",
        "master.rangeEyebrow": "PLAGE",
        "master.distEyebrow": "RÉPARTITION PAN / VOLUME",
        "master.curveEyebrow": "COURBE ANGULAIRE · ±12 dB",
        "master.curveLegendMs": "Gain équivalent M/S",
        "master.curveAxisX": "G −100 · −50 · C 0 · +50 · D +100",
        "master.curveEmptyHint":
            "Double-cliquez pour ajouter un point de contrôle",
        "master.leadSelectDefaultNote": "0 = suivre l'analyse (par défaut)",
        "range.manual": "Manuel",
        "master.rangeLoopStale":
            "La boucle n'est plus valide — la plage précédente est conservée",
        "master.rangeLoopMissing":
            "L'hôte ne fournit pas de boucle — le mode reste affiché mais non sélectionnable",
        "master.rangeStart": "Début de plage",
        "master.rangeEnd": "Fin de plage",
        "master.setToPlayhead": "Définir sur la tête de lecture",
        "master.barsEstimateNote":
            "Les numéros de mesure sont estimés ; ils deviennent exacts après lecture de cette zone",
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
            "Les données de capture dépassent 8 Mo et ont été déplacées dans un fichier externe — un envoi à un tiers nécessite une nouvelle capture",
        "scale.confirmBody": "Échelle appliquée, retour dans {s} s",
        "scale.keep": "Conserver",
        "master.printLock.group":
            "Changement de groupe impossible pendant l'impression",
        "master.printLock.version":
            "Changement de version impossible pendant l'impression — arrêtez d'abord le transport",
        "master.printLock.copy":
            "Copie de version impossible pendant l'impression",

        // ---- T33 Wave 1 新增(FR 为 T33 自译,**发布前必须人工审校**,05 §5 / U17;
        // 7 滑杆短标 / ORIGIN 为 mono 微标,三语同值)----
        "wave.sldThreshold": "THRESHOLD",
        "wave.sldHysteresis": "HYSTERESIS",
        "wave.sldHangover": "HANGOVER",
        "wave.sldPadPre": "PAD PRE",
        "wave.sldPadPost": "PAD POST",
        "wave.sldSensitivity": "SENSITIVITY",
        "wave.sldMinSeg": "MIN SEG",
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
            "L'armement ne concerne que la capture ; la plage de sortie est inchangée",
        "wave.inspectorTitle": "Inspecteur de segment",
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
            "Glissez pour déplacer la limite (aimantée aux creux d'énergie, Alt pour désactiver) ; double-clic pour scinder ici",
        "wave.reidentifyConfirm":
            "Efface {k} marques d'édition manuelle et recalcule ; {l} segments verrouillés restent inchangés. Continuer ?",
        "wave.clearCoverageConfirm":
            "Supprime les données de caractéristiques capturées pour les pistes sélectionnées × la sélection. Continuer ?",
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
