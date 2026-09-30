# 契约变更说明 —— 20260922-sl478-notimeline

> **状态:待批(随本 PR 挂 `status/frozen-contract`)。** 变更实质用户已批:**[J100]**(2026-09-22,
> 对 [SL-478] 裁「修」)。本文档记两张卡:
>
> - **[SL-478]**:契约在 9 处承诺了 `noTimeline`,实现整条没接。本 PR 把实现接上,契约侧只补
>   **判据定义**(连续 ≥0.5s、恢复即撤、判序),不改任何既有承诺。
> - **[SL-480]**:§1.2 / §1.3 的「返回」行与 §7 manifest 漏登了实现早已存在的两支
>   (`{observer:true}` 与 `{ok:false, reason:"badArg"}`)。同一处 handler,顺手写全。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。`noTimeline` 是纯桥面事件,
      不碰段名、布局、abi。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。`hostTimelineMissing` 是运行期态,不落盘。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.2/§1.3/§1.23/§5.1 文字 + manifest 三行,见下「逐条改法」与「复审轮改动」第 2 条。
      函数名 / 参数 / 事件名 / 载荷字段**零变化**,`reason` 十一值闭集**零变化**。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

### [SL-478] 实现侧:四个落点接上

| 契约承诺 | 落点 |
| --- | --- |
| §5.1 / §2.9 `scvb.error{code:"noTimeline"}`(横幅⑥) | `OutputEditor::emitNoTimelineError`,每拍由 `emitTick` 调;边沿/撤销/不可见不记账由纯函数 `planConditionErrorEmit`(`src/output/BridgeArgs.h`)判定 |
| §1.2 `setCaptureEnabled` → `{ok:false, reason:"noTimeline"}` | `handleSetCaptureEnabled`,observer 判据之后、badArg 之前 |
| §1.3 `setOutputEnabled` → 同上 | `handleSetOutputEnabled`,同上 |
| §1.23 / §5.5 `recaptureArm` 第四个 `reason:"noTimeline"` | `handleRecaptureArm`,排在 `readOnly` 之后(与 mock 判序一致),照旧先撤防 |

条件源是 `ScvbOutputAudioProcessor::hostTimelineMissing()`:定时器里**既有的**「连续无时间线
≥0.5s → 清注入 mask」判据(04 §4.2 [J51],`kTimelineInvalidTicks = 12` 拍 × 25Hz)同时置起它,
时间线一恢复即清。**没有新造判据**:单块的 `timelineValid_` 每块刷新,直接上桥会让横幅⑥ 随
宿主抖动逐块翻转;复用这一条也保证了「横幅亮着」与「注入 mask 已清」是同一个状态。

### 契约文字逐条改法

1. **§1.2「返回」行** —— 旧:`{ok:true}` 或 `{ok:false, reason:"noTimeline"}`;
   新:再加 `{observer:true}` 与 `{ok:false, reason:"badArg"}`(`on` 不是严格布尔)。【SL-480】
2. **§1.2「拒绝态」行** —— 原句保留,补三件事:`noTimeline` 场景的判据与 §5.1 同一个
   (连续 ≥0.5s);`on` 取 `true` / `false` **一律拒**(原句「收到调用时返回」本就不分方向,
   写明免得读成「只拒打开」);判序 `observer` → `noTimeline` → `badArg`。
3. **§1.3「返回」行 / 「拒绝态」行** —— 同 1.2。
4. **§1.23「拒绝态」行** —— 补 `noTimeline` 一支与四值判序;「**三**条拒绝路径都先走一次撤防」
   改成「**四**条」(第四条就是本次接上的这一支,实现上它走的是同一段撤防代码)。
5. **§5.1 九码表 `noTimeline` 行「触发条件」列** —— 旧:「宿主未提供时间线(无 `timeInSamples`)」;
   新:补「且连续 ≥0.5s(与 04 §4.2 [J51] 同一判据;单块抖动不上桥,负 t0 算有效时间线);
   时间线恢复即发 `active:false`」。
6. **§7 manifest** —— `setCaptureEnabled` / `setOutputEnabled` 的 `returns` 串补
   `{observer:true}` 与 `{ok:false,reason:"badArg"}` 两支。【SL-480】

§5.5(`recaptureArm` 四值表)与 §5.6(十一值闭集)**本来就含** `noTimeline`,不动。

### 为什么 badArg 也一起补(卡面只点了 observer)

[SL-480] 卡面写的是「实现比契约多一态(`{observer:true}`)」。实际读 handler,`strictBool` 失败
那一支回 `{ok:false, reason:"badArg"}`,契约同样没登。只补 observer 会让「返回」行照旧不全,
与 §5.6 末句「每个函数条目的『返回』行按 §0.8 第 5 条写明本函数实际可能出现的取值」仍不符。

### 复审轮改动(#278 第 1 轮,两家 bot)

