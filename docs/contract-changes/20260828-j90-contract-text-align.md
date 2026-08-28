# 契约变更说明 —— 20260828-j90-contract-text-align

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)
- [x] **docs/STATE_SCHEMA.md** —— §4.3 `session_guid` 生成时机;§三 `PRMS` 内容清单补 `session_guid`;§三 `CFGS` `capture_enabled` 存的是哪个值
- [x] **docs/SCVB_CONTRACT.md** —— §1.23 `recaptureArm` 语义列(门控面写反 + 补记两条副作用);§1.2 `setCaptureEnabled` 语义列补「用户接管」口径
- [ ] tests/golden/(golden 快照)

**零行为变更**:本 PR **一行代码都没碰**(`git diff --stat` 只有 `docs/`)。改的是**文字追上已合入的实现** ——
SL-215(#127)、J87(#124/#131/#146)三批实现早已在 `feature/v1` 上,契约文字停在实现之前的口径,
其中 §1.23 一句话与实现**方向相反**。参数集合 / state 布局 / abi / 桥函数名与签名 / 事件名 / 载荷字段
一个字节未动:`params_v0.tsv` 与 `check-bridge-parity.mjs` 不受影响,**不升 abi、不需要迁移函数**。

## 变更内容:逐行对照

代码行号均指本 PR 基线 `e8dc312`(`feature/v1`)。

### ① docs/STATE_SCHEMA.md §4.3 —— `session_guid` 生成时机(SL-215 / PR #127)

| 项 | 内容 |
|---|---|
| **原文** | 「**session_guid**:Output **首次 `getStateInformation()` 时** `juce::Uuid().toDashedString()` 自生成,永久随 state(VST3 无工程路径 API,这是唯一可靠方案);Input 不持有 GUID。」 |
| **新文** | 「**session_guid**:Output **实例构造期**由 `juce::Uuid().toDashedString()` 自生成(**Output 侧的 `juce::Uuid()` 生成点唯一** = `ScvbOutputAudioProcessor` 构造函数;**不是**「本字段的值只可能来自那一处」—— 保存期 copy-on-write 会经 `SidecarStore::generateSessionGuid()` 换成新 GUID 并随本次 PRMS 落盘,加载期还会被 PRMS 存值与**校验通过的** FEAT 引用节 GUID 覆盖),永久随 state(……);Input 不持有 GUID。生成时机提前到构造期是为了让**设置页在首次存盘前就显示真值** —— 否则「存储状态」行在用户第一次保存工程之前恒是废话。加载工程时 `setStateInformation` 读到形状合法的旧值即覆盖它(**工程 > 新生成**);缺失(老工程)或形状非法时保留构造期这一个,下次保存写回。**PRMS 值不一定是加载期终值**:工程内 FEAT 走 sidecar 引用节时,引用节的 GUID **经校验通过后**再压过 PRMS 值(`readFeaturesChunk` 排在 PRMS 之后)。落盘面见 §三 `PRMS`。」 |
| **依据(PR)** | #127([SL-215] 会话 GUID 落地) |
| **依据(代码行号)** | 生成点:`src/output/OutputProcessor.cpp:60`(构造函数内 `sessionGuid_ = juce::Uuid().toDashedString();`,行注 52-59 明写「§4.3 写的『首次 getStateInformation() 时自生成』说的是**生成时机**;这里提前到构造期」)。落盘:`src/output/OutputProcessor.cpp:1165`(`writeSessionGuid(state, sessionGuid_)`,在 `getStateInformation` 内)。读回覆盖:`src/output/OutputProcessor.cpp:1625-1628`(`readSessionGuid` 非空即 `sessionGuid_ = loadedGuid`)。UI 落点:`src/output/OutputEditor.cpp:172` → `web/output/tab-settings.js:492`。形状校验:`src/output/OutputUiState.h:94-102`(非 36 字符 dashed UUID 一律当「没有」)。 |

**「生成时机」是本节唯一写错的字**:落盘格式(36 字符 dashed UUID)、「永久随 state」、「工程 > 新生成」的
优先级,以及 sidecar 路径 `<base>/sessions/<GUID>/` 全部与实现一致(`src/core/state/SidecarStore.cpp:307`、
`SidecarStore.h:24-26`),原样不动。

### ② docs/STATE_SCHEMA.md §三 —— `PRMS` 内容清单补 `session_guid`

| 项 | 内容 |
|---|---|
| **原文** | `PRMS` 行「内容」格 = 「APVTS 参数树(**123** 参数值 [J59/J65])+ ui{scale, language, active_tab, guide_seen, tour_seen, **lang_chosen**([J81])}」—— **没有 `session_guid`** |
| **新文** | 同上,末尾并上「+ **`session_guid`**([SL-215];见 §4.3)」;并在表下新增一条 bullet 说明**为什么落 `PRMS` 而不是 `CFGS`** |
| **新增 bullet** | 「**`session_guid` 落在 `PRMS` 根节点属性面而非 `CFGS`**([SL-215]):`CFGS` 是**定长**布局,新字段只能靠「已知字段后未知尾部原样回写」这一条机制兜底(`OutputStateCodec.cpp` 的 `unknownTail`,且**仅在 abi=2 的枚举尾字段齐全时**才生效),已知字段的失败态还分三种 —— 头部**五个**越界即**整块拒载**,两个枚举尾字段越界**回落默认并计数**,`ui.scale` **不作范围校验**;`ValueTree` 则对字段增删**两个方向**都天然容忍,无需升 abi、无需迁移函数,也不动本节的冻结布局。理由与同挂 `PRMS` 的 `ui.guide_seen`/`tour_seen`/`lang_chosen` 三位逐字相同。」 |
| **依据(PR)** | #127 |
| **依据(代码行号)** | 属性名与写入面:`src/output/OutputUiState.h:81`(`kSessionGuidProp{"session_guid"}`)、`:83-90`(`writeSessionGuid` → `apvtsState.setProperty(...)`,写的是 **APVTS 根节点属性**);同文件 `:69-80` 的头注即本 bullet 的理由原文。调用点:`src/output/OutputProcessor.cpp:1165`(紧挨 `writeUiFlags`,同一张 `apvts.copyState()`);`OutputProcessor.cpp:1159-1162` 的行注对 ui 三位写的是同一条理由。 |

**这一格此前是漏登记**:`session_guid` 确实随 `PRMS` 往返,清单里却没有它 —— 只看 §三 会以为它没有落盘面,
或者去 `CFGS` 里找(那里没有,而 `CFGS` 恰恰是**不能**放它的那一节)。

### ③ docs/SCVB_CONTRACT.md §1.23 `recaptureArm` —— 门控面**写反**(J87 / #124 / #131 / #146)

| 项 | 内容 |
|---|---|
| **原文** | 「布防**只门控采集**:输出引擎恒按 `global.range ∩ coverage` 工作,**与本次选区无关**(03 §2.1,04 §4.2)。」 |
| **新文** | 「**门控面**:布防期**特征记账**按「**工作选区 × 选中轨掩码**」两维硬约束门控,`global.range` **不参与**(落在选区外或未勾选轨的 hop 一律丢弃不记账,04 §4.2 ①);**撤防后当拍恢复 `global.range` 门控**。门控只挡**写入**,既有特征覆盖原样保留(04 §4.1);曲线真身与输出发射一概不受布防影响。」 |
| **依据(PR)** | #124(J87 布防门控)、#131(布防前积压补拉)、#146(SL-225 采集态持久化口径) |
| **依据(代码行号)** | `src/output/OutputProcessor.cpp:1824-1855` `applyFeatureGates()`:布防分支 `1845-1849` 把 `gate` 换成 `HopRange{toHop(recaptureStartS), toHop(recaptureEndS)}` **且** `trackMask = recaptureTracksMask`;`global.range` 分支 `1850-1853` 是 `else if` —— 布防期**根本不进**,故 `global.range` 不参与。落点 `1853-1854`(`setFeatureGate` / `setFeatureTrackMask`)。撤防恢复:`:1986`(`disarmRecaptureLocked` 末行 `applyFeatureGates();` 行注「门控当拍回落到 `global.range`」)。布防期换门控前先按旧门控补拉:`:1930`(`session_.flushFeaturePull()`)。 |

**为什么算「写反」而不是「说得不全」**:原文把 `global.range` 说成布防期**仍然生效**的那一维,而实现里
`global.range` 分支是布防分支的 `else if` —— 布防期它是**唯一不生效**的那一维。按原文实现会当场毁掉
04 §4.1 要守的性质(选区外的既有特征被盖)。**同时删掉的那半句**「输出引擎恒按 `global.range ∩ coverage`
工作」也不成立:`src/core/engine/DspArbiter.cpp` 里 `range` / `coverage` 两个词一次都没出现,输出引擎读的是
**曲线真身**;`global.range` 的实际读者只有 `applyFeatureGates`(记账门控)与分析范围
(`OutputProcessor.cpp:2626-2627`)。新文用「曲线真身与输出发射一概不受布防影响」保留原句想表达的
**真意**(布防不动输出),但不再给出那个错误的机制归因。

### ④ docs/SCVB_CONTRACT.md §1.23 —— 补记两条副作用(裁定①/③)

| 项 | 内容 |
|---|---|
| **原文** | 语义列**只字未提** `recaptureArm` 会写 `capture_enabled`。 |
| **新文** | 「**副作用(布防会替用户开采集)**:布防在 `global.capture_enabled` **原为 `false`** 时把它置 `true`([J87] 裁定①),并记一笔「这一下是我们开的」(`recaptureAutoEnabledCapture`);中途改选区/改轨勾选会再次布防,但该记账**只在 `false→true` 那一跳**记,不重记。**撤防恢复布防前原值**(裁定③):只有**我们替他开的**才关回去,布防前本来就开着的保持开;用户在布防期间**显式**调用 §1.2 `setCaptureEnabled` 即视为**接管**,撤防不再替他动。该临时值**不进工程** —— `getStateInformation` 存的是用户自选的采集态(见 `docs/STATE_SCHEMA.md` §三 `CFGS`)。」 |
| **依据(PR)** | #124(裁定①③)、#146(SL-225「布防替用户开的采集不得存进工程」) |
| **依据(代码行号)** | 裁定①:`OutputProcessor.cpp:1935-1939`(`if (!runtime_.recaptureArmed) { recaptureAutoEnabledCapture = !captureEnabled_; ... }` —— 只在 `false→true` 那一跳记账,行注写明重记会「撤防后再也关不回去」)、`:1951-1954`(`if (!captureEnabled_) applyCaptureEnabled(true);`,且在 `:1949` `applyFeatureGates()` **之后** —— 顺序不可倒)。裁定③:`:1981-1985`(`if (recaptureAutoEnabledCapture && captureEnabled_) applyCaptureEnabled(false);` 后清账)。用户接管:`:1912-1920` `setCaptureEnabled` 首行即 `runtime_.recaptureAutoEnabledCapture = false;`。自动撤防边沿:`:1901`(`tickRecapture` 越右边界 → `disarmRecaptureLocked()`,与桥面撤防同一段代码)。 |

**顺带对齐的一处**:`recaptureArm` 与 `disarmRecapture` 走**同一段** `disarmRecaptureLocked()`,
「传 `tracksMask=0 ∧ startS=endS=0` 视为撤销布防」这句原文成立,原样保留(`:1963-1975` 幂等分支)。

### ④b docs/SCVB_CONTRACT.md §1.23「拒绝态」行 —— 三条拒绝路径也会撤防(复审补登记)

| 项 | 内容 |
|---|---|
| **原文** | 「`armed:false` + `reason`(见上);只读观察态 → `reason:"readOnly"`」—— **只说返回值,不说副作用** |
| **新文** | 同上,并补「**三条拒绝路径都先走一次撤防**(与「撤销布防」「越界自动停」同一段代码):否则上一次布防若是我们替用户开的采集,这一发被拒之后采集会一直开着、门控还留在旧选区上,没人再去撤。」 |
| **依据(代码行号)** | `src/output/OutputEditor.cpp:1939-1957`:三条 `reason`(`noTracks`/`noSelection`/`readOnly`)判完后,`if (reason != nullptr)` 分支**首行**即 `processor_.disarmRecapture();`,行注写的就是上面这条理由 |
| **来源** | PR #153 复审【建议】1(claude-pr-review)。与 ④ 同属「§1.23 副作用漏登记」,**不扩范围** —— 同一函数、同一批 J87 实现、同一条「文字追上实现」的批准面。 |

### ⑤ docs/STATE_SCHEMA.md §三 `CFGS` + docs/SCVB_CONTRACT.md §1.2 —— #146 的「只存用户自选的采集态」

**先答「既有文字是否已覆盖」:没有。** §三 `CFGS` 行原文只写 `capture_enabled` 一个词,
`SCVB_CONTRACT` §1.2 语义列只写「写 state `global.capture_enabled`」—— 两处都读不出
「布防临时值不落盘」这条,也读不出「用户中途拧开关 = 接管」。故**两处各补一句**,落点如下:

| 落点 | 新增文字 | 依据(代码行号) |
|---|---|---|
| `STATE_SCHEMA.md` §三 `CFGS` 行 | `capture_enabled` 后并上「(**存的是用户自选的采集态**:布防期由 `recaptureArm` 临时替用户打开的那一下不存,见下 [SL-225] 一条)」,并在表下新增一条 bullet 展开理由(临时接管 + 布防位本身不持久化 + 采集 ON 期间 Input 不发 `fp_report` 导致上游改动 ⚠ 消失) | `OutputProcessor.cpp:1188`:`s.captureEnabled = (captureEnabled_ && !runtime_.recaptureAutoEnabledCapture) ? 1u : 0u;`;理由原文 = 同文件 `:1176-1187` 行注 |
| `SCVB_CONTRACT.md` §1.2 语义列 | 「**本函数是「用户自选采集态」的唯一入口**:在重采集布防期调用即视为用户**接管**这把闸,撤防不再替他恢复(§1.23 裁定③),且该值照实存进工程(`docs/STATE_SCHEMA.md` §三 `CFGS`)。工程恢复那一路不经过本函数。」 | `OutputProcessor.cpp:1912-1920`(清账 + `applyCaptureEnabled`,行注明写「唯一的调用方是桥面 §1.2 setCaptureEnabled —— 工程恢复那一路直接写 `captureEnabled_` + `session_`,不经过这里」) |

**为什么两处都要写**:`CFGS` 那一格回答「**存什么**」,§1.2 那一格回答「**谁的写入算数**」——
只写一处,另一处的读者仍会得到错的结论。两句互指,不重复叙述机制。

### ⑥ 顺带:§1.23 真源行

`05 §1.4 / §2.3;04 §4.2` → `05 §1.4 / §2.3;04 §4.1 / §4.2;[J87] 裁定①③(变更文档 20260828-j90-contract-text-align)`。
新增的 04 §4.1 是「范围外不覆盖既有特征」的立卡原则(新文第二句的出处),J87 是两条副作用的裁定出处。

## 兼容性影响

**零。** 逐项:

- **参数面**:123 参数的 id / 顺序 / 值域 / 默认值未动,`tests/golden/params_v0.tsv` 不受影响。
- **state 布局 / abi**:`kCurrentAbi=2` 不变,chunk 集合与 TLV 布局不变,**无新字段**
  (`session_guid` 是 #127 就已在 `PRMS` 上的既有属性,本 PR 只是把它**登记进清单**),
  无迁移函数改动,旧工程读新构建、新构建读旧工程的行为一字未变。
- **桥的冻结表面**:函数名 / 签名 / 返回字段 / 事件名 / 载荷字段零改动,
  `node scripts/check-bridge-parity.mjs` 照常绿;§7 manifest **未改**(§1.23 与 §1.2 的
  `returns` 一字未动,本次改的全在 `语义` / `拒绝态` / `真源` 三列)。
- **`contractVersion` 保持 `1.0`,§9.0 第 4 条不触发**:该条要求「改动同步更新 §7 manifest 与
  版本行」,针对的是**冻结表面**的增删(§9 的「只增不改」流程)。本次零表面改动、零新增函数/事件/
  字段,manifest 与版本行都无可同步之处 —— 升版本号反而会让「1.0 = 这一版冻结表面」这个约定失真。
  记在此处不静默(PR #153 复审【建议】3)。
- **i18n**:未新增/改名任何 key,`check-i18n` 不受影响。
- **既有 DAW 自动化 / 既有工程**:不适用 —— 没有行为改动可影响。

## 机器门禁

- `branch-gate` 冻结契约 path guard:本 PR 触碰 `docs/STATE_SCHEMA.md` 与 `docs/SCVB_CONTRACT.md`,
  本文件即其要求的变更文档,PR 挂 `status/frozen-contract`。
- 本机跑绿(与 PR 描述同一份清单):`node scripts/check-bridge-parity.mjs`、
  `pwsh scripts/check-readme-parity.ps1`(其内部即 `check-doc-parity.ps1` 的两对调用)、
  `node scripts/check-ipc-doc-parity.mjs --strict-missing`、
  `npx prettier --check` 三个改动文件。

## 审批

- [x] **用户批准:已批准(2026-08-28,用户,J90)** —— 批准面 = 本文件 ①—⑥ 六处文字对齐,
      范围限定「零行为变更、只让契约文字追上已合入的实现」。
- [x] **复审后在同一批准面内的三处收口**(PR #153,deepseek-pr-review【重要】① + claude-pr-review
      【建议】1/2/3、pr-agent 焦点项;**均为本 PR 新写文字的精度问题,不扩大批准面**):
      ① §4.3 的「生成点唯一」加限定 —— 原写法比原文更强,且与本节自己的 copy-on-write 条
      (「生成 newGUID、本实例改用 newGUID」)打架;`sessionGuid_` 实有四个赋值点
      (`OutputProcessor.cpp:60` 构造期 `juce::Uuid()` / `:1376` CoW 换新 / `:1510` 校验通过的
      FEAT 引用节 / `:1627` PRMS 读回),故改为「**Output 侧的 `juce::Uuid()` 生成点**唯一」并
      逐条列明其余覆盖路径;同时补明「**PRMS 值不一定是加载期终值**」—— `readFeaturesChunk`
      (`:1799`)排在 PRMS 读取(`:1609-1630`)之后,校验通过的 FEAT 引用节 GUID 会再压过它
      (`:1508-1511`),否则 CoW 换过 GUID 的工程删不掉旧 sidecar 目录、留下孤儿。
      ② §三 新 bullet 的 CFGS 理由软化 —— 原写法「尾部追加字段会让旧构建整块拒载并静默把配置
      打回默认」是从既有代码注释原样搬入的**历史口径**,与同表 `CFGS` 行「已知字段后未知尾部原样
      回写」以及 `OutputStateCodec.cpp:192-196` 的 `unknownTail` 机制自相矛盾(且容器级
      `RejectedNewer` 走的是「UI 横幅 + `preservedOriginal` 原样回写」,并非静默重置)。改为按
      现行机制陈述:定长布局的新字段只能靠未知尾部兜底且**仅在 abi=2 枚举尾字段齐全时**生效,
      不如 `ValueTree` 两个方向都天然容忍。**结论未变**(`session_guid` 该落 `PRMS`),变的是理由的
      准确度。
      ③ 新增 ④b:§1.23「拒绝态」行补登记「三条拒绝路径也先撤防」(`OutputEditor.cpp:1939-1957`)。
      ④ 第二轮复审【建议】1:上条软化后的「已知字段各带范围校验与回落」把两种失败态混成一句 ——
      按 `OutputStateCodec.cpp:126-141`,头部六个字段越界是**硬拒载**(`return false`),只有
      `:174-189` 的 `loudness_mode` / `center_slot_policy` 两个枚举尾字段才**回落默认并计数**
      (另 `:148-152`:`0 < remaining < 8` 枚举被截断同样拒载)。已按实现分开写。
      ⑤ 第三轮复审【重要】:上条写成「头部**六个**」把 `ui.scale` 算了进去,而 `uiScale` 在
      `OutputStateCodec.cpp` 里只被 `:122` 读出、`:171` 原样赋值,**全程没有任何范围校验**。
      改为「头部**五个**硬拒载 + 两个枚举回落 + `ui.scale` 不校验」三分。
      另按【建议】2 统一了本节脚本清单与 PR 描述,按【建议】3 补了「`contractVersion` 保持 1.0」
      的理由(见「兼容性影响」末条)。
- **未扩范围(核对时另见两处出入,已单列上报,本 PR 一个字都不动)**:
  ① **契约文档层,第三处出入 —— `STATE_SCHEMA` §4.3 `owner.lock` 的「每 10s 由消息线程刷新」在实现里
     不存在**:`SidecarStore::writeOwnerLock` 只有两个调用点 —— `SidecarStore.cpp:352`(随
     `SidecarStore::write()`,即**保存工程**时)与 `:480`(copy-on-write 抢占时),**没有任何周期性刷新**;
     `OutputProcessor.cpp:795` 那个 250ms 心跳是 IPC registry 的(`Registry.h:32`
     `kHeartbeatIntervalMs = 250`),与 `owner.lock` 无关。判活阈值 30s 本身是真的
     (`SidecarStore.h:22` `kOwnerLockAliveHeartbeatMs = 30000`)。**后果**:一个开着不保存的实例,
     其 `owner.lock` 30 秒后即被判死,后开者不会走 copy-on-write 而是直接共享同一份 sidecar。
     这不是文字问题,**要么补实现要么改契约**,超出 J90「零行为变更」的批准面,留待单开卡裁定。
     复审补的一条独立佐证:`OutputProcessor.cpp:1284-1287` 的代码注释**自己写明**该 10s 周期刷新
     「全仓尚未实现(T40 遗留,已单独落卡)」—— 即这是**已跟踪的实现缺口**,不是本次才发现的漂移。
     开卡后请把 issue 号回填到本条(PR #153 复审建议)。
     **回填(2026-08-28):裁定为「补实现」,契约文字一字未动** —— **SL-233**(PR #154)。
     落地口径分**两个**入口,读实现时别只看其中一个:
     - **续租**(`SidecarStore::refreshOwnerLock()` + `kOwnerLockRefreshIntervalMs = 10000`):
       由 `ScvbOutputAudioProcessor::tickOwnerLockRefresh()` 挂在既有 25Hz tick 上分频调用,
       仅当本实例已走 sidecar、引用节已解开、**且**盘上的 `owner.lock` 归本进程所有时才刷新;
       **锁不存在时不新建**(续租只续自己的租约),锁属他人一律不覆盖。
     - **加载期认领**(`SidecarStore::claimOwnerLockIfUnheld()`):在 `readFeaturesChunk` 里
       sha256 校验通过、确实认下这份 sidecar 之后调用。**这一处会在「锁不存在 / 已判死」时写盘**,
       因为「只有写过 sidecar 的实例才持锁」会让「打开一份上次会话存的工程、本会话还没保存」
       的实例永远不持锁 —— 那正是 §4.3 CoW 要覆盖的最常见一幕。认领同样**绝不覆盖他人的活锁**
       (那是 copy-on-write 唯一的判据),另有 pid=0 与「无特征文件不凭空造只有锁的空目录」两道守卫。
     两个入口的归属判定都用 `pid + processStartEpochMs` 双元组,与 `copyOnWriteIfNeeded` 逐字同口径。
     `OutputProcessor.cpp` 里那条自认「T40 遗留」的注释同步订正。
  ② **代码注释层(非契约文档),两条,建议并成一张「注释订正卡」**:
     - `src/output/OutputUiState.h:69` 把 sidecar 文件名写成 `<basename>-<GUID前8>.scvbfeat`,
       而实现与 §4.3 一致地采用目录式 `<base>/sessions/<GUID>/`(`SidecarStore.cpp:307`、
       `SidecarStore.h:24-26`)。
     - `OutputProcessor.cpp:1158-1159` 与 `OutputUiState.h:78-80` 仍写着本次被替换掉的**历史口径**
       (「追加字段会让旧构建整块拒载并静默把 group/开关/版本打回默认」)。本 PR 把契约改准之后,
       **不准的那一侧变成了代码注释**(PR #153 第二轮复审【建议】2)。
     两条都是契约对、注释旧;本卡零行为变更、不碰代码。
  ③ **实现层的一处观察(核对 ⑤ 时顺带看到,本卡不动)**:`CFGS` 的 `ui.scale` 在
     `OutputStateCodec.cpp` 里只被 `:122` 读出、`:171` 原样赋值,解码器与加载路径
     (`OutputProcessor.cpp:1657` `uiScale_ = static_cast<int>(s.uiScale);`)**都不夹取**;
     夹取只发生在桥面 `setUiScale` 那一路(`:2056` `juce::jlimit`)。而 §7.3 的口径是
     「`setStateInformation` 处理的是用户工程文件里的不可信字节」。是否要在加载路径补夹取,
     属实现裁定,超出 J90 批准面,一并留给上面 ① 的开卡窗口。
     **回填(2026-08-28):裁定为「加载路径补夹取」,契约文字一字未动** —— **SL-234**
     (PR #154)把百分比边界换算收拢成 `scvb::bridge::clampUiScalePercent()`
     (边界仍是 `Min/MaxUiScale` 那一对常量,不新增第二份真源),桥面 `setUiScale` 与加载路径
     共用它。Input 侧 `InputProcessor.cpp:428` 逐字同款的缺口一并补上。
