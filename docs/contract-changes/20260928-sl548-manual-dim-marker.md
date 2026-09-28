# 契约变更说明 —— 20260928-sl548-manual-dim-marker

> **状态:实质已批(用户 2026-09-28,J162)。** 用户原话:「18批,19 stage是最新已发布的包括预发布,prod只是正式版。rc期间404没问题。20同意。」
> 其中「18批」批的是清单第 18 项 = 台账卡 SL-548「手动维要显式标记」:§2.8 `scvb.segments` 每段加可选字段
> `manualPan` / `manualVol`(§0.1 第 3 条的加法,`contractVersion` 仍为 1.0),段 flags 用空闲位 bit3 / bit4
> (盘上布局与 abi 不变),语义按卡上 ①–④。
> 本文档与实现放在**同一个 PR** 里,挂 `status/frozen-contract`。
> 前情:`20260928-j131-sl180-manual-one-dim.md`(#302)「已知连带」里的「哪一维是手动是按值推断的」一条,本卡按其
> 方案 (A) 落地;那一条已改为指向本文件。

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)—— **不动**:参数集合、id、顺序、值域、默认值零改动。§二 的 YAML 视图里也有
      一行 `segments[]`,本卡**不改**它 —— 那一节是参数视角的 state 摘要(同样没列 `lead_timeline`),state 字段的
      真源是 `STATE_SCHEMA.md`。
- [x] docs/IPC_CONTRACT.md(共享内存段名/布局)—— **布局不动**。只改 `VizTrackState` 的「`panNow` / `volDb` 口径」
      第 ② 条的判据文字:「该轨每段 `user_edited` 且该维各段值相等(按值推断)」→「每段都带该维的手动位;旧工程
      兼容规则」。
- [x] docs/STATE_SCHEMA.md(state schema)—— §一 `segments[]` 加 `manual_pan` / `manual_vol`;§三 `CRVS` 行写明
      flags 的位分配,并补一条说明(语义、维护规则、兼容)。**零布局 / 零 abi / 零迁移**(见下「兼容性」)。
- [x] docs/SCVB_CONTRACT.md(桥面契约)—— §2.8 `scvb.segments` 载荷行加 `manualPan?:bool, manualVol?:bool`,
      字段纪律补一段;§1.16 ② 的写入面与「某一维是手动常值」判据;§5.4「共同后置」补手动标记的维护。函数名 /
      签名 / 事件名零改动。
- [ ] tests/golden/(golden 快照)—— **不动**(CRVS 布局不变;`abi6.bin` 格式锁照旧)。

## 变更内容

**一句话**:「某一维是手动接管固定的常值」从**按值推断**改成**读段上的显式标记**。

### 标记放在哪

| 层 | 位置 | 形状 |
|---|---|---|
| state | CRVS 段记录的 `u32 flags`(`src/core/state/StateCodec.h`) | bit0-1 origin / bit2 locked / **bit3 = manual pan / bit4 = manual vol** / bit5-31 保留(产品内写 0) |
| 桥面 | §2.8 `scvb.segments` 每段 | **`manualPan?:bool` / `manualVol?:bool`**(可选;native 恒发,缺席按 `false` 读) |
| 判据 | `src/core/output/DistReadback.h` `manualDimOf` ⇄ `web/shared/readback.js` `manualDimOf` | 两侧逐字同形 |

桥面上发的是**逐段的原始位**,不是「这一维是不是手动常值」的结论 —— 结论(含兼容规则)由 JS 按同一判据自己算,
与 native 的 `DistReadback.h` 对拍(Monitor 的 `panNow` / `volDb` 走 native 那份)。

### 语义(卡上 ①–④)

1. **置位**:只有 `setTrackManual` 的**手动接管通道**(§1.16 ②,该维未冻结)置位,且只置**被拖的那一维**,逐段置。
   段表非空时,另一维**已有**的位逐段原样保留(先拖 vol 再拖 pan ⇒ 两位都在;另一维从没接管过 ⇒ 不会被顺手补上);
   段表为空时写出的那一段只带被拖那一维的位 —— 另一维是默认值(pan 0 / vol 0 dB),不是用户固定的值。
   冻结通道(§1.16 ①)不写段表,自然也不动位。
