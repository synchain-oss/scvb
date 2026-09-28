# 契约变更说明 —— 20260928-sl218-219-state-not-restored

> **状态:变更实质已批(随本 PR 挂 `status/frozen-contract`)。** 依据:用户 2026-09-28 裁定
> **[J134]**(SL-218「做」:新增错误码 `stateNotFullyRestored`,detail 带没恢复的 fourcc + 琥珀横幅
> 「段表没能恢复,原数据会原样保留」,与 [J122] 配套)与 **[J135]**(SL-219 两项「都现在做」)。
> 用户原话:「298 180 167b 94 218按你说的做,不过180记得要代码上也要改。219都现在做。」
> 本文档与实现放在**同一个 PR** 里;[SL-524](按 [J122] 拒载后原样写回原字节)是本 PR 的前置。

## 变更了哪个冻结契约

- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §5.1 错误码表加一行 `stateNotFullyRestored`(七码 → 八码)、
      表后加一段「新增一码」、降级纪律② 的持续性横幅编号补 ⑪;§0.8 第 2 条、§2.9 / §4.5 两处载荷行
      「七码」→「八码」;§7 manifest `enums.errorCode` 末尾加一个值。函数名 / 参数 / 事件名 / 既有载荷
      字段**零变化**。
- [x] docs/STATE_SCHEMA.md(state schema)—— 只改 §三「同 abi 但 CRVS minor 更高」一条的**接线实况**
      (原文「仍未接线」→ 原样回写与横幅⑪ 已接)+ 文首「最后更新」。**零布局 / 零 abi / 零迁移**:
      state 格式一个字节没变,`kCurrentAbi` 与 `kCrvsMinorVersion` 不动。
- [x] docs/PARAMETERS.md(自动化参数)—— 只改 §四「读到高版本 → 拒载并提示升级」那条括注里
      CRVS minor 一支的接线实况 + 文首「最后更新」。**123 参数表逐字未改**。
- [ ] docs/IPC_CONTRACT.md —— **不动**。
- [ ] tests/golden/ —— **不动**。

## 变更内容

### [SL-218] 第八个错误码 `stateNotFullyRestored`

卡面(ledger)与复审里叫它「第十个错误码」,那是按撤回前的九码数的;[J101] 撤回
`lowSample` / `projectCopy` 两码之后契约是七码,所以它落在第八位。名字与卡面一致。

| 列 | 内容 |
| --- | --- |
| 触发 | 最近一次载入(**容器层已通过**)时段表 `CRVS` 没能恢复:不在 blob 里;在但没被采用(解不开 / 段值非有限或越界 / 更高 CRVS minor);或 `CFGS` 不在 / 解不开,载入在读 `CRVS` 之前就停了。撤下(`active:false`)三个时机:下一次段表恢复成功的载入;用户改动段表 / 版本后的那次保存(仅 `rejected` 含 `"CRVS"` 时);载入一份 abi 更高的工程 |
| `ch` | —(页级) |
| `detail` | `{missing:string[], rejected:string[]}`,值只取 `"CFGS"` / `"CRVS"`;`missing` = 不在 blob 里,`rejected` = 在但没被采用;每次 `active:true` 都至少含一个 `"CRVS"`;两张表变了即重发 |
| UI 落点 | 琥珀横幅⑪「段表没能恢复,原数据会原样保留」(文案逐字取用户裁定);不 disable 任何控件;不给 ✕(降级纪律② 的持续性条件) |

