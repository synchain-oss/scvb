# S5 macOS 进程外加载(AUHostingService)共享内存 spike 结果

> 交付卡:B 线 M01 · 分支 `feat/M01-auhost-oop-spike` · base `feature/macos-au`
> 工具:[`tests/macos/auhost-spike/`](../../tests/macos/auhost-spike/README.md) · CI:`build-vst3.yml` → caller `macos-oop` → `.github/workflows/macos-oop.yml`
> 本文只记录 GitHub 托管 macOS runner 上的实测,**不等于 Logic 的行为**。G0 最终判定由统筹做;本文给的是建议判定与依据。

## 0. 要回答的问题

SCVB 在 Windows 上靠 `Local\SynchainSCVB.v1.*` 命名共享内存在 Input / Output / Monitor 三个插件之间传音频与控制面。移植到 macOS 后,M05 计划改用 POSIX `shm_open`,并在真实 home 下放 `flock` 生命周期锁。风险在于宿主把 AU 放进 `AUHostingService` 进程外加载(Logic 的做法)时:

1. 进程外加载的插件能不能 `shm_open` / `ftruncate` / `mmap`,第二个实例能不能附着?
2. 同一个宿主里的 A、B 两个**不同二进制**会不会落进同一个进程?与宿主进程能不能互见?
3. 真实 home 下的锁文件能不能建、能不能 `flock`?`HOME` / `NSHomeDirectory()` 是否被容器化?
4. 如果 shm 被拒,同进程内能不能靠 ObjC 运行时会合(方案 B)?

## 1. 方法

见工具 README。要点:

- 同一份 `SpikeProcessor.cpp` 编出两个 AU:`aufx Sxsa Snch`(A)、`aufx Sxsb Snch`(B);AU 选项与 SCVB 将来一致(`kAudioUnitType_Effect`、`AU_SANDBOX_SAFE FALSE`、JUCE 默认 resourceUsage)。
- 宿主 `auhost_spike` 依次实例化并初始化 A1、A2、B1。两种模式各跑一个独立进程:
  - `--inproc`:`AudioComponentInstantiate(flags = 0)`;
  - `--oop`:`kAudioComponentInstantiation_LoadOutOfProcess`。
- 探针在插件的 `prepareToPlay` 里跑,结果走两条独立通道:AU 参数(不依赖文件系统)与 JSON 文件(真实 home 下)。
- 宿主另建一段 `/SynchainSCVB.v1.g8.spike.host` 给插件只读打开(宿主 → 插件),自己再打开插件建的段核对 magic(插件 → 宿主)。渲染若干块后,宿主看段里每个实例的计数有没有前进。
- 判定表只有一份实现(`spike_judge.h`),先用合成输入自测(`--self-test`),再判实测结果。

## 2. 判定表与退出码

| 结论           | 条件(进程外运行)                                                                         | 后续                                          |
| -------------- | ------------------------------------------------------------------------------------------ | --------------------------------------------- |
| A              | shm 创建与附着全成功;与宿主双向互见;锁文件可建可 flock;`HOME`/`NSHomeDirectory` 未容器化 | M05 照做                                      |
| A′(A-prime)   | shm 与互见都成立,但锁文件路径被拒,或 `HOME`/`NSHomeDirectory` 被容器化                    | M05 前出设计补丁,另加 1–2 人日               |
| B              | shm 被拒(`EPERM`/`EACCES`),但三个实例同 pid,且 B 能找到 A 注册的会合类                  | 加 M05b                                       |
| C              | shm 被拒,且实例 pid 不同                                                                  | mac beta 只面向进程内宿主,Logic 写「不可用」 |
| 不定           | 进程内对照失败;或 OOP 实例化失败 / 实例其实没跑到宿主进程外 / 探针没报完成 / 落在表外组合 | 按 A 继续,LUNA 会话加 5 分钟 `--oop` 真机探针 |

- **表外组合的处理**:shm 全成功、只是 A/B 不在同一进程时,仍判 A 并附注(方案 A 本来就靠 shm 跨进程)。其余表外组合一律判「不定」并在附注里写明。
- **退出码**:负对照不成立时为 1,包括组件没注册上、进程内实例不在宿主进程里、进程内 shm 没成功、32 字符名没报 `ENAMETOOLONG`、进程内探针没跑完。其余结论都是 0,包括因 OOP 起不来而判的「不定」。

## 3. 实测结果

### 3.1 环境与 run

| 项 | macos-15 | macos-26 |
|---|---|---|
| 系统 | macOS 15.7.9(24G830)arm64 | macOS 26.6.2(25G83)arm64 |
| 工具链 | Xcode 16.4 / Apple clang 17.0.0 / CMake 4.4.3 | Xcode 26.6 / Apple clang 21.0.0 / CMake 4.4.3 |
| 用户会话 | `launchd gui/501` 存在 | 同左 |
| JUCE | 8.0.8(`.juce-version`,tag 身份断言通过) | 同左 |

数据来自两次 `workflow_dispatch`(`build-vst3.yml`,ref = 本分支):

