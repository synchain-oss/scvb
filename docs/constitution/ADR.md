> 本文件是 masterPlan/constitution 的仓内只读副本，改动须走修宪流程（sha256 同步由 scripts/check-constitution-sync.ps1 断言）。
# SCVB 架构决策记录(ADR)——P1 宪法,起草文档不得与之矛盾

状态:**v2.0**(2026-08-11,J57-J62 用户变更修宪,正文已就地改写;历史修订节见文末;裁决依据 plan/adjudications.md)。起草 agent 如认为某条有误,在自己文档末尾「对宪法的异议」一节提出,**不得**silently 偏离。GitHub org = `synchain-oss`。

依据:masterPlan/decisions.md(D1-D8)+ research/01~10 报告。

---

## ADR-001 插件形态:两个插件目标 + 共享核心库
一个仓库,CMake 三个目标:`scvb_core`(静态库:DSP/分析/IPC/状态/版本引擎,可离线单测)、`SCVB Input`(VST3)、`SCVB Output`(VST3)。JUCE 一个 target 一个插件,不做单插件双模式(用户会同时开十几个实例,双模式易误操作)。
- PLUGIN_CODE:Input=`Scvi`,Output=`Scvo`(Snb1 已被 Bridge 占用;厂商码统一 `Snch`)
- BUNDLE_ID:`com.synchain.scvb.input` / `com.synchain.scvb.output`
- PRODUCT_NAME:"SCVB Input" / "SCVB Output"(显示名带 Synchain 由厂商列免)…最终名在 05/UI 文档定,插件码/bundle id 冻结

## ADR-002 路由架构(=D5,细则)
- Input 插在人声轨最后一个推子后插槽:捕获 → 写共享内存 → 向下游输出**静音**(保住 DAW 依赖图排序)
- Output 插在总线第一格:按自身 block 的时间线区间从各 channel 环形缓冲读取 → 每轨 gain/pan → 求和 → 替换总线输入
- 人声轨必须保持 DAW 路由指向该总线(用户手册红字;Output 检测不到对应时间线数据时 UI 警告)
- Output 不向 host 报告额外 latency(对齐靠时间线寻址,不靠 PDC);Input 报告 latency=0
- 多 Output 实例:registry 里 channel 归属唯一,第二个实例抢占同一 channel 时 UI 警告并只读
- 失准语义:缺口→该轨该块静音+警告计数;重叠→取时间线正确者

## ADR-003 声道语义:v1 支持 mono 与 stereo 源(v2.0/J57 改写)
- Input 检测轨道布局:mono 轨捕获单通道;**stereo 轨捕获/转发双通道**(IPC 环 interleaved,channels=1|2)
- Output 总线固定 stereo;mono 源经 equal-power pan 摆入 L/R;**stereo 源用 dual-pan+width 模型**(L/R 各自 equal-power pan:pan 参数=弧中心,每轨 width 参数=张开度,width=0 收成 mono;**不用 M/S 拉宽**,避极性反转)
- 特征提取按 BS.1770 多通道求和;stereo 轨参与音量平衡按 J64 近似(求和能量+摆位理论分布)

## ADR-004 参数布局:自动化参数全部在 Output(冻结顺序;v2.0/J59 改写)
- 自动化参数共 **123 个**:全局 3(width/ms_balance/lead_select)+ 15 轨 × 2 版本 × (Pan+Vol+Width+Freeze)= 120。详见 params-v0.md v2.1,ID/顺序冻结点=首个公开 rc(J21),全部 versionHint=1
- 宿主可见 124(+bypass,J02);Ableton Live 128 上限余 **4(J65 后预算封顶)**;**绝对禁止再加自动化参数**,新需求一律走 state
- 引擎 write 仅打印 30 条(15 轨×pan/vol,J63);width/ms_balance/lead_select 可被用户自动化但引擎不打印(host 恒权威)
- Input 无自动化参数;channel id / 输入端配置全在 state
- 曲线编辑器、阈值、分段灵敏度、优先级、主唱锁、配对、每轨 auto 开关、采集/输出开关、version 选择:全部 state(非自动化)
- 配置类数据的唯一真源在 **Output state**(大脑);Input state 只存 channel id + UI 偏好。优先级等即使在 Input UI 上显示/可调,实际读写的是 Output 的值(经控制面 IPC)

## ADR-005 版本系统:内部曲线是真身,参数是打印头
- 分析结果 = 每版本每轨的时间线分段曲线(pan/vol),存 Output state
- 90 个轨道参数是可自动化面;其中 30 条(pan/vol)兼为"打印头"(J63),width 类 host 恒权威:输出开关 ON 时引擎按播放位置把当前版本曲线值经 gesture 写到参数(供 DAW write 录制);OFF 时参数忠实跟随 host(读 DAW 自动化)
- version 复制 = state 内曲线+配置复制,零 gesture、零自动化污染
- `version_active` 为非自动化 state(否则会被 write pass 自录进自动化)
- DSP 取值仲裁:输出开关 ON → 引擎值直接进 DSP(参数只是对外打印);OFF → host 参数值进 DSP

