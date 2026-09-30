# 契约变更说明 —— 20260929-j177-withdraw-input-uiguideseen

> **状态:变更实质已由用户裁定 [J177] 批准,随本 PR 挂 `status/frozen-contract`。** 裁定事项 = 发版前清单第 8 项
> 「SL-238 / KI-3:`STATE_SCHEMA` 声明了 Input `uiGuideSeen` 而实现没做」,裁定 = **现在就从冻结文档撤回该声明
> (纯文档、零行为)**。用户原话:「8的话现在就改」。卡 **SL-238**。实施细则见统筹裁定 **[J177a]**(下文两处引用)。
> **实现侧零行为改动**:没有改任何 C++ / JS 逻辑;C++ 与 JS 只改了注释(见下「非冻结件」),另加了一组源码级 / 文档级冒烟断言。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。它的 §三 Input state 树里本来就没有 `guide_seen`。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **不动**。
- [x] docs/STATE_SCHEMA.md(state schema)—— §二 Input `ui.guide_seen`:YAML 行尾注释、表里「持久化」一行、
      「编码落点」一条改为「无」,「迁移语义」「反向兼容」两条合成一条「迁移 / 兼容:不涉及」;§三「Input 插件 state
      同用此容器」一句改成实现的实际内容;表里「语义」一行不再写导览步数([J177a],见下);头注「最后更新」补一条。
      **零布局 / 零 abi / 零迁移**。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §3.1 `requestInitialState`(Input)语义格里 `ui.guide_seen` 的一句:
      「随工程持久化」改为「只在本次会话内有效、不随工程保存(每成功载入一份工程 state 就清零)」。
      函数名 / 签名 / 事件名 / **载荷字段零变化**,§7 manifest 不动。
- [ ] tests/golden/(golden 快照)—— **不动**。
- [x] **宪法** `docs/constitution/params-v0.md`(masterPlan `constitution/params-v0.md` 的仓内只读副本)
      v2.4 → **v2.5**:§三 / §四 撤回 Input `ui.guide_seen` 的编码落点;§三 同段不再写导览步数([J177a])。
      字段、类型、默认值、首启判据、state 容器 `abi`、自动化参数面(123 个)**零变化**。

`contractVersion` 保持 `1.0`,不需要豁免:§0.1 第 3 条的禁止面是改名、改参数顺序、改既有字段语义、删除、收窄取值域。
`ui.guide_seen` 的名字、类型、取值、什么时候变 `true`(`setGuideSeen` 写入后)、在 §3.1 快照与 §4.1 `scvb.state`
里的位置都没变。改的是「它能不能在一次工程载入之后留下来」这句持久化描述 —— 那是 state 契约的内容,而实现从来就是现在
写的样子,桥面上任何一方收到的值一个都没变。

## 变更内容

### 为什么撤回(KI-3)

