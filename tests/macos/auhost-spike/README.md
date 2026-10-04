# auhost-spike:AUHostingService 进程外共享内存 spike(B 线 M01)

一次性的测量工具,回答一个问题:SCVB 的 AU 被宿主**进程外**加载时(Logic 的做法:插件跑在 `AUHostingService` 里),POSIX 共享内存、真实 home 下的锁文件、ObjC 运行时会合还能不能用。结论与判定记录在 [`docs/spikes/S5-macos-auhost-oop.md`](../../../docs/spikes/S5-macos-auhost-oop.md)。

这是独立 CMake 工程:根工程和 `tests/CMakeLists.txt` 都不引用本目录,Windows 构建与 `gates.ps1` 不受影响。CI 入口是 `.github/workflows/macos-oop.yml`(由 `build-vst3.yml` 的 caller job `macos-oop` 调用)。

## 文件

| 文件                 | 作用                                                                                                     |
| -------------------- | -------------------------------------------------------------------------------------------------------- |
| `SpikeProcessor.cpp` | 探针插件。同一份源码编出两个 AU:`aufx Sxsa Snch`(A)与 `aufx Sxsb Snch`(B),模拟 SCVB 三个独立二进制 |
| `auhost_spike.cpp`   | 宿主。纯 C++ + clang blocks + AudioToolbox/CoreFoundation(不写 `.mm`)                                 |
| `spike_protocol.h`   | 两侧共用的段名、段布局、参数表与状态码                                                                   |
| `spike_judge.h`      | 判定表的唯一实现(只用标准库,可脱离 mac 自测)                                                          |
| `CMakeLists.txt`     | 独立工程;AU 选项与 SCVB 将来一致:`AU_MAIN_TYPE kAudioUnitType_Effect`、`AU_SANDBOX_SAFE FALSE`        |

## 探针(都在插件的 `prepareToPlay` 里跑一次)

1. 30 字符名 `/SynchainSCVB.v1.g8.spike.ch15`:`shm_open(O_CREAT|O_EXCL)`、`ftruncate`(4 MB)、`mmap`;后到的实例走附着(绝不 `ftruncate`)并读段头 magic。
2. 负对照:32 字符名必须得到 `ENAMETOOLONG`;31 字符名只记录。
3. 用 `getpwuid` 取真实 home,在 `Library/Application Support/Synchain/SCVB/ipc/` 下建锁文件并 `flock`(EX 拿不到时要 SH);同时记录 `HOME`、`NSHomeDirectory()`、`CFFIXED_USER_HOME` 的实际值,判断是否被容器化。
4. `getpid`、`getppid`、进程名、可执行文件路径。
5. 对宿主 pid 做 `kill(pid, 0)` 与 `sysctl(KERN_PROC_PID)`。
6. 方案 B 前置:A 用 ObjC 运行时 C API(`objc_allocateClassPair` / `objc_registerClassPair`)注册会合类,B 用 `objc_lookUpClass` 找它并调用其类方法。

另有一段由宿主创建的段 `/SynchainSCVB.v1.g8.spike.host`,插件只读打开,核对宿主写入的 magic 与 token(宿主 → 插件方向);渲染期间插件在段里给自己的槽位计数,宿主据此确认两边映射的是同一块物理内存。

结果走两条互相独立的通道:

- **AU 参数**(不依赖文件系统):编码见 `spike_protocol.h` 的 `Param` 表;
- **JSON 文件**:写到真实 home 的 `Library/Logs/Synchain/SCVB-spike/`。写不出来本身就是结论,`out.json` 参数会带回 errno。

状态码:`1` 成功;`2` 另一种成功(附着已有段 / `EWOULDBLOCK` / 值不同);`3` 意外(负对照竟然成功、`HOME` 未设);`4` 没走到;`5` 数据不符;`6` 找不到;`7` 注册失败;`100+errno` 系统调用失败。

## 判定表(G0 由统筹判,本工具给建议)

| 结论           | 条件(进程外运行)                                                                         | 后续                       |
| -------------- | ------------------------------------------------------------------------------------------ | -------------------------- |
| `A`            | shm 创建与附着全成功;与宿主双向互见;锁文件可建可 flock;`HOME`/`NSHomeDirectory` 未容器化 | M05 照做                   |
| `A-prime`(A′) | shm 与互见都成立,但锁文件路径被拒,或 `HOME`/`NSHomeDirectory` 被容器化                    | 设计补丁,另加 1–2 人日    |
| `B`            | shm 被拒(`EPERM`/`EACCES`),但三个实例同 pid,且 B 能找到 A 注册的会合类                  | 加 M05b                    |
| `C`            | shm 被拒,且实例 pid 不同                                                                  | mac beta 只面向进程内宿主  |
| `inconclusive` | 进程内对照失败;或 OOP 实例化失败 / 实例其实没跑到宿主进程外 / 探针没报完成 / 落在表外组合 | 按 A 继续,LUNA 加真机探针 |

shm 全成功、只是 A/B 不在同一进程时,仍判 `A` 并附注:方案 A 本来就靠 shm 跨进程,不依赖同进程。

**退出码**(`--judge`):负对照不成立时为 `1`,包括组件没注册上、进程内实例不在宿主进程里、进程内 shm 没成功、32 字符名没报 `ENAMETOOLONG`、进程内探针没跑完。其余结论都是 `0`,包括因 OOP 起不来而判的 `inconclusive`。

**反向注入**:CI 里常驻一步,让宿主按错误的 subtype(`Sxsz`)找组件,判定必须是 `inconclusive` 且退出码为 `1`,不能是任何一种结论。判定表本身由 `--self-test` 用合成输入逐格断言。

## 在 Mac 上手动运行

```sh
cmake -S tests/macos/auhost-spike -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH=/path/to/JUCE
cmake --build build-spike
ditto build-spike/SpikeA_artefacts/Release/AU/SCVBSpikeA.component ~/Library/Audio/Plug-Ins/Components/SCVBSpikeA.component
ditto build-spike/SpikeB_artefacts/Release/AU/SCVBSpikeB.component ~/Library/Audio/Plug-Ins/Components/SCVBSpikeB.component
killall -9 AudioComponentRegistrar; sleep 5
./build-spike/auhost_spike --self-test
./build-spike/auhost_spike --mode inproc --out-dir spike-out
./build-spike/auhost_spike --mode oop --out-dir spike-out
./build-spike/auhost_spike --judge --out-dir spike-out
```

输出在 `spike-out/`:`<mode>.kv`(判定用)、`<mode>.json`、`<mode>-<实例>-plugin.json`、`verdict.json`、`summary.md`。

**隐私**:宿主默认把真实 home 前缀替换成 `~`,在自己的 Mac 上跑也不会把用户名带进输出目录。只有 CI 加 `--raw-paths`(runner 的 home 是 `/Users/runner`)。插件写进 `~/Library/Logs/Synchain/SCVB-spike/` 的原始 JSON **没有脱敏**,不要直接分享。

## 局限

- runner 上的 `AUHostingService` 结果只是近似,不等于 Logic 的行为。
- 进程外实例化需要用户会话;runner 没有时会落到 `inconclusive`。
- 段布局只在本目录有效,与 `docs/IPC_CONTRACT.md` 的冻结布局无关。段名借用冻结前缀,只是为了让名字长度与将来的真实名字同量级。
