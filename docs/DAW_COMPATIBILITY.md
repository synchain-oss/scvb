> 状态: 演进中
> 最后更新: 2026-09-28(首个公开发布前;版本号以 `CMakeLists.txt` 的 `project(SCVB VERSION …)` 为准,本文不写死)
> 真源: 本文件

# SCVB DAW 兼容矩阵与支持等级表

本文是 SCVB 的 DAW 兼容性唯一文档(真源 = masterPlan 12 §3.3)。结构 = 一张总矩阵 + 每 DAW 一节 + 通用坑清单;末尾附「README 支持等级表(供 T39b 转贴)」小节,与总矩阵严格一致。

> **口径说明(务必先读)**:
> - 证据分两层。**spike 层**:**S1 路由可靠性 spike(T02)** 2026-08-16/17 的真机实测(Cubase 14 / 15,REAPER 7 兜底;证据见 [S1-daw-checklist.md](spikes/S1-daw-checklist.md) 完成列与 [S1-routing.md](spikes/S1-routing.md) §6 收口报告)。**成品层**:**T37 成品测试包**(Input / Output / Monitor 成品插件)在 **Cubase 15 Pro** 上的多轮真机实测,2026-08 下旬的 v5.6 测试包起,到 2026-09-28 的 v5.6.19 测试包(测试包编号是内部轮次号,不是发布版本号)。
> - **自动化 Write 录制**列:**S2 自动化写入 spike(T03)** 的逐宿主落点回填**没有做**([S2-automation.md](spikes/S2-automation.md) §6、[S2-daw-checklist.md](spikes/S2-daw-checklist.md) §4 汇总格仍空);**Cubase 这一格的证据来自成品测试包**(见矩阵脚注 7),其余宿主仍为「未验证」,并标注已知风险(RD-04/RD-02)。
> - **成品层只覆盖 Cubase 15。** REAPER 只有 spike 层证据;Ableton Live 与 Studio One 两层都没有(U27)。spike 结论是架构级验证,不替代成品复测。
> - 状态图例:✅ 已验证 / ⚠️ 未验证(原因见单元格)/ ➖ 不适用。

## 1. 总矩阵

| DAW | 版本 | 路由(实时) | 路由(离线渲染) | 自动化 Write 录制 | state 往返 | 验证日期/SCVB 版本 |
|---|---|---|---|---|---|---|
| **Cubase(主测)** | 14 / 15 | ✅ 已验证 | ✅ 已验证 | ✅ 已验证(Cubase 15 成品;见脚注 7) | ✅ 已验证 | 2026-08-16/17(S1 spike v8/v10);2026-08 下旬 – 2026-09-28(Cubase 15 Pro 成品测试包 v5.6 – v5.6.19) |
| **REAPER** | 7 | ✅ 已验证(anticipative 三档 + dedicated process) | ✅ 已验证 | ⚠️ 未验证(S2 待上机;RD-04 已知风险) | ⚠️ 未验证(S1 未单跑 state 往返) | 2026-08-16(S1 spike v8) |
| **Ableton Live** | 12 | ⚠️ 未验证(U27:S1 跳过) | ⚠️ 未验证(U27:S1 跳过) | ⚠️ 未验证(S2 可选未做;RD-02 已知风险) | ⚠️ 未验证(未上机) | — |
| **Studio One** | 6 | ⚠️ 未验证(U27:仅作可选对照,未做) | ⚠️ 未验证(U27:仅作可选对照,未做) | ⚠️ 未验证(S2 可选未做) | ⚠️ 未验证(未上机) | — |

**矩阵脚注**:

