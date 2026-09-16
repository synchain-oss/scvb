# 契约变更说明 —— 20260911-sl395-sidecar-autoswitch-off

> **状态:已批(统筹 2026-09-13;用户 2026-09-11 A20 / B23 / B25 裁定)。** 用户 2026-09-11 已表态
> 「>8MB 自动转 sidecar 先暂时不上,没测试过不知道稳定性,实际也很难碰到 8MB」(统筹转达);
> 本文档是该意向的契约面记录,随实现 PR 一起落地并挂 `status/frozen-contract`。

## 变更了哪个冻结契约

- [x] docs/PARAMETERS.md —— **仅 state 镜像注释一行**(`:74` 的 `embedded` 注释,**口径一致**,
      引用指向 `STATE_SCHEMA.md §4.2`);自动化参数面**不动**
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动
- [x] docs/STATE_SCHEMA.md(state schema)—— **§4.2「8MB↔sidecar 切换与回滞」整节的口径**。
      容器布局、FEAT 节编码、`u16 flags` 的 `bit0: embedded` 语义**一个字节不动**
- [x] docs/SCVB_CONTRACT.md(桥面契约:函数名 / 签名形态 / 事件名 / **载荷字段**)——
      **只改 §5 `sidecarSwitched` 那一行的「语义」列**(事件本身留在 §7 manifest 枚举里,
      不许删:manifest 是冻结面,删枚举是另一件事)。载荷字段零变化
- [ ] tests/golden/(golden 快照)—— 不动

> ADR-007(采集:存特征不存音频)里「8MB 以上转 sidecar」那句是**机制**描述,本变更不改机制、
> 只改**默认开关**;ADR 正文若要点明「v1 默认关闭自动切换」,走 masterPlan 侧修宪流程,
> repo 内 `docs/adr/0007-capture-features.md` 是从 `docs/constitution/ADR.md` 镜像出来的,
> **只读副本,不得就地编辑**。

## 变更内容

**一句话**:给「>8MB 自动转 sidecar」加一个**单点开关**,v1 出厂值 = **关**;
关掉之后压缩后特征流**一律内嵌**(`embedded=1`),sidecar 的**读写代码与用例全部保留**。

具体口径:

| 面 | 改前 | 改后 |
| --- | --- | --- |
| 阈值判定 | 压缩后 `gzBytes > kSidecarThresholdBytes`(8MB)→ 转 sidecar | 同一个判定,**前面加一道开关**:`kSidecarAutoSwitch == false` ⇒ 恒 `false`(不转) |
| FEAT 节 | 超过 8MB 写 `embedded=0` + sidecar 引用节 | 一律 `embedded=1` 内嵌 |
| §4.2 回滞 | 「一旦转 sidecar,压缩后 <6MB 才收回内嵌」 | **保留原文**,但自动切换关闭时这条不可达(v1 出厂态);开关打开即按原文生效 |
| `sidecarSwitched` 事件 | 转存时发 `{bytes}` + toast② | v1 出厂态**不可达**(新工程不会产生 sidecar);**枚举与文案保留** |
| `sidecarMissing` 事件 | 外部文件缺失/校验失败时发 | 可达性不变(读路径保留:老工程若带 `embedded=0` 仍要能读、能报缺失) |
| 设置页「存储状态」区 | 内嵌 / 外置(9MB 外置样例)两态 | **写入侧**只显示**内嵌**一态;**读路径例外**:打开带 sidecar 的老工程显示「外置」,保存一次后变回内嵌。`FEATURES_EXTERNAL_BYTES` **已删**(web/ + web-preview/ 零引用,阈值真源 = native `kSidecarThresholdBytes`);「外置」文案**保留** |

**为什么是单点开关而不是删代码**:用户要的是「**先暂时不上**」,不是「不要这个功能」。
删掉 `SidecarStore` 会让「读一个别人发来的、带 sidecar 的老工程」这条路径一起死,
而那条路径与本变更无关;单点开关把风险面收到一行判定上,用例也只需改**期望**而不是删格。

