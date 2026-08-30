# 契约变更说明 —— 20260830-j91-capture-not-persisted

## 变更了哪个冻结契约

| 文件 | 位置 | 驱动裁定 |
|---|---|---|
| `docs/STATE_SCHEMA.md` | §三 `CFGS` 行的 `capture_enabled` 括注 | J91 |
| `docs/STATE_SCHEMA.md` | §三 表下 `[SL-225]` 整条 bullet(**整条替换**) | J91(推翻 SL-225 旧口径) |
| `docs/SCVB_CONTRACT.md` | §1.2 `setCaptureEnabled` 语义行 | J91 + J92a(同一行,合并改) |
| `docs/SCVB_CONTRACT.md` | §1.3 `setOutputEnabled` 语义行 | J92a |
| `docs/SCVB_CONTRACT.md` | §1.23 `recaptureArm` 语义行末句 + 布防豁免 | J91 + J92a |

**abi 不变(仍为 2)。** `CFGS` 的字段布局、长度、顺序一个字节不动;变的只是**写进去什么值**、以及**加载后怎么用**。不加迁移函数。

**批准链**:J91 与 J92a 均由用户 2026-08-30 拍板;四处 J91 文字 + 两处 J92a 文字经统筹转呈用户批准后才动冻结件(本卡先出草稿过目、后动手,未自行改冻结件)。

## 为什么

### J91 —— 采集态不落盘

SL-239 定谳。04 §4.5 的上游改动 ⚠ **只在采集 OFF 期间比对**(`FeatRing::accumulateFp` 的 `if (capturing) return;` —— 那一秒的特征正被写成新基线,拿它跟自己比毫无意义)。于是重开工程时采集若是 ON,这条提醒对用户**根本不存在,而且查不出原因**。

`#146`([SL-225])已经堵过一次「布防替用户开的那一下不许存进工程」,但那只堵住了**一种**来源。用户自己开着采集保存、或打开一份 v5.6 期间已被污染的旧工程,重开后照样是 ON —— 用户 v5.6.2 实测第二轮仍然等不到 ⚠,就是这条路。

根因是把「采集」当成了**工程设置**;它其实是**录制动作**。没有哪个 DAW 会把「录音已布防且正在滚动」存进工程再替你恢复。

### J92a —— 采集 ↔ 跟随引擎互斥(布防豁免)

用户原话:「让采集和跟随引擎开关互斥吧,开一个另外一个就自动关这样」。两个开关同时开着对用户没有意义,而他无法从界面看出自己正处在哪一种组合里。

**为什么要「布防豁免」这一条**:§1.23 [J87] 裁定① 让 `recaptureArm` 在采集原为 `false` 时**替用户打开采集**。若互斥无差别生效,点一次「重采集选区」就会**静默关掉用户的输出引擎**,而撤防只恢复采集(裁定③ 只记了采集那一笔),输出**不会自己回来** —— 一次静默、不可逆、无提示的副作用。用户拍板的是「我手动拨两个开关时互斥」,不是「点一个业务按钮把引擎带走」。故三选一中取「硬互斥保留,但排除布防」。

## 变更内容:逐行对照

### ① `docs/STATE_SCHEMA.md` §三 `CFGS` 行 —— `capture_enabled` 括注

| 项 | 内容 |
|---|---|
| **原文** | `capture_enabled`(**存的是用户自选的采集态**:布防期由 `recaptureArm` 临时替用户打开的那一下不存,见下 [SL-225] 一条) |
| **新文** | `capture_enabled`(**恒写 `0`,采集态不随工程走**:采集是一次录制动作、不是工程设置,重开工程一律为**关**;见下 [J91] 一条。字段保留在布局里,**不删不挪**) |
| **依据(裁定)** | J91 |
| **依据(代码行号)** | `src/output/OutputProcessor.cpp:1199`(`s.captureEnabled = 0u;`,在 `getStateInformation` 内;原为 `(captureEnabled_ && !runtime_.recaptureAutoEnabledCapture) ? 1u : 0u`) |

### ② `docs/STATE_SCHEMA.md` §三 `[SL-225]` bullet —— 整条替换

