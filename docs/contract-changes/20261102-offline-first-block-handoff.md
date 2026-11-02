# 契约变更说明 —— 20261102-offline-first-block-handoff

> **状态:只补措辞,挂 `status/frozen-contract`。** 对应的代码已经随 #429、#430 合入 `feature/v1`,本 PR 不动代码。
> 措辞由用户 2026-11-02 批准(「这段说明和备注可以加,你补吧」)。
> **纯文档**:`src/` / `web/` / `tests/`(含 `tests/golden/`)/ `scripts/` / `.github/` 零改动。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 加两条说明文字:§2「离线一轮的清环」,§4「健康 Output 判定(J12)」下面的「[J95①] `connected_mask` 注」。
      同步改三处:§2 `epoch` 行的行内注释多列「离线一轮清环」(SL-505 新增几何改写时也改过这行注释),[A-5] 的「`write_head` 归 0」括注多列「离线清环」,§1 `connected_mask` 行的行内注释补「离线下另含 Output 已接上这一轨」。
      段名、字段、偏移、大小、对齐、abi **零变化**(`node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0)。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**。
- [ ] tests/golden/(golden 快照)—— **不动**。

path guard 之外同 PR 改两处:`docs/adr/0002-routing-architecture.md` 在 [J95①] 下面加「第一块交接」补注,用户同样批准;`CHANGELOG.md` 加本条,并订正 #429、#430 条目里和实现不符的说法(「两边在同一块交接」「Output 读不到时它照旧直通,声音不会丢」、淡入落点、第一块判不了的例外、KI-4 的范围)。

`docs/constitution/` 下的宪法只读副本不动。**待同步**:

- `docs/adr/0002` 是从 `docs/constitution/ADR.md` 拆出来的,头注要求改动走修宪流程,而宪法原件在 masterPlan 仓,不在本仓。这段补注是这条规矩的例外,由用户批准,目前只在 `docs/adr/0002`,宪法原件和副本还没有。
  原件更新之后,副本按「头注 + 原件逐字节」重建(`scripts/check-constitution-sync.ps1` 比对)。在那之前两边有差异。权威文本仍是 `docs/constitution/ADR.md`(`docs/adr/README.md`),这段补注是等待修宪的说明,不改变权威顺序;文字和实现不一致时,按 IPC_CONTRACT 的仲裁规则以实现代码为准。
- `src/core/output/OutputSession.h` 的 `setNonRealtime` 注释、`src/input/InputProcessor.cpp` 的 `processBlock` 里 [SL-254 / J95①] 那段的小标题还写着「同块交接」。本 PR 是纯文档,留到下一张碰这段代码的卡一起改。

## 变更内容

改前的文字说离线是「同块交接」:Output 的 [M] 判定在线就当块注入,Input 逐块读 `connected_mask` 同块静音。#429、#430 之后实现变了,原因是 [M] 按墙钟 25Hz 跑,等它第一拍就是导出开头留一段没平衡的原声
(REAPER 从窗口导出实测 0.25–3.8 秒,命令行导出约 3 秒),长度随机器快慢变,嵌套时每个 Output 还各切各的。

改后(实现:`OutputSession::offlineInjectMask` / `offlineConfirmInjected`、`ScvbOutputAudioProcessor::renderBus`、`InputSession::resetRingForOfflinePass`):

| 情形 | 改前 | 改后 |
| --- | --- | --- |
| 离线:Output 从哪一块开始读 | [M] 发布的 `injectMask`:没 release 就重新 prepare 时是实时留下的那份,第一块就读;release 过或之前没跑过 [M] 时等 [M] 第一拍 | 这次 prepare 之后第一个时间线有效(t0 ≥ 0)的块,由 [A] 自己判定(这之前 [M] 已抢先发布判定时用 [M] 的);[M] 第一次发布之后交回 [M] 每拍发布的判定 |
| 离线:第一块判不了(有一条活着的 Input 心跳陈旧、采样率不符、读方没绑上或几何变了) | —— | 全部不读,留在原声,等 [M] 第一拍。全有或全无只管 [A] 这一次判定,这份判定一直用到 [M] 第一次发布。[M] 的判定是逐轨的,之后还判不在线的轨不进混音,别的轨一读到,它的声音就没了 |
| 离线:总线交叉与逐轨 fade | 80 ms | 同块到位(#429),Output 读到数据的那一块整块替换总线 |
| 离线:`connected_mask` 谁置位 | [M] 判定在线就置 | [A] 这一块读到了这一轨才置(面板关着的轨没东西要读,也置);[M] 只撤不置 |
| 离线:[M] 撤位的依据 | 与实时同一份判定 | 走带停住时写头冻结不清位;心跳陈旧连续两拍才算;看门狗 hold 不闩;失准只在缺口之后还没读到过数据时才算,失准的轨不退出读取。看门狗跳闸、连续无时间线 ≥0.5 秒照旧清全部位,并清空判定 |
| 离线:Input 什么时候静音 | 名义上与 Output 注入同块;[M] 落在 Input 和 Output 的 processBlock 之间时差一块(旧注释写的残留上限 ≤1 块) | Output 读到这一轨之后、Input 自己处理的下一块。宿主让写方提前跑一段时,这个点在时间线上还要再晚这一段。Output 那一块已经整块替换总线,不叠加;接在 Input 之后的发送听得到这几块原声 |
| 离线:Input 的无时间线、负时间线块 | 看 C18 模式字 | 也走「本位在 ∧ 槽活跃 ∧ 心跳没确认陈旧 ⇒ 静音」这条判据 |
| 离线:Output 一直读不到某轨 | [M] 判定在线就置位,Input 照样静音,这条轨没声;持续读不到要过宽限才计失准,计入后才清位 | 位不置,这条 Input 一直直通。别的轨也都读不到时总线直通,原声留在总线上;别的轨读到时总线被替换,这条轨不在导出文件里 |
| 离线:Input prepare | 环不动(采样率或声道变了时照旧改写几何) | 写头归零、`epoch`+1、环体清零。清环这一步不动几何,采样率或声道变了时 prepare 照旧先改写几何 |
| 离线:Output prepare,没有先 release | 上一轮的位留着 | 清空全部位 |
| 离线:读方下锚 | 按实测提前量设限 | 不按实测提前量设限(退回一个环长那一档),锚可以早于写方第一帧,从锚到写方第一帧之间交出清环留下的零,算读到。这是 [A-5] 在离线下的例外 |
| 只有 Input 这条链离线、Output 还在实时路径上 | [M] 置位,Input 静音(KI-4) | **不变** |
| 实时 | [M] 置位、清位;200 ms / muted 确认位 / 80 ms ramp | **不变** |

| 措辞落点 | 说的是什么 | 实现落点 |
| --- | --- | --- |
| IPC_CONTRACT §2「离线一轮的清环」 | Input 离线 prepare 时写头归零、`epoch`+1、环体清零,几何不变;读方怎么处理;[A-5] 的离线例外;只在 `ProcessData` 里报离线的宿主走不到 | `ScvbInputAudioProcessor::prepareToPlay`、`InputSession::resetRingForOfflinePass`、`ShmRingMixSource::firstObservation` / `newGeneration`(规则 5)/ `tryAnchor` / `setLeadUnbounded` |
| IPC_CONTRACT §4「[J95①] `connected_mask` 注」 | 离线下 [A] 置位、[M] 只撤;判定比 [A-8] 宽;Input 什么时候静音;总线的归属;撤和补的先后;Output 离线 prepare 清位 | `OutputSession::offlineConfirmInjected`、`evaluateChannels`(`withdraw`)、`forceClearMask`、`prepare`;`ScvbOutputAudioProcessor::renderSpan` / `renderBypassedUnity` |
| ADR-002「[J95① 补注:第一块交接]」 | 交接点不等 [M];Input 在 Output 读到之后才静音;为什么不违反 ⛔;豁免范围不变 | `OutputSession::offlineInjectMask` / `offlineStartVerdict`、`ScvbOutputAudioProcessor::renderBus`(#429) |

**不属于契约、因此没写进冻结文档的**:`offlineReadyMask_`(含标志位 `kOfflineJudged`)、`preparedAtMs_`(进程内 [M]↔[A] 原子量),`ShmRingMixSource::layoutCurrent`(读方进程内)。`setLeadUnbounded` 本身也在读方进程内,它放宽下锚判据的后果写进了 §2。

## 兼容性影响

- **布局 / abi / 段名零变化**,新旧版本照常互认,不拒连。
- `connected_mask` 仍是 Output 写、Input 读的同一个位。Input 的判据(本位在 ∧ 槽活跃 ∧ 心跳没确认陈旧 ⇒ 静音)本身不变。#430 起离线下无时间线和负时间线的块也走这条判据,之前这些块看 C18 模式字。其余变化在 Output 什么时候置和撤这个位。
- 读方对写头归零的处理(规则 5)本来就有,几何改写走的是同一条路,这次没有改。
- 用户可感知的变化已由 #429、#430 的 CHANGELOG 记录,本 PR 不带来新的行为变化。
- 未修的边界(本 PR 还没进 KNOWN_ISSUES,嵌套测试的 PR 把它和第一块交接的其他边界一起登记):只在 `ProcessData` 里报离线的宿主(JUCE 在 process 里才切标志)prepare 时看不到离线。这时不清环,这一轮没被调用的 Input 留下的音频理论上可能读进文件开头;Output 也不清位,实时留下的位还在,Input 第一块就可能静音,第一块可能用 [M] 已发布的判定。
  第一块判定为 0 而位还留着时,总线直通、Input 静音,这一轨到 [M] 第一拍之前没声。这些都是读代码得出的,没有实测过这样的宿主。
  清环没有挪到标志上升沿去补:在 [A] 上清 4 MB 太重,在 [M] 上会和正在写的 [A] 撞。
- 冻结项自查:`node scripts/check-ipc-doc-parity.mjs --strict-missing` 与 `node scripts/check-bridge-parity.mjs` 退 0;golden 与宪法副本未动。

## 审批

挂 `status/frozen-contract`。用户 2026-11-02 批准补这段说明和备注。