## 兼容性影响

- **state 容器与 FEAT 节布局零变化** ⇒ `abi` **不升**,段名**不升 v2**。
- **写方向**:新工程一律内嵌 ⇒ 不再产生 `sessions/<GUID>/` 目录;发给他人时不再有
  「特征体不在工程里」的坑(原 toast② 说的那件事在 v1 上不会发生)。
- **读方向必须保持**:`embedded=0` + 合法 sidecar 引用仍要能正常载入(老工程 / 未来打开开关
  之后写出的工程)。**本变更只关写不关读** —— 这一点是机检要钉住的。
- **老工程保存一次即收回内嵌(要写明)**:打开一份 `embedded=0` 的老工程、**保存一次** ⇒
  写路径走内嵌那一支,同时把外部副本 `store.remove()` 回收(`OutputProcessor.cpp` 的
  `wasSidecar && !refWasUnresolved` 两支)。**不可逆,工程体积随之变大**;这是开关关掉的
  直接后果,不是新增行为。
  判据 = **`HOST SL395`**(起点 `embedded=0` + 合法 sidecar 目录 ⇒ 真 `setStateInformation`
  载入 ⇒ 真 `getStateInformation` 保存一次 ⇒ `featuresInSidecar()` 变假 **且目录已删**);
  core 的 `FEAT-SIDECAR-11` 是**同形复刻**(删除式 **D4** 钉开关;**D6** 注掉生产那句
  `store.remove` ⇒ host 那格红在「目录仍在」)。
- **降级方向**:若将来把开关打开,行为与今天完全一致(§4.2 原文逐字有效)。
- **旧版本读新工程**:v1 写出的工程恒 `embedded=1`,任何版本都能读,无降级问题。
- **不影响任何自动化参数 / IPC 布局 / 桥面载荷字段**。

### 已知口径(要写进文档与文案)

1. 「>8MB 转 sidecar」在 v1 是**已实现但默认不启用**的机制,ADR-007 与 §4.2 的文字仍然成立,
   只是出厂默认关。**不要**把 §4.2 写成「已废弃」。
2. 设置页的「存储状态」**写入侧**只有「内嵌」一态;**读路径例外**(gates 复审第 2 轮更正):
   打开一份带 sidecar 的老工程时会显示「外置」(native 读到引用节即置 `featuresSidecar_`,
   `scvb.state` 发 `embedded:false`),**保存一次之后变回「内嵌」** —— 别再写「v1 恒为内嵌」。
   `toast.sidecarSwitched` 文案保留但 v1 不可达 —— 若 UI 上有「可跳过,机检覆盖」类的自测条目
   (清单 B23/B25 那一族),口径要同步改成「v1 不可达,机检覆盖」。

## 要改的文件与行(只列,不改)

### 代码

| 文件 | 行 | 改什么 |
| --- | --- | --- |
| `src/core/state/SidecarStore.h` | `:18`(`kSidecarThresholdBytes` 旁) | **新增**单点开关常量(建议名 `kSidecarAutoSwitch`,值 `false`),注释写清「出厂关;打开即回到 ADR-007 原文行为」 |
| `src/core/state/SidecarStore.cpp` | `:552`(`return gzBytes > kSidecarThresholdBytes;`) | 前面加开关判定;函数名/签名不动 |
| `web/output/tab-settings.js` | `:68-69`(`FEATURES_EXTERNAL_BYTES = 8 * 1024 * 1024`) | **删掉该常量**(开关关掉后 web/ + web-preview/ 零引用,留着就是死常量);`storageOf()` 的 `external` 改由 `embedded` 驱动;「外置」文案保留(读路径仍走得到) |
| `tests/core/test_state_features_roundtrip.cpp` | `:423`(FEAT-SIDECAR-1 的 `REQUIRE(gz.size() > kSidecarThresholdBytes)` 前置)、`:570`(`kB8`)、新增 `FEAT-SIDECAR-11` | **改期望**:同一份 >8MB 载荷在开关关闭时断言 `embedded=1` 且无 sidecar 目录;原「转 sidecar」断言改挂到「开关打开」这一支(用例保留,不删);新增的 `-11` 是 host `HOST SL395` 的**同形复刻** |
| `tests/host/test_host_harness.cpp` | 新增 `HOST SL395` | 「老工程保存一次 ⇒ 收回内嵌 + 外部目录回收」的**真判据**(走真 `setStateInformation` / `getStateInformation`);删除式 **D6** |