[J81] 修宪(#97)把 Input 首启引导的已读位 `ui.guide_seen` 登记为「随工程走」,并在三处写了它的编码落点:

- `docs/STATE_SCHEMA.md` §二:表里「持久化 | 是,随工程走」,「编码落点」= `InputStateCodec` 的 `InputState` 尾部追加一个 `u32`;
  §三:「Input 插件 state …… `CFGS`(group_id + channel_id + 该位的尾扩)」;
- `docs/SCVB_CONTRACT.md` §3.1:「Input 首启轻量引导已读位,随工程持久化」;
- 宪法 `params-v0.md` §三 段末「编码 = …… 尾部追加 `u32`」与 §四 编码落点注记里的 Input 一条。

**实现从来没做过**,从 #97 起就是这样:`InputStateCodec` 没有这个字段,也没有给尾部追加字段留余地;Input 的这一位
只活在实例内存里。仓内已知限制 KI-3 登记了这处不一致,给了两条路:① 给 `InputStateCodec` 加尾扩机制后实装;② 走契约流程
把声明撤回。**J177 取 ②**:跨工程的「不再显示」本来就由系统级全局位 `guide_seen_global_input` 承担([SL-258] 已真落盘),
首启链里走完或跳过 mini tour 都会连全局位一起写(`web/input/tour-in.js` 的 `endTour`:`setGuideSeen(true, true)`),
所以工程位随不随工程走,用户看不出区别。

### 实现事实核对

在本 PR 的基线 `origin/feature/v1` `61b7575d` 上跑:

```text
$ git show 61b7575d:src/core/state/InputStateCodec.h | sed -n '/^struct InputState/,/^};/p'
struct InputState
{
    std::uint32_t channelId = 0; // 0 = 未分配(首次插入默认)
    std::uint32_t groupId = kInputDefaultGroupId; // 1..8
    std::uint32_t uiScale = 100; // percent
    std::string uiLanguage = "en";
};
$ git show 61b7575d:src/core/state/InputStateCodec.cpp | grep -n "kHeaderBytes + langBytes != size"
77:    if (langBytes > kInputLanguageMaxBytes || kHeaderBytes + langBytes != size)
$ git show 61b7575d:src/input/InputProcessor.cpp | grep -n "chunks.set(\|uiGuideSeen_ =\|return uiGuideSeen_\|decodeInputState("
523:    chunks.set(scvb::state::kFourccCfgs, std::move(payload));
567:    if (!scvb::state::decodeInputState(cfg->payload.data(), cfg->payload.size(), s))
594:    uiGuideSeen_ = false;
948:    uiGuideSeen_ = seen;
954:    return uiGuideSeen_;
$ git log --oneline -S "uiGuideSeen" -- src/core/state/
(无输出)
```

即:`InputState` 只有四项;解码严格等长(长度对不上就整份不载入);`getStateInformation` 只写一块 `CFGS`(523 行);
这一位在 `setStateInformation` 里**解码成功之后**清零(594 行),另外只有桥的写入口(948)与读出口(954)碰它;
`src/core/state/` 的历史里从来没有出现过这个字段名。

### 逐处改动

| # | 文件 / 位置 | 改前 | 改后 |
| --- | --- | --- | --- |
| ① | `STATE_SCHEMA.md` §二 YAML `ui` 行 | 行尾注释只写「guide_seen 默认 false」 | 补「[J177] guide_seen **不落盘**(只在本次会话内有效);scale / language 随工程落盘」 |
| ② | `STATE_SCHEMA.md` §二 表「持久化」 | 是,随工程走 | **否**:只在本次会话内有效,不随工程保存;跨工程的「看过了」由全局默认位承担 |
| ③ | `STATE_SCHEMA.md` §二「编码落点」 | `InputState` 尾部追加一个 `u32`(向后兼容追加段) | **无**(撤回),写明 `InputStateCodec` 的实际内容、谁读写这一位、每成功载入一份工程 state 就清零 |
| ④ | `STATE_SCHEMA.md` §二「迁移语义」「反向兼容」 | 按「老工程无该键 → 默认 false」「旧版忽略未知键」写 | 合成一条「迁移 / 兼容:不涉及」(不在 state chunk 里,新旧字节里都没有它) |
| ⑤ | `STATE_SCHEMA.md` §三 Input 容器一句 | 只含 `PRMS`(无参数,仅 ui)+ `CFGS`(group_id + channel_id + 该位尾扩) | 只含一块 `CFGS`(channel_id + group_id + ui.scale + ui.language),没有 `PRMS`;`ui.guide_seen` 不在其中 |
| ⑥ | `SCVB_CONTRACT.md` §3.1 语义格 | 随工程持久化 | 只在本次会话内有效、不随工程保存(每成功载入一份工程 state 就清零) |
| ⑦ | `STATE_SCHEMA.md` §二 表「语义」 | 独立语言卡 + 5 步 mini tour | 独立语言卡 + mini tour,步数以 `web/input/tour-in.js` 的 `TOUR_IN_STEPS` 为准([J177a],见下) |
| ⑧ | 宪法 `params-v0.md` | 见下一张表 | |

⑤ 那一句里「只含 `PRMS`」与实现也不符(`getStateInformation` 只写 `CFGS`,`ui.scale` / `ui.language` 在 `CFGS` 的
payload 里,Input 没有参数树)。同一句要改,就按实现一起改实,不另起一张卡。

### 顺带:Input 导览不再写步数([J177a])

`STATE_SCHEMA.md` §二 与宪法 §三 都在括注里写着「独立语言卡 + **5 步** mini tour」。[J176] 给 Input 导览加一步
(仓内 #344),这个数随之过期。统筹裁定 [J177a]:v2.5 同时把这两处改成**不写步数** ——
「mini tour,步数以 `web/input/tour-in.js` 的 `TOUR_IN_STEPS` 为准」。步数是界面内容,不是 state 契约;写死在冻结文档里,
每加减一步都要走一次契约变更。本位的名字、语义、首启判据都不因步数变化。历史记录不改:`20260825-input-guide-seen.md`
的提案原文、`CHANGELOG.md` 已发版条目里的「5 步」照原样保留,那是当时的事实。

### 宪法 `params-v0.md` 动了哪几处(原件与副本同改;副本 = 头注行 + 原件逐字节,副本行号 = 原件行号 + 1)

| # | 位置 | 改动 |
| --- | --- | --- |
| ① | 状态行(副本第 4 行) | `v2.4` → `v2.5`,补一句本次修订摘要;v2.4 / v2.3 的原描述整句保留 |
| ② | §三 YAML `ui` 行(副本第 95 行) | 行尾注释补「[J177] guide_seen 不随工程保存(只在本次会话内有效)」 |
| ③ | §三「[J81] Input `ui.guide_seen`」段(副本第 99 行) | 段首括注「5 步 mini tour」改为不写步数([J177a]);段末「编码 = …… 尾部追加 …」一句改为「不随工程保存」,写明清零时机与全局位;其余逐字未动 |
| ④ | §四 编码落点注记的 Input 一条(副本第 109 行) | 改为「不落 state chunk」;同一条里 **Output 侧的半句删去**(见下) |
| ⑤ | 文末 | 追加 `## v2.5 修订(2026-09-29,J177 撤回 Input ui.guide_seen 的编码落点)` 节(修订节只追加,历史节一字未动) |

**④ 里 Output 半句为什么一并删**:原文说「同批的 Output 侧对应改动是 CFGS 布局尾部追加」guide / tour 两个 `u32`。
这与宪法紧上一条(`lang_chosen` 条)自述的「与 `guide_seen` / `tour_seen` / `active_tab` 同处 `PRMS`」矛盾,
实现也在 `PRMS`(`src/output/OutputUiState.h` 的 `writeUiFlags` / `readUiFlags`;`OutputStateCodec.h` 里没有这两个字段,
`web-preview/tests/smoke-tab4-settings.mjs` ⑥ 早有一格钉着)。这半句是当年提案时引用的 T37 分支做法,后来 Output 改走了
`PRMS`。删的是一句与本文件自相矛盾的旧描述,Output 侧的落点与行为都不变。这半句超出 J177 的字面范围,**已由统筹裁定
[J177a]:随 v2.5 一并删**(与前一条自相矛盾、代码也在 `PRMS`,属同一处错登记,同一次修宪订正)。

## 修宪流程落地(`docs/constitution/ADR.md` 文末「修宪流程」第 4 条)

| 项 | 落点 |
| --- | --- |
| ① 改原文并升版本号 | masterPlan `constitution/params-v0.md`:上表五处(一个 commit,message 带 J177) |
| ② 同步只读副本 | 本 PR 的 `docs/constitution/params-v0.md`(头注行 + 原件逐字节) |
| ③ INDEX 口径行 | masterPlan `plan/INDEX.md` 口径行 `params v2.4` → `params v2.5`(与 ① 同一个 commit) |
| ④ 受影响的文档 | 仓内:`20260825-input-guide-seen.md` 文首补一段「后续」指路到本文档(提案原文不改);`docs/KNOWN_ISSUES.md` 删去 KI-3 |
| ⑤ 通知在跑的 agent | 由统筹处理 |

## 非冻结件

- `docs/KNOWN_ISSUES.md`:KI-3 整条删除(它登记的就是这处文档与实现的不一致,撤回之后不再成立);文件头补一句「不再成立的条目
  整条删除,编号不复用」—— KI-4 起的编号在 `DAW_COMPATIBILITY.md` / `RELEASE.md` 等处被按号引用,重新编号会让那些引用指错。
- 注释订正(**只改注释**,把「持久化待 SL-238」「SL-238 落了尾扩字段后改成解码值」这类指向一个已撤回落点的说法改掉):
  `src/input/InputProcessor.h`(`bridgeSetGuideSeen` 头注与 `uiGuideSeen_` 行尾注释)、`src/input/InputProcessor.cpp`
  (`setStateInformation` 里清零那一行的注释)、`src/input/InputBridgeLogic.cpp`(快照 `guide_seen` 行尾注释)、
  `src/output/OutputUiState.h`(头注里讲 Input 那一段)、`src/plugin-common/UiDefaultsStore.h`(头注原写「工程内的
  guide_seen / tour_seen 归 CFGS chunk」,两侧都不对:Output 在 `PRMS`,Input 不随工程保存)、`web/input/app.js`、
  `web/input/tour-in.js`、`web/shared/mock-data.js`(原写「native 未落地」,[SL-258] 起已不成立)。
- `web-preview/tests/smoke-tab4-settings.mjs` ⑥:新增 [J177] 一组五格,见下「验证」。

## 兼容性影响

**零行为变化**:没有改任何读写 state 的代码,工程文件的字节、容器 `abi`、迁移链、首启判据、全局位的键名与落盘位置一律不动。
老工程、新工程、新旧版本互通都不受影响 —— 这一位本来就不在任何工程文件里。

## 验证

- **冒烟断言**(`web-preview/tests/smoke-tab4-settings.mjs` ⑥,[J177] 五格):
  1. `InputStateCodec.h` 剥注释后,`struct InputState` 里没有带 `guide` 的成员;
  2. `InputProcessor.cpp` 剥注释后,`setStateInformation` 里 `decodeInputState(` 之后有 `uiGuideSeen_ = false;`;
  3. `STATE_SCHEMA.md` §二 Input `ui.guide_seen` 小节的「持久化」一行以「否」开头;
  4. `STATE_SCHEMA.md` 全文不再出现 `uiGuideSeen`(§二 编码落点与 §三 Input 容器两处旧写法都带这个名字);
  5. `SCVB_CONTRACT.md` 写出「Input 首启轻量引导已读位 …… 不随工程保存」,且旧写法「……,随工程持久化」零命中。
- **删除式**(每次只动一处,跑完原样复原并逐字节核对;读数见 PR 描述):给 `InputState` 加一个 `uiGuideSeen` 成员 ⇒ 1 红;
  删掉清零那一行 ⇒ 2 红;把清零挪到解码之前 ⇒ 2 红;「持久化」一行改回「是,随工程走」⇒ 3 红;§三 Input 容器一句改回旧文 ⇒ 4 红;
  §二 编码落点改回旧文 ⇒ 4 红;§3.1 改回旧文 ⇒ 5 红;§3.1 保留新文、在别处补回旧文 ⇒ 5 红;§3.1 新文去掉「不随工程保存」⇒ 5 红。
  每一次都只红设计上接它的那一格,其余全绿。
- **宪法同步**:`pwsh scripts/check-constitution-sync.ps1 -MasterPlanDir <按本 PR 改过的 masterPlan constitution 副本>` ⇒ 三份 PASS;
  对**未改**的原件跑 ⇒ `params-v0.md` FAIL、其余两份 PASS。
- **没有**给宪法副本加内容级断言,理由同 J160(`20260928-j160-constitution-guide-keys.md` 的「验证」一节):副本由
  `check-constitution-sync.ps1` 钉成与原件逐字节相等。
- **导览步数([J177a])没有加断言**:「不写步数」是措辞,钉它的只有回扫。回扫命令与逐类结果见 PR 描述(在合入 #344
  之后的本分支上跑);结论:`STATE_SCHEMA.md` 里「5 步」零命中;宪法副本里只剩文末 v2.5 修订节「导览不再写步数」那一条
  对旧文的引述;其余冻结文档(`SCVB_CONTRACT.md` / `PARAMETERS.md` / `IPC_CONTRACT.md`)零命中。

## 审批

- 用户裁定 **J177**(masterPlan `plan/adjudications.md`),原话见文首。
- 统筹裁定 **J177a**(同一文件):① 宪法 §四 Output 半句随 v2.5 一并删;② v2.5 同时把宪法 §三 与 `STATE_SCHEMA.md`
  的「5 步 mini tour」改成不写步数。
