# 契约变更说明 —— 20260831-j95-nonrealtime-handover

> 状态:**已获批准并落库**(J95①,用户 2026-08-31 批准;统筹逐行过目后放行)。PR 挂 `status/frozen-contract`。

## 一、变更了哪个冻结契约

| 文件 | 位置 | 驱动裁定 |
|---|---|---|
| `docs/constitution/ADR.md` | `:140` 的 **[J32→ADR-002]** 条,**追加非实时分支**(实时那半句逐字不动) | J95 ① |

**只此一处。** `docs/PARAMETERS.md` / `IPC_CONTRACT.md` / `STATE_SCHEMA.md` / `SCVB_CONTRACT.md` / `tests/golden/` **零改动** —— 本卡不新增桥面函数/事件/字段,不动 IPC 段布局与 abi,不动 state schema。

## 二、为什么(SL-254 实证)

用户 v5.6.3 实测 #48:**离线导出前约 10 秒完全静音**,而实时渲染反相可完美抵消。

### 机理
J32 的注入握手**全程按墙钟闸控**,与音频时间线解耦:
- Output 判 `online` 后**立刻**置 `connected_mask`;
- Input 只看 `connected_mask` 就立刻切静音(直通→静音无滞回);
- 但 Output 要等「Input 的 muted 确认位 **或** `nowMs - onlineSinceMs_ >= 200ms`」才注入。

⇒ 中间存在「**Input 已静音、Output 未注入**」的全零窗。闸门取 `steadyNowMs()`(steady clock),且判定只在 **[M] 25Hz** 定时器里跑。实时下该窗 ≈ 一拍 [M] = 40ms 音频,被 80ms ramp 盖住、听不出;**离线渲染时音频线程跑在墙钟前面 N 倍,同一个墙钟窗覆盖 N×40ms 的样本。**

### 实测:静音时长随倍速线性放大(harness,免 DAW)
| 等效倍速 | 时间线静音 | 折合墙钟 |
|---|---|---|
| 1.7x | **0 s** | — |
| 5.0x | 0.128 s | 25.7 ms |
| 16.4x | 0.512 s | 31.2 ms |
| 48.1x | 2.13 s | 44.3 ms |
| 121.9x | 4.69 s | 38.5 ms |

折合墙钟**恒定 ~25–45ms**,时间线静音线性增长 —— 墙钟闸的判据。10 s ⇒ 约 250x 倍速。**1.7x 那档静音为 0**,正是用户「实时能抵消、离线不能」的由来。

### 为什么**不能**只把 200ms 闸改成样本闸(已实测,J95 明令禁用)
| 倍速 | online@ | Input 静音@ | 实际注入@ | 「仅样本闸」下会注入@ |
|---|---|---|---|---|
| ~5x | 0 | 24 | 36 | **18** |
| ~48x | 0 | **264** | 432 | **18** |
| ~122x | 0 | **456** | 864 | **18** |

**Input 的静音时刻本身也随倍速线性变晚**(它同样是 [M] 驱动)。只改 Output 一侧的闸门 ⇒ 注入远早于 Input 静音 ⇒ **几百块双路叠加**。对 null test 而言与静音同样致命,且**更隐蔽**(它是响的,不是哑的)。**根因不是那个常数,而是两侧切换都挂在墙钟 [M] 上。**

## 三、变更内容:逐行对照

### ① `docs/constitution/ADR.md:140` —— [J32→ADR-002] 追加非实时分支

**原文**(`[J32→ADR-002]` 条,**一字不动**):

```text
- **[J32→ADR-002]** J12 切换协议:5s 滞回仅作用于 静音→直通 方向;直通→静音在确认健康后立即 80ms ramp;Output 置 mask 位后延迟 ≥200ms 再注入(或等 Input muted 确认位);S1 增双路叠加验证项。
```

**新文**:原条**逐字保留**,其后追加一条子项。下为落库后的**原件逐字**(直接从
`masterPlan/constitution/ADR.md` 取行、仅去掉列表缩进;放进代码围栏而非表格单元格,
是为了让反引号原样保留 —— 早前塞进单元格时把反引号换成了 U+02CB `ˋ`,那就不再是「逐字」了):

