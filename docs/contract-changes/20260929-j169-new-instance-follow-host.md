# 契约变更说明 —— 20260929-j169-new-instance-follow-host

> **补写**(2026-09-30,[SL-582]):#343 合并时没有附变更文档 —— 它没碰任何冻结契约文件,branch-gate 的
> path guard 不要求;但它在 CHANGELOG 里列在 `### ⚠️ 契约变更` 下(参数默认值变更),而发版清单 A2 与
> `docs/RELEASE.md` 发版清单第 1 步要求这一节每条都有一份文档。做法照 #334 给 #270 补的
> `20260919-sl442-pan-curve-realtime.md`:本文件只登记 #343 合并时的事实,内容取自它的合并提交 `1df80047`
> 与 PR 描述,删除式的读数以 PR 描述为准。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— 不动:`output_enabled` 行只写「on=引擎驱动参数(write),off=follow host」,
  没写默认值;参数面零变化。
- [ ] docs/IPC_CONTRACT.md(共享内存段名/布局)—— 不动。
- [ ] docs/STATE_SCHEMA.md(state schema)—— 不动:§一 的 `output_enabled` 行同样只写语义、不写默认值;布局与 abi 不变。
- [ ] docs/SCVB_CONTRACT.md(桥面契约)—— 不动:没有一处写输出开关的初值;函数、载荷字段与 `contractVersion` 不变。
- [ ] tests/golden/(golden 快照)—— 不动。

归入 ⚠️ 契约变更,是因为它是**参数默认值变更**(CHANGELOG 文件头列的几类之一):新插实例的 `global.output_enabled`
初值由开改为关。它是 state 字段不是自动化参数,但对用户的效果与「默认值变了」相同。

## 变更内容

用户裁定 J169(卡 SL-568):**新插入的 SCVB Output 实例,输出开关默认「跟随宿主」(关)**。

**修前的洞**:新实例的输出开关默认开,且不带任何守卫。J166 的首次开输出守卫只在桥面 OFF→ON 那一下
(`setOutputEnabled(true, {requireConfirm:true})`)置位。平常先开 01 采集会经 J92a 把输出连带关掉,之后再开就一定出
确认条;但局部重采集(`armRecapture`)替用户开采集走的是内部那条路,不触发 J92a。于是「新实例 → 先局部重采集 →
分析 → 播放」这一路上输出一直开着,播放一进已分析区间就不经确认写宿主自动化。

两处落点:

1. **新实例初值开 → 关**:`src/output/OutputProcessor.h` 的 `outputEnabled_` 初值 `true` → `false`。打印器三态求值读它,
   桥面 §1.1 快照的 `global.output_enabled` 也取它,所以界面首帧就是「跟随宿主」。
