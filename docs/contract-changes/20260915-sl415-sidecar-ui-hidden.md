# 契约变更说明 —— 20260915-sl415-sidecar-ui-hidden

> **状态:待批(随实现 PR #262 挂 `status/frozen-contract`)。** 定谳:统筹 2026-09-15(**SL-415**,
> 采纳复审【红旗】1 的**结论**、不采纳其**理由**);用户 2026-09-14 裁定「sidecar 不上了」。
> 本文档是该变更的契约面记录,与实现放在**同一个 PR**里。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。`features` 不是自动化参数,123/124 个参数的
      ParamID / index / 顺序 / versionHint 一个字节没碰。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。`features.bytes` 是 state 镜像字段,
      不是共享内存段布局的一部分;段名前缀与 header 前两字段无关。
- [x] docs/STATE_SCHEMA.md(state schema)—— **两处描述改实**,容器布局 / abi / 迁移链**零变化**:
      §4.2 `setStateInformation()` 失败分支的 UI 落点句(`:209`)、§4.3 `session_guid` 提前构造的
      **理由句**(`:213`)。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— **四处描述改实**,函数名 / 签名形态 / 事件名 /
      **载荷字段零变化**:§1.1 语义行的 `features.bytes` 句(`:95`)、§5.1 `sidecarMissing` 的
      「UI 落点」列(`:726`)、§5.1 表下的降级纪律①(`:732`)、§9 补白表的
      `scvb.state.features.bytes` 行(`:1016`)。
- [ ] tests/golden/(golden 快照)—— **不动**。本变更不碰任何字节布局。

> **不涉及 abi**:state chunk abi 不升、IPC abi 不升、段名不升 v2。改的全是**描述实现行为**的
> 句子 —— 它们此前描述的行为在 v1 上不存在(见下),本 PR 让它们与实现逐字一致。

## 变更内容

**一句话**:用户 2026-09-14 裁定「sidecar 不上了」之后,设置页「存储状态」行、横幅⑤、toast②
三处 UI **收起**(DOM 留 + `hidden` 恒挂 + JS 不再 `show()`),首启导览里讲「存储状态」的那一步
整步移除(44 → 43 步);六处冻结文档据此改成事实。

### 先更正一条事实(两家复审共同依据的那条是错的)

两份复审都写「`sidecarMissing` 是一条**活的**读路径,收起横幅 = 静默了一条会真的发生的错误」,
并据此判【红旗】。**这条事实不成立** —— 该 code 在插件侧**从来没有生产者**:

```
$ grep -rn "sidecarMissing" src/
src/core/state/SidecarStore.h:31:// ⚠ 本开关**只关写、不关读**:`embedded=0` + 合法引用仍要能载入、缺失仍要报 `sidecarMissing`。

$ grep -rn "sidecarSwitched" src/
(零命中)
```

`sidecarMissing` 在 `src/` 下**只有一条注释**,`sidecarSwitched` **一条都没有**;两者在
`web/`(消费端 `app.js`、`KNOWN_CODES`、三语词条、`mock-data.js` 夹具)之外没有任何发送点。
`SidecarStore.h:31` 那句是**待接线的意向**,不是已接的线。对照组:`srMismatch` 在
`src/input/InputEditor.cpp` 一带有真实发送边沿,那才是活的。

⇒ 所以**不给它留「非静默出口」**:给一条没有生产者的 code 留横幅,是留一块永远不会亮的死 UI,
既不解决任何用户可见问题,又与用户裁定相反。走的是**补本文档 + 把六处描述改实**这一边。

**但【红旗】的结论成立**,只是理由换了:那六处描述的行为今天不存在,本 PR 让它更不存在 ——
这属于**行为面**的契约变更,「不动布局所以不用走 §5 流程」站不住。

### 六处逐条改法

