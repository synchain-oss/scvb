# 契约变更说明 —— 20260928-j131-sl180-manual-one-dim

> **状态:实质已批(用户 2026-09-28,J131)。** 用户原话:「298 180 167b 94 218按你说的做,不过180记得要代码上也要改」。
> 统筹对 SL-180 报的方案是「只把音量维固定为常值,pan 维保留原曲线;冻结契约相应条文随改」,用户按此拍板。
> 本文档与实现放在**同一个 PR** 里,挂 `status/frozen-contract`。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:参数集合、id、顺序、值域、默认值一个字节没碰。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **布局不动**(段名 / abi / 偏移 / 字段零改动)。只改
      `VizTrackState` 下「`panNow` / `volDb` 口径」那一段的第 ② 条文字:「手动接管常值段(单段 `user_edited`)」
      → 「该**维**是手动接管常值(逐维判定)」。这是读回链的取值口径,随下面 §1.16 的写入面一起变。
- [ ] docs/STATE_SCHEMA.md(state schema)—— **不动**:没有新字段,段记录格式不变,不需要迁移。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §1.16 `setTrackManual` 的「返回」「语义」「线程/频率」三行;
      §2.8 字段纪律里 `openEnded` 那一句的出处说明。函数名 / 签名 / 返回字段 / 事件名 / 载荷字段**零改动**,
      `check-bridge-parity.mjs` 不受影响。
- [ ] tests/golden/(golden 快照)—— **不动**。

## 变更内容

**一句话**:未冻结轨上拖音量卡箍(`setTrackManual(ch, "vol", v)` 的手动接管通道)**只把音量固定为常值,
pan 曲线原样保留**;拖声像卡箍镜像同款(只固定 pan,音量曲线保留)。

| | 改前 | 改后 |
|---|---|---|
| 段表非空时写入什么 | 整表换成**一段**覆盖全时间线的常值段;被写的那一维 = `value`,**另一维取首段的值** | 段数、每段 `t0`/`t1` 不变;**每一段**的被写维度 = `value`,**另一维逐段原样保留** |
| 段表为空时 | 单段全时限常值,另一维取默认 | **不变**(同左) |
| 段标 | 那一段 `origin=user_edited`、`locked=false` | 每一段 `origin=user_edited`、`locked=false`(同一口径,只是作用到每段) |
| `replacedSegments` / `replacedLocked` | 被替换的段数 / 其中锁定的段数 | 被改写的段数 / 其中原本锁定的段数(数值上相同:都是写入前的段数 / 锁定段数) |
| 撤销 | 一步,回到写入前的段表 | 不变 |
| 参数面 | 同值写进 `v{v}_t{t:02d}_pan` / `_vol`(带 gesture、自写标记) | 不变 |