| 项 | 内容 |
|---|---|
| **原文** | 「**`CFGS.capture_enabled` 存「用户自选值」而非「此刻的运行值」**([SL-225];见 `docs/SCVB_CONTRACT.md` §1.23):…… 用户中途**显式**拧过采集开关(视为接管)与布防前本来就开着的两种情况,都照实存。」(全文见 git 历史) |
| **新文** | 标题改为「**`CFGS.capture_enabled` 恒写 `0` —— 采集态不随工程走**」,正文四段:① 推翻 SL-225 的理由(只堵住一种来源);② **加载侧口径**(`setStateInformation` 一律忽略,恒为关)与**读侧那一道不是冗余**的理由(旧工程磁盘上仍是 1,只有读侧也忽略才自愈);③ 字段保留在布局里、abi 仍 2、双向兼容;④ `recaptureAutoEnabledCapture` 保留、只服务裁定③。 |
| **依据(裁定)** | J91 |
| **依据(代码行号)** | 写侧同①;读侧 `src/output/OutputProcessor.cpp:1741`(`captureEnabled_ = false;`,原为 `captureEnabled_ = s.captureEnabled != 0;`) |

### ③ `docs/SCVB_CONTRACT.md` §1.2 `setCaptureEnabled(on)` 语义行

| 项 | 内容 |
|---|---|
| **原文** | 「…… 变更经 `scvb.state` 回推。**本函数是「用户自选采集态」的唯一入口**:在重采集布防期调用即视为用户**接管**这把闸,撤防不再替他恢复(§1.23 裁定③),且该值照实存进工程(`docs/STATE_SCHEMA.md` §三 `CFGS`)。工程恢复那一路不经过本函数。」 |
| **新文** | 原首句保留;其后改为:「**采集态是纯运行时态,不随工程持久化**([J91] ……)。**副作用(J92a 互斥)**:本函数以 `on=true` 被**用户手动**调用时,同时把 `global.output_enabled` 置 `false`。**布防豁免** —— §1.23 `recaptureArm` 替用户打开采集的那一下**不**触发互斥(§1.23 裁定① 优先),因此点「重采集选区」**不会**关掉用户的输出引擎。在重采集布防期调用即视为用户**接管**这把闸,撤防不再替他恢复(§1.23 裁定③)。工程恢复那一路不经过本函数。」 |
| **删了什么、为什么** | ⑴ 删「**本函数是「用户自选采集态」的唯一入口**」——J92a 之后 §1.3 `setOutputEnabled` 也会写 `capture_enabled`,「唯一入口」当场不成立;且 J91 之后「用户自选采集态」已无持久化含义,留着会让人去找一个不存在的存储语义(统筹裁定:删)。⑵ 删「该值照实存进工程」——J91 之后不存。 |
| **依据(裁定)** | J91、J92a |
| **依据(代码行号)** | 互斥:`src/output/OutputProcessor.cpp:2036`(`if (on && outputEnabled_) applyOutputEnabled(false);`)。豁免**靠调用点区分**,不是 if:布防走内部路 `:2073`(`armRecapture` 内 `applyCaptureEnabled(true)`),不经过本函数。 |

### ④ `docs/SCVB_CONTRACT.md` §1.3 `setOutputEnabled(on)` 语义行

| 项 | 内容 |
|---|---|
| **原文** | 「写 state `global.output_enabled`。ON 且「播放中 ∧ 在 range 内」= PRINT 态……确认入口 = §1.34 `confirmPrintGuard()`)。」 |
| **新文** | 原文全保留,末尾追加「**副作用(J92a 互斥)**」一段:以 `on=true` 手动调用时同时把 `capture_enabled` 置 `false`(并视为接管、清 `recaptureAutoEnabledCapture`);布防期这样做**本次重采集当场作废**;**布防位保留不自动撤防**,理由与提示形态见下。 |
| **为什么保留布防位而不自动撤防** | 撤了用户就丢了刚拖出来的工作选区、且毫无痕迹。留着则 `recapture.armed ∧ !capture_enabled` 这个组合**本身就是可观测的证据**,UI 用 §2.1 两个**现成**字段即可判定 —— **不新增契约字段**。该提示同时覆盖另一条路:用户在布防期手动**关**采集(§1.23 裁定③ 的「接管」)落到同一组合 —— 那是**早于本卡**就存在、界面上一个字都没有的洞,一并盖住。 |
| **提示形态** | **横幅**(持续状态,不是一次性事件),**非阻塞、不弹确认框**(沿 [J85] 口径)。统筹裁定:横幅而非 toast —— toast 会飘走,而「重采集已作废」不会自己好。 |
| **依据(裁定)** | J92a |
| **依据(代码行号)** | `src/output/OutputProcessor.cpp:2123`(`if (on && captureEnabled_) { runtime_.recaptureAutoEnabledCapture = false; applyCaptureEnabled(false); }`);对称内部写点 `applyOutputEnabled` 定义在 `:2015`。 |