## ADR-006 自动化写入
- gesture 三段式(beginChangeGesture/setValueNotifyingHost/endChangeGesture),**只在消息线程**(50Hz Timer);音频线程只发布 playhead 快照(SPSC)
- 推荐用户用 DAW 的 Write/Latch;文档标注各 DAW 已知坑(Cubase 车道位置、REAPER 关 GUI 不写、Pro Tools 循环只录第一遍等,见 research/08)
- host echo 防回环:写入期间忽略参数回调对引擎的影响(引擎为源);写入结束后参数回归 follow 语义

## ADR-007 采集:存特征不存音频
- 特征流(per-hop,hop=10ms@48k):K-weighted mean-square + peak;VAD 存连续后验(阈值离线可调,无需重采集)
- 采集开关 ON 且 transport 播放时才写入;按时间线寻址合并(重播覆盖同区间,新数据优先)
- 持久化分层:分段/区间/2 版曲线+配置(≈几百 KB)进 state;特征流压缩后默认内嵌 state,超 8MB 转 sidecar 文件(sessionGUID 自生成,存于 state;sidecar 放系统应用数据目录)
- 局部重采集/重分析:按(轨道 × 时间区间)为单位失效与重算,**绝不触碰其他区间已有结果**;分析结果数据结构按轨道独立分段列表组织

## ADR-008 VAD 与分段
- v1:K-weighted 能量域双阈值 + 滞回 + hangover + 前后 padding,默认宁多勿少,阈值/灵敏度用户可调可实时预览(因为存的是连续特征)
- 分段:能量谷检测 + 最小段长 + 换气容忍;用户手动改边界后该段标记 manual,自动重分析不覆盖 manual 边界(除非用户显式要求重识别)
- v2 升级路径:Silero VAD(ONNX,MIT)作精确模式,不进 v1

## ADR-009 响度度量
- 段响度 = 段内 ungated K-weighted 积分响度(EBU Tech 3341:M/S 本身不 gating)
- K-weighting 自研(两个 biquad,系数来自 BS.1770),libebur128(MIT)作为测试对拍参考,不进运行时依赖
- 所有平衡计算在线性能量域做(O(N) 算术)

## ADR-010 Pan 数学
- equal-power pan(sin/cos),Output 内部实现,与宿主 pan law 无关(前提:人声轨与总线的宿主 pan 保持居中,文档写明)
- 期望宽度 = 几何角度缩放(把分配角乘以 width 系数),**不用 M/S 拉宽**(width>1 会对幅度声像源造成极性反转,见 research/07)
- pan 角度域增益曲线:x=pan 角[-100,+100],y=gain dB;点类型 bell/shelf/cut 三种,带 Q;实现按 EQ 曲线插值同构
- 自动分配:规则槽位生成(主唱锁中、成对对称、优先级高→角度大、中心可分配)+ 匈牙利指派;L/R 平衡用 ρ=cos(2θ) 杠杆闭式解迭代 3-5 次(research/07)
- 段间过渡:在段边界(停顿/换气)切换,默认 ramp 80ms(可调),避免可闻跳变

## ADR-011 技术栈与质量门
- JUCE 8.0.8 / CMake ≥3.22 / C++17 / MSVC 静态 CRT(对齐 Bridge)
- UI:JUCE WebView(WebView2 静态 loader),纯 HTML/CSS/JS,复用 Bridge 的资源嵌入+resource provider+native function 桥模式与缩放机制(web 固定设计盒 + CSS zoom + setSize 同步 + 10 秒确认防呆)
- 浏览器预览:复刻 web-contract 模式(mock 桥后端),axe-core 可跑无障碍检查
- 单测:Catch2 测 scvb_core(VAD/分段/响度/分配/曲线插值全部离线可测);pluginval strictness 5(--skip-gui-tests)进 CI
- clang-format(仓库带 .clang-format,风格对齐 Bridge 现状);/W4(MSVC)+ -Wall -Wextra -Wpedantic
- IPC 结构体:static_assert 布局与 lock-free;所有跨线程原子 std::atomic

