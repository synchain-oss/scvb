# 契约变更说明 —— 20260926-j101-j103-contract-withdrawals

> **状态:待批(随本 PR 挂 `status/frozen-contract`)。** 三项变更的实质用户均已裁定:
> **[J101]** / **[J102]** / **[J103]**(2026-09-22,发布前全盘扫描后的五项裁定之三)。
> 本文档记三张卡:
>
> - **[SL-479] / J101**:§5.1 的 `lowSample`、`projectCopy` 两个错误码**撤回**。
> - **[SL-477] / J102**:state 树里的 `global.range` **移出**,写明它是运行期状态、不随工程走。
> - **[SL-466] / J103**:§5.2 `idle` 的定义句补上 `kUnavailable` 那一支(**只改定义句,不拆名**)。
>
> 三项都**不改任何 C++ / JS 行为代码**:前两项是把契约收回到实现早就在做的事上,第三项是把
> 实现早就存在的一支写进定义句。

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md(自动化参数)—— **只动 §二 state 树里的 `range` 一行**(J102,与
      STATE_SCHEMA §一 同一棵树的镜像,只改一份会让两份契约互相矛盾)。**零参数面变更**:
      ParamID / index / 顺序 / skew 一个不动。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 移出 `global.range`、§三 补一条(J102)。
      **零布局 / 零 abi / 零迁移**:`CFGS` 里本来就没有 range。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §5.1 表删两行 + 表下补撤回注记、§2.9 / §4.5 载荷行
      与 §0.8 第 2 条的「九码」→「七码」、§2.9 字段纪律的轨级错误列举去掉 `lowSample`、§7 manifest
      `errorCode` 九值 → 七值(J101);§5.2 `idle` 行(J103)。
- [ ] tests/golden/(golden 快照)—— **不动**。

另改三处非冻结件:`scripts/check-bridge-parity.mjs` 的 `EXPECTED_ENUM_ARRAYS.errorCode` 与它的
OK 行措辞(与 §7 manifest 同源,manifest 删值它必须同步,否则门禁 3i 当场红);
`docs/spikes/E2E-journey.md` 步 7 / L-7 的真机验收项里「有效唱段 <1.5s 黄标」一项(用户照着它
上机会去找一个不存在的提示);`src/input/InputBridgeLogic.{h,cpp}` 里 `kUnavailable → idle` 那两行
**纯注释**(见下 J103)。

## 变更内容

### [J101] 撤回 `lowSample` / `projectCopy`

| 码 | 原承诺 | 实况 |
| --- | --- | --- |
| `lowSample` | 该轨采集后有效唱段 <1.5s → Tab2/Tab3 黄标「样本不足」 | `src/` 全仓零命中;VAD 只产出逐 tile 的后验,**没有**「有效唱段总长」这个聚合,也没有 1.5s 判据 |
| `projectCopy` | sessionGUID 不匹配 → 建独立采集数据副本 + toast① | `src/` 全仓零命中;与 `sessionGuid` 相关的比对 / 换新只在 sidecar 的引用与 owner-lock 路径里(v1 恒内嵌,这条路出厂态不走),**从不发 `scvb.error`**,也没有「检测到工程副本」这一用户提示的生产者 |

用户裁定:两码无生产者、无设计、无用户诉求 ⇒ **契约撤回**,不当待实装项挂账。

改法(`docs/SCVB_CONTRACT.md`):

1. §5.1 标题「九码」→「七码」,括注「与 05 §2.0 警告面一一对应」改为「每码对应 05 §2.0 的一个
   警告面」(05 是计划侧文档,不在本仓;它那边这两码的枚举另行清理,清理之前「一一」不成立);表里删 `projectCopy`、`lowSample` 两行;表下加一段「已撤回两码」
   注记,点名两码并指回本文档 —— 留名字是为了让以后读到 web 侧残留代码的人能查到出处,
   **不是**保留承诺(注记里写明「本节不再承诺它们对应的任何提示面」)。
2. §2.9 / §4.5 两处载荷行 `code:<§5.1 九码之一>` → 七码;§0.8 第 2 条的「§5.1 九码」→ 七码。
3. §2.9 字段纪律「`ch` 仅在轨级错误(`srMismatch`/`channelConflict`/`lowSample`)出现」去掉
   `lowSample`。
4. §7 manifest `enums.errorCode`:九值 → 七值(删 `projectCopy`、`lowSample`,其余顺序不动)。

**第三个零生产者码 `secondOutput` 不在本次范围**:它在 `src/` 里同样没有字面生产者,但横幅②
另有一条真生产者通路(`scvb.conn` 的 `outputReadOnly`),用户可见行为并不缺;J101 只裁了前两码。

### [J102] `global.range` 移出落盘树

三方原本对不上:STATE_SCHEMA §一 把 `global.range.{mode,start_s,end_s}` 列在 state 树里,
§三 的 `CFGS` 行两个清单(当前已落盘 / 挂账未落盘)都没有它,代码**有意**不存。

代码的理由成立(`src/output/OutputProcessor.cpp` 中 `ScvbOutputAudioProcessor::setStateInformation`
里 `[J87]` 那段注释的「range 三字段」一条):不复位的话,上一个工程的 `manual` 区间会继续给新工程
当采集门,而 UI 读到的是默认 `follow` —— 屏幕说「全时间线」,引擎按旧区间挡。

改法:

1. `docs/STATE_SCHEMA.md` §一:删 `range:` 一行,原位留一行 YAML 注释指到 §三。
2. `docs/STATE_SCHEMA.md` §三:在 `CFGS.capture_enabled` 那条之后补一条「`global.range` 是运行期
   状态,不随工程走」,写明复位时机(载入带 `CFGS` 的工程时复位到 `follow` / 0 / 0;只带 `PRMS`
   的参数预设不动)、理由出处(指到函数与注释,不抄行号)、用户可感知的后果、以及桥面镜像不受影响。
3. `docs/PARAMETERS.md` §二:同一棵树的镜像,同样处理。
4. 两份文件头的「最后更新」行各补一条。

### [J103] `idle` 定义句补 `kUnavailable`

`idle` 这个字符串由 C++ 的两支合成(`src/input/InputBridgeLogic.cpp` 的 `claimValue`):
`kActive` 且 `connected_mask` 本位为 0 → `idle`;`kUnavailable` → `idle`。§5.2 原定义句只写了
前一支,UI 落点也只写了前一支(「等待 Output」/「Output 未运行」)。后一支下 `channel_id` 为 `0`
(§3.1 的对外承诺),Input 页的 pill 推导(`web/input/app.js` 的 `pillState`)在 `channel_id === 0`
处先行拦下,真实 UI 是灰色「未选择通道」。

改法:§5.2 `idle` 行的「含义」与「UI 落点」两格各写成 ① / ② 两支,并写明两支靠 `channel_id`
分辨。**不改**枚举到字符串的映射,**不**给 `kUnavailable` 拆新字符串(拆名是值域变更,面比
这次要解决的问题大)。`claimState` 六值枚举与 §7 manifest 不动。

**先核了哪边对,再动文字**:`kUnavailable` 的产生条件全在 `InputSession::openAndClaim()` 的失败分支
(换组 / 打开 registry 非 abi 的失败、`claimInput` 非 `kConflict` 的失败、`createSegments` 失败并
`releaseInput`),每一支都以「本实例未持有任何 slot」结束。`claimValue` 里这一支的行注原写
「§5.2 idle:slot 已声明但 Output 尚未健康读取」—— 那是 ① 支的定义,套在 ② 支上是错的;头文件
那句「kUnavailable(I0 段未打开)→ idle」没错但不全。两行注释改为指向 §5.2 的 ② 支,**映射代码一字未动**。

## 兼容性影响

- **行为零变化**:三项都没有改 C++ / JS 行为代码(C++ 只改了两行注释)。撤回的两码此前就没有任何一处会发出;
  range 此前就不落盘;`idle` 的两支此前就是这样映射、这样渲染。
- **工程 / 自动化 / 新旧版本互通**:零影响。state 布局、abi、迁移链、ParamID 全部不动。
- **`contractVersion` 保持 `1.0`(豁免)**:撤回两码是**真收窄** —— 两个枚举成员被删掉,§0.1 第 3 条
  与 §9.0 第 3 条**两条都命中**,字面上要升主版本。本次豁免是**一次新的裁定**,援引的是 [SL-468]
  的**处理方式**(显式豁免 §0.1 第 3 条),**不是**它的事实认定:SL-464 那次触的是「改既有字段语义」、
  没有删枚举成员,与本次不同类(见 `20260921-sl464-channel-id-zero-semantics.md` 与 #275)。
  豁免之所以对下游无害,理由三条:① 两码在 `src/` 里**零生产者**,从未有一条消息带着它们上桥;
  ② 因此**没有任何消费方会因撤回而坏掉** —— web 侧对它们的处理是七码的超集,只是永远等不到;
  ③ `contractVersion` 这个值只存在于本契约版本行与 §7 manifest,**不与任何一侧对拍**(SL-464
  变更文档里有 grep 证据),升不升都不改变任何构建或运行行为。批准人见下「审批」。
- **§9.0 第 3 条要求的「同批更新 mock 后端」推迟**:本 PR **没有**同步 mock 与 web 侧,推迟到
  同一发版内的后续 PR(SL-528)。在那之前,下面「web 侧残留」一条列出的代码仍按九码处理。
- **web 侧残留**(本 PR 不动,转 SL-528):`web/output/app.js` 的 `KNOWN_CODES`、
  `web/shared/mock-data.js` 的 `ENUMS.errorCode` 与夹具、三语词条 `toast.projectCopy` /
  `lowSample` / `lowSample.full`、`web/output/index.html` 的 `toast-projectCopy`、Tab2/Tab3 的
  lowSample 黄标与 `lowSampleChannels`,以及用到这两码的 web-preview 冒烟夹具。它们是
  契约七码的**超集**,不会让任何门禁变红,也没有生产者会触发它们;但它们不再有契约依据。

## 审批

挂 `status/frozen-contract`。

- **变更实质**:**J101 / J102 / J103**,用户 2026-09-22 裁定。
- **`contractVersion` 豁免**:统筹 2026-09-26 依 SL-468 的处理方式裁定豁免;**用户未对本次单独裁定**。

## 关联

- 卡:SL-479、SL-477、SL-466,`masterPlan/review/suggestion-ledger.md`。
- 未处理(写在 PR 描述):`secondOutput`(用户未裁);`docs/constitution/params-v0.md` 的
  `range` 行(只读宪法副本,改动须走修宪流程);`docs/design/` 设计稿里的「工程副本」toast 样张
  (设计存档,不是契约)。