| # | 文件:行 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | `SCVB_CONTRACT.md:726` | `sidecarMissing` 的 UI 落点列写「琥珀横幅⑤『采集数据缺失/过期,请重新采集』」 | 「**v1 无出口**:sidecar 不随 v1 出厂,且该 code 在 `src/` 里**没有任何生产者**(grep 证据见本文档)。**接线那天必须同时恢复横幅⑤**——锚点与三语词条都还在」 |
| ② | `SCVB_CONTRACT.md:732` | 降级纪律①「UI **不静默**任何 code」 | 补例外并点名两条 code;**例外的判据写成「在 `src/` 里没有生产者」而不是「已隐藏」** —— 写成「已隐藏」的话,下次有人加了生产者会照着例外继续静默。同处写明「接线那天必须同时恢复横幅⑤ / toast②」,并指向那条会变红的判据 |
| ③ | `SCVB_CONTRACT.md:95` | `features.bytes` = 特征数据字节数(**Tab4 存储状态显示**,04 §5.4/ADR-007) | 同字段、同语义,**UI 消费面改实**:「本版已收起,故无 UI 消费面;字段仍在事件里照常下发」 |
| ④ | `SCVB_CONTRACT.md:1016` | 补白表该行的「说明」列写「特征数据字节数」 | 同上一格的「v1 收起」句,保留字段仍下发这一半 |
| ⑤ | `STATE_SCHEMA.md:209` | 失败/缺失分支写「FrameStore 置空 **+ UI 横幅『采集数据缺失/过期,请重新采集』**」 | 「FrameStore 置空」保留;**UI 那半改实**:该分支的横幅⑤在 v1 无出口(无生产者 + 用户裁定后已收起),接线那天必须同时恢复 |
| ⑥ | `STATE_SCHEMA.md:213` | `session_guid` 提前构造的理由写「让设置页在首次存盘前就显示真值 —— 否则『存储状态』行在用户第一次保存工程之前恒是废话」 | 理由句**失去指涉,改实**(那一行已收起);**「提前到构造期」这件事本身保留不动** —— 它是既有行为,与本次 UI 收起无关 |

> ⚠ ⑥ 是本次唯一容易改过头的格子:**要改的是理由,不是行为**。`juce::Uuid()` 仍在
> `ScvbOutputAudioProcessor` 构造函数里生成,`session_guid` 仍随 state 落盘。

## 兼容性影响

- **容器 / abi / 迁移链 / golden:零变化**。本 PR 不动 `src/` 与 `tests/`(`git diff --stat` 可证),
  工程文件的字节布局与本变更前逐字节相同 —— 新版本读旧工程、旧版本读新工程的行为都没有变。
- **用户可见变化只有一处**:设置页「存储状态」行不再显示;首启导览少一步(44 → 43)。
- **数据面没有丢**:`features.bytes` 仍在 `scvb.state` / 首帧快照里照常下发(只是没有 UI 消费端),
  `storageOf()` 的纯函数判据(内嵌 / 外置两态)照旧在跑,`embedded=0` 的读路径照旧可达。
- **能力面一个字节没动**:`SidecarStore`(读写 / `owner.lock` / copy-on-write / 8MB↔6MB 回滞)、
  `STATE_SCHEMA §4.3` 整节、四份冻结契约的字段与枚举、`FEAT-SIDECAR-1..11`、`HOST SL395` 全部保留。
- **已知的用户可见口子(照实登记)**:横幅⑤收起之后,「打开一份外部特征文件已丢失的早期工程」
  在界面上不再有任何解释(特征为空;分段与曲线不受影响)。因为该 code 今天本来就没有生产者,
  **这个口子在本 PR 之前也从未被填上过** —— 变的是「以后接线时不能再忘了同时恢复横幅⑤」这件事
  现在有判据守着(见下)。

## 判据与机检