- run `37173401344`(提交 `328b3d3`):两个矩阵格都判 **A**;
- run `37173704949`(提交 `5a0b703`,判定加了「进程内 / 进程外」两条判据之后):两个矩阵格仍判 **A**,探针原始值与上一次逐项相同(pid、token、耗时除外)。

下文原始值取自后者。artifact `SCVB-macos-oop-spike-<runner>-<sha>` 保留 14 天,所以关键原始值抄录在这里。

### 3.2 建议判定:**A**(两个 runner 一致)

| 判据 | 进程内 | 进程外 |
|---|---|---|
| 三个实例都在宿主进程**里** / **外** | 里(实例 pid = 宿主 pid) | 外(宿主 5012,实例 5013;macOS 26 上宿主 22728,实例 22734) |
| 进程外宿主进程 | — | `AUHostingServiceXPC_arrow`(`/System/Library/Frameworks/AudioToolbox.framework/XPCServices/AUHostingServiceXPC_arrow.xpc/...`),ppid = 1(launchd 拉起) |
| A1 / A2 / B1 同 pid | 是 | **是**:两个不同二进制的三个实例落在同一个 AUHostingService 进程 |
| 30 字符名 shm | A1 创建(`O_CREAT \| O_EXCL`)+ `ftruncate` 4 MB + `mmap`;A2、B1 附着并读到 magic | 同左,`fstat` 尺寸 4194304 |
| 32 字符名 | `ENAMETOOLONG` | `ENAMETOOLONG` |
| 31 字符名(只记录) | 成功 | 成功 |
| 宿主看插件建的段 | 读到 magic 与创建者 pid | 同左(宿主 5012 读到创建者 5013) |
| 插件看宿主建的段 | magic 与 token 都对 | 同左 |
| 渲染期间同一块物理内存 | 三个槽位计数都从 0 到 16 | 同左 |
| 真实 home 下的锁文件 | 可建;A1 拿到 EX 后降 SH,A2/B1 得 `EWOULDBLOCK` 后拿到 SH | 同左;宿主进程对同一文件试 EX 得 `EWOULDBLOCK`(跨进程 flock 生效) |
| `HOME` / `NSHomeDirectory()` | 都是 `/Users/runner` | 都是 `/Users/runner`;`CFFIXED_USER_HOME`、`APP_SANDBOX_CONTAINER_ID` 都未设置 |
| `TMPDIR` | runner 默认 | `/var/folders/<哈希>/T/AUHostingService/`(服务专属子目录) |
| `kill(宿主 pid, 0)` / `sysctl(KERN_PROC_PID)` | 成功 | 成功 |
| ObjC 会合类 | A1 注册、A2 发现已注册、B1 找到并调通 | 同左;B1 拿到的会合数据来自 A(subtype `Sxsa`、同一 pid) |
| JSON 文件写进真实 home | 成功 | 成功 |
| 进程外实例化耗时 | — | 首个实例 385 ms / 755 ms(两次 run 里最慢 1046 ms),之后 29–147 ms |
| 参数通道 | 初始化返回时已就位 | 同左(等待 0 ms) |
| 收尾 | 宿主 `shm_unlink` 插件建的段、`unlink` 锁文件都成功 | 同左 |

另外,`auval -v aufx Sxsa Snch` 与 `auval -oop -v aufx Sxsa Snch` 在两个 runner 上都报 `AU VALIDATION SUCCEEDED`(只记录,不参与判定)。

### 3.3 对后续卡的含义

- **M05 照 §3.4 原设计实施**:POSIX shm、`O_EXCL` 创建、附着方不 `ftruncate`、真实 home 下的 `flock` 锁、`kill(pid, 0)` 探活,在 runner 的 AUHostingService 里全部可用。
- 锁目录**不要**放 `TMPDIR`:进程外时 `TMPDIR` 是服务专属子目录,与宿主进程、进程内实例看到的不是同一个目录。真实 home 下的路径在两边一致。
- 方案 B(M05b)不需要;但前置条件实测成立(同进程 + 跨二进制会合可用),留作备案。
- 这只是近似:runner 的 AUHostingService 没有沙箱,探针 AU 的 Info.plist 用的是 JUCE 默认的 `resourceUsage`(声明了 `temporary-exception.files.all.read-write`),与 SCVB 将来一致。**如果以后改成 `AU_SANDBOX_SAFE TRUE` 或去掉 `resourceUsage`,本结论不再适用**,要重跑本 spike。Logic 是否用同样的宿主配置无法在 CI 上验证,仍按计划在 LUNA 会话里加 5 分钟真机 `--oop` 探针。

## 4. 反向注入与删除式

每一格都只动一个落点;改前先跑基线(绿),改后恢复再跑一次(绿)。

### 4.1 CI 上(判定 + 实测链路)

