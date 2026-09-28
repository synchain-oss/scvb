# 契约变更说明 —— 20260928-j147-playhead-host-tempo

> **状态:实质已批(随本 PR 挂 `status/frozen-contract`)。** 用户裁定 **J147**(masterPlan
> `plan/adjudications.md`,发布前 17 条拍板的第 3 条「Tab1 手动范围『小节』」)= **按小节做**:
> 读宿主 AudioPlayHead 的 BPM / 拍号 / 拍位置换算小节;停带时用最后一次读到的值,变速用最近 BPM
> 估算(「估算值,播放后校准」);范围同时显示小节与 mm:ss,±4 按钮真挪 4 小节。
> 用户原话:「我们不能读取bpm来判断小节吗…别的插件不都可以吗」。
> 本文件记的是**这一实质落到桥面契约上的形状**(四个可选字段的名字、出现条件与取值域)。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。速度/拍号走的是进程内的 playhead
      快照(`PlayheadShot`,音频线程 → 消息线程,非 IPC),不进任何共享段。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**,abi 不升。速度信息只在内存里,不落盘
      (重开工程后第一帧 `scvb.playhead` 就会把它带回来,不需要存)。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §2.6 `scvb.playhead` 的载荷**新增四个可选字段**
      `bpm?:f64` / `timeSigNum?:i32` / `timeSigDen?:i32` / `ppq?:f64`,字段纪律与 UI 消费两格同步补写;
      §10 Monitor 事件表的 `scvb.playhead` 行注明 Monitor 不发这四个字段。
- [ ] tests/golden/(golden 快照)—— **不动**。

`contractVersion` 保持 `1.0`:这是 §0.1 第 3 条明文允许的「在既有 payload 中新增**可选**字段」,
不改名、不删、不改既有字段语义、不收窄取值域 —— §9.0 第 3 条的「破坏性变更」一条都不命中,
不需要豁免(与 J105 那次「撤回错误码」要单独豁免不是一类)。

## 变更内容

### 四个字段

| 字段 | 类型 | 出现条件 | 含义 |
| --- | --- | --- | --- |
| `bpm` | f64 | 宿主给了速度(`kTempoValid`)**且**给了拍号;值有限且 `0 < bpm ≤ 999` | 本帧播放位置的速度 |
| `timeSigNum` | i32 | 与 `bpm` 同进同出;`1..64` | 拍号分子 |
| `timeSigDen` | i32 | 与 `bpm` 同进同出;`1..64` | 拍号分母 |
| `ppq` | f64 | 上面三项在场,**且**本帧有时间线(`timeInSamples` 有效)、宿主给了拍位置(`kProjectTimeMusicValid`)、值有限、换算 `timeS` 用的采样率与该帧发布时一致 | 本帧播放位置的拍位置(四分音符数);与同一载荷的 `timeS` 出自同一次快照读 |

- **bpm 与拍号同进同出**:页面换算小节两样缺一不可,只给一样等于没给。拆成各自独立出现的话,
  页面还得再判一遍「够不够」,而那条判定没有别的消费者。
- **`ppq` 要求本帧有时间线**:`timeS` 在宿主不给时间线时填的是 0.0(§2.6 既有行为),那时把 `ppq`
  发出去,页面会拿它和一个假的 0 秒配成换算锚点。
- **`ppq` 还要求采样率对得上**(复审补):插件停用(`releaseResources`)后处理器采样率回 0,
  `emitPlayhead` 算出的 `timeS` 是 0.0,而停用时补发的那一帧快照里 `ppq` 仍是真实位置 —— 假锚点的
  另一条来路。`hostTempoOf(pod, timeRate)` 的第二个实参就是换算 `timeS` 用的那个采样率,
  与快照发布时记下的 `sampleRate` 不等(或 ≤ 0)即不发 `ppq`。
- 越界 / 非有限值按「宿主没给」处理(不夹取、不猜):取值纪律集中在一个纯函数
  `scvb::engine::hostTempoOf`(`src/core/engine/PlayheadShot.h`),`OutputEditor::emitPlayhead` 只是逐项 put。

### 实现落点

| 层 | 文件 | 改动 |
| --- | --- | --- |
| 音频线程 | `src/output/OutputProcessor.cpp` `publishPlayhead` | 读 `PositionInfo::getTimeSignature()`,拷两个 int 进 `PlayheadPod`(新增 `timeSigNum`/`timeSigDen` 与标志位 `kPlayheadTimeSigValid`)。**零分配、零锁**,与既有的 bpm/ppq 同一次整体发布 |
| 消息线程 | `src/core/engine/PlayheadShot.h` `hostTempoOf` / `src/output/OutputEditor.cpp` `emitPlayhead` | 按上表取值纪律组装四个字段 |
| 页面 | `web/output/host-tempo.js`(新)/ `tab-master.js` / `app.js` / `index.html` | 速度模型、小节换算行、注释行三选一、±4 小节 |
| 预览 | `web-preview/mock/juce-bridge-mock.js` / `state-driver.js` | mock 同口径发这四个字段;`?tempo=none` / `?tempo=<bpm>/<分子>/<分母>` 与预览专用 `ctl.setHostTempo` |
| 门禁 | `scripts/check-bridge-parity.mjs` | 载荷字段对拍一节加上 `scvb.playhead`(此前不在表里),并认契约里 `name?:type` 的可选标记 |

