# scvb_auhost:二进制级调度宿主(B 线 M13)

只通过 AudioComponent 加载**已构建、已安装**的三个真实 SCVB component(Input ×3、Output、Monitor),在**进程内**(`AudioComponentInstantiate` flags = 0)与**进程外**(`kAudioComponentInstantiation_LoadOutOfProcess`,插件跑在 `AUHostingService` 里,Logic 的做法)两种模式下按宿主调度渲染,用 A 线 A-1 的逐轨在场判据([`tests/support/presence_meter.h`](../../support/presence_meter.h))判 Output 输出上每一轨在不在、对不对。由 M01 的 [`tests/macos/auhost-spike/`](../auhost-spike/README.md) 演化而来。

这是近似测试:runner 上的 `AUHostingService` 不等于 Logic,本工具也只模拟了宿主调度里的两个场景(见下)。

独立 CMake 工程:根工程和 `tests/CMakeLists.txt` 都不引用本目录,Windows 构建与 `gates.ps1` 不受影响;不链 JUCE。CI 入口是 `.github/workflows/macos-oop.yml` 的 `scvb-auhost` job(由 `build-vst3.yml` 的 caller `macos-oop` 调用,needs `macos-build`,测的是同一个 run 里上传的 bundle)。

## 文件

| 文件              | 作用                                                                                                         |
| ----------------- | ------------------------------------------------------------------------------------------------------------ |
| `scvb_auhost.cpp` | 宿主(macOS):实例化 / 配置 / HostCallbacks / 写通道号 / 渲染线程 / registry 只读映射 / 落盘                 |
| `auhost_engine.h` | 调度模型(只用标准库):场景、游标 → 时间线、每个 Output 周期的预取 / live / 总线 / Output 顺序、预热      |
| `auhost_judge.h`  | 判定 + 判据 + 报告,以及 `--self-test`:一对假插件把整条调度与判据跑一遍,外加 5 种故障注入                  |
| `CMakeLists.txt`  | 独立工程;复用产品的 `src/core/state/StateCodec.cpp`、`InputStateCodec.cpp`(写 Input 的 state)与它引用的 `src/core/analysis/PanCurve.cpp` |

## 宿主做的事

- **每个实例独立的走带**:`kAudioUnitProperty_HostCallbacks` 给每个实例一份 `transportState` / `beatAndTempo` / `musicalTimeLocation`,返回**该实例当前这一块**的时间线位置(`currentSampleInTimeLine`)、在放、Cycle 状态与循环点。`AudioTimeStamp.mSampleTime` 是该实例的渲染游标加一个各实例互不相同的大偏移:HostCallbacks 若没传进插件(插件只能拿 `mSampleTime` 当时间线),各实例的时间线必然对不上,在场判据必然红,不会碰巧对齐。
- **调度**(与 A 线 `tests/host/sched/SchedRig.h` 同形):每个 Output 周期(512 样本)里,预取轨先突发渲染到「Output 游标 + 提前量 + 一块」(块长 1024),live 轨渲染本周期,再渲染 Output。块在循环点切开。总线 = 各 Input 输出求和、**只进 L、增益 0.5**;SCVB 混音是居中的,所以判据分得开「未平衡原声」与「SCVB 混音」。
- **通道号**:经 `kAudioUnitProperty_ClassInfo` 的 `jucePluginState` 把 SCVB 的 Input state(通道 1/2/3,组 1)在 `AudioUnitInitialize` 之前写进去,并读回核对。
- **实时节拍**:渲染在独立线程上按墙钟实时推进(SCVB 有 500 ms 停滞一类的墙钟门限);主线程一直转 CFRunLoop(进程内模式下 SCVB 的 25 Hz 定时器靠它)。
- **进程证据**:只读映射插件建的 registry 段 `/SynchainSCVB.v1.g1.registry`,读每个 Input / Output 实例写下的 pid 与 `connected_mask`。

## 场景与判据