| 层 | 位置 | 钉什么 |
| --- | --- | --- |
| 页面级(真执行) | `web-preview/tests/smoke-ui-layout-page.mjs` G 节 | 三处**触发条件成立**下的真实布局盒:`banner-sidecarMissing` / `toast-sidecarSwitched` 的 `getClientRects().length === 0`;Tab4 上 `settings-storage` 同样 `=== 0`,且其**计算 `display === "none"`**(非继承属性,不依赖面板当前在第几页)。四处对照(`banner-srMismatch` / `toast-projectCopy` / 诊断卡 / `#content[data-tab]`)与真判据**同一次读出**,免空过 |
| node 侧负向 | `web-preview/tests/smoke-tab4-settings.mjs` | **剥注释后** `tab-settings.js` 里不含针对 `settings-storage` 卡的 `show(...)` / `hidden = false` —— 把「不要给它加 show()」那句注释变成牙齿 |
| **事实守卫** | `web-preview/tests/smoke-tab4-settings.mjs` | **`src/` 下 `sidecarMissing` 的命中数恰为 1,且那一处是注释** —— 这是本变更文档立论的那条 grep 事实。⚠ **接线那天这一格会红,是设计好的**:届时必须同时恢复横幅⑤ 并把这一格改成「有生产者」的形态,不许把断言放宽 |
| node 侧负向 | `web-preview/tests/smoke-tab1-interactions.mjs` | 剥注释后 `app.js` 里不再有 `show($("banner-sidecarMissing"))` / `show($("toast-sidecarSwitched"))`,且模板里两处恒挂 `hidden` |
| 导览 | `web-preview/tests/smoke-tour.mjs` | 步数 43;步骤表**不再含 `storage` 锚点**(负向);`index.html` 里 `data-tour="storage"` 属性仍在(DOM 留) |
| 既有回归 | `tests/core/test_state_features_roundtrip.cpp` / `tests/host/test_host_harness.cpp` | `FEAT-SIDECAR-1..11` 与 `HOST SL395` **零改动**,执行读数由统筹在沙箱外跑并回填 PR 描述 |

## 变更文件

- `docs/contract-changes/20260915-sl415-sidecar-ui-hidden.md`(**新增**,本文档)
- `docs/SCVB_CONTRACT.md`(① ② ③ ④ 四处描述改实)
- `docs/STATE_SCHEMA.md`(⑤ ⑥ 两处描述改实;**行为零变化**)
- `web/output/index.html`(三处 DOM 留 + `hidden` 恒挂 + 注释写清裁定与日期)
- `web/output/app.js`(横幅⑤ / toast② 两行 `show()` 摘除,原句逐字留在注释里)
- `web/output/tab-settings.js`(`renderStorage()` 头注钉住「不许让它重新可见」)
- `web/output/tour.js`(「存储状态」那一步整步移除,44 → 43;`data-tour="storage"` 属性保留)
- `web/shared/i18n.js`(`tour.step42/43/44.*` 三语重编号;`tour.step36.body` 三语各删一个词)
- `web-preview/tests/smoke-ui-layout-page.mjs`(G 节页面级判据)
- `web-preview/tests/smoke-tour.mjs` / `web-preview/tests/smoke-tab1-interactions.mjs` /
  `web-preview/tests/smoke-tab4-settings.mjs`(三套的收起口径与负向断言)
- `docs/USER_GUIDE.zh-CN.md` / `docs/USER_GUIDE.md`(sidecar / `owner.lock` 段改成「本版不提供外置存储,特征恒内嵌」)
- `CHANGELOG.md`(预写条目,`变更` 小节)
- `docs/contract-changes/20260911-sl395-sidecar-autoswitch-off.md`(末尾追记本次裁定;**上文一行未改**)

## 审批

待批:随实现 PR **#262** 挂 `status/frozen-contract`,由用户明确批准后合入(06 §3.7)。
定谳来源:统筹 2026-09-15 **SL-415 第 1 轮裁定**(采纳两家复审【红旗】1 的结论、更正其理由;
用户 2026-09-14 B23/B25 裁定「sidecar 不上了」)。