| 落点 | 做法 | 期望 | 实测 |
|---|---|---|---|
| 宿主按错误 subtype 找组件(常驻步) | 每次 run 都以 `--subtype-a Sxsz` 再跑一次进程内并判定 | `inconclusive` + 退出码 1 | run `37173401344`、`37173704949`、`37173714999` 的两个 runner 全部 `inconclusive` / 1 |
| 插件编出错误 subtype(一次性) | 临时分支把 `SPIKE_SUBTYPE_A` 改成 `Sxsy`,run `37173714999`;分支用后已删除 | Judge 步红,`inconclusive` + 退出码 1 | 两个 runner 都红在 Judge 步,`inproc.components_registered = no`,`inconclusive` / 1 |
| 判定表自测 | `--self-test`,合成输入逐格断言 | 全过 | run `37173704949` 两个 runner `23 cases, 0 failed`;之后补了「附着方抢跑」一格(共 24 格),本地 g++ 与 PR 上的 macos-15 都是 `24 cases, 0 failed` |

### 4.2 判定表删除式(本地,g++ 13.1 编 `spike_judge.h` 的自测)

自测逐格比对、全部跑完再汇总(一格红不影响其它格的判断)。

| # | 删掉的规则 | 变红的自测格 |
|---|---|---|
| J1 | 进程外实例必须不在宿主进程 | OOP flag ignored;OOP host pid unknown |
| J2 | 进程内 32 字符名必须 `ENAMETOOLONG` | inproc 32-char name succeeded |
| J3 | 进程内组件必须注册上 | inproc component not registered (wrong subtype) |
| J4 | 进程内 shm 必须创建 + 附着成功 | inproc shm failed |
| J5 | 进程内实例必须在宿主进程里 | inproc instance outside the host process |
| J6 | A 要求锁文件可用 | lock path denied -> A-prime |
| J7 | A 要求 HOME / NSHomeDirectory 未容器化 | NSHomeDirectory containerized;HOME containerized |
| J8 | A 要求宿主看得见插件的段 | shm ok in service but host cannot see it |
| J9 | B 要求同 pid | shm EPERM / EACCES, different pid -> C(两格) |
| J10 | B 要求会合类可见 | shm EPERM, same pid, objc not visible |
| J11 | 进程外必须实例化 / 初始化成功 | OOP instantiation failed |
| J12 | 恰好一个创建者 | no creator (stale segment) |

**本卡途中补的一条判据**:第一次全绿后核对原始数据时发现,判定从不检查「进程外实例真的不在宿主进程里」。进程外标志若被静默忽略,量到的其实是进程内结果,却会被报成进程外的 A。已补上(J1、J5 两格兜着),实测数据重判仍为 A。

### 4.3 native 路径门禁(`scripts/check-native-paths.mjs`,本地)

| 落点 | 变红的断言 |
|---|---|
| 删 `build-macos.yml` 分支 | ② 应命中却没命中 `.github/workflows/build-macos.yml` |
| 删 `macos-oop.yml` 分支 | ② 应命中却没命中 `.github/workflows/macos-oop.yml` |
| 删 `scripts/package-macos.sh` 分支 | ② 应命中却没命中 `scripts/package-macos.sh` |
| `macos-oop` 分支去掉 `$` | ② 不该命中却命中 `.github/workflows/macos-oop.yml.bak` |
| `build-macos` 分支把 `\.` 写成 `.` | ② 不该命中却命中 `.github/workflows/build-macosXyml` |
| `package-macos` 分支去掉 `$` | ② 不该命中却命中 `scripts/package-macos.sh.orig` |
| 删 `scripts/package-macos.sh` 正例 | ③ 删掉该分支没有任何正例变红(判据无牙) |
| `MIXED_EXPECT` 删 `macos-oop.yml` | ④ `.github/` 新长出命中文件但没登记 |
| `NON_NATIVE_PINNED` 的 key 写错 | ④ 僵尸条目 + 路径不在该顶层条目底下 |
| 入库一个 `scripts/package-macos.sh`,并删掉它的 pin | ④ `scripts/` 里有命中文件且不在 `NON_NATIVE_PINNED` 里(保留 pin 时通过) |
| `grep` 改回经 argv 传正则 | ②b 只有 ERE 命中 `build-macosXyml`(本机 Git for Windows 的 grep 会吃掉反斜杠,见脚本注释) |

## 5. 局限

- runner 上的 `AUHostingService` 只是近似。Logic 自己的进程外宿主、沙箱配置与 runner 上的不一定相同。
- 进程外实例化需要用户会话;runner 没有时只能判「不定」。
- 宿主按 A1 → A2 → B1 **串行**实例化并初始化,附着方的探针不等待创建方(不重试 `fstat` 尺寸与段头 magic)。若改成几个实例并行初始化,附着方可能先于创建方的 `ftruncate` / 写 magic 读到段,记成 `5`(数据不符);判定会因此落到「不定」(部分 shm 结果),不会被读成 A、B 或 C。
- 段布局只在 spike 目录有效,与 `docs/IPC_CONTRACT.md` 的冻结布局无关;段名借用冻结前缀,只为让名字长度与将来的真实名字同量级。