2. **判据**:某一维是手动常值 ⇔ 段表非空 ∧ **每一段**都带这一维的位。**不比值**。
3. **维护**:
   - `set_locked` **保留**(锁只管重分析碰不碰这一段,不改段值);
   - `set_values` / `split` / `move_boundary` / `merge` **清掉被编辑段的位**(整字改写 flags):`move_boundary` 是
     `segIdx` 那一段,随之收缩的邻段 flags 不动;`split` 是两个子段;`merge` 是合并结果。段值或边界被逐段改过,
     这一维就不再是「整条固定的常值」;
   - 重分析与 `clearManual`(「恢复自动」/「重新识别(含手动段)」)**新产出**的段不带位;被保留下来的段(用户段、
     锁定段、范围外的段)原样带着自己的位 —— 于是被接管的轨上一旦混进新产出的段,就不再「每段都带位」;
   - 撤销 / 重做 / 复制版本是整段拷贝,原样带着。
4. **旧工程兼容**:整表**一个手动位都没有**(两维都没有)∧ 只有一段 ∧ 那一段 `origin=user_edited` ⇒ **两维都算**。
   这是改造前的判据;旧构建在空表上接管写出的就是这个形状。

「带位的段在该维上都是同一个值」靠写入口维持(置位只在接管通道、它逐段写同一个值;改值 / 改边界的 op 都清位),
所以读回取首段即可,判据里不需要再比值。

### 改前 / 改后

| 情形 | 改前(按值推断) | 改后(显式标记) |
|---|---|---|
| 已分析轨、各段 pan **碰巧全等**,拖 vol 卡箍 | pan 也判成手动常值:输出 OFF 时 pan 读回停在段值、不跟宿主参数面 | 只有 vol 算手动;输出 OFF 时 pan 跟参数面(与声音同路) |
| 已分析轨、各段 pan 不同,拖 vol 卡箍 | vol 手动、pan 不是 | 同左 |
| 空表上拖 vol 卡箍(单段) | 两维都算手动;pan 读回 = 段里的默认 0,不看输出档 | 只有 vol 算手动;pan 输出 ON 读段(0,与 DSP 同)、OFF 读参数面 |
| 检查器里逐段改过值、某一维碰巧全等 | 那一维判成手动,亮「手动接管」标 / 「恢复自动」入口 / 解冻提示 | 不算手动(改值清位) |
| 接管后对某段 `set_locked` | 手动(值没变) | 手动(位保留) |
| 接管后对某段 `set_values` 写**同一个值** | 仍算手动(值还全等) | 不算手动(被编辑段清位) |
| 旧工程:单段 `user_edited`、无位 | 两维都算 | 两维都算(兼容规则) |

「手动接管」标、「恢复自动」入口、解冻提示、Output 分布图、Tab2 行读回、Monitor `panNow` / `volDb` 都走
`manualDimOf` / `manualConstantOf` / `readbackSegsOf` 这三个函数,本卡只改了第一个的判据,调用方一行没动。

## 兼容性影响

- **state**:段记录仍是 28 字节,CRVS minor(1)与容器 abi(6)都不动,不需要迁移。CRVS 编解码本来就把 `flags`
  这个 u32 原样读写(`STATE-CRVS-5` 钉「原样」,含一个 bit5 以上的保留位)。