`PlayheadPod` 是进程内快照,不跨进程、不落盘;加两个 int 不牵动任何布局断言。

### 页面怎么用(不是契约面,记在这里便于复核「字段够不够用」)

插件读不到宿主的整张速度表(VST3 只给「此刻」),页面据此分四态,详见 `web/output/host-tempo.js` 头注:

1. **一帧速度都没收到过** ⇒ 按秒显示,注释行明说(`master.rangeSecondsNote`),±4 挪 4 秒;
2. **本次会话没见过速度变化** ⇒ 按最近一次的 bpm 与拍位置换算,精确,注释行隐藏;
3. **见过速度变化**(bpm 变了,或「`ppq` − `timeS` × bpm / 60」这个偏移变了)⇒ 离开走带经过的地方就是
   估算值(按最近 BPM 外推),注释「小节为估算值,播放该区域后校准」;走带经过时每 0.25 s 留一个锚点,
   端点落在锚点 0.25 s 之内即「已校准」;认出用户在宿主里**改了速度表**时(同一时刻速度变了 /
   拍位置没动而秒位置变了 / 旁边的旧锚点推不出此刻的拍位置,三条见 `host-tempo.js` 头注),此前的
   锚点与判断整份作废、从这一帧重新观察(复审补;否则旧锚点旁的端点会被判成精确);
4. **见过拍号变化** ⇒ 小节号取决于整段拍号历史,播放也校准不了,注释换成「拍号有变化,小节号为估算值」。

## 与 05 规格的出入(如实记)

05 §2.1 ② 写的是起/终各一组「**小节+拍+tick**」与「mm:ss.mmm」**双向联动输入**。本 PR 做的是 J147 裁定的
那一半:**同时显示**小节(小节.拍)与 mm:ss、±4 真挪小节;**可编辑侧仍是 mm:ss.mmm**(桥面 §1.8 只收秒),
小节侧是只读换算行,不显示 tick。把小节侧做成可输入(含「小节输入吸附小节线」,04 §2.3)不在 J147 的
裁定文字里,留给统筹决定是否另起。

footer 打印状态行与写入确认条的 `{x}–{y}` 仍填 mm:ss(`web/shared/i18n.js` 那几条注释同步订正了理由):
J147 只裁了 Tab1 手动范围。

## 验证

- **host(真 Processor + 假宿主 playhead)**:`tests/host/test_host_harness.cpp` `[j147]` 两格 ——
  宿主给 120 BPM / 3/4 / 拍位置(故意带 +0.5 拍偏移)⇒ 快照带拍号、`hostTempoOf` 取出四项且 `ppq`
  是宿主给的那个值;只给 bpm / 只给拍号 / 不给时间线 / 插件已停用四种情形各一格;取值域逐项越界各一格
  (含采样率对不上)。
- **页面逐函数**:`web-preview/tests/smoke-host-tempo.mjs`(换算、估算判定、校准窗、吸附、上限)。
- **页面级**:`web-preview/tests/smoke-range-bars-page.mjs`(120/4-4 下 +4 ⇒ 终点 +8 s、换算行显示小节;
  `tempo=none` ⇒ 按秒;变速工程 ⇒ 估算;端点落在播放过的地方 ⇒ 校准;换拍号 ⇒ 拍号提示;
  停着改速度表 ⇒ 旧观察作废、按新速度精确显示)。
- **契约对拍**:`node scripts/check-bridge-parity.mjs` 的载荷字段对拍一节现在覆盖 `scvb.playhead`。
- 每处落点的删除式读数见 PR 描述。

## 兼容性影响

- **加法**:旧页面不读这四个字段 ⇒ 行为不变;新页面遇到不发这四个字段的宿主(或 Monitor)⇒ 按秒显示。
- `scvb.playhead` 仍是 diff-then-emit:停带时四个字段逐字节稳定,不增加停带期的发帧;播放时 `timeS`
  本来就每帧在变,`ppq` 跟着变不改变发帧频率。
- 不动 IPC、不动 state、不动参数面,Input 与 Monitor 零改动。

## 审批

- 用户裁定 J147(masterPlan `plan/adjudications.md`),原话见文首;实质已批,本文件与 PR 挂
  `status/frozen-contract` 标签。
