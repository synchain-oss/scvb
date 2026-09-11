# 契约变更说明 —— 20260911-sl395-sidecar-autoswitch-off

> **状态:草案,待统筹批准。** 用户 2026-09-11 已表态「>8MB 自动转 sidecar 先暂时不上,没测试过
> 不知道稳定性,实际也很难碰到 8MB」(统筹同日转达);本文档是该意向的契约面草案,
> 批准后随实现 PR 一起落地并挂 `status/frozen-contract`。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— 不动
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
| 设置页「存储状态」区 | 内嵌 / 外置(9MB 外置样例)两态 | 只显示**内嵌**一态;`FEATURES_EXTERNAL_BYTES` 常量与「外置」分支**保留**(开关打开即用) |

**为什么是单点开关而不是删代码**:用户要的是「**先暂时不上**」,不是「不要这个功能」。
删掉 `SidecarStore` 会让「读一个别人发来的、带 sidecar 的老工程」这条路径一起死,
而那条路径与本变更无关;单点开关把风险面收到一行判定上,用例也只需改**期望**而不是删格。

## 兼容性影响

- **state 容器与 FEAT 节布局零变化** ⇒ `abi` **不升**,段名**不升 v2**。
- **写方向**:新工程一律内嵌 ⇒ 不再产生 `sessions/<GUID>/` 目录;发给他人时不再有
  「特征体不在工程里」的坑(原 toast② 说的那件事在 v1 上不会发生)。
- **读方向必须保持**:`embedded=0` + 合法 sidecar 引用仍要能正常载入(老工程 / 未来打开开关
  之后写出的工程)。**本变更只关写不关读** —— 这一点是机检要钉住的。
- **降级方向**:若将来把开关打开,行为与今天完全一致(§4.2 原文逐字有效)。
- **旧版本读新工程**:v1 写出的工程恒 `embedded=1`,任何版本都能读,无降级问题。
- **不影响任何自动化参数 / IPC 布局 / 桥面载荷字段**。

### 已知口径(要写进文档与文案)

1. 「>8MB 转 sidecar」在 v1 是**已实现但默认不启用**的机制,ADR-007 与 §4.2 的文字仍然成立,
   只是出厂默认关。**不要**把 §4.2 写成「已废弃」。
2. 设置页不再出现「外置」态,`toast.sidecarSwitched` 文案保留但 v1 不可达 ——
   若 UI 上有「可跳过,机检覆盖」类的自测条目(清单 B23/B25 那一族),口径要同步改成
   「v1 不可达,机检覆盖」。

## 要改的文件与行(只列,不改)

### 代码

| 文件 | 行 | 改什么 |
| --- | --- | --- |
| `src/core/state/SidecarStore.h` | `:18`(`kSidecarThresholdBytes` 旁) | **新增**单点开关常量(建议名 `kSidecarAutoSwitch`,值 `false`),注释写清「出厂关;打开即回到 ADR-007 原文行为」 |
| `src/core/state/SidecarStore.cpp` | `:545`(`return gzBytes > kSidecarThresholdBytes;`) | 前面加开关判定;函数名/签名不动 |
| `web/output/tab-settings.js` | `:63`(`FEATURES_EXTERNAL_BYTES = 8 * 1024 * 1024`) | 保留常量;「存储状态」区改成只渲染内嵌一态(外置分支保留在代码里,由开关驱动) |
| `tests/core/test_state_features_roundtrip.cpp` | `:420`(FEAT-SIDECAR-1 的 `REQUIRE(gz.size() > kSidecarThresholdBytes)` 前置)、`:527`(`kB8`) | **改期望**:同一份 >8MB 载荷在开关关闭时断言 `embedded=1` 且无 sidecar 目录;原「转 sidecar」断言改挂到「开关打开」这一支(用例保留,不删) |

### 文档

| 文件 | 位置 | 改什么 |
| --- | --- | --- |
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

<!-- 待统筹批准后:挂 status/frozen-contract 标签;用户明确「批」后随实现 PR 落地。
     落地时 docs/constitution/ 若需同步,一律从 masterPlan 真源同步,不就地编辑。 -->