- **新构建读旧工程**:旧工程的段上没有位。空表接管留下的单段 `user_edited` 走兼容规则,两维照旧算手动。
  ⚠ **#302 起到本卡之间的构建**在**已分析轨**上接管写出的是多段 `user_edited`、没有位 —— 兼容规则只认单段,
  这类轨打开后**两维都不再算手动**:「手动接管」标与「恢复自动」入口、解冻提示不再出现。**声音不受影响**(判据只进显示链);
  读回值在该维已冻结(UI 接管后会置冻结位,随工程保存)或输出 ON 时不变,输出 OFF 时改为跟参数面 —— 与声音同路;要把这类轨清回自动,用
  波形页的「重新识别(含手动段)」。按卡上 ④ 的原文落地,没有为这一档另加规则(多段无位的表与「检查器里逐段改过、
  碰巧全等」分不开,放宽就会把 #302 那条误判带回来)。
- **新构建里也会落进兼容规则的形状**:单段表上 `set_values`(或 `split` 后再 `merge` 回单段)得到的「单段
  `user_edited`、无位」与旧工程接管的产物同形,两维都算手动 —— 与改造前的判定相同,不是新引入的误判;要区分它得
  给「旧工程」另打标记,那是一次 abi / 迁移级别的改动,不在本卡范围。
- **旧构建读新工程**:旧构建只取 bit0-2,这两位不参与任何判定(读回照旧按值推断);存盘时原样写回。旧构建对段做的
  编辑(五个段编辑 op,含 `set_locked`;以及手动接管)会整字重写被编辑段的 flags、丢掉这两位,回到新构建里就按
  上面的判据判(该维不再算手动,整表无位时走兼容规则)。
- **桥面**:§0.1 第 3 条的加法(既有 payload 新增可选字段),`contractVersion` 保持 `1.0`;JS 侧缺席按 `false`,
  mock 生成器的段本来就不带这两个键。
- **参数面 / 宿主自动化 / DSP**:零影响。判据只进显示链(message thread / UI),实时路径一行未动。

## 判据与机检

| 层 | 位置 | 钉什么 |
|---|---|---|
| 判据 native | `tests/core/test_viz_plane.cpp`「DistReadback:[SL-548] 手动维按显式标记判定,不再按值推断」 | (a) pan 全等 + 只带 vol 位 ⇒ pan 不算手动、OFF 读回回落参数面;空表接管的单段只算被拖那一维;(b) 每段带位才算、缺一段不算、判据不比值、两维都带;(d) 兼容:单段 UE 无位两维都算、单段 auto 不算、多段 UE 无位不算 |
| 判据 native(既有格改写) | 同文件「DistReadback:段选择口径…」 | `manual` 夹具标明是旧工程形状,优先级链几格照旧 |
| 段编辑 op | `tests/core/test_segment_edit.cpp` SEGEDIT-MANUAL-1..5 | set_locked 保留(加锁 / 解锁 / 只保留自己有的那一位);set_values / split / move_boundary / merge 只清被编辑段 |
| 接管写入 | `tests/core/test_segment_edit_service.cpp` SERVICE-5/6/7/8、SERVICE-13 | 非空表只置被拖那一维;空表只置被拖那一维;先 vol 后 pan 两位都在;另一维的位逐段保留 |
| CRVS | `tests/core/test_state_codec.cpp` STATE-CRVS-5 | 手动位与 bit9 保留位逐字节往返 |
| 生产接线 | `tests/host/test_host_harness.cpp` HOST SL-548 | 真 `setTrackManual` → CRVS → viz 段:① 每段只带 vol 位;② 输出 OFF 时 `panNow` 跟参数面、`volDb` 仍是手动常值;③ set_locked 保留;④ 存盘重开位原样;⑤ set_values **同值**清位 ⇒ vol 回落参数面;⑥ 撤销位回来;⑦ 旧工程单段无位两维仍读段 |
| 生产接线(既有格补) | HOST SL-180 / SL-188 | SL-180 两支各只带被拖那一维的位;SL-188 真分析夹具(各段 pan 相同)拖 vol 后 pan 不算手动、`clearManual` 重分析产出的段无位且标熄灭 |
| 读回 JS | `web-preview/tests/smoke-tab1-interactions.mjs` ⑦ 组 (a9)-(a13) | 与 native 判据格同款((a10) 即 #302 的「已知近似」翻过来) |
| mock 对拍 | `web-preview/tests/smoke-tab2-interactions.mjs` ⑥ 组 `[SL-548]` | mock 桥拖 vol ⇒ 每段只带 `manualVol`;set_locked 保留;set_values 同值只清被编辑段 ⇒ vol 不再算手动;split / move_boundary / merge 各一格,只清被编辑段 |
| 桥面字段 | `scripts/check-bridge-parity.mjs` 四、载荷字段对拍 | ① 认 `name?:` 可选字段写法;② **新增反方向**:§2.8 段对象那一层登记的字段,`buildSegmentsPayload` 都得 `put` |

已删除的判据:`test_viz_plane.cpp` 的「已知近似」块与 `smoke-tab1-interactions.mjs` 的旧 (a10) —— 它们钉的是按值推断的
现行行为,本卡把它们翻成上表 (a) 那几格。

### 删除式(本机实测)

每条只动一处代码;注入与复原都把 mtime 顶到未来再经 build slot 增量重编,复原后 `git hash-object` 与 HEAD 的
blob 逐一对上;全部复原后三个测试目标 + `SCVBOutput` 整体重编(0 warning),`scvb_tests` 579 例、`scvb_params_tests`
96 例、`scvb_host_tests` 211 例与两份 tab 冒烟全绿。结果栏是**实得**。行号是实测时的行号(提交 `a70dd339`,合并 base 上的 #324 / #328 之前);合并之后只有
`test_host_harness.cpp` 挪了位置 —— HOST SL-180 / SL-188 那一段整体下移 1 行,HOST SL-548 那一段整体下移 242 行,
其余测试文件的行号不变。
native 跑的是定向用例(`scvb_tests "[readback],[viz]"` / `"[segedit]"` / `"[crvs]"`、`scvb_params_tests "[service]"`、
`scvb_host_tests` 按行内所列 tag);JS 用 `node` 直接跑 tab1 / tab2 冒烟。

| 编号 | 注入 | 实得 |
|---|---|---|
| ND1 | `DistReadback.h` `manualDimOf`:判位不分维(`bit = kSegmentManualMask`) | `[SL-548]` 判据用例 7 处红:(a) `:779` pan 全等格、`:782` OFF 回落、`:786` ON 取播放头段、`:792` 空表接管;(b) `:799`、`:804`、`:808`。host 3 例红:HOST SL-548 ① `:10142`、② `panNow` 跟参数面 `:10150`;HOST SL-180 读回 `:4104`、`:4108`;HOST SL-188 `:4467`。HOST SL-363(无手动段)不红 |
| ND2 | 同函数:兼容规则永不成立(`segs.size() == 1` → `== 0`) | 3 例红:段选择口径用例 `manual` 夹具 `:724`(REQUIRE,截断该例)、(d) 单段无位 `:842`/`:843`/`:845`/`:846`、VizPublisher 手动段 `panNow` `:957`。host:HOST SL-548 ⑦ `:10201`/`:10202` |
| ND3 | 同函数:兼容规则不限单段(`>= 1`) | 1 例红:(d) 多段无位 `:854`/`:855` |
| ND4 | 同函数:兼容规则越权到带位的单段(`if (anyManualBit && segs.size() != 1)`) | 1 例红:空表接管 pan 不算 `:792` |
| ND5 | 同函数:判据加回值比较 | 1 例红:「不比值」格 `:827` |
| NE1 | `SegmentEdit.cpp` `set_locked` 回到只拼 origin + locked | SEGEDIT-MANUAL-1 `:360`/`:365`/`:370`;HOST SL-548 ③ `:10159`(位)、`:10161`(`volDb` 回落参数面),其后 ④⑤⑥ `:10170`/`:10172`/`:10182`/`:10190` 级联 |
| NE2 | `set_values` 保留被编辑段的位 | SEGEDIT-MANUAL-2 `:382`;SERVICE-13 前置 `:259`;HOST SL-548 ⑤ `:10182`(位)、`:10186`(`volDb` 仍是 −9) |
| NE3a | `split` 左子段不重写 flags | SEGEDIT-SPLIT-1 `:151`(既有格)+ SEGEDIT-MANUAL-3 `:397` |
| NE3b | `split` 右子段不重写 flags | SEGEDIT-SPLIT-1 `:152` + SEGEDIT-MANUAL-3 `:398` |
| NE4 | `move_boundary` 保留本段的位 | SEGEDIT-MANUAL-4 `:412` |
| NE5 | `merge` 结果段继承左段的位 | SEGEDIT-MANUAL-5 `:426` |
| NS1 | `SegmentEditService.h` 非空表不置被拖那一维的位 | params 4 例红:SERVICE-5 `:177`、SERVICE-6 `:196`、SERVICE-8 `:234`、SERVICE-13 前置 `:252`;host 3 例全红:HOST SL-548 ① `:10138`/`:10141` 起级联、HOST SL-180 `:4092`/`:4106`/`:4107`/`:4146`、HOST SL-188 `:4466` |
| NS2 | 非空表不保留另一维已有的位 | SERVICE-8 `:234`、SERVICE-13 `:263`/`:265` |
| NS3 | 空表不置位 | SERVICE-7 `:211`/`:217`、SERVICE-8 `:234` |
| NS4 | 空表两位都置 | SERVICE-7 `:211`/`:217` |
| NC1 | `StateCodec.cpp` CRVS 解码只取 bit0-2(`r.u32() & 0x7`) | STATE-CRVS-5 `:371`/`:372`/`:378`;HOST SL-548 ④ 存盘重开 `:10170`/`:10172`,其后 `:10182`/`:10190` 级联 |
| NP1 | `OutputProcessor.cpp` 分析产出的新段带 vol 位 | HOST SL-188 `clearManual` 重分析之后 `:4481`(位)/`:4483`(标仍亮) |
| JD1 | `readback.js` `manualDimOf` 判标记不分维 | tab1 (a9) 两格、(a10) 两格、(a11);tab2 `[SL-180] pan 维不算手动`、`[SL-548] vol 算手动、pan 不算` |
| JD2 | 同函数:兼容规则永不成立 | tab1 (a6)、(a8)、(a13) 单段;tab2 ③ B 形态、④ 常值段读回两格、(c6)(c8) |
| JD3 | 同函数:兼容规则不限单段 | tab1 (a13) 多段;tab2 ③ C 形态 |
| JD4 | 同函数:加回值比较 | tab1 (a12) 值不等格;tab2 全绿 |
| JM1 | mock 非空表接管不标 `manualVol` | tab2 14 条:既有 §1.16 常值段读回四条 + `[SL-548]` 全部;tab1 全绿 |
| JM2 | mock `set_values` 不清标记 | tab2 `[SL-548]` set_values 两条 |
| JM3 | mock `set_locked` 清标记 | tab2 `[SL-548]` set_locked 一条 + 其后 set_values 一条(首段的位已被清) |
| JM4 | mock `split` 不清标记 | tab2 `[SL-548] split` 一条 |
| JM5 | mock `move_boundary` 不清标记 | tab2 `[SL-548] move_boundary` 一条 |
| JM6 | mock `merge` 不清标记 | tab2 `[SL-548] merge` 一条 |
| PD1 | `check-bridge-parity.mjs` 字段正则不认 `?:` | `scvb.segments 实发了契约未登记的载荷字段:manualPan, manualVol` |
| PD2 | 删 `OutputEditor.cpp` 的 `put(seg, "manualPan", …)` | `scvb.segments 契约载荷行登记了、OutputEditor::buildSegmentsPayload 却没有 put(seg, …) 的字段:manualPan`(本 PR 新增的反方向) |

**照实写的缺口**:
- `OutputEditor.cpp` 那两行 `put` 只有 PD2 这道**文本级**对拍守着 —— harness 编不进 `OutputEditor`(与 HOST R4 头注同一个
  限制),运行期没有一格会因为它红。
- mock 空表接管那一支(`proto.manualPan` / `proto.manualVol`)没有冒烟格:fixture 里没有空段表的轨可拖。native 的
  同一支由 SERVICE-7 守。
- JD2 首次注入时 tab1 在 (a6) 抛 TypeError、JM1 首次注入时 tab2 在 §1.16 那一条抛 TypeError,其后各格都没跑到;
  把这三处读取改成 `?.` 判空之后重跑,上表是重跑的结果。

## 变更文件

- `src/core/state/StateCodec.h`(`kSegmentManualPanBit` / `kSegmentManualVolBit` / `kSegmentManualMask` / `segmentManualBit`)
- `src/core/state/SegmentEdit.cpp` / `.h`(set_locked 保留位;其余四 op 注明清位)
- `src/output/SegmentEditService.h`(`makeManualDimSegments` 置位)
- `src/core/output/DistReadback.h`(`manualDimOf` 判据)
- `src/output/OutputEditor.cpp`(§2.8 两个字段上桥)
- `src/output/OutputProcessor.cpp`(注释:接管置位、分析产出段不带位)
- `web/shared/readback.js`(`manualDimOf` 判据)、`web/output/tab-tracks.js`(注释)
- `web-preview/mock/juce-bridge-mock.js`(mock 同款维护)
- `scripts/check-bridge-parity.mjs`(可选字段写法 + 段对象反方向对拍)
- `docs/SCVB_CONTRACT.md`、`docs/IPC_CONTRACT.md`、`docs/STATE_SCHEMA.md`、`docs/contract-changes/20260928-j131-sl180-manual-one-dim.md`(指路)、`CHANGELOG.md`
- 测试:见上表

## 审批

实质已批(J162,用户 2026-09-28「18批」)。本文件随实现 PR 挂 `status/frozen-contract`;契约文字的具体落笔请统筹 / 用户过目。