1. **Cubase「路由(实时/离线)」** :S1 的 C-3(Real-Time / Export Mixdown 双 null)、C-4(ASIO-Guard)、C-5(实时全曲)、C-6(loop×100)、C-7(定位×20)、C-8(buffer 四档)、C-9(采样率切换)、C-10(solo/mute)全部 ✅;上游高延迟插件(PDC)下 rt/offline 零错位(offset 0)。
2. **Cubase「state 往返」** :C-12(强杀宿主重开工程自动重连)+ G-3(存工程关 DAW 重开时间线锚定恒等)实测通过;成品层:v5.6.19 测试包 A1–A7(各项设置存工程、完全关掉 Cubase 重开后保留)通过。
3. **REAPER「路由(实时)」** :R-2/R-3/R-4(anticipative FX 开/最大/关)+ R-12(插件 Run as dedicated process)实测通过;基础实时播放并入 Cubase C-5 口径,未在 REAPER 单独重跑。
4. **REAPER「路由(离线渲染)」** :R-5(File→Render 1× 与 Full-speed Offline 各一次,与 ref_A null)实测通过。
5. **版本口径** :Studio One 6 为 U9(用户 2026-08-11 决定)的权威版本;S1/S2 的 spike 清单曾把 Studio One 标为「7(可选对照)」且未执行,以 U9 的「6」为准。
6. **Live / Studio One 的「未验证」原因** :U27(用户 2026-08-14 决定)S1 跳过 Live、Studio One 仅作可选对照;此后的成品测试包也只在 Cubase 15 上跑。Live 的设备停用行为(L-5)是按设计推出来的,没有在 Live 上实测过。
7. **Cubase「自动化 Write 录制」** :成品层证据,Cubase 15 Pro。① 2026-08 下旬 v5.6 测试包的完整功能测试清单第 32–34 条通过(宿主自动化 Read 档优先;开 Write → 播放 → 车道真被写入、冻结维度写成平直线;打印密度/平滑合理、关掉 write 后手绘车道可覆盖),此后各版全量清单把这三条归入「已过存档」;② v5.6.19 测试包 B51(写自动化中停用 Output、再启用后照常写自动化)通过。**RD-01**(Cubase 录不进插件自发的参数变化)在 Cubase 15 上**没有复现**;Cubase 14 没有用成品复测过。
8. **Cubase「路由(离线渲染)」成品层** :v5.6.19 测试包 B53,同一工程的实时导出与离线导出逐样本对齐(互相关最小在 lag 0),两份都没有断音;差异只在开头与段切换处的过渡段。

## 2. 每 DAW 一节

### 2.1 Cubase(主测宿主)

#### 建总线

1. Project → Add Track → Group Channel,建一条 **stereo 总线**(命名如 VOX BUS)。
2. 把每条人声轨的输出(Output Routing)全部指向 VOX BUS,总线输出保持送主输出。

#### 放 Input

每条人声轨插件链(**Insert** 架)的**最后一格**放 **SCVB Input**(J45 措辞:「人声轨插件链的最后一格」)。

#### 本宿主该位置的叫法 / 是否推子前后

Cubase 的 Insert 架有**可移动的 pre/post 分隔线**——「最后一格」可能落在 **post-fader 区**。**必须确认 Input 位于 pre-fader 区的最后一格**;若误置于 post 区,轨道推子会进入采集通路,破坏「总线集中处理」的假设(J45 特别提示)。

#### 放 Output

在 VOX BUS 的 Insert **第一格**放 **SCVB Output**。

#### 确认宿主 pan 居中

所有人声轨与 VOX BUS 的 pan 居中;stereo 轨的宿主平衡器居中、源宽度不被塌成 mono。

#### 录自动化

推荐 **Write / Latch**(不要 Touch)。打印的自动化**默认不在可见车道**:位于 Show/Hide Automation → More → Ins → 插槽号 → 参数名 的隐藏车道之下(R3,所有第三方插件统一行为)。

#### 离线渲染注意事项

- Export → Audio Mixdown 是正确导出路径(S1 C-3 已验)。
- Render in Place / Freeze 对含 Input 的单条轨会得到**静音**产物(Direct Offline Processing 处理的是音频事件本身、不经过轨道的 Insert 链,不在此列);若渲染结果替换原素材,原素材就被静音替换了(⚠ 用户数据,见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-4)。请对**总线整体导出**。

#### 已知坑

- **RD-01 · 插件自发参数变化录不进**(Cubase 15 成品实测未复现,见 §1 脚注 7;Cubase 14 未用成品复测):有 JUCE 开发者报告 Cubase 12+ 下 setValueNotifyingHost 发起的自动化录不进,**无公开确定解**。万一在你的环境里遇到,兜底 = 建议表 + CSV 导出(T41)。出处:masterPlan 11-risks RD-01、[S2-automation.md](spikes/S2-automation.md)。
- **Ins 隐藏车道**(R3):自动化写进隐藏车道,用户误以为没写成功。见上文「录自动化」。
- **pre/post 分隔线**(J45):Input 误入 post 区会把推子带进采集通路。
- **Render in Place + Ctrl-Z 复制体卡死**:已修复(v7)。见 [S1-routing.md](spikes/S1-routing.md) §6.1 与本文 §3.1。

### 2.2 REAPER

#### 建总线

新建一条 **stereo track** 作总线(建议用 folder track / 手动 send),把所有人声轨的输出 send 到该总线。

#### 放 Input