### ⑤ `docs/SCVB_CONTRACT.md` §1.23 `recaptureArm` —— 末句改写 + 布防豁免

| 项 | 内容 |
|---|---|
| **原文(末句)** | 「该临时值**不进工程** —— `getStateInformation` 存的是用户自选的采集态(见 `docs/STATE_SCHEMA.md` §三 `CFGS`)。」 |
| **新文(末句)** | 「该临时值**不进工程** —— `getStateInformation` 对 `capture_enabled` **恒写 `0`**([J91]),布防替用户开的那一下自然也不会存;`recaptureAutoEnabledCapture` 这本账只服务于本节裁定③ 的撤防恢复,与持久化无关。**本副作用不触发 §1.2/§1.3 的 J92a 互斥**([J92a],裁定① 优先):布防替用户开采集时**不**关闭输出引擎,故 `footer.recaptureOutputWarn`(「输出引擎仍按全局范围工作,与本次重采集选区无关」)在布防期**仍然可达** —— 它正是为「布防期输出仍 ON」这一格准备的。」 |
| **为什么把豁免写成正面一句** | 不写的话,后来人会顺着「采集与输出互斥」推出「布防期输出必然是关的」,进而把 `footer.recaptureOutputWarn` 当成死代码删掉。它不是死的。 |
| **依据(裁定)** | J91、J92a |
| **依据(代码行号)** | 同③的豁免行(`:2073` 内部路);`footer.recaptureOutputWarn` 判据在 `web/output/app.js` 的 `armed && (output_enabled || recapOutputOpened)`。 |

## 兼容性

| 场景 | 行为 |
|---|---|
| **老工程(v5.6.x 及更早)在新构建里打开** | `CFGS` 照常解码;`capture_enabled` 读到 `1` 也**一律忽略**,采集恢复为关。**v5.6 期间被布防泄漏污染的工程从此自愈**,用户不用做任何事。 |
| **新工程在老构建里打开** | 读到 `capture_enabled = 0`,老构建照旧恢复成「关」。与「用户保存前主动关掉采集」**不可区分**,无异常、无拒载。 |
| **abi / 迁移** | 不升 abi、不加迁移函数、布局零变化。 |
| **用户可感知的变化(代价)** | 「重开工程后 01 采集保持上次状态」这条行为**没有了**。终验清单文字由统筹同步更新;CHANGELOG **两面都写**(自愈的好处 + 旧行为消失)。 |

## 用例

- `HOST J87:工程恢复复位布防运行时态` —— 末行由 `CHECK(captureEnabled())` 改判为 `CHECK_FALSE`。**期望反转,不是回归**;注释就地指向本文件。
- `HOST J87:工程恢复后,陈旧布防不得把恢复出来的采集关掉` —— **被测不变量一个字未改**,只换制造落差的手法:改判前靠「载回一份采集 ON 的工程」,改判后靠**用户手动开采集**(并按回 `recaptureAutoEnabledCapture`,因为桥面 setter 会清它)。
  **反向验证**:注掉 `setStateInformation` 里布防七字段的整块复位 ⇒ 该用例的 `CHECK(captureEnabled())` 转红。
  ⚠ 记一笔:**只**注掉 `recaptureArmed` 一个字段**不足以**让它红(`autoStop` 仍被复位 ⇒ 自动撤防根本不触发)。太窄的反向注入会给出「有牙齿」的假象。
- J91 / J92a 的新增 e2e 见 PR 描述与 `tests/host/test_host_harness.cpp` 的 `[SL247]` 标签。