### 文档

| 文件 | 位置 | 改什么 |
| --- | --- | --- |
| `docs/PARAMETERS.md` | `:74`(`embedded: bool # 超 8MB 转 sidecar`) | state 镜像注释同步成与 `STATE_SCHEMA.md :52` **口径一致**(引用写 `见 STATE_SCHEMA.md §4.2` —— PARAMETERS.md 自己的 §四是「命名与兼容规则」,裸 `§4.2` 在那边指不到 state 视图);**只此一行**,自动化参数面不动 |
| `docs/STATE_SCHEMA.md` | §4.2(`:192-195` 那一节) | 节首加一句「**v1 出厂态:自动切换关闭**(单点开关),下列回滞与切换逻辑在开关打开时逐字有效」;§四 `:173` 的 `bit0: embedded` 说明补一句「v1 恒 1」 |
| `docs/STATE_SCHEMA.md` | `:52`(`embedded: bool # 超 8MB 转 sidecar`) | 注释改成「超 8MB 转 sidecar(**v1 默认关,恒内嵌**)」 |
| `docs/SCVB_CONTRACT.md` | `:729`(`sidecarSwitched` 行) | 「语义」列补「v1 出厂态不可达(自动切换关闭);枚举与文案保留」;**载荷字段列不动** |
| `docs/USER_GUIDE.zh-CN.md` | 「硬约束」小节**之外**的 sidecar 段 | 真源在这里,改完跑 `node scripts/gen-hard-rules.mjs` 同步另外 6 处 |
| `docs/USER_GUIDE.md` | 同上(en) | 由生成器产出,不手抄 |
| `docs/ARCHITECTURE.md` | `:75`(「原始音频不存,只存特征(ADR-007);8MB 以上转 sidecar」) | 补「v1 默认不自动转存」 |
| `docs/ANALYSIS.md` | `:16` 附近 | 同上(若该句提到 sidecar) |
| `docs/adr/0007-capture-features.md` | 全文 | **只读副本**(镜像自 `docs/constitution/ADR.md`)⇒ **不得就地编辑**;要改走 masterPlan 修宪 + `check-constitution-sync.ps1` |
| `docs/constitution/*.md` | — | 同上,只读副本,一律不动 |

### 机检 / 清单

- `tests/core/test_state_features_roundtrip.cpp` 的 `FEAT-SIDECAR-1` / `-5` 期望(见上表)。
- `web-preview/tests/smoke-tab4-settings.mjs`(存储状态区内嵌 3MB / 外置 9MB 那两格)与
  `smoke-tab1-interactions.mjs`(`banner.sidecarMissing` / `toast.sidecarSwitched`)——
  **保留**,但口径改成「外置态需开关打开」或标注 v1 不可达。
- 随包自测清单里 B23/B25 两条(>8MB 那两格)标「v1 不可达,机检覆盖」。

## 兼容性影响(降级方向复述,给审批用)

打开开关 = 回到今天的行为,开关是**双向**的;关掉它不会让任何既有工程读不出来。

## 审批

已批:统筹 2026-09-13(用户 2026-09-11 A20 裁定「先暂时不上」;B23 / B25 两格按本文档「机检 / 清单」那一节的口径标「v1 不可达,机检覆盖」);随实现 PR 落地并挂 `status/frozen-contract`。

