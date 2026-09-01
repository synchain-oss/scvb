# KNOWN_ISSUES —— SCVB v1 已知限制

本文件记录 v1 已裁定接受、且不构成发版阻断的已知限制。每个条目写明「现象 / 原因 / 影响 / 缓解 / 彻底修复方向」。新增条目遵循同一格式。

## KI-1:run 切换时旧 run 尾部 ≤40ms 漏拉(04 异议 1)

- **现象**:增量拉取(04 §3.3)的读方以 base_hop 前后双读 + write_hop 做 seqlock 校验。若 run 切换(跳变 / 停止 / 开关)发生在两次 25Hz 轮询之间,旧 run 尚未拉取的尾部 hop 会被跳过,不并入 FrameStore。
- **原因**:FeatHeader 只有 base_hop / write_hop 两个寻址字段(ipc-contract §3 冻结布局),没有 run_id。切换后 base_hop 已指向新 run 起点,读方见 base != lastBase 即重置 lastPulled = base,旧 run 未拉积压整段跳过——旧 write_hop 已被新 run 覆盖,无法界定,永不补拉。
- **影响(已裁定接受)**:常态 ≤40ms(≈4 hop),且 run 切换通常落在换气/静音概率区,分析层 guard(04 §4.3 R⁺)与 padding 吸收;最坏情形 = 切换发生在 [M] 长停顿期间(加载工程/模态框/插件扫描,J10 场景),上限 = 停顿时长。**无假覆盖**:coverage 如实显洞 → UI 显示未采集 → 补播即复原。
- **缓解**:无(数据不可追回);后果可自愈(补播)。受限追赶(kMaxBurstHops=256)只保证 **run 不变**时的积压分拍补完,不覆盖 run 切换。
- **彻底修复方向**:FeatHeader 增 run_id(需 abi+1 且段名升 v2,04 文末异议 1 / J35)。v1 不做。

## KI-2:Cubase 15 为首测宿主,WebView2 承载面尚无实测基线

- **现象**:暂无已确认故障。本条是**基线缺口登记**,不是已知 bug。
- **原因**:S1 全系真机实测都在 Cubase **14** 完成;Cubase **15** 首次进入测试矩阵,其插件扫描/沙箱进程模型与 WebView2 子进程(`msedgewebview2.exe`)如何相处没有实测数据。已知风险面三处:
  1. **user-data 目录跨进程冲突**。宿主把插件扫描放在独立 sandbox 进程,音频进程与扫描进程同时活着;两进程若拿到同一个 user-data 目录,WebView2 环境创建会失败。**已修**:目录名带 PID(`PlatformWebView::makeUserDataFolder`),跨进程唯一。原实现只有进程内自增后缀,不同进程都拿 `_0`。
  2. **user-data 目录可写性**。目录已从 `%TEMP%` 迁到 `%LOCALAPPDATA%\Synchain\SCVB\WebView2`(`%TEMP%` 会被 Storage Sense 在会话中途扫掉),并在编辑器构造期建目录 + 写探针,结果进诊断面板。
  3. **子进程链被宿主策略挡掉**。插件侧无法规避,只能让它**可见**:导航事件一次都没来时,兜底面板直接显示「WebView2 环境没起来」而不是「加载太慢」。
- **影响**:无(三条都已转成可诊断状态)。
- **缓解**:测试包内附 `诊断.ps1`(= `scripts/check-webview2.ps1`),一次查完运行时版本 / user-data 可写性 / 子进程是否在跑。
- **彻底修复方向**:等 Cubase 15 真机回归数据。**JUCE 侧有一处上游限制须记住**:`juce_WebBrowserComponent_windows.cpp` 把 `CreateCoreWebView2EnvironmentWithOptions` 与 `CreateCoreWebView2Controller` 两个完成回调的 HRESULT 形参都写成**无名参数直接丢弃**,失败时既不抛也不通知客户端。故「环境为什么没起来」的确切 HRESULT 在 JUCE 8.0.8 下拿不到,只能靠「导航事件从未到达」判定这一类。若后续需要精确 HRESULT,得自己直连 loader 复现一次环境创建(需引 `<WebView2.h>`),或向上游提 patch。

## KI-3:`STATE_SCHEMA` 声明了 Input 侧 `uiGuideSeen` 的编码落点,而 `InputStateCodec` 没做

- **现象**:Input 首启引导的已读位 `ui.guide_seen` **不随工程持久化** —— 重开工程一律回 `false`。
- **原因**:`docs/STATE_SCHEMA.md`(**冻结文档**)第 100 / 154 行逐字写着 `InputStateCodec` 的 `CFGS` 尾扩 `u32 uiGuideSeen`,而实现里 `InputStateCodec` **严格等长校验、没有尾扩机制**,从来没编码过这个字段。属「冻结文档声明了、实现没做」——缺口自 [J81] 修宪写进文档时就存在。
- **影响**:**用户可见承诺不受影响**。跨工程的「不再显示」由系统级全局位 `guide_seen_global_input` 兜住([SL-258] 已真落盘,按侧分键);工程内的位只在本次会话有效,且换载工程时显式清零(不会把上个工程的位带过去)。至多是「同一工程重开后引导再弹一次」,而全局位已勾时根本不弹。
- **缓解**:无需用户操作。
- **彻底修复方向**:**SL-238** —— 二选一:① 给 `InputStateCodec` 加尾扩机制 + 测试后实装该字段;② 走 §5 契约流程把该字段从 `STATE_SCHEMA` 撤回。定性未决。