```text
- **[J95①→ADR-002 补:非实时(离线渲染)分支]**(2026-08-31 用户批准)上述**时间窗与 ramp 全部只对实时路径有效**。宿主经 `setNonRealtime(true)` 宣告离线渲染时,两侧改为**同块交接**:Output 一经判定该轨 `online` 即**当块**置 `injectMask`(不等 muted 确认位、不计 200ms),Input **逐块**读 `connected_mask` 同块静音(无 80ms ramp、无滞回)。理由:200ms 与 5s 都是**墙钟**量,而离线渲染的音频时间线跑在墙钟前面 N 倍,同一窗口会吃掉 N 倍样本 —— 实测折合墙钟恒为一拍 [M] ≈40ms,而时间线静音 5x→0.128s / 48x→2.13s / 122x→4.69s(SL-254,用户离线导出前约 10 秒全静音 ≈250x;1.7x 近实时那档为 0,故实时反相可完美抵消)。**⛔ 不得只把 Output 侧闸门改成样本计**:Input 的静音时刻同样由 [M] 驱动、同样随倍速变晚(实测 48x 下第 264 块才静音),单改一侧会让注入远早于静音 ⇒ 数百块**双路叠加**,对 null test 与静音同样致命且更隐蔽(它是响的,不是哑的)。离线渲染是确定性的、且不存在「双路进录音」的实时风险,故同块交接不违反本条原始意图;**S1 的双路叠加验证项仍只约束实时路径**。**非实时分支豁免的仅是时间窗与 ramp,不豁免健康前提**:`isHealthy()` 的全部条件(claim 存活、slot 为 kSlotActive、心跳不陈旧、mask 位)在非实时下同样必须整体满足——mask 位可能在 Output 释放后残留(释放路径不清 mask),只读 mask 一位会把「无 Output」渲染成整条静音,正是 [J12] 立条要消除的事故面。
```

| 项 | 内容 |
|---|---|
| **依据(裁定)** | J95 ①(用户 2026-08-31 批准) |
| **依据(实证)** | 见 §二 两张实测表(harness 探针,本卡整理为入库回归,见 §五) |
| **依据(代码行号)** | Output 免延迟:`src/core/output/OutputSession.cpp` 的 `nonRealtime() \|\| muted \|\| …`(`evaluateChannels`);标志同步:`src/output/OutputProcessor.cpp` 的 `timerCallback` 首行**与** `prepareToPlay` 各一次 `session_.setNonRealtime(isNonRealtime())`(两处都要:tick 首管运行期切换,prepare 那次卡在首块之前收掉「Input 逐块即时生效、Output 要等下一拍 [M]」的不对称窗)。Input 逐块交接:`src/input/InputProcessor.cpp` 步骤 6 的 `isNonRealtime()` 分支 + `RampSwitcher::snapTo`(`src/core/input/OutputStage.h`);[A] 侧 OutputSlot 寻址:`Registry::outputSlotAtBase`(`src/core/ipc/Registry.h`)经与 `registrySlot` **同一条 registry 租约** + 冻结偏移,零分配零锁零系统调用。 |
| **依据(健康前提)** | 非实时静音条件 = mask 位 ∧ `state == kSlotActive`(**逐块**读,零延迟)∧ ¬`outputStale_`([M] 25Hz 发布 = `InputSession::outputClaimedButStale()`)。三者分工**不可对调**:mask 与 state 必须逐块读(用 [M] 的滞后位替代会让 Input 晚静音而 Output 已注入 = 本条明令要防的双路叠加);心跳新鲜度需要时钟、音频线程取不到,只能由 [M] 发布。`isHealthy()` 与 `connSnapshot()` 现共用 `outputOnline()` 单一真源 —— 两处各写一遍正是本次红旗的成因。 |
| **依据(否决位的极性)** | `outputStale_` 的语义刻意是「**已确认死**」(自称 `kSlotActive` 却心跳陈旧)而**不是**「在场」。第一版按复审建议写成「在场位」(`maskBit ∧ alive`),回归 ② **当场红**:`alive` 在上升沿晚一拍 [M],于是 Output 已注入而 Input 还直通 —— **注入@304 早于静音@320**,正是本条明令要防的双路叠加。改成默认 0 的否决位后,Output 一置 mask,Input **当块**即可静音;而「优雅退场」由逐块 state 零延迟兜住,「崩溃未释放」由本位兜住,两条路都不经过上升沿。 |

## 四、兼容性