每条人声轨 **FX chain 的最后一格**放 **SCVB Input**(J45:REAPER 无 post-fader slot 概念,「插件链最后一格」即最后插入的 FX)。

#### 本宿主该位置的叫法 / 是否推子前后

REAPER 的 FX chain 无 pre/post 分隔线概念;「最后一格」= FX chain 末尾,推子位于 FX 链之后,天然满足「Input 在推子前」。

#### 放 Output

总线轨 FX chain 的**第一格**放 **SCVB Output**。

#### 确认宿主 pan 居中

所有人声轨与总线轨 pan 居中;stereo 轨平衡器居中。

#### 录自动化

推荐 **Write / Latch**。REAPER 有「**关闭插件 GUI 后不写自动化**」的历史坑(RD-04,VST 时代确认,VST3 待复验);若打印落点为空,先做:Preferences → Plug-ins → VST → VST compatibility → Parameter automation notifications = process all notifications,并**打印期间保持 SCVB Output 窗口打开**。

#### 离线渲染注意事项

File → Render 正确;S1 R-5 已验 1× 与 **Full-speed Offline** 两档 null 通过。单轨 Freeze / Render in Place 会得到静音文件。

#### 已知坑

- **RD-04 · 关 GUI 不写自动化**:见上文「录自动化」的宿主端解法。出处:masterPlan 11-risks RD-04、[S2-daw-checklist.md](spikes/S2-daw-checklist.md) R-1..R-4。
- **anticipative FX**:S1 R-2/R-3/R-4 三档(开/最大/关)均 gapCount 0;render-ahead 实测上限 5000ms 下环有余量。
- **Run as dedicated process**:R-12 已验跨进程共享内存仍工作。
- **同机双工程/双 tab 抢 channel**:v1 明示同机同时只支持一个使用 SCVB 的工程。

### 2.3 Ableton Live

#### 建总线

建一条 **Audio Track** 作总线(或使用 Return Track),把所有人声轨的输出 send 到该总线。

#### 放 Input

每条人声轨 **device chain 的最后一格**放 **SCVB Input**(J45:Live 无 post-fader slot 概念)。

#### 本宿主该位置的叫法 / 是否推子前后

Live 的 device chain 无 pre/post 分隔线;「最后一格」= 设备链末尾,推子在设备链之后。

#### 放 Output

总线轨 device chain 的**第一格**放 **SCVB Output**。

#### 确认宿主 pan 居中

所有人声轨与总线轨 pan 居中。

#### 录自动化

推荐 **Write / Latch**。**128 参数上限(RD-02)**:SCVB 声明 123 + wrapper 合成 bypass = **124 宿主可见,Live 128 上限余 4**(封顶,不得再加自动化参数)。打印过程中 **Re-Enable Automation** 按钮亮起属正常现象,打印完点击它(或重新播放)即可恢复读取。

#### 离线渲染注意事项

Export Audio/Video 正确;**Freeze & Flatten** 对含 Input 的轨道会得到静音产物。**设备停用**(J52):Live 设备停用不经 bypass、直接停止调用 processBlock,SCVB 检测到后会通知各 Input 转直通,人声约 **~5.5 秒**内恢复(未经平衡的原始声像);A/B 请用 SCVB 面板轨道开关,不要停用设备。

#### 已知坑

- **RD-02 · 128 参数上限**(124 口径,余 4)。出处:masterPlan 11-risks RD-02。
- **Re-Enable Automation 频繁亮起**(R4/R9)。出处:masterPlan 03 §4.4。
- **设备停用 ~5.5s 直通兜底**(L-5/J52)。
- **S1/S2 均未上机**:U27(2026-08-14)决定 S1 跳过 Live,L-5 已挂 T24。

### 2.4 Studio One 6

#### 建总线

建一条 **Bus** track,把所有人声轨的输出 send 到该总线。

#### 放 Input

每条人声轨 **插件链的最后一格**放 **SCVB Input**(J45)。

#### 本宿主该位置的叫法 / 是否推子前后

Studio One 的插入链无 post-fader slot 概念;「最后一格」= 插件链末尾。

#### 放 Output

总线轨插件链的**第一格**放 **SCVB Output**。

#### 确认宿主 pan 居中

所有人声轨与总线轨 pan 居中。

#### 录自动化

**自动化模式必须在插件窗口内设 Write/Latch**(仅在轨道上开不够)。这是 Studio One 的已知行为,落点由 S2-S1 验证(待上机)。

#### 离线渲染注意事项

Song → Export Mixdown 正确;**Dropout Protection** 是唯一已知会出现**异 block size** 的宿主,值得在四档各跑一轮确认 gapCount 全 0(S1-S1 待上机)。