## ADR-012 SCVB 仓库结构(骨架)
```
scvb/
├── CMakeLists.txt          # 顶层:core + input + output 三 target
├── src/core/               # scvb_core:dsp/ analysis/ ipc/ state/ engine/
├── src/input/              # Input 插件(Processor/Editor)
├── src/output/             # Output 插件(Processor/Editor)
├── web/                    # WebView UI(input/ output/ shared/ fonts/)
├── web-preview/            # 浏览器 mock 预览(复刻 web-contract 模式)
├── tests/                  # Catch2
├── scripts/                # build.ps1 一键构建等
├── docs/                   # 架构/契约/DAW 指南/发布
└── .github/workflows/      # build-vst3 / review bots / branch-gate
```

## ADR-013 分支与任务命名(三仓库)
- feature 主支线:`feature/v1`;子支线:`feat/<TASK-ID>-<slug>`(如 `feat/T03-ipc-ring`)
- 任务编号 `T01…`,由 07-execution-plan 统一分配;Bridge 抽取仓任务 `B01…`,CLI 仓 `C01…`,主仓善后 `M01…`
- commit 规范沿用 `type(scope): 中文描述`

## ADR-014 前置 spike(实施最前,fail-fast,全部在 feature/v1 下)
- S1 路由可靠性:最小 Input/Output 对,Reaper/Cubase/Live/Studio One × 实时/离线渲染,验证时间线对齐与静音通路排序假设
- S2 自动化写入:123 参数骨架 + gesture 写入,同矩阵验证 write/latch 录制与回读
- S3 WebView 承载力:一页 30+ 控件 + 曲线画布 + 波形渲染的帧率/内存
- S4 状态体量:满配 state(2 版曲线+特征内嵌)save/load 完整性与速度
任一 spike 失败 → 停下修正架构再继续(S1 失败的备选:预选 F2「仅分析+建议表 CSV 导出+Input 就地 gain」,详见 11-risks §4 与裁决 J25)

---

# v1 修订(2026-08-10,统稿裁决,编号对应 plan/adjudications.md)