| 场景 | 行为 |
|---|---|
| **实时播放** | **逐字不变** —— 非实时分支只在 `isNonRealtime()` 为真时生效;所有既有实时用例(J12/J32 族、`HOST N1`/`HOST I3` 等)期望不变。 |
| **离线导出** | 前段静音消失;交接在同一块完成,无 ramp、无延迟。 |
| **不宣告非实时的宿主** | 走实时路径(现状),行为不变 —— 本变更**不改变**任何未宣告宿主的行为。 |
| **abi / 段布局 / 桥面** | 零变化。 |
| **Output 中途退场(旁通/删除/`releaseResources()`)** | 非实时下 Input **回直通**,不静音。`Registry::releaseOutput()` 把 `state` 置回 `kSlotFree` 时**不清 `connected_mask`**(清 mask 只在 `claimOutput` 的两条路径),故 mask 位会残留;只读 mask 一位会把「无 Output」渲染成整条导出全零 —— 正是 [J12] 要消除的事故面。健康前提(见 §三 ①)堵住这条。回归见 §五 ④。 |
| **混版本(新 Output + 旧 Input)** | ⚠ **前提:两侧都带 J95①**。IPC abi 冻结 ⇒ 新旧插件可同工程共存;若只升 Output,新 Output 在离线下跳过 200ms 闸立即注入,而旧 Input 仍等自己的 [M] C18 路径才静音(迟一拍墙钟 = N×40ms 音频),期间是「Input 直通 ∧ Output 注入」的**双路叠加** —— 即本文档 §二 称之为「更隐蔽(它是响的)」的那类失败。反向(新 Input + 旧 Output)只退回原静音缺陷,无新增风险。本次**不加 IPC 能力位**(那要动 abi);作为**有意识的已知边界**记此:发版说明需提示 Input/Output 同版升级。 |

## 五、用例(四条入库回归,把四个失败方向都钉住)

**集成级(`tests/host/test_host_harness.cpp`,`[host][SL254]`)**

1. **静音时长不随倍速放大** —— 正向:非实时下,不论推样本多快,「Input 静音 ∧ Output 未注入」的重叠窗恒为 0 块。
   反向:退回墙钟闸 ⇒ 重叠窗随倍速线性增长(168→456),该断言红。
2. **注入不早于 Input 静音** —— 守「仅样本闸」那个被明令禁用的修法:非实时下 Output 的首个注入块**不得早于** Input 的首个静音块。
   反向:把闸门改成「仅样本闸」⇒ 注入@368 而 Input 静音@376 ⇒ 该断言红,**而 ① 照样绿**。
3. **实时路径仍走 J32 原协议** —— 断言落在**过渡期**而非稳态:实时下必有「Input 电平严格介于 0 与源之间」的**ramp 过渡块**。
   反向:把非实时那套(`snapTo` 硬切)铺到实时 ⇒ 每块非满即零 ⇒ ramp 块归 0 ⇒ 该断言红(**已实跑验证**),而 ①②④ 照样绿。
   与 ① 的 `rampBlocks == 0` 构成**镜像对**:实时必须有 ramp,非实时必须没有。
   两处踩过的坑,记在此免得后人重蹈:(a) 复审 r1 前只断言稳态「最终注入 + meter>0」,把实时改成硬切照样全绿 —— 测不出它声称要防的事;
   (b) 改写时先试过「注入晚于静音 ≥1 拍 [M]」,**恒假** —— J32 的闸是「muted 确认位**或** 200ms,先到者」,实时下 Input 一静音就置确认位、Output 下一拍即注入,200ms 那条几乎从不生效(实测 注入@16 静音@16);
   且 [M] 的确认位取的是 stageMachine 的**目标档**,[A] 还在走 80ms ramp,故注入合法地落在 ramp 中途(实测 注入@16 而全静音@20)。ramp 的存在性才是实时路径稳定可观测的印记。
4. **健康前提不被豁免(优雅退场)** —— 非实时下 Output 走 `releaseResources()` 退场后(`state = kSlotFree` 而 mask 位**残留**),Input 必须回**直通**。
   反向:只读 mask 一位 ⇒ Input 恒静音 ⇒ 整条导出全零,该断言红(**已实跑验证**,与 ③ 的反向注入同一轮)。