**改前的现象**(SL-180,#106 遗留①):未冻结轨上拖一下音量卡箍,该轨**整条 pan 曲线**被压成首段那一个 pan 值
—— 声像从此不再随段移动,而用户动的只是音量。这是冻结之外另一条「抹平分析曲线」的路;J85 ⑤ 当时明令不动它,
留给用户裁定,J131 裁定改。

**读回口径随之改成逐维**(不然显示上会把 pan 曲线再压平一次):「某一维是手动常值」=
该轨**每段**都是 `user_edited` 且**该维各段值相等**。命中的那一维读回这个常值(不看输出档,同改前);没命中的
那一维照「输出 ON 读播放头所在段 / OFF 读参数面」走。单段 `user_edited` 是两维同时命中的特例,与改前逐字同形。
行上的「手动接管」标、「恢复自动」入口、解冻提示改为「任一维命中」。实现两侧同形:`web/shared/readback.js`
的 `manualDimOf` / `manualConstantOf` / `readbackSegsOf`,native `src/core/output/DistReadback.h` 同名三函数
(`VizPublisher` 的 `panNow` / `volDb` 走后者)。

**确认条文案**(`tracks.manualOverwriteConfirm`,key 不变)三语同改:
中「将以固定值替换该轨(当前版本)全部分段的这一项,另一项保留原曲线,可撤销」/ 英 / 法同义。
改前那句「替换该轨的全部分段结果」对新行为不再成立。

## 镜像(拖声像卡箍)为什么一并改

两个卡箍走的是**同一个函数的两支**(`makeManualDimSegments(existing, isPan, value)`,改前是
`makeManualConstantSegment`)。只改 vol 那一支要么得在函数里按维度分叉写两套语义,要么留一个「拖 pan 会压平
音量曲线」的对称缺陷 —— 后者与 J131「只固定被拖的那一维」的意思直接相悖。所以按同一条裁定、同一处代码一起改,
并在报告里向统筹单列。

## 已知的连带影响(照实登记)

- **过渡斜坡宽度会变**:段内稳态值逐段不变,但 `CurveEvaluator` 的段间 ramp 宽度按两维较大的那一维反推
  (02 §8.2:`T_eff = 1.5 × max(|ΔP|/rate_pan, |Δv|/rate_gain)`)。被固定的那一维原先在某条边界上主导 ramp 宽度时,
  固定后那条边界上另一维的 ramp 会变窄(更快过渡)。要做到逐样本不变得让 ramp 按维度分开算,那是 02 §8.2 的
  DSP 规格改动,不在本裁定范围内,留给统筹决定要不要立卡。
- **之后新分析出来的区段不继承手动常值**:改前那条单段覆盖全时间线,后来在别处分析出的新段与它重叠、按 ADR-008
  被丢弃,所以手动值「覆盖一切」;改后段边界保留,时间线上**尚无段**的地方(例如跟随模式下还没播到的后半首)
  之后分析出来的新段是 `auto`、带分析值。**被拖的那一维此时一般已被 UI 置了冻结位**(tab-tracks.js「拖动 =
  接管手动」),冻结维度的声音读参数面、不读曲线,所以冻结期间听感不受影响;解冻之后新区段跟随分析值,与 J85
  「解冻即回引擎分析曲线」同向。此时该轨不再满足「每段都是 user_edited」,行上的「手动接管」标会熄灭 —— 如实反映
  「这条轨已不全是手动值」。
- **锁**:与改前同一口径 —— 被写的段一律摘锁(J34 的 locked 保护只约束重分析;确认条如实报「含 N 个锁定段」)。
  这样「恢复自动」(`clearManual`)仍能把整条轨清回 `auto`,与改前一致。

## 兼容性影响

- **工程 state**:格式不变、不迁移。已经用旧构建压成单段的轨,打开后仍是单段常值(旧数据不回推);要找回 pan 曲线,
  用「恢复自动」重新识别该轨。
- **参数面 / 宿主自动化**:零影响(写参数面那一步一个字没改)。
- **桥面**:签名、返回形状、事件与载荷字段零改动;变的是 §1.16 手动接管通道写到段表里的内容。
- **`contractVersion` 保持 `1.0`**:按 J105 的做法 —— 名字 / 签名 / 载荷字段集合都没变,变的是一条行为语义,
  这个值不与任何一侧对拍(见 `20260921-sl464-channel-id-zero-semantics.md` 的核查);若统筹认为「改既有语义」
  须升主版本,请另行裁定,本 PR 不自行升。

## 判据与机检(删除式见 PR 描述)

| 层 | 位置 | 钉什么 |
|---|---|---|
| 纯函数 | `tests/core/test_segment_edit_service.cpp` SERVICE-5/6/7/8/9 | 三段、两维各异的夹具:写 pan 逐段保留 vol、写 vol 逐段保留 pan、段数与边界不变、flags;空表单段 + 钳制;交替写;NaN |
| 读回 native | `tests/core/test_viz_plane.cpp` `DistReadback` 用例 | 多段 vol 常值 / pan 曲线:`manualDimOf` 逐维、`readbackSegsOf` 在输出 ON/OFF 下两维各走各的 |
| 生产接线 | `tests/host/test_host_harness.cpp` `HOST SL-180` | 真 `setTrackManual`:两段曲线拖 vol ⇒ 段数 2、逐段 pan 不变、vol 常值、参数面;读回;**听感**(总线 L/R 在前段偏左、后段偏右);撤销/重做逐字节;镜像拖 pan ⇒ 逐段 vol 保留 |
| 读回 JS | `web-preview/tests/smoke-tab1-interactions.mjs` (a9) | 同 native 读回格 |
| mock 对拍 | `web-preview/tests/smoke-tab2-interactions.mjs` §1.16 段 | mock 桥拖 vol ⇒ 回推段表逐段 pan / 边界与拖前一致、每段 vol = 写入值、pan 维不算手动常值 |

## 变更文件

- `src/output/SegmentEditService.h`(`makeManualConstantSegment` → `makeManualDimSegments`)
- `src/output/OutputProcessor.cpp`(`setTrackManual` 接线与头注)
- `src/core/output/DistReadback.h`(`manualDimOf`,读回逐维)
- `src/output/BridgeArgs.h`(注释:无末端哨兵只来自空表)
- `web/shared/readback.js`、`web/output/tab-tracks.js`(读回逐维、锁定判定、注释)、`web/shared/i18n.js`(三语确认条)
- `web-preview/mock/juce-bridge-mock.js`(mock 同款)
- `docs/SCVB_CONTRACT.md`、`docs/IPC_CONTRACT.md`、`CHANGELOG.md`
- 测试:见上表

## 审批

实质已批(J131,用户 2026-09-28)。本文件随实现 PR 挂 `status/frozen-contract`;契约文字的具体落笔请统筹 / 用户过目。