## 追记:2026-09-14 用户裁定「sidecar 不上」⇒ UI 收起,读路径保留(SL-415)

> 本节**不改动**上面任何一行 —— 上面记录的是 2026-09-11/13 那次「关掉自动切换开关」的
> 契约面,照实保留。本次是用户在同一方向上又走了一步,故**只追记**。

用户 2026-09-14(B23 / B25)裁定:「我们说过直接不上了,不要 sidecar 了」。统筹按**最小改动**
理解并派工(**只收 UI,不删代码**),落地形态如下:

- **收起的是界面**(三处 + 一处导览):
  - 设置页「存储状态」行(`web/output/index.html` 的 `data-gb="settings-storage"` 卡片);
  - 横幅 ⑤「采集数据缺失/过期」(`banner-sidecarMissing`);
  - toast②「已转存外部文件」(`toast-sidecarSwitched`);
  - 引导导览里讲「存储状态」的那一步**整步移除**(`web/output/tour.js`;44 → 43 步,
    `web/shared/i18n.js` 的 `tour.step42/43/44.*` 三语随之重编号 —— 文本按步号取词条,
    不重编号会让「诊断」那一步显示存储状态的说明)。
  - 共同形态:**DOM 留 + `hidden` 恒挂 + `app.js` / `tab-settings.js` 不再对它们调 `show()`**
    (照 [SL-382] / #251 藏灵敏度杆的先例)。判据 = 页面级 `smoke-ui-layout-page.mjs` G 节,
    在**触发条件成立**下量三处 `getClientRects().length === 0`(各配一条对照,免空过)。
- **保留的是能力,一个字节没动**:`SidecarStore`(读写 / `owner.lock` / copy-on-write /
  8MB↔6MB 回滞)、本文档上文那套 `STATE_SCHEMA §4.3` 口径、四份冻结契约里的字段与枚举
  (§7 manifest 的 `sidecarSwitched` 枚举照样在)、`tests/core/test_state_features_roundtrip.cpp`
  的 `FEAT-SIDECAR-*`、`HOST SL395`、以及 `tab-settings.js` 的 `storageOf()` 三格。
  **打开早期版本写的外部特征工程仍照常读回**(`embedded=0` 的读路径依然可达),
  只是屏幕上不再有这一行、也不再有那句提示。
- **契约布局零变化,但冻结文档的描述分家了 ⇒ 走 (b)**:追记这一节第 1 推写的是「本 PR
  **不挂** `status/frozen-contract`、**不新增**变更文档」,**那句已被 2026-09-15 统筹裁定取代** ——
  六处(第 5 推起为**七处**)冻结文档描述的行为在 v1 上不存在,属**行为面**的契约变更,
  「不动布局所以不用走 §5 流程」站不住。现改为:**已补变更文档
  `docs/contract-changes/20260915-sl415-sidecar-ui-hidden.md` 并挂 `status/frozen-contract`**
  (本文档记录的那次契约变更没有被推翻,被用户进一步裁定的是它的**界面出口**)。
- **横幅 ⑤ 与那句提示(按事实改,勿照旧读)**:第 1 推这一节曾写「横幅 ⑤ 此前是『打开一份外部
  特征文件已丢失的老工程』时**唯一**的提示出口」。2026-09-15 统筹 grep 更正:`sidecarMissing`
  在 `src/` 里**没有任何生产者**(只有 `SidecarStore.h` 一条待接线注释),`sidecarSwitched`
  一条都没有 —— 所以**这个口子在本卡之前也从未被填上过**,收起横幅并没有夺走一条活着的提示;
  本卡做的是让契约文档与实现一致,并给「接线时必须同时恢复横幅⑤」装上判据
  (`smoke-tab4-settings.mjs` 的「`src/` 非注释命中数 == 0」)。要恢复提示,把
  `web/output/app.js` 里那一行 `show($("banner-sidecarMissing"), …)` 接回来即可
  —— 原句逐字留在该处注释里。
