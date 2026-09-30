# 契约变更说明 —— 20260825-ctrl-broadcast-layout

## 变更了哪个冻结契约

- [ ] docs/PARAMETERS.md(自动化参数)
- [x] **docs/IPC_CONTRACT.md(共享内存段名/布局)—— ctrl 段「广播区」填入正式布局**
- [ ] docs/STATE_SCHEMA.md(state schema)
- [x] **tests/golden/(golden 快照)—— ipc-layout.txt 新增 `CtrlChannelConfig` / `CtrlBroadcast`**
- [ ] docs/SCVB_CONTRACT.md(JS↔C++ 桥契约)

> 按 #89(T44)确立的做法:**本 PR 不动 `docs/IPC_CONTRACT.md` 本体**,只冻 golden + 写本文件。
> 用户批准后由统筹统一转正进文档(届时 gate 3g 的 WARN 自动变回正向对拍)。

## 变更内容

ctrl 段(`SynchainSCVB.v1.g{G}.ctrl`,16 KB)的**广播区**在 T06 冻结时只划了预算、内容留白 ——
`CtrlPlane.h` 原注释逐字写着「广播区占位(T25/params v2.x 填内容)」「本卡不发明布局」。
本 PR 把它填上,用于 **Output → Input 的配置只读镜像**(ADR-004:配置真源在 Output)。

### 落点(全部在既有预算内,**不改任何已冻结字段**)

| 常量 | 值 | 变化 |
|---|---|---|
| `kCtrlBroadcastOffset` | 64 | 不变(T06 冻结) |
| `kCtrlBroadcastBytes` | 9344 | 不变(T06 冻结) |
| `sizeof(CtrlBroadcast)` | **2048** | 新增(落在 9344 预算内,余 7296 字节留给后续) |

段内其余落点(`kCtrlGlobalInfoOffset = 9408`、`kCtrlRingsOffset = 9664`、段总长 16384)一字未动,
`static_assert` 原样保留。**ipc abi 不变**(布局只在留白区内新增,不移动、不改写任何既有结构体)。

### 两个结构体

```
CtrlChannelConfig  size 32  align 4     每轨配置镜像
  priority         @0       u32  0..10
  flags            @4       u32  bit0 enabled / bit1 lead_lock / bit2 lead_vol_exempt / bit3 participate_in_auto_pan
  pair_id          @8       u32  0=无配对,1..7
  freeze           @12      u32  bit0=pan 冻结,bit1=vol 冻结(J65 同一参数两位)
  source_channels  @16      u32  1|2(Input 实测值,Output 回镜像给全组)
  _reserved        @20      u32[3]

CtrlBroadcast      size 2048  align 64  广播区总布局
  seq              @0       atomic<u32>  seqlock:写前 +1(奇)→ 写载荷 → 写后 +1(偶)
  config_seq       @4       atomic<u32>  广播区整体版本号;**从 1 起算**,0 保留给「本组无 Output 在广播」
  lead_select      @8       u32          0=无,1..15
  _pad             @12      u32
  _reserved        @16      u32[12]      填满 cacheline 0(seq 与载荷分离)
  channels         @64      CtrlChannelConfig[15]
  labels           @544     char[15][100]  UTF-8、NUL 结尾;≤24 字符最坏 4 字节/字符 + 余量
  _tail            @2044    char[4]        显式补齐到 64 的整数倍
```

`_tail` 是刻意的:`alignas(64)` 的结构体若不是 64 的整数倍,编译器会**隐式**补齐(MSVC /W4 C4324),
而隐式补齐意味着布局由编译器决定 —— 跨进程共享内存结构不接受这种不确定性。

### 并发口径

跨进程 **seqlock**:写方 = 本组 `kActive` 的那一个 Output 的 [M](只读观察实例不写,避免两个实例
抢写让 Input 在两份配置之间抖动);读方 = 各 Input 的 [M],撕裂即沿用上帧、**不自旋**。
写侧奇数增量取 `relaxed` + 紧跟一道 `atomic_thread_fence(release)` —— release **store** 只挡
「之前的写下沉」,挡不住「其后的载荷写上浮到奇数 seq 之前」,而那正是这里要防的方向
(载荷 2KB、跨进程、读写双方分别编译,窗口比进程内的 `PlayheadShot` 宽两个量级)。

## 为什么必须填

`scvb.config`(契约 §4.3)是 Input 页的远程只读视图。广播区没有布局 ⇒ 没有写方也没有读方 ⇒
`InputBridgeLogic::buildConfigPayload` 里 `label`/`priority`/`lead_lock`/`pair_id`/`freeze`/
`channelLabels` **全是硬编码字面量**(`priority` 恒 0,而 Output 侧默认是 5 —— 连回退值都对不上)。

用户 T37 三轮真机报告:「Input 侧优先级恒 0,Output 显示 5;Output 改 5→6,Input 不动;
lead 开关也不同步」。这条链断在这里。

## 兼容性影响

- **ipc abi**:**不变**。布局只在 T06 留白的广播区内新增,既有结构体的偏移/尺寸/段名全部未动;
  `tests/golden/ipc-layout.txt` 的既有行一行没改,只新增两个结构体块。
- **旧构建互操作**:旧 Output 不写广播区 → 新 Input 读到 `config_seq == 0` → 判定「本组没有
  Output 在广播」→ 走默认值分支,与旧行为一致,不误把全零当实况。反向(新 Output + 旧 Input)
  旧 Input 不读该区,无影响。这正是 `config_seq` 从 1 起算的用途。
- **参数面 / state chunk**:均未涉及。

## 配套(同 PR)

- `tests/golden/ipc-layout.txt` 冻两个结构体;`tests/core/test_ipc_layout.cpp` 补
  `sizeof`/`alignof`/`offsetof` 三张分派表 —— 布局由**编译期真值**钉死,不是人肉抄写;
- `scripts/check-ipc-doc-parity.mjs` 的 `NON_LAYOUT_STRUCTS` 登记 `CtrlBroadcastSnapshot`
  (宿主侧普通 POD 快照,不进段);
- 顺带修 `CtrlPlane::broadcastBase()`:原先返回 `base_`(段起点 = `CtrlHeader` 地址),任何按
  `broadcastBytes()` 的写入都会砸掉 magic/abi/generation 并越界踩进 `OutputGlobalInfo`。
  当时零调用方所以一直没暴露。

## 审批

- [ ] **用户批准:待批准**(PR #87 已挂 `status/frozen-contract`)
- [x] gate 3g(`check-ipc-doc-parity`)已按本文件从 FAIL 降为 WARN(修宪在途);
      `test_ipc_layout` 编译期断言全绿。