5. **健康前提不被豁免(崩溃未释放)** —— Output **停心跳但不走释放路径**(`state` 仍 `kSlotActive`、mask 位仍在)时,Input 同样必须回直通。
   反向:删掉 `outputStale_` 否决位 ⇒ **⑤ 红(`Input 峰值 = 0`)而 ④ 绿**。这一对实测结果正说明两条覆盖**不同路径**:
   ④ 由 [A] 的**逐块 `state` 读**兜住,⑤ 由 [M] 的**否决位**兜住。
   **没有 ⑤,`outputStale_` 就是零覆盖的死代码** —— 复审 r2 抓出:④ 走 `kSlotFree`,而 `outputClaimedButStale()` 在 state 非 active 时提前 `return false`,故 ④ 全程用不到它。
   实现注意(两个坑,都是实测踩出来的,复审 r2/r3 各一个):
   - 不能停 Output 的 [M](`juce::Timer` 是私有继承,无公开 stop),故用「**持续拨旧 `heartbeat_ms`**」+ settle 循环
     (每轮泵满 ≥1 拍 [M])。一次性拨旧 + 只泵 60ms 会与 Output 的 4Hz 心跳**竞态** ⇒ 偶发假红。
   - **光断言「回了直通」没有牙齿。** settle 循环拉长后,Output 有机会把该轨判下线并 `clearConnectedMaskBit`,
     那时 Input 回直通与否决位无关。实测:只断言 `wentPassthrough` 时,删掉否决位该用例**照样绿**,
     诊断输出 `回直通=1 直通时 mask 仍在=0 期间曾观测到 mask 被清=1`。
     故必须在**观测到直通的那一轮**同时钉住「mask 位仍在 ∧ `state` 仍 active」——
     **「残留 mask 之下仍回直通」才是否决位独有的效果**,别的路径都满足不了。

回归 ① 另补了 ② 同款前置断言 `REQUIRE(firstInputSilent >= 0)` / `REQUIRE(firstInject >= 0)`:
两条断言都是「不许出现 X」的形态,**没发生交接就恒真** ⇒ 空跑全绿,而 ① 是本卡主防线。
(补后 `blocksPerPump = 32` 那档实测**照样绿**,说明此前并未真的空跑;这道前置是防将来。)

**单元级(`scvb_core`,CLAUDE.md §7)**

- `tests/core/test_output_stage.cpp`:`snapTo` 后**立即**进稳态(C19 判据)、紧接 `render` **无中间增益样本**(硬切非 ramp)、未调 `snapTo` 时实时 ramp 行为不变。
- `tests/core/test_ipc_layout.cpp`:`Registry::outputSlotAtBase` 空基址返回 `nullptr`、偏移与冻结的 `kOutputSlotOffset` 逐字相等且不越界。
- `tests/core/test_output_session.cpp`:`setNonRealtime(true)` 令 `evaluateChannels` **当拍**置 `injectMask`(与既有 [J32] 用例同一时序,实时形态在该处红);运行期切回实时后 200ms 闸原样恢复。
- `tests/core/test_input_session.cpp`:`outputClaimedButStale()` 四格 —— active+陈旧→`true`、active+新鲜→`false`、**free+陈旧→`false`**(刻意不重复否决,把「极性反常」变成可执行文档,防后人「顺手修」成 `true`)、无 slot→`false`,含阈值边界;并钉 `outputOnline()` 为 `isHealthy`/`connSnapshot` 的**单一真源**(心跳一陈旧三者同时翻,不许漂移)。

①②③ 三条构成方向相反的钳子:**任一单独存在都会被另一个方向的错误修法绕过**(这正是本卡定谳时先后踩到的坑,且 ③ 的强度不足在复审 r1 被抓出)。④⑤ 守的是 [J12] 的事故面的两条不同路径。

## 六、过目时的两点已裁(J95①)

1. **非实时下无 ramp** —— 已裁:去掉。原议:同块硬切在离线渲染里不会有咔哒(渲染是确定性的、且交接点两侧一侧是静音一侧是注入的同一份信号),但如果你们希望保守,可保留 80ms ramp 只去掉 200ms 延迟 —— 代价是 ramp 期间仍有 80ms×N 的电平凹陷。我倾向**无 ramp**,理由:凹陷同样会被倍速放大。
2. **不加倍速启发式** —— 已裁:不加。原议:JUCE 宿主在 bounce 前调它,但并非所有宿主都严格遵守。若某宿主不宣告,则退回实时路径 = 现状,**不会更糟**;要不要再加一条兜底启发式(如「连续 N 块的时间线推进速度远超墙钟」),我倾向**不加** —— 启发式会在实时大缓冲/系统卡顿时误判,把实时路径也切成同块硬切。