#### 已知坑

- **插件窗口内设 Write/Latch**(S2-S1)。出处:[S2-daw-checklist.md](spikes/S2-daw-checklist.md) S-1。
- **Dropout Protection 异 block size**。出处:[S1-daw-checklist.md](spikes/S1-daw-checklist.md) §6。
- **S1/S2 均未上机**:U27(2026-08-14)决定 Studio One 仅作可选对照,未执行。

## 3. 通用坑清单

### 3.1 已修复(S1 修复链 v1..v10,按「已修复(vX)」归类)

1. **同 pid 重认领 → connected_mask 锁 0 永久静音**(已修复 v2/v10):旧 claimInput 把「同 pid 重认领」误判为认领成功,导致每个新实例都把已占 slot 判「认领成功」,自动选槽永远停在最小号 slot;切采样率/导出后 connected_mask 锁 0,整条通路永久静音。v2(PR #23)先修同 pid 重认领与心跳跨宿主挂起存活;v10 最终把「复制体同 pid 抢槽」改为冲突语义(claimInput 废除同 pid 刷新,强制认领改冲突)。出处:[S1-routing.md](spikes/S1-routing.md) §6.1、[S1-daw-checklist.md](spikes/S1-daw-checklist.md) §0d。
2. **导出纯零**(已修复 v2):即上述问题的用户可见表现——C-3(Export Audio Mixdown)导出全零/静音,与「connected_mask 锁 0」同源;C-12 强杀进程自愈反证是重认领逻辑而非 DSP 问题。出处:[S1-daw-checklist.md](spikes/S1-daw-checklist.md) §0d。
3. **channel 塌缩(15 实例全挤 ch1)**(已修复 v8):v2 的同 pid 重认领让自动选槽永远停在 ch1,15 个 Input 全部挤进同一 channel(导出只剩单轨内容)。v8 新增 claimInputAuto(自动认领专用,不抢已占 slot),15 实例各占一 channel(multiclaim 压测 mask=32767)。出处:[S1-routing.md](spikes/S1-routing.md) §6.1。
4. **Cubase 渲染轨道复制实例 Ctrl-Z 卡死(RIP/undo 后「Input 直通 + Output 静音」)**(已修复 v7):Render in Place(dry)后 Ctrl-Z(undo),宿主复制出 Input 实例,复制体同 pid 重认领接管 slot、析构置 Free,原实例不再 prepareToPlay → 永久卡死。v7 加 Input 25Hz timer:发现 claimed 但 slot 非 Active 即 doClaim() 自愈(≤40ms)。出处:[S1-routing.md](spikes/S1-routing.md) §6.1。
5. **mono/stereo 段几何越界崩溃**(已修复 v5/v6):宿主 freeze/RIP/导出时 mono⇄stereo 重建,环几何视图与数据视图不一致导致越界写(转储同址实锤)。根因:命名段已存在时 CreateFileMapping 忽略请求大小、map() 把请求大小当实际大小 →「小视图 + 大几何」。v5 先做 AudioRing 几何+视图不可分快照单点发布;v6 根治:段恒按 stereo 容量创建 + 真实段大小回填(GetFileSizeEx)+ Input 降级/Output 拒绝 + journal 日志。出处:[S1-routing.md](spikes/S1-routing.md) §6.2、[S1-daw-checklist.md](spikes/S1-daw-checklist.md) §0d。
6. **宿主异步销毁实例 → 写环 AV(进程寿命泄漏)**(已修复 v3/v4):宿主异步销毁插件实例导致已解映射视图被写(转储锁定同指令);v3 退休视图保活 + isOpen 三条件 + 缓冲构造定容;v4 视图/环/registry 进程寿命泄漏池根治。出处:[S1-routing.md](spikes/S1-routing.md) §6.1。

### 3.2 宿主侧已知开放问题(未修复,按操作/兜底)

7. **RD-01 · Cubase 录不进插件自发参数变化**(宿主侧开放问题;**Cubase 15 成品实测未复现**,见 §1 脚注 7):Cubase 12+ 下 setValueNotifyingHost 发起的自动化可能录不进,无公开确定解。万一遇到,**兜底 = 建议表 + CSV 导出(T41)**。出处:masterPlan 11-risks RD-01。
8. **RD-04 · REAPER 关 GUI 不写自动化**(未修复,宿主端设置兜底):打印期间保持插件窗口打开,或把 Parameter automation notifications 设为 process all notifications。出处:masterPlan 11-risks RD-04。
9. **RD-02 · Live 128 参数上限**(未修复,预算封顶):123 声明 + bypass = 124 宿主可见,余 4;任何加自动化参数的 PR 必须先改宪法。出处:masterPlan 11-risks RD-02。
10. **设备停用 / smart disable 停调插件**(未修复,设计内兜底):Live 设备停用 / FL smart disable 不经 bypass 直接停调 processBlock;Output 停摆后人声 ~5.5s 内转直通恢复(未经平衡),FL 用户请对总线关 smart disable。详见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-6。
11. **同机双工程限制**(未修复,v1 明示限制):共享内存段名只按组号区分、不带工程标识,同机同时只支持一个使用 SCVB 的工程(双工程/双 DAW 用同一个组时后开者抢 channel)。v2 走 documentToken 隔离。详见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-5。

### 3.3 环境依赖与通用路由坑

12. **WebView2 运行时依赖**:Windows 上编辑器 UI 依赖 **WebView2 Evergreen Runtime**([README](../README.md) Requirements 已列)。插件侧经 GetAvailableCoreWebView2BrowserVersionString 探测;缺运行时时走 **FallbackPanel 原生兜底面板**(含「下载 WebView2 Runtime」引导),5s 看门狗超时亦可切兜底。WebView2 user-data 目录必须可写(DAW 安装目录只读会致初始化失败,插件已改指临时目录)。macOS(WKWebView)/ Linux(WebKitGTK)恒可用。出处:[S3-webview.md](spikes/S3-webview.md)。
13. **采样率切换**(已修复 v2):切换采样率后同 pid 重认领曾致永久静音,v2 已修(心跳跨宿主挂起存活)。语义:SR 不符时该轨禁用(CH_SR_MISMATCH),**v1 不做重采样**。出处:[S1-routing.md](spikes/S1-routing.md) §6.1、masterPlan 01 §4.3-f。
14. **#68 冻结平直线 write 语义**(已修复 #68):冻结(PAN/VOL)维度在 write 时以**平直线(冻结时的手动静态值)写入自动化**,不再停写该车道;优先级链 = 宿主自动化 > 冻结手动值 > 手动微调 > 引擎曲线。出处:masterPlan 03 §J78、T37 旅程文档 §3.3。
15. **通用路由坑(12 §3.3 六条)**:①改人声轨路由 → Output 报时间线缺口;②宿主 pan 不居中 → equal-power pan 结果偏移;③依赖 PDC 对齐 → SCVB 不报告额外延迟,不要用 PDC「修正」;④同一 channel id 被两个 Input 抢占(后到者冲突不生效);⑤第二个 Output 实例进只读;⑥采样率不一致 → 该轨禁用。
16. **单轨 Freeze / 部分 stem 导出得静音文件(⚠ 用户数据)**:替换式渲染会覆盖原素材;请对总线整体导出。详见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md) KI-4。