- **[J12→ADR-002 实质修订]** Input 检测不到健康 Output 时输出**直通**(80ms ramp + 5s 滞回防抖),仅在连接健康时静音。消除「无 Output=全轨静音」事故面;S1 增加该切换的验证项。
- **[J14→ADR-011/014]** 增补:pluginval 双插件对偶场景「对端缺席时不阻塞不崩溃」为 S1 测项与常规 CI 关注点。
- **[J15→ADR-014]** 停线粒度:S1 失败=停 T 线并 48h 内完成兜底选型;S2 失败=按 DAW 分 Tier 降级(Cubase 单独决策);S3/S4 失败=局部停(UI/state 域)。
- **[J16→D2 补充]** spike 产物:报告 merge 进 docs/spikes/;验证代码放 spikes/ 目录,对应正式任务完成时删除。
- **[J17→ADR-013]** 任务命名空间增 **O**(org/共享基建);编号分配权在 07-execution-plan(已行使,其映射表为准)。
- **[J13→ADR-013]** feature 主支线命名按仓库语义:SCVB=`feature/v1`,Bridge/CLI=`feature/extraction`。
- **[J18→ADR-009]** libebur128 对拍口径:M(400ms)+ungated,禁用 global gated。
- **[J19→ADR-011]** 测试层增补 L1:双进程 IPC 契约测试(用例见 10-validation §2)。
- **[J20→ADR-011/新安全条款]** 禁止使用 `pull_request_target` 向 fork PR 暴露 secrets;此为三仓库 CI 安全禁令。
- **[J22→ADR-001 澄清]** 「三目标」指三主目标;juce_add_binary_data 资源目标与 scvb_tests 等辅助目标不违宪。
- **[J28→D2 澄清]** branch-gate:v1 即启用 dev 来源规则(仅接受 feature/*),prod/stage 规则待发版引入。
- **[J08→ADR-005/006 观察项]** S2 显式观察「输出开关 ON + 宿主 Read 模式」行为;异常则增补非 gesture 试听档(纯 state 字段,不动 81 参数)。
- **[J25→ADR-014 清理]** 删除 v0 中「要求用户手动对齐」备选表述(无可操作含义)。

## v1.1 补充(2026-08-10,R1 补裁,编号见 adjudications.md R1 补充裁决)

- **[J32→ADR-002]** J12 切换协议:5s 滞回仅作用于 静音→直通 方向;直通→静音在确认健康后立即 80ms ramp;Output 置 mask 位后延迟 ≥200ms 再注入(或等 Input muted 确认位);S1 增双路叠加验证项。
- **[J31/J41→ADR-011 安全条款扩展]** fork PR 统一门禁:review bot 仅 same-repo PR;fork PR 只跑无 secrets 构建/测试;branch-gate 仅约束 same-repo 分支命名。
- **[J37→ADR-012]** 仓库骨架补开源必备文件:LICENSE、.clang-format、.gitignore、.gitattributes、CLAUDE.md、CONTRIBUTING.md、CODE_OF_CONDUCT.md、SECURITY.md、.github/ISSUE_TEMPLATE(12 §1.1 八件套为准)。
- **[J38→ADR-011]** clang-format 版本钉 **18.1.8**,CI 与本地同版本。
- **[J39→ADR-013 附注]** required checks 宪法化:dev required=构建+测试+人审;bot 非 required(U8 可改)。
  **[J49 口径限定(2026-08-11)]**:「人审」指 D2 的用户人工审核与 merge 动作(由 `required_conversation_resolution: true` + 用户亲自 merge 承载),**不得**配置 `required_approving_review_count`——GitHub 不允许 PR 作者 approve 自己的 PR,单人 org 下会令所有 PR 永久无法合并(06 §7.2 的「关键坑」)。将来有第二位维护者时再开启 approval 强制。
- **[J42→ADR-013]** B/C 线主支线 `feature/extraction` 在各线首任务(仓库脚手架)创建,后续任务 base 于它。
- **[J34→ADR-008]** segments 语义字段改 origin+locked(见 params v1.1)。

## v1.2 补充(2026-08-10,收口后裁决)

- **[J45→ADR-002]** 「人声轨最后一个推子后插槽」措辞修订为「**人声轨插件链最后一格**」(多数宿主无推子后插槽概念);「推子 0 dB」仅为 null test 可比性前提(10 文档),非产品要求;逐宿主插槽位置指南归 DAW_COMPATIBILITY 文档。
- **[J47→交互真源确认]** 分段微调的松手档语义以 05 §1.4/§2.3 为唯一真源(300ms 防抖自动应用,仅 origin=auto 未 locked;PRINT/分析中抑制)。

---

# 修宪流程(P4 成文,G7;此前 P1-P4 实际运转的机制归拢)

1. **提案入口**:实施 agent 发现宪法有误 → 在 PR 描述单列「对宪法的异议」(代码仍按现行宪法写,不等修宪——07 §5.4 既有约定);调度者(Fable)收敛异议。
2. **裁决人**:技术类由 Fable 裁决;涉及用户既定决策(decisions.md D 系)或产品形态的转用户(进 user-decisions)。
3. **编号**:裁决续接 J57 起,记入 plan/adjudications.md(编号+来源+事项+A/N/C/U+说明)。
4. **落地清单**(一次修宪必须全做):①改 constitution/ 原文并升版本号(修订节追加,不改历史);②同步三仓 `docs/constitution/` 只读副本(sha256 随之更新,`check-constitution-sync.ps1` 才能过);③更新 plan/INDEX.md 口径行;④受影响的计划文档/任务卡同步(修订记录留痕);⑤通知在跑的 agent(经 progress.md 阻塞原因列或直接消息)。
5. **版本历史**:masterPlan 已本地 git 化(P4/G14),每次修宪一个 commit,message 带 J 编号。

---

# v2.0 修订(2026-08-11,用户审阅后变更,J57-J61;首次按「修宪流程」执行)

- **[J57→ADR-003 改写]** v1 **支持立体声源**:Input 检测 stereo 轨→捕获/转发双通道(IPC 环 channels=1|2);Output 对 stereo 源用 **dual-pan+width** 模型(L/R 各自 equal-power pan,pan 参数=弧中心,每轨 width 参数=张开度,width=0 收成 mono;**不用 M/S 拉宽**,避极性反转);特征提取按 BS.1770 多通道求和。「真立体声延后 v2」的原限制作废。
- **[J59→ADR-004 改写]** 参数布局 v2.0:**2 版本 × 15 轨 × (Pan/Vol/Width) + 全局 width/ms_balance/lead_select = 93 声明/94 宿主可见**(Live 余 34);versions 4→2(ADR-005 的曲线真身/打印头/复制语义不变,仅版本数减半);轨道数 10→15(IPC registry 15 slots、UI、槽位算法同步)。
- **[J58→新增语义]** `lead_select` 全局自动化参数(0=遵循分析,1-15=强制该轨实时居中,其余轨不重分布);与分析期 `lead_lock`(逐段)双层;`lead_vol_exempt` 为**独立**每轨选项,不与任何 lead 机制强制关联(用户澄清)。
- **[J60→ADR-010 补充]** 自动分配:每轨 `participate_in_auto_pan` 开关(stereo 默认 false/mono 默认 true);参与的 stereo 轨以**中心点**入槽位分配(不区间化);全部轨参与 L/R 音量平衡(stereo 按实际双通道能量)。
- **[J61]** 连锁修订:ipc 升 v1.4(15 slots/stereo 环);01/02/03/04/05/07/10/11+HANDOFF 按 J57-J60 修订。
