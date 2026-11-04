# 契约变更说明 —— 20261103-realtime-play-start

> **状态:措辞由用户 2026-11-03 批准,与实现同 PR 落地,挂 `status/frozen-contract`。**
> 段名、字段、偏移、大小、对齐、abi **零变化**。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 只补措辞:§2 `epoch` 行的行内注释和「写」那一行补「停着之后从停住的位置后面起播时 +2」,§2 在「离线一轮的清环」后面加「实时起播」一条,§4「健康 Output 判定(J12)」下面加「[实时起播] 注入注」。
      `node scripts/check-ipc-doc-parity.mjs --strict-missing` 退 0。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— **不动**。
- [ ] tests/golden/(golden 快照)—— **不动**。

path guard 之外同 PR 改两处:`docs/adr/0002-routing-architecture.md` 在 [J32] 下面加「[J32 补注:实时起播]」,用户同样批准;`CHANGELOG.md` 加本条。

`docs/constitution/` 下的宪法只读副本不动。**待同步**:同 `20261102-offline-first-block-handoff.md` 的处理,这段补注目前只在 `docs/adr/0002`,宪法原件(masterPlan 仓)和副本还没有;在那之前两边有差异,权威顺序不变。

## 变更内容

走带停着不到约 5.5 秒(Input 还在 5 秒滞回里静音)再按播放,之前开头约 0.3 秒全零:宿主按下播放时一口气先算掉几十块(REAPER 7.82 实测 0.5 ms 内跑完前约 0.2 s),Output 要等 [M] 下一拍看到走带在跑才注入,而 Input 这时已经静音。一层、两层嵌套都一样。Output 装在主轨上时(REAPER 主轨上的 Output 实测比各轨晚 7–10 ms 拿到第一块),约五分之一的起播还会在这之后再断约 1 秒([M] 那一拍正好落在中间,写方已经跑出去、Output 还停着,读方锚不上,按失准踢出去 1 秒)。

| 情形 | 改前 | 改后 |
| --- | --- | --- |
| 实时:停着不到约 5.5 秒再从停住的位置按播放 | 等 [M] 下一拍,开头约 0.3 秒全零 | 第一块判定通过就读,从第一块起混音(80 ms 淡入) |
| 判定条件 | —— | 每一条在用的轨都满足才开,一条不满足就都不开:[M] 上一拍判它可达;Input 的 muted 位已置;[M] 停着时看到它的写头紧贴 Output 停住的位置(或者上一拍是这样、这一拍它已经从这里先走出去而 Output 还停着);第一块的时间线位置就是停住的位置 |
| 判定之后 | —— | [A] 用这份判定(再和 [M] 最新的可达判定求交)读到 [M] 看到走带在跑为止,然后按原判据交还给 `injectMask` |
| 读方 | 按实测提前量下锚;起播的突发让读方锚不上,或者写头一动就撤锚 | 起播窗口里:写方换过代才读;两方同块跳变、中间没有隔代时首次下锚不看提前量,这样下的锚写头往前走多远都不撤;读失败不计失准、不计入 [A-6] 宽限;不量提前量、步长,不算 primed |
| 写方:停着之后从停住的位置后面起播 | `epoch`+1 | `epoch`+2(读方看到的和中间隔了一代一样,不放宽,也不读上一代尾段)。从停住的位置上或它前面起播仍是 +1。停在哪不随 Input 的 prepare 清掉 |
| 写方:走带中定位、循环回跳 | +1 | **不变** |
| Input releaseResources | 模式字留在上一轮的静音,等 [M] 下一拍 | 模式字跟状态机一起回直通 |
| 停得更久(Input 已经回到原声)、刚定位不到一拍就按播放、起播位置和停住的位置对不上、有一条 Input 没确认静音、Input 被旁路 | 等 [M] | **不变**,照旧等 [M] |
| 离线 | 归 `offlineInjectMask` | **不变**,不走这条 |