| 场景 | 内容                                                                       | 判据(预热完再放 0.1 s 之后)                                  |
| ---- | -------------------------------------------------------------------------- | -------------------------------------------------------------- |
| LS-1 | 稳态回放,无 Cycle:lane 0 live,lane 1 / 2 预取提前 4096 / 16384 样本     | 5 s 判定窗,逐轨 SCVB 混音在场 ≥ 99%,Wrong = 0               |
| LS-4 | Cycle 2 s + 混合提前量(H1 现场版):lane 0 live,lane 1 / 2 提前 4096 / 48000 | 连续 4 圈,每圈每轨丢失 ≤ 1 帧(每圈 93 帧),Wrong = 0     |

取值是 A 线 LS-1(提前量 0 / 1024 / 4096 / 16384)与 LS-4(Δ 1024 / 4096 / 48000,圈长 2 / 8 / 12 s)格子里的取样。按精简裁定**不做** LS-5、离线导出、「用修复前 artifact 留证」。

- **在场** = presence 判 `Present` 且来源是 SCVB 混音(L、R 都有)。只进 L 的原声算丢失:那说明这一路没交给 Output。
- **预热** = 每条被调用的 lane 都已自静音(交接走完),且 registry 里 Output 活跃、各 lane 的 InputSlot 活跃、`connected_mask` 有这一位,连续 8 个周期。12 s 音频内等不到算「没测成」。
- **进程**:进程内 ⇒ registry 里 4 个 pid 全等于宿主 pid;进程外 ⇒ 全不等于宿主 pid(并记下那个进程的名字)。

## 退出码

| 退出码 | 含义                                                                                     |
| ------ | ---------------------------------------------------------------------------------------- |
| `0`    | 判据全过(`--negative-control` 下:负对照成立)                                         |
| `1`    | 判据不过,或进程判据不过(`--negative-control` 下:负对照不成立)                        |
| `2`    | 没测成:组件没注册、实例化 / 配置 / 写通道号 / Initialize 失败、渲染出错、预热等不到;用法错 |

`--negative-control`:宿主从不调用 lane 1 的 Input。负对照成立 = 判据红在 lane 1、而且只红在 lane 1(lane 0 / 2 照过,进程判据照过)。CI 上常驻一步。

## 自测(`--self-test`)

假插件(Input 按时间线写环、交接后自静音;Output 按自己的时间线读环、居中混音)跑理想调度,两个场景都必须过、每轨 100% 在场、预取轨确实比 Output 早一个提前量跨过循环点;再逐个注入 7 种故障,每种必须红在设计接住它的那一轨、那一条判据上:宿主不调 lane 1、Output 读 lane 2 错位 512 样本、读方 H1(循环后领先轨整圈读不到,LS-1 照绿)、lane 0 从没交给 Output、插件拿渲染游标当时间线、LS-1 里 lane 0 有 2 帧读成上一圈环(在场率仍 ≥ 99%,只有 Wrong = 0 接得住)、LS-4 每圈 lane 1 有 1 帧读成上一圈环(每圈丢失仍 ≤ 1,只有 Wrong = 0 接得住)。只用标准库,本地可用任意 C++17 编译器编 `auhost_judge.h` 跑同一份自测。

## 在 Mac 上手动运行

先按安装说明把三个 `.component` 装进 `~/Library/Audio/Plug-Ins/Components`(未签名的包先执行 `xattr`),然后:

```sh
cmake -S tests/macos/auhost -B build-auhost -DCMAKE_BUILD_TYPE=Release
cmake --build build-auhost
./build-auhost/scvb_auhost --self-test
./build-auhost/scvb_auhost --mode inproc --scenario ls1 --out-dir auhost-out
./build-auhost/scvb_auhost --mode oop --scenario ls4 --out-dir auhost-out
```

输出在 `auhost-out/`:`<mode>-<scenario>.kv`(全部数字)、`.md`(小结)、`-frames.csv`(逐帧逐轨判定)。只记录组件名与版本、进程名与系统服务路径,不记录任何用户目录。

## 局限

- 一条渲染线程串行调用各实例,不模拟宿主把不同轨放到不同线程并发渲染。
- lane 只用 mono,组只用默认的组 1;不覆盖停调策略、定位、起播怪癖、live⇄预取切换(这些在 A 线 `SchedRig` 的处理器级用例里)。
- runner 的 `AUHostingService` 没有沙箱;Logic 自己的进程外宿主配置无法在 CI 上验证。