2. **构造期同步 `session_`**:`src/output/OutputProcessor.cpp` 构造函数里加 `session_.setOutputEnabled(outputEnabled_)`。
   音频线程的 DSP 权威读的是 `OutputSession` 那一份,它自己的成员缺省仍是开(核心类的缺省,#343 没动)。不同步的话,
   界面显示跟随宿主,总线却按引擎曲线出声。

## 载入路径不变

`setStateInformation` 一行没改。三条路径改前改后的结果:

| 路径 | 改前 | 改后 |
|---|---|---|
| 新插实例 | 开,无守卫 | **关,无守卫** |
| 载入,`CFGS` 解得开 / 关(本插件存过的每一份工程) | 按存的值;开 ⇒ 加载守卫 | **不变** |
| 载入,`CFGS` 缺失(只带 `PRMS` 的部分状态)/ 解不开 / 整份拒载 | 不碰开关,保持实例当前值 | **不变**(保持当前值) |

- 以前的版本里新插后没动过开关就保存的工程,存下的是开:重开照旧是开,先出加载守卫横幅,点「继续写入自动化」之后才写。
- 「state 里缺 `output_enabled` 这一个字段」的形态不存在:它是 `CFGS` 头部定长必备字段,读不到就整块拒载
  (`decodeOutputState`);自 T24(#53)起 `getStateInformation` 每次都写 `CFGS`(唯一例外是原样回写被拒载的更高 abi
  blob,那种 blob 本版本本来就不载入)。所以旧工程的缺省口径只剩上表第三行「整节没有」那一种,那一行的代码没动。

### J169a:只带 PRMS 的部分状态载入到全新实例,停在关

上表第三行的代码没动,但它落在**全新实例**上的结果变了:改前是「开且无守卫」(同样能不经确认就写),改后停在新初值
「关」。统筹裁定 **J169a** 认可这个结果:它由新实例初值的变化带来,与 SL-226「没有信息不读成回默认」及 J166 / J169
「没确认绝不写」的严格口径一致。已经开着的实例载入这类部分状态,仍保持开。

## 冻结文档为什么零改动

- 冻结面(`docs/STATE_SCHEMA.md` §一、`docs/PARAMETERS.md`、`docs/SCVB_CONTRACT.md`、`docs/IPC_CONTRACT.md`)与宪法只读
  副本 `docs/constitution/params-v0.md` 都没写输出开关的默认值:`output_enabled` 行只有「on=引擎驱动参数(write),
  off=follow host」这一句语义。核查命令(#343 的 PR 描述里逐条看过,本文件补写时在 `feature/v1` @ `4a003021` 上重跑,
  结论相同):

  ```bash
  grep -rn "output_enabled\|outputEnabled\|输出开关" docs/STATE_SCHEMA.md docs/PARAMETERS.md docs/constitution/params-v0.md docs/SCVB_CONTRACT.md docs/IPC_CONTRACT.md
  ```

- state 布局与 abi、参数表、golden、桥面都没变,所以 #343 没挂 `status/frozen-contract`。
- 代码里写着缺省 `1` 的两处都不是新实例的初值,#343 只改了注释:`OutputStateCodec.h` 的 `OutputState::outputEnabled`
  是值对象的缺省(解码时本字段恒从字节读,不拿它补缺席);`OutputSession.h` 的成员缺省由插件构造时按 `outputEnabled_`
  覆盖。

## 验证用例

`tests/host/test_host_harness.cpp`,tag `[sl568]`:

1. 「HOST SL-568(J169):新插实例的输出开关默认关(跟随宿主),无守卫,存盘写 0」
2. 「HOST SL-568(J169):新插实例的 DSP 跟随宿主参数(不是引擎曲线)」—— 不碰开关,把宿主 vol 压到 −24 dB,总线峰值
   必须明显下降(钉落点 2)
3. 「HOST SL-568(J169):新插实例 → 先局部重采集 → 分析 → 播放,宿主参数零写入」—— 挂在真 processor 上的
   `AudioProcessorListener` 从布防前挂上,gesture 与写入都是 0、打印器 Follow;对照:同条件 `setOutputEnabled(true)`
   之后写入 > 0
4. 「HOST SL-568(J169):载入工程不受新实例初值影响 —— 存的开恢复成开并进加载守卫,存的关恢复成关」
5. 「HOST SL-568(J169):不带 CFGS 的部分状态(只带 PRMS)不动输出开关 —— 开着的仍开,全新实例仍关」(J169a)

另:既有「HOST 加载守卫:恢复 OFF 不设守卫;关输出解除;开输出不算确认」里新实例那条断言由「开」改为 `CHECK_FALSE`。

#343 的 PR 描述记了四格删除式(本机实测,跑 `[sl568],[loadguard],[J166]` 这 9 格):落点 1 改回 `true` 红 5 格
8 条(含修前的写入路径:`begins 6 == 0`、`writes 6 == 0`);删掉落点 2 只红用例 2;载入路径改成「一律关」红用例 4
与既有加载守卫格;`CFGS` 缺失那一支改成「一律关」只红用例 5 前半。

## 兼容性影响

- 既有工程:按存的值恢复,不受影响(见上表第二行)。
- 既有 DAW 自动化:新插实例在用户自己打开输出开关之前不写;打开时照常出首次开输出确认条(J166)。
- 只带 `PRMS` 的轨道 / 参数预设载入到全新实例:输出开关停在关(J169a),改前是开且无守卫。
- 新旧互通:state 布局与 abi、IPC、参数面、桥面都不变。

## 用户文档

#343 在两份用户手册「输出」一节开头各补一段:新插的 SCVB Output 输出开关默认关(跟随宿主:总线按宿主参数出声,
不写自动化),要用时自己打开;打开已有工程时恢复成保存时的状态,存的是开会先出加载守卫横幅。

## 审批

- 用户裁定 J169;统筹裁定 J169a(masterPlan `plan/adjudications.md`)。
- #343 没碰冻结契约文件,没挂 `status/frozen-contract`。本文件是发版前的补登记,不改 #343 的任何结论。