1. **Tab1 写闸收窄(用户可见,本 PR 引入)**。`tab-master.js` 的 `isWriteBlocked()` 此前是
   `readOnly || noTimeline`,挡着两把开关、参数卡、过渡、范围档与「分析」按钮。noTimeline
   没有生产者时它恒 false,宽出来的部分走不到;本 PR 接上生产者后,宿主不给时间线时 Tab1 会
   大面积灰死,而真桥对这些函数照常受理。两家 bot 同指此条。收窄依据:§5.1 该行 UI 落点逐字
   只有「采集/输出开关 disabled」,05 §2.0 横幅⑥ 同文,mock 只在两把开关与 `recaptureArm` 上拒,
   Tab2(`tab-tracks.js`)与 Tab3(`tab-wave.js`)的写闸本来就只认 `readOnly`。现在
   `isWriteBlocked()` 只认只读,两把开关改走新的 `isSwitchBlocked()`(只读或无时间线)。
   页级冒烟 `smoke-output-stale-page.mjs` 的 `no-timeline` 档加两条:输出开关被挡、分析按钮
   不是 disabled。**契约文字零改动**(本来就是这么写的)。
   ⚠ 另有一处未对齐,本 PR 不处理:04 §2.6 写的是无时间线时「只允许全曲跟随(follow)」,即
   范围档也该受限,而契约 §1.8 与 §5.1 都没有这一条。已报统筹另行定夺。
2. **§1.23 返回行与 manifest 补 `{ok:false, reason:"badArg"}`**(`autoStop` 给了却不是严格
   布尔)。与 §1.2/§1.3 补 badArg 同一类。这一支在撤防之前就返回,不撤防、不改 state,契约里写明了。
3. **`releaseResources` 复位单块时间线标志**。宿主先丢时间线、再停音频引擎的话,标志会冻在
   「无时间线」,`hostTimelineMissing()` 跟着冻在 true,两把开关一直被拒。现在由定时器下一拍
   在消息线程上撤掉。host 用例加 T5 一格。
4. 顺手订正 `OutputProcessor.cpp` 里「钳住,避免重复清」那句注释:钳住只防计数溢出,条件
   持续期间清 mask 每拍照调。

**已知、本 PR 不处理**(#278 第 2 轮 bot 建议,已报统筹):

- mock 的 `recaptureArm` 不校验 `autoStop`,真桥回 `{ok:false, reason:"badArg"}` 的那一格
  mock 回布防成功。今天 UI 恒传严格布尔,不可达;`check-bridge-parity.mjs` 不比 `returns` 串,
  这条分叉没有机检。
- `processBlockBypassed` 不刷新单块时间线标志,宿主 bypass 期间 `hostTimelineMissing()`
  冻在进 bypass 前的值。与上面第 3 条同属「宿主不再调 `processBlock`」一族。
- 「写入双后果」确认板的「撤销」钮直调 `setOutputEnabled(false)`,不过开关闸;被拒时板子
  照样收起。只读态下同样如此,是既有形态,不是本 PR 引入。

## 兼容性影响

- **不改名、不改参数、不删任何东西、不收窄取值域** —— §0.1 规则 3 的禁止面一条都没碰:
  - `noTimeline` 的返回值与 error code 在冻结时就写进了契约,本次是**兑现**;
  - `{observer:true}` / `badArg` 两支在实现里早就存在,本次是**登记**;
  - 「连续 ≥0.5s」是给原本没写判据的「宿主未提供时间线」补定义,不是改一个已有的定义。
- **`contractVersion` 保持 `1.0`**。先例:manifest 自冻结起改过多次(如 `76bd67a`),
  版本行从未动过;[J90] 变更文档写明「1.0 = 这一版冻结表面」这个约定。§9.0 第 4 条要求的
  manifest 同步已做,`node scripts/check-bridge-parity.mjs` 退 0(它只比名字集合,
  `returns` 串的内容照不出来 —— 这正是 [SL-480] 当初漏掉的原因)。
- **mock 同步**(`web-preview/mock/juce-bridge-mock.js`):`setCaptureEnabled` / `setOutputEnabled`
  补只读闸,判序与真桥同款(observer → noTimeline);`smoke-mock.mjs` 的 second-output 一族
  加这两行断言。mock 的 `noTimeline` 分支此前就有。
- **用户可见**:只在宿主不给时间线时出现。**Cubase 恒有时间线,真机验不到**;验证全靠
  自动化(host harness 真 processor + 无时间线 playhead;纯函数;源码钉子)。

## 审批

挂 `status/frozen-contract`。实质:**J100**(用户 2026-09-22 裁 SL-478「修」);[SL-480] 为同一处
handler 的文档补登,零行为改动。

## 关联

- 卡:SL-478(P2)、SL-480(P3),`masterPlan/review/suggestion-ledger.md`。
- 消费侧:`web/output/app.js` 横幅⑥ 与 `vs.noTimeline` 本次未改;`web/output/tab-master.js`
  的写闸本次收窄,见上「复审轮改动」第 1 条。