## 4. README 支持等级表(供 T39b 转贴)

> 本节与 §1 总矩阵严格一致,可直接复制进 README 的「Supported DAWs」小节。Tier 定义(masterPlan 10 §3.4):Tier 1 = 完全支持 / Tier 2 = 有限制(有可执行的用户侧规避)/ Tier 3 = 未验证或不支持。

| DAW | 版本 | 支持等级 | 状态与已知限制 |
|---|---|---|---|
| Cubase | 14 / 15 | **Tier 1(主测)** | 路由(实时/离线)、存工程重开、自动化写入已用成品插件真机实测通过(Cubase 15 Pro);Cubase 14 在路由 spike 阶段验过路由;自动化藏 Ins 隐藏车道;Input 须在 pre-fader 区最后一格 |
| REAPER | 7 | **Tier 2(部分验证)** | 只在路由 spike 阶段验过路由(实时/离线);成品插件与自动化写入尚未在 REAPER 上测过;关 GUI 可能不写自动化(需 process all notifications);同机单工程限制 |
| Ableton Live | 12 | **Tier 3(未验证)** | 尚未真机测试。设计上已知:128 参数上限(本插件占 124,余 4);Re-Enable Automation 需点击;停用 Output 设备后约 5.5 秒无声,之后人声转直通 |
| Studio One | 6 | **Tier 3(未验证)** | 尚未真机测试。设计上已知:自动化模式须在插件窗口内设 Write/Latch;Dropout Protection 会改变 block size |

> **支持等级说明**:未验证的宿主不代表不能用(插件是标准 VST3),只是还没有人在真机上确认过;等级只在真机实测之后才上调。FL Studio(03 §4.7)不在 v1 支持矩阵内。
