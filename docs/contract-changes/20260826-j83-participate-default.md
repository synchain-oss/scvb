# 契约变更说明 —— 20260826-j83-participate-default

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md(§`participate_in_auto_pan` 默认值行)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [x] docs/STATE_SCHEMA.md(`channels[].participate_in_auto_pan` 默认值行)
- [ ] tests/golden/(golden 快照)
- [x] docs/SCVB_CONTRACT.md §1.15 `setChannelConfig` 语义里的默认值口径
- [x] **用户可见硬约束第 7 条**(真源 `docs/USER_GUIDE.zh-CN.md`,生成物 7 处 + i18n 三语)

> 宪法侧(`docs/constitution/params-v0.md`、`docs/constitution/ADR.md`、`docs/adr/0010-pan-math.md`、
> `docs/ANALYSIS.md`)的同款默认档由统筹随 masterPlan 同步,不在本 PR。

## 变更内容

`channels[].participate_in_auto_pan` 的**默认档**(仅默认档;字段本身的类型、可写性、语义不变):

| | 变更前([J60]) | 变更后([J83]) |
|---|---|---|
| 未显式设置时 | `source_channels == 1` → true;`== 2` → **false** | **一律 true** |
| 显式设置后 | 以用户设置为准 | 不变 |

排除权移到**轨道页每轨的「参与自动声像」开关** —— 立体声轨若要保留原有声像宽度与位置,
由用户显式关闭该轨。

## 为什么改([J83] 裁决,masterPlan `30c5806`)

[J60] 的前提是「立体声源自带声像,自动 pan 会把它压塌」。这个前提在**检测值的实际来源**上不成立:

`source_channels` 来自 `ScvbInputAudioProcessor::prepareToPlay` 的
`getMainBusNumInputChannels() == 1 ? 1 : 2` —— 那是**轨道总线布局**,不是素材本身的声道数。
Cubase 里一条单声道人声放在立体声轨上就报 2,而这是最常见的工程组织方式。

后果在 v5.1 真机上暴露(P0-B):#100 把 `source_channels` 检测接通之后(此前该字段恒 0、
`(0 != 2)` 使**全部 15 轨都参与**),绝大多数人声轨被判成「不参与」→ `AutoAssign` 对不参与轨
按「保持现值」处理 → 现值取自从未被打印过的 pan 参数 = 0 → 分析把 0 烘焙进段表 →
打印器写进自动化。用户看到的是「播到一半大部分轨回到正中、只剩两条在左边」
(那两条恰好是真被识别成 mono 的)。

也就是说:按 [J60] 的字面默认档执行,产品的核心功能(多轨自动声像)在最常见的工程配置下
**整体失效**,而且失效方式是静默的 —— 段照出、轨数照报,只有位置全是 0。

## 兼容性影响

- **既有工程**:`participate_in_auto_pan` 一旦被用户显式设置过就随 CFGS 持久化,**不受影响**;
  只有「从未设置过」的轨改变默认值。
- **既有分析结果**:不自动重算。要让新默认生效需重新分析(段表是分析产物)。
- **听感方向**:此前被静默排除的立体声轨现在会参与自动声像。若用户确实要保留某轨原有声像,
  在轨道页关掉该轨的开关即可 —— 这是**用户可见、可撤销**的一次操作,比一个猜错的默认值好。
- 无 state 编码 / IPC 布局 / 参数表改动;`source_channels` 字段本身照旧(仍服务分布图 ST 角标与
  张开线、viz `stereoMask`、dual-pan 解码 —— 后者本就直接读音频环几何,不经本字段)。

## 硬约束第 7 条改写

真源 `docs/USER_GUIDE.zh-CN.md`「硬约束」节第 7 条,由

> stereo 人声轨默认不参与自动声像分配,需要它参与时必须手动打开。

改为

> 所有轨默认参与自动声像;立体声轨如需保留原有声像宽度与位置,请在轨道页关闭该轨的
> 「参与自动声像」。

改真源后经 `node scripts/gen-hard-rules.mjs --write` 重生成 7 处生成物,
`docs/hard-rules.i18n.json` 的 en/fr 同步并更新 `zhSha256`;EN/FR 译文入 **U17** 人工审校清单。

## 回归

- `HOST P0-B:stereo 检测值不得把轨挤出自动声像` —— 三种检测态(未检测 / mono / stereo)一律参与,
  且显式设置仍然说了算;改回 `sourceChannels != 2` 即红;
- `HOST P0-1:多轨分析后段表 pan 非全零且轨间有差异` —— 端到端守住「分析真的把轨分开」;
- `node scripts/gen-hard-rules.mjs --check` + `node scripts/check-i18n.mjs` 守真源与生成物一致。

## 审批

挂 `status/frozen-contract` 标签;用户明确批准后合入。
裁决真源 = masterPlan `plan/adjudications.md` 的 **J83**(`30c5806`)。