**为什么容器整份拒载的两支不发它**:更高 abi 已有红横幅④ `newerState`,再亮一条是重复;容器损坏那一支
什么都没载入,**保存时写的是 live 状态、并不原样保留原字节**,在那里亮「原数据会原样保留」是假话。
两支对上一次载入留下的横幅处置不同(#307 复审第 1 轮):更高 abi 那一支**清掉**它 —— 此后保存写回的是
这份更高 abi 的原始 blob,上一份工程的留底不会再写出去,⑪ 说的已不是当前工程;容器损坏那一支**不动** ——
什么都没载入,上一份工程仍是当前工程,保存照旧写它的留底。

**用户改过段表之后**(#307 复审第 1 轮):保存判出「改过」、保留态解除的那一刻,「原数据会原样保留」不再
成立 ⇒ 同时把位图清成 0、横幅撤下。只在 CRVS 位是 `rejected` 时清(那正是保留态对应的一支);CRVS 位是
`missing`(其后又载了一份只带 PRMS 的预设)时不清,那条横幅说的是那份预设。

**「原样保留」怎么兑现**:段表保留不清空([SL-217]);blob 里那份没被采用的 `CRVS`,保存时原样写回,
直到用户在这个实例里改动段表 / 版本,之后写新表([SL-524] / [J122])。缺 `CRVS` 的情况(预设)没有原字节
可写回,「原数据」指的是保留下来的当前段表。

**为什么 detail 只收 CFGS / CRVS**:横幅那句话讲的是段表;`CFGS` 进表是因为它决定了段表读没读。
`FEAT` 有自己的读失败处置(原样留底 + `featCodecNewer_` / `featRefUnresolved_`),`UICF` / `PRMS` 解不开时
退回默认、不影响段表 —— 三者都不进本码。

实现落点:

| 落点 | 位置 |
| --- | --- |
| 条件源(位图,每次载入整份重算) | `ScvbOutputAudioProcessor::setStateInformation` 三处:两处 CFGS 早退(见下 SL-219②)、CRVS 段;访问器 `stateNotRestoredMask()`(原子,编辑器在消息线程读) |
| 位定义 + detail 两张表 | `src/core/output/StateRestoreDiag.h`(JUCE-free)`notRestoredFourccs` |
| 边沿 / 换位图重发 / 撤销 / 不可见不记账 | `src/output/BridgeArgs.h` `planStateNotRestoredEmit`(纯函数) |
| 上桥 | `OutputEditor::emitStateNotRestoredError`,由 `emitTick` 每拍调 |
| 横幅 | `web/output/index.html` `banner-stateNotRestored` + `app.js` `renderBanners` ⑪;三语词条 `banner.stateNotRestored` |
| 枚举对拍 | `app.js` `KNOWN_CODES`、`web/shared/mock-data.js` `ENUMS.errorCode` / `makeError`、`scripts/check-bridge-parity.mjs` |

### [SL-219]① 更高 CRVS minor:载荷原样带走

STATE_SCHEMA §三 与 `StateCodec.h` 挂账 2 早就写着「同 abi 但 CRVS minor 更高 → 等同拒载 + 原样回写 +
提示升级,不得让旧插件抹掉新版曲线真身」。`decodeCrvs` 对「minor 更高」与「载荷损坏」回同一个 `false`,
[SL-524] 的原样写回因此已覆盖这一支;本 PR 补这一支的专门用例(载荷带一段本构建不认识的尾部,保存两次
逐字节相同)与提示(横幅⑪)。

⚠ **与挂账原文的一处差别,照实登记**:按 [J122],用户在这个实例里改动段表 / 版本之后,保存写新表 ——
那一刻起新版曲线真身被本构建的段表取代。不写新表就会丢掉用户之后的编辑,这是 J122 裁定的取舍;
「什么都不做就保存」这一最常见的场景是完整保住的。横幅不分「更高 minor」与「载荷损坏」,不专说
「请升级」。

### [SL-219]② CFGS 缺失 / 解不开的早退点

`setStateInformation` 在 `CFGS` 缺失或解不开时早退,位置在读 `CRVS` 之前。此前「段表没恢复」只在后面
`CRVS` 那一段赋值,于是这两支里它停在上一次载入的值上 —— 最常见的触发(轨道 / 参数预设只带 `PRMS`)
不亮。本 PR 在两处早退都置位(`CFGS` 进 `missing` 或 `rejected`,`CRVS` 按在不在进 `rejected` 或
`missing`)。

顺带补上一个同根的洞(否则横幅那句「原数据会原样保留」在这两支里是假话):blob 带着 `CRVS` 时,它在
这两支里没被采用,而保存一律从 live 表重编码 ⇒ 被覆盖。现在按 [SL-524] 同一条口径原样留底(共用
`holdRejectedCrvs`)。不带 `CRVS` 的(预设)不动保留态,与 [SL-217]「缺 chunk 不等于删除」同口径。

## 兼容性影响

- **加法**:新增一个枚举值与一个事件载荷形态;不改名、不删、不收窄既有取值(§0.1 规则 3 的禁止面一条都
  没碰)。旧 UI 收到未知 code 按降级纪律① 进诊断区,不静默。
- **`contractVersion` 保持 `1.0`**。先例:[J90] 变更文档写明「1.0 = 这一版冻结表面」;`20260922-sl478-notimeline.md`
  与 `20260926-j106-j107-sl505-contract-supplement.md` 同样保持 1.0;§9.0 只在「改名 / 删除 / 收窄」时升主版本。
- **state 格式零变化**:abi、CRVS minor、各 chunk 布局一个字节没动;旧工程、新工程互相打开的行为不变。
- **用户可见**:只在段表没恢复时出现一条琥珀横幅。只带参数的预设也会亮它(这是 SL-219② 要的:段表确实没
  从这份 blob 里恢复),下一次完整载入撤下。

## 判据与机检(每条都实跑,删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| 纯函数 | `tests/core/test_bridge_args.cpp` `[SL218]` 两格 | 发送面六态(边沿 / 不重发 / 换位图重发 / 撤销 / 不发空撤销 / 不可见不记账);位图 → 两张表(预设、CFGS 坏、四位各落各表) |
| 条件源 | `tests/host/test_host_harness.cpp` `[sl219]` 四格 + 下一行一格 | 更高 minor 原样写回两次;只带 PRMS ⇒ 置位(先清成 0 再比)、完整载入清回 0;CFGS 缺失 + 带 CRVS ⇒ 位图 + 原样写回;CFGS 解不开 + 带 CRVS ⇒ 位图 + 原样写回 |
| 清位时机 | `test_host_harness.cpp` `HOST SL-218:横幅条件的清位时机` 三节(#307 复审第 1 轮) | 改段前保存不清、改段后保存清;更高 abi 清;容器损坏不动 |
| 调用点 | `web-preview/tests/smoke-tab2-interactions.mjs` [SL-218] 四条行形态钉子 | emitTick 调它、条件与判定、信封与 detail、闩锁 |
| 页面级 | `web-preview/tests/smoke-group-lock-page.mjs` ⑥c | 推一帧 ⇒ 横幅⑪ 上屏、文案逐字等于词条、琥珀、不改变输出开关挡否;`active:false` ⇒ 撤下 |
| 契约面 | `smoke-tab1-interactions.mjs` (a21) | 横幅⑪ 在、没有 ✕ |

⚠ 离线不可达的一跳(照实登记):`OutputEditor::emitStateNotRestoredError` 依赖 WebView2,不在任何 C++ 测试
目标里;它的输入(位图)由 host 用例断,判定由纯函数断,装配由源码钉子断。

## 审批

实质已批:**J134 / J135**(用户 2026-09-28)。挂 `status/frozen-contract`。

## 关联

- 卡:SL-218、SL-219(`masterPlan/review/suggestion-ledger.md`);前置 SL-524([J122])、SL-217、SL-483。
- 来源:#126 复审两条【重要】(minor 更高 / 早退点不置位)。
