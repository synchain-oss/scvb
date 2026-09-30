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
- **「哪一维是手动」是按值推断的,碰巧全等的另一维会被误判**(#302 复审【重要】)。
  > **【已落地,本条改为历史记录】** 统筹选了方案 (A),用户 2026-09-28 批([J162],「18批」),由 SL-548 实装:
  > 段 flags 的 bit3 / bit4 记录「哪一维被接管」,§2.8 `scvb.segments` 每段加可选字段 `manualPan` / `manualVol`,
  > 读回判据改成只看这两位(另有一条旧工程兼容规则)。语义、兼容与删除式见
  > `docs/contract-changes/20260928-sl548-manual-dim-marker.md`。下面这段描述的是 #302 合并时的状态。

  读回链判「某一维是手动常值」
  只能看「每段都是 `user_edited` ∧ 该维各段值相等」—— 段上没有记录「哪一维被接管」的字段(flags 只有 origin 与
  locked)。于是拖过音量卡箍的轨若 pan 各段**碰巧全等**(单段轨,或分析出来每段都居中),pan 也被判成手动常值。
  这不是只在构造夹具里才有的形态:本仓 `HOST SL-188` 用单声源分析出的多段 auto 表,各段 pan 就是相同的
  (见下「删除式」ND5)。后果:
  - 输出 OFF(跟随宿主)时 pan 读回停在段值、不跟参数面。宿主自动化把 pan 拉到别处时,Output 分布图与 Monitor
    `panNow` 仍显示段值,与 J78「显示的是该维度的权威」不符。只影响显示:OFF 下 DSP 两维都读参数面
    (`DspArbiter::processBlock` 的 follow 分支),声音本来就跟着宿主。
  - 检查器里逐段改过、某一维碰巧全等的轨,同样亮「手动接管」标、出「恢复自动」入口与解冻提示。

  改前「单段 `user_edited`」也有同类误判,但只落在单段轨上,范围窄得多。
  **本 PR 为什么不做显式标记**:不升 abi 的前提下,唯一能放标记的地方是段 flags 的空闲位(bit3 起;CRVS 原样读写
  这个 u32,旧插件只取 bit0-2,盘上布局不变)。但是 ① `STATE_SCHEMA.md` §一 `segments[]` 的字段清单要加一项;
  ② JS 读回链要看得到它,`SCVB_CONTRACT.md` §2.8 `scvb.segments` 载荷要加字段(连带 mock 与 parity 检查);
  ③ 分段编辑五个 op、`clearManual`、重分析各自怎么维护这两位,要先定语义;④ 旧构建存下的手动常值段没有这两位,
  要定兼容规则。四件都超出 J131 的范围,需另裁。
  **可选方案(交统筹)**:(A) 按上面做显式标记,走一次冻结契约变更;(B) 读回链让手动维也受输出档约束(OFF 一律读
  参数面)—— 手动接管本来就同时写了参数面,OFF 下声音也走参数面,改后显示与声音一致;要改 §1.16 与
  `IPC_CONTRACT.md` §6.1 ② 的「不看输出档」一句(那是 SL-211 复审定下的口径),而且「手动接管」标误亮的问题 B 解决
  不了;(C) 维持现状,也就是本 PR 的做法。
  #302 合并时的行为由 `tests/core/test_viz_plane.cpp`(`DistReadback` 用例的「已知近似」块)与
  `web-preview/tests/smoke-tab1-interactions.mjs` (a10) 钉住。**SL-548 已按方案 A 把这两格翻过来**(「已知近似」块
  移除,换成 `DistReadback:[SL-548] …` 用例与 tab1 ⑦ 组 (a10)-(a13))。
- **保留下来的另一维曲线不再随重分析更新**(#302 复审):origin 是整段一个值、没法逐维标,手动接管把每段都标成
  `user_edited`,而重分析按 ADR-008 v1.1 不覆盖 user 段。所以拖过音量卡箍之后,这条轨保留下来的 pan 曲线停在拖的
  那一刻:之后调 VAD / 分段参数、局部重新采集,都不会再改它。要让它重新跟着分析走只能「恢复自动」,而那会连手动音量
  一起清掉。改前整条轨是一段常值,同样不随重分析更新(pan 还被压平了),所以这一点不是新引入的;新情况是用户看到 pan
  「还在动」,容易以为它仍是自动的。另外,原本 `origin=user_created` 的段也会被改写成 `user_edited`,波形页该段的
  「C」标记随之变成「E」(改前整表被替换,同样没有 C 了)。要不要在确认条或解冻提示里说明这一点,交统筹。
- **段表为空的那一支,确认条那句「另一项保留原曲线」不成立**(#302 复审):段表为空(还没分析过)时,手动接管照旧写
  一段全时限常值,另一维落默认值(pan 0 / vol 0 dB),而这一段在读回链上两维都算手动 —— 另一维的读回从参数面换成
  这个默认值。行为与改前相同(见上表「段表为空时:不变」),但确认条文案是本 PR 改的,在这一支上「保留原曲线」没有
  对象。可选:① 确认条按段表空 / 非空分成两句(新增 key,三语 + 字体子集);② 空表那一支的另一维改取参数面当前值
  (行为变更)。两者都需另裁,本 PR 不动。

## 兼容性影响

- **工程 state**:格式不变、不迁移。已经用旧构建压成单段的轨,打开后仍是单段常值(旧数据不回推);要找回 pan 曲线,
  用「恢复自动」重新识别该轨。
- **参数面 / 宿主自动化**:零影响(写参数面那一步一个字没改)。
- **桥面**:签名、返回形状、事件与载荷字段零改动;变的是 §1.16 手动接管通道写到段表里的内容。
- **`contractVersion` 保持 `1.0`**:按 J105 的做法 —— 名字 / 签名 / 载荷字段集合都没变,变的是一条行为语义,
  这个值不与任何一侧对拍(见 `20260921-sl464-channel-id-zero-semantics.md` 的核查);若统筹认为「改既有语义」
  须升主版本,请另行裁定,本 PR 不自行升。

## 判据与机检

| 层 | 位置 | 钉什么 |
|---|---|---|
| 纯函数 | `tests/core/test_segment_edit_service.cpp` SERVICE-5/6/7/8/9 | 三段、两维各异的夹具:写 pan 逐段保留 vol、写 vol 逐段保留 pan、段数与边界不变、flags;空表单段 + 钳制;交替写;NaN |
| 读回 native | `tests/core/test_viz_plane.cpp` `DistReadback` 用例 | 多段 vol 常值 / pan 曲线:`manualDimOf` 逐维、`readbackSegsOf` 在输出 ON/OFF 下两维各走各的 |
| 生产接线 | `tests/host/test_host_harness.cpp` `HOST SL-180` | 真 `setTrackManual`:两段曲线拖 vol ⇒ 段数 2、逐段 pan 不变、vol 常值、参数面;读回;**听感**(总线 L/R 在前段偏左、后段偏右);撤销/重做逐字节;镜像拖 pan ⇒ 逐段 vol 保留 |
| 读回 JS | `web-preview/tests/smoke-tab1-interactions.mjs` (a9) | 同 native 读回格 |
| mock 对拍 | `web-preview/tests/smoke-tab2-interactions.mjs` §1.16 段 | mock 桥拖 vol ⇒ 回推段表逐段 pan / 边界与拖前一致、每段 vol = 写入值、pan 维不算手动常值 |
| 已知近似(钉 #302 合并时的行为;**已由 SL-548 翻过来**,见 `20260928-sl548-manual-dim-marker.md`) | `test_viz_plane.cpp` `DistReadback` 用例「已知近似」块;`smoke-tab1-interactions.mjs` (a10) | vol 接管 + pan 各段碰巧全等 ⇒ pan 也判成手动、输出 OFF 读回停在段值(见上「已知连带」;改成方案 A / B 时应当翻) |

### 删除式(本机实测,#302 第 2 轮)

每条只动一处代码;跑完即复原(sha256 回到原值),复原后整套重编重跑全绿。结果栏是**实得**,不是预期。
native 用 `scvb_tests "[viz]"`(15 例)、`scvb_params_tests` / `scvb_host_tests`;JS 用 `node` 直接跑两份冒烟。

| 编号 | 注入 | 实得 |
|---|---|---|
| ND1 | `DistReadback.h` `readbackSegsOf`:两维改回共用 `out.manual`(撤掉逐维) | 1 例红:`test_viz_plane.cpp:723`(输出 ON 下 pan 读成首段)、`:727`(输出 OFF 下 pan 没回落参数面) |
| ND2 | 同处只改 pan 那一行:手动维受输出档约束(方案 B 的形态) | 2 例红:`:749`(「已知近似」格翻过来)、`:764`(单段手动常值 OFF 读回;REQUIRE 截断本例)、VizPublisher 用例 `:878`(手动段的 `panNow`)。即改成方案 B 时这两格要一起改 |
| ND3 | `manualDimOf` 加一行「多段轨上 pan 不再按值推断」 | 1 例红,只红「已知近似」两格 `:747`、`:749` |
| ND4 | `SegmentEditService.h` `makeManualDimSegments` 非空表那一支改回「压成单段、另一维取首段」 | `scvb_params_tests "[service]"` 12 例红 2:SERVICE-5 `test_segment_edit_service.cpp:165`、SERVICE-6 `:182`;`scvb_host_tests "[SL180]"` 2 例全红:HOST SL-180 `test_host_harness.cpp:3477`、HOST SL-188 `:3846`。四处都是开头那条段数 REQUIRE,其后的逐段断言被截断、没跑到 —— 所以另做 ND5 |
| ND5 | 同一函数:段边界保留,但另一维逐段改成首段的值(只压平另一维) | SERVICE-5 `:169`、SERVICE-6 `:186` 红;HOST SL-180 红 5 处:`:3480`(逐段 pan)、`:3497`(读回取后段)、`:3501`(pan 不算手动)、`:3522`(**听感**:后段偏右)、`:3538`(镜像:逐段 vol)。**HOST SL-188 不红** —— 它的多段 auto 夹具来自单声源分析,每段 pan 本来就相同,压平前后一样;这一格只守段数与边界(ND4 红在它的段数 REQUIRE 上),「另一维逐段保留」由 HOST SL-180 守,用例头注已照实写明 |
| JD1 | `readback.js` `readbackSegsOf`:两维共用手动判定 | tab1 只红 (a9) 两格;tab2 全绿 |
| JD2 | `readback.js` 只改 pan 那一处:手动维受输出档约束(方案 B 的形态) | tab1 在 (a6) 抛 TypeError(单段手动常值 OFF 读回变成 null),(a10) 没跑到;tab2 红 1 条「未冻结 + 常值段 ⇒ 仍读常值段(pan)」。即改成方案 B 时 (a6) 与这一条要一起改 |
| JD3 | `readback.js` `manualDimOf` 加一行「多段轨上 pan 不再按值推断」 | tab1 只红 (a10) 两格;tab2 全绿 |
| JD4 | mock `setTrackManual` 非空表那一支改回压成单段 | tab2 只红 2 条 `[SL-180]`(逐段 pan 保留 / pan 维不算手动);tab1 全绿 |

## 变更文件

- `src/output/SegmentEditService.h`(`makeManualConstantSegment` → `makeManualDimSegments`)
- `src/output/OutputProcessor.cpp`(`setTrackManual` 接线与头注)
- `src/core/output/DistReadback.h`(`manualDimOf`,读回逐维)
- `src/output/BridgeArgs.h`(注释:无末端哨兵只来自空表)
- `web/shared/readback.js`、`web/output/tab-tracks.js`(读回逐维、锁定判定、注释)、`web/shared/i18n.js`(三语确认条)
- `web/output/tab-wave.js`(注释:无末端段只来自空段表上的手动接管)
- `web-preview/mock/juce-bridge-mock.js`(mock 同款)
- `docs/SCVB_CONTRACT.md`、`docs/IPC_CONTRACT.md`、`CHANGELOG.md`
- 测试:见上表

## 审批

实质已批(J131,用户 2026-09-28)。本文件随实现 PR 挂 `status/frozen-contract`;契约文字的具体落笔请统筹 / 用户过目。