| 措辞落点 | 说的是什么 | 实现落点 |
| --- | --- | --- |
| IPC_CONTRACT §2 `epoch` 注释、「写」那一行 | 停着之后从停住的位置后面起播 +2 | `ScvbInputAudioProcessor::processBlock`(`startedElsewhere`)、`AudioRing::bumpEpoch` |
| IPC_CONTRACT §2「实时起播」 | 写方的 +1 / +2;读方起播窗口的开启条件与窗口里的规则 | `OutputSession::noteBlockTransport` / `realtimeStartVerdict` / `evaluateChannels`(停住位置的证据)、`ShmRingMixSource::setStartWindow` / `tryAnchor` / `read` |
| IPC_CONTRACT §4「[实时起播] 注入注」 | [A] 不等 [M] 置位就读;J32 的确认就是 muted 位;交还给 [M] 的时点 | `OutputSession::realtimeInjectMask`、`ScvbOutputAudioProcessor::renderSpan` / `renderBypassedUnity` |
| ADR-002「[J32 补注:实时起播]」 | 用的是 J32 括号里那一支(等 muted 确认位),先读、后置位,顺序和字面不同 | 同上 |

**不属于契约、因此没写进冻结文档的**:`reachableMask_`、`tickSawPlaying_`、`stoppedAt_`(停的位置与轨掩码装在一个原子量里)、`startMask_`、`blockWasPlaying_`(Output 进程内 [M]↔[A] 原子量),`ShmRingMixSource` 的起播窗口状态(读方进程内),Input 的 `stoppedLastBlock_` / `stoppedT0_`(写方进程内)。

## 兼容性影响

- **布局 / abi / 段名零变化**,新旧版本照常互认,不拒连。
- `connected_mask` 的写法不变:实时下仍由 [M] 置位、清位。Input 的判据不变,它在起播那一刻本来就静音,不会叠加。
- 新 Input 配旧 Output:旧读方把 +2 当成隔了一代,按原来的规则走(不读上一代尾段),起播照旧等 [M]。旧 Input 配新 Output:从停住的位置后面起播时写方只 +1,新读方的判定靠「写头紧贴停住的位置」和「第一块就在停住的位置」两条证据拦大部分情形;Input 起播时间戳和 Output 不一致的那一种拦不住。两个插件随同一个包发布,混用新旧版本只在装了两份不同版本时才会出现。

## 残留(不进冻结文档)

- muted 只在起播第一块看一次:Input 恰好在这几十毫秒里走到 5 秒滞回终点切回直通时,有不到一拍的混音和原声重叠,总线交叉正把原声往下带。
- Input 单独在播放中重新 prepare(实时)时,接在 Input 之后的发送可能听到最多一拍 [M] 的原声。
- 起播时 Input 和 Output 的时间戳差不到一块时,同 R2。
- 写方预取(领先读方)、从停住的位置后面起播时,读方按隔了一代处理,先等写头确认(R3),第一块读不到;live 的写方不受影响。这是 +2 的代价:不加的话,Input 停着时照常被调用、起播却跳过起播点那一段(LS-17 skip-after-stop)时,读方的起播窗口会把环里的旧内容读出来。宿主调度模拟的 LS-2 因此从「第一帧起在场」改成「预取的轨最多缺第一帧」。

这次没改、实测仍在的开头(登记进 KNOWN_ISSUES 的 KI-21、KI-22):

- 从窗口实时渲染(REAPER「online」):宿主重新 prepare 之后 Input 回到原声,判定不通过,走原来的 J32 路径,开头约 0.12 秒是原声再交叉到混音(不是静音)。
- 按播放之前宿主主线程卡住超过 2 秒:[M] 补跑的那几拍看到 Input 心跳过期,判它不可达,开头约 0.2–0.26 秒原声。
- 定位后不到一拍([M] 25 Hz)就按播放:停住位置的证据还没更新,判定不通过,约 0.3 秒,和改前一样。
- 交接要等 [M] 的那几种情形里,宿主主线程恰好在 Input 静音之后卡住:Output 等不到 [M] 下一拍,中间没声,长度等于卡住的时间(REAPER 在看不见的桌面上从窗口实时渲染时实测约 1.3 秒,KI-22)。v0.10.0 一样,这次没改。

## 实测(REAPER 7.82,自动化,48 kHz / 512)

| 场景 | 改前 | 改后 |
| --- | --- | --- |
| 一层,停一下再播,开头缺的长度 | 0.30–0.34 s | ≤ 0.05 s |
| Output 在主轨上,36 次起播 | 开头 0.31 s,另有 0.63 s 的洞 | 全部 ≤ 0.05 s,没有洞 |
| 从窗口实时渲染、Output 在主轨上,18 次 | 9 次里 2 次断约 0.9 s | 没有约 1 秒的断;开头典型 0.12 s 原声 |
| 两层嵌套从窗口实时渲染,19 次 | 开头静音多为 0.30 s | 静音 ≤ 0.24 s |

## 审批

挂 `status/frozen-contract`。用户 2026-11-03 批准这几处措辞。
