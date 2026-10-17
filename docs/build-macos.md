# build-macos —— 在 Mac 上本地编译、测试与打包

> 状态:演进中(macOS 为 beta)
> 真源:`.github/workflows/build-macos.yml`(构建与验证门)、`.github/workflows/macos-oop.yml`(`scvb_auhost`)、`scripts/package-macos.sh`(打包)、根 `CMakeLists.txt`(架构与部署目标)
> 面向读者:要在 Mac 上自己编、自己跑测试的开发者与维护者。想装现成的包请看 [README](../README.zh-CN.md) 与 [用户手册](USER_GUIDE.zh-CN.md);发版流程见 [RELEASE.md](RELEASE.md)。

本文的命令取自上述 workflow 的对应步骤(去掉了 CI 专用的环境变量与 summary 输出)。**workflow 与脚本才是真源**:本文与它们不一致时以它们为准,并请顺手改本文。Windows 的本地流程是 [CONTRIBUTOR_ONBOARDING.md](CONTRIBUTOR_ONBOARDING.md) 与 `scripts/gates.ps1`,二者互不替代:`gates.ps1` 是 Windows 专属,mac 侧的每一道门都写成 `build-macos.yml` 里的独立步骤,没有本地一键脚本,本文第 4 节给出等价的手动做法。

目前只支持 **Apple Silicon(arm64)**,构建产物是 arm64 单架构,不做 Intel 与 Universal。

---

## 1. 你需要装什么

| 组件 | 要求 | 怎么装 / 备注 |
| --- | --- | --- |
| Xcode 或 Command Line Tools | 能用 Apple clang(含 libc++)、`ditto`、`lipo`、`xattr`;`auval` 随系统自带 | `xcode-select --install`(只装命令行工具足够编译;CI 在 `macos-15` 镜像自带的 Xcode 上编) |
| CMake | ≥ 3.22(与 [CLAUDE.md](../CLAUDE.md) §6 一致) | `brew install cmake` |
| Ninja | 任意近版 | `brew install ninja`(CI 的生成器是 Ninja,本文命令也用它) |
| JUCE | **`.juce-version` 里那个 tag** | 见第 2 节 |
| pluginval | **`.pluginval-version` 里那个 tag** | 见第 5 节 |
| PowerShell 7(`pwsh`) | 仅 `scripts/package-macos.sh` 需要 | `brew install powershell`;打包脚本把合规判据交给 `scripts/package.ps1 -Preflight`,不另写第二套 |

WebView2 在 mac 上不适用:mac 的界面走系统的 WKWebView,不需要额外的运行时或 SDK。

版本纪律与 Windows 相同:JUCE 与 pluginval 的版本只认仓库根的 `.juce-version` / `.pluginval-version`,**不要在命令、脚本或文档里写版本号字面量**(本文也因此都用命令从文件里读)。

## 2. 取 JUCE

JUCE 不在本仓里,构建时用 `-DJUCE_PATH` 指向它。取与 `.juce-version` 完全一致的 tag(CI 用的就是这条浅克隆):

```bash
git clone --depth 1 --branch "$(tr -d '[:space:]' < .juce-version)" https://github.com/juce-framework/JUCE.git JUCE
git -C JUCE tag --points-at HEAD     # 应当列出与 .juce-version 相同的 tag
```

`JUCE/` 放在仓库根目录下是 CI 的做法;放在别处也行,把下面的 `-DJUCE_PATH` 改成实际路径。

## 3. 配置与编译

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJUCE_PATH="$PWD/JUCE" \
  -DSCVB_BUILD_TESTS=ON \
  -DCMAKE_COLOR_DIAGNOSTICS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel -- -k 0 2>&1 | tee build.log
```

- **架构与部署目标不要在命令行传。** 唯一真源是根 `CMakeLists.txt` 里 `project()` 之前的两段默认值:`CMAKE_OSX_ARCHITECTURES` 默认 `arm64`,`CMAKE_OSX_DEPLOYMENT_TARGET` 默认 `11.0`(只在你没自己定义这两个变量时生效)。配置完用下面的命令读回,CI 也是这样核的:

  ```bash
  sed -n 's/^CMAKE_OSX_ARCHITECTURES:[A-Z]*=//p;s/^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]*=//p' build/CMakeCache.txt
  ```

  应当得到 `arm64` 与 `11.0`。在命令行另传一个值会让「默认值真的在起作用」无从证明,也会让你本地编出的东西和 CI、发行包不一致。
- `-DCMAKE_COLOR_DIAGNOSTICS=OFF` 不是可有可无:诊断行里夹进 ANSI 颜色码后,下面零警告检查的匹配会静默失效(门禁绿是因为没认出来)。
- `-k 0` 让一个编译单元红了也继续编别的,一次就能看到全部编译错误。
- 默认 `all` 目标一次编出三个插件(SCVB Input / Output / Monitor)各自的 VST3 与 AU(格式表在根 `CMakeLists.txt` 的 `SCVB_PLUGIN_FORMATS`)、全部 mac 上登记的测试和 `tests/tools` 下的工具。
- 成品 bundle 在 `build/` 下的各插件目标目录里,恰好应是 3 个 `.vst3` 与 3 个 `.component`:

  ```bash
  find build -type d \( -name '*.vst3' -o -name '*.component' \) -not -path '*/_deps/*'
  ```

**零警告**是 CI 的门:first-party(`src/`、`tests/` 等,排除 `_deps/` 与 `JUCE/`)不许有 clang 警告。本地等价检查(只认编译器诊断行,链接器的 `ld: warning:` 不算):

```bash
grep -E ':[0-9]+:[0-9]+: warning:' build.log | grep -vE '^([^:]*/)?(_deps|JUCE|vcpkg)/' || echo "first-party warnings: 0"
```

注意这只在 `build.log` 真的包含完整编译输出时才有意义:增量构建没重编的文件不会再报警告。要对一个干净的结论,先 `rm -rf build` 重新配置编译。

## 4. 跑测试

### 4.1 ctest 条目

CI 用 `-R` **按名字精确列出**要跑的十二条,并先断言每条恰好注册一次(`ctest --test-dir build -N -R '^<名字>$'` 必须命中 1 条),不靠 ctest 的隐式全选,「mac 上跑哪几套」因此是写下来的事实:

```bash
ctest --test-dir build --output-on-failure --no-tests=error --timeout 300 \
  -R '^(scvb_tests|scvb_seqlock_stress|scvb_params_tests|scvb_plugin_common_tests|scvb_input_bridge_tests|scvb_ipc_tests|scvb_monitor_tests|scvb_host_tests_shard1of4|scvb_host_tests_shard2of4|scvb_host_tests_shard3of4|scvb_host_tests_shard4of4|scvb_host_sched_tests)$'
```

只想跑一套时把 `-R` 换成那一条的名字,例如 `-R '^scvb_tests$'`。几点要注意:

- **不要加 `-j`。** `scvb_ipc_tests`、`scvb_monitor_tests`、host 各片与 `scvb_tests` 用真实的共享内存段名建段并拉对端进程,靠 `tests/support/exclusive_guard.h` 的 `flock` 互斥,串行跑是设计。
- **跑测试时退出 DAW,且别同时开着另一份在跑的 SCVB。** 段名前缀 `SynchainSCVB.v1.` 全机唯一,测试和真实插件会抢同名段,抢到时的红不是回归。
- `--timeout 300` 只给没有 `TIMEOUT` 属性的条目兜底;每个 `add_test` 另有自己的上界,CI 用 `pwsh -NoProfile -File scripts/check-ctest-timeouts.ps1 -BuildDir build` 检查(先跑 `-SelfTest`);装了 `pwsh` 的 Mac 上同样能跑。
- 条目数量的下限在 workflow 里另有一道(`scvb_ipc_tests` ≥ 40、`scvb_monitor_tests` ≥ 11、`scvb_host_tests` ≥ 246、`[sched]` ≥ 49 个用例),防某个用例被平台条件挡掉后「整套绿」掩盖它不在场。手动核:`"$(find build -type f -name scvb_host_tests -perm -u+x -not -path '*/_deps/*' -print -quit)" --list-tests | tail -n 2`。

### 4.2 host 在 mac 上分四片

Windows 上免 DAW 宿主 harness 是一条 `scvb_host_tests`;mac 上同一个二进制按 Catch2 的 `--shard-count 4 --shard-index <i>` 分成 `scvb_host_tests_shard1of4` … `shard4of4` 四条,另有 `scvb_host_sched_tests`(按 `[sched]` 选出的宿主调度模拟器)。原因(取自 `tests/CMakeLists.txt` 里 host 块的注释,macos-15 arm64 CI 实测):

- 不分片的一条在 CI runner 上约 1260 s,超过那条 900 s 的上界,到点被杀、后面的用例没跑(跑完的全绿)。慢的是**等待**不是算力:同一进程里 25 Hz 的 `juce::Timer` 实得间隔 p50 92 ms / p90 179 ms,`runDispatchLoopUntil(1)` 实得 p50 10 ms,而 host 用例大多是「泵消息直到某个 Timer 驱动的条件成立」。App Nap 豁免等手段没有改善,粒度来自 runner 环境。
- 不把 900 s 抬高,是为了保住「挂死早拦」;改成分片,每片仍用 900 s。分片**不省总时长**(四片仍然串行),它保证每条都在上界之下,以及一片挂死时另几片的结论还在。
- 四片的切点按用例数、不按耗时,所以不等长(一次实测 145 / 249 / 544 / 459 s;CI 里整个 ctest 一步约 26–27 分钟)。自己的机器上的耗时没有量过,取决于机器;分片的对应关系固定,不随机器变。
- 一片被超时杀掉会在 POSIX 上留下无主的共享内存段(SIGKILL 来不及 `shm_unlink`),所以 CI 里有残段基线门;本地遇到时按第 6 节清掉。

### 4.3 墙钟场景与 M12b-fix 的口径

mac CI runner 上 `runDispatchLoopUntil` 被拉长:同一个测试里,「一块的墙钟」是它样本时长的 3–4 倍(一块 10.67 ms 的样本,mac 上平均 30–44 ms 墙钟,Windows 上基本准时),**墙钟与样本在 mac 上不再等价**。M12b-fix 的口径是:

- **每个断言必须在产品自己判的那个时间域里量。** 产品按墙钟判(例如停摆看门狗 500 ms)的,测试就量墙钟;产品按样本判的(例如失准告警的宽限 `unreadGrace`),测试就量样本。
- **判据不放宽。** 数字不变,只换时间域;不为让 mac 变绿而把阈值调松。
- 写新的 host / 调度测试遇到 mac 上偶发红,先问「断言量的时间域和产品判的是不是同一个」,再考虑别的。已知另有一类偶发(sched 里 LS-1 / LS-2 / LS-4 的预取轨定位后丢帧)当时判为**不是**时间域错配、未改,这类红要在 PR 里照实说,不要靠放宽判据抹平。

墙钟敏感的格(`pace=1` 的 LS-11a/b、LS-12 实时格、LS-14、LS-15 等)的清单与理由写在 `tests/CMakeLists.txt` 的 sched 块注释里。

## 5. auval 与 pluginval

两者都对**已安装、已注册**的 AU 跑(pluginval 验 `.component` 时,JUCE 读 Info.plist 拿三元组再去注册表找组件)。先装 AU:

```bash
dest="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$dest"
find build -type d -name '*.component' -not -path '*/_deps/*' -print0 | while IFS= read -r -d '' b; do
  rm -rf "$dest/$(basename "$b")"
  ditto "$b" "$dest/$(basename "$b")"      # 不要用 cp -r:会丢符号链接与扩展属性,bundle 散架
done
killall -9 AudioComponentRegistrar 2>/dev/null || true   # 让系统重扫
```

这会覆盖你本机已装的同名 SCVB AU;只想隔离测试时,在另一个账户里做。

### auval

三个 AU 三元组是冻结值(ADR-001),写死在 `build-macos.yml` 的 auval 步骤里:

```bash
auval -v aufx Scvi Snch
auval -v aufx Scvo Snch
auval -v aufx Scvm Snch
auval -a | grep Snch        # 每个三元组应恰好对应一个组件(多出来的行先查 ~/Library/Audio/Plug-Ins/Components 与 /Library/Audio/Plug-Ins/Components 里的旧安装)
```

CI 的判据是**输出含 `AU VALIDATION SUCCEEDED` 且退出码为 0**,两样缺一不可(auval 的退出码历史上不可靠,所以不单看它)。`auval -strict -v ...` 只记录、不作门;不过 pluginval 在 strictness 5 自带一项会跑 `auval -strict -v`。注册刚改过时 `auval -a` 里可能要等几秒才出现,CI 轮询至多 30 s。

### pluginval

版本只从 `.pluginval-version` 读,按上游发行页取 mac 版:

```bash
pv="$(tr -d '[:space:]' < .pluginval-version)"
mkdir -p pluginval-bin && curl -fsSL -o pluginval-bin/pluginval_macOS.zip "https://github.com/Tracktion/pluginval/releases/download/$pv/pluginval_macOS.zip"
unzip -q -o pluginval-bin/pluginval_macOS.zip -d pluginval-bin
PLUGINVAL=pluginval-bin/pluginval.app/Contents/MacOS/pluginval
chmod +x "$PLUGINVAL"
"$PLUGINVAL" --version            # 形如 pluginval - <版本>,应与 .pluginval-version 去掉前导 v 后逐字相同

# 参数与 Windows 的 gate 7 逐字对齐;.vst3 验构建树里那份,.component 验上面装好的那份
"$PLUGINVAL" --strictness-level 5 --timeout-ms 60000 --skip-gui-tests --validate "build/<含路径>/SCVB Output.vst3"
"$PLUGINVAL" --strictness-level 5 --timeout-ms 60000 --skip-gui-tests --validate "$HOME/Library/Audio/Plug-Ins/Components/SCVB Output.component"
```

CI 对 3 个 `.vst3` 加 3 个已装 `.component` 共 6 格各跑一遍,每格要求:退出码 0、输出里有单独一行 `SUCCESS`、恰好测到 1 个对应格式的插件(`Testing plugin: VST3-…` / `Testing plugin: AudioUnit-…`)。带 GUI 的 pluginval 在 mac 上只是**记录项**(只对 `SCVB Output.vst3` 跑一次,不作门)。

### 共享内存残段检查

auval / pluginval 前后各跑一次(CI 的残段基线门与残段门),验证后不许留下 SCVB 的共享内存段:

```bash
SCVB_TESTS="$(find build -type f -name scvb_tests -perm -u+x -not -path '*/_deps/*' -print -quit)"
"$SCVB_TESTS" "[shm-leftover]"      # 退出码 0 = 8 组真实段名一个都不在
```

## 6. 日志与残留文件在哪

| 位置 | 内容 |
| --- | --- |
| `~/Library/Logs/Synchain/SCVB/input.log`、`output.log`、`monitor.log` | 每个插件的平台诊断日志,约 1 MB 轮转;含开场行与周期性的 `viz-rate` 行。**行里可能含你的用户名,发出去之前先看一遍。** |
| `~/Library/Application Support/Synchain/SCVB/` | 跨工程偏好;其下 `ipc/` 是插件之间共享内存用的锁目录,**不要在 DAW 运行时手动删它** |
| `~/Library/Audio/Plug-Ins/Components/` | 装好的 AU(上面第 5 节装的) |
| (共享内存段) | 是 POSIX 命名段,名字前缀 `SynchainSCVB.v1.`;没有现成的列表命令,用第 5 节的 `[shm-leftover]` 用例探测 |

测试被强杀后留下残段,又没有其他 SCVB 进程在用时,重启机器是最省事的清法;也可以用 `scvb_tests` 的 `[shm-leftover]` 用例确认清掉没有。

## 7. 打包:`scripts/package-macos.sh`

`scripts/package-macos.sh` 是 mac 发行打包的**唯一真源**(CI 的打包冒烟与发版共用它,绝不要在 workflow 里内联打包命令)。输入是**一个只放着 6 个 bundle 的目录**,所以先把成品 `ditto` 出来(与 CI 一样,避免构建树里别的同名目录混进去):

```bash
rm -rf package-in && mkdir -p package-in
find build -type d \( -name '*.vst3' -o -name '*.component' \) -not -path '*/_deps/*' -print0 | while IFS= read -r -d '' b; do
  ditto "$b" "package-in/$(basename "$b")"
done
bash scripts/package-macos.sh --dry-run                                  # 只解析版本、跑合规预检,什么都不写
bash scripts/package-macos.sh --build-dir package-in --out-dir dist
```

- 版本缺省读根 `CMakeLists.txt` 的 `project(SCVB VERSION X.Y.Z)`;`--version X.Y.Z[-pre]` 显式指定(不带前导 `v`)。
- 产出恰好三件:`SCVB-v<版本>-macos-arm64.zip`、`.zip.sha256`、`package-summary-macos.md`。
- 脚本断言恰好 3 个 `.vst3` + 3 个 `.component`、名字恰好是 SCVB Input / Output / Monitor、每个 Mach-O 都只有 arm64;打包后重新打开 zip 逐项核对,并用 `.sha256` 自验。同一份输入重跑,zip 的 sha256 不变(CI 连跑三次核这一条)。
- 合规判据(许可证全文、`THIRD-PARTY-NOTICES.md` 点名的声明文件)由 `pwsh scripts/package.ps1 -Preflight` 负责,所以需要 `pwsh`;`LICENSES/` 缺许可证全文时脚本会红。`--allow-missing-license-texts` **只用于演练 tag**(`v0.0.0-test*`)与本地试打包,不要用在真实版本上。
- 拷贝与压缩一律是 `ditto`(`cp -r` / `zip -r` 会丢符号链接与可执行位,用户解压后拿到加载不了的死壳)。脚本只用 macOS 自带的 bash 3.2 与 BSD 工具能跑通的写法。
- **mac 包不签名、不公证。** 用户安装时要执行 `xattr`(见 zip 里的 `INSTALL.txt` 与 README)。本地试打包得到的 zip 同样未签名。

## 8. `scvb_auhost`:二进制级调度宿主

`tests/macos/auhost/` 是一个独立 CMake 工程(根工程与 Windows 构建都不引用它,不链 JUCE),只通过 AudioComponent 加载**已构建、已安装**的三个真实 component,在**进程内**与**进程外**(`kAudioComponentInstantiation_LoadOutOfProcess`,插件跑在 `AUHostingService` 里,Logic 的做法)两种模式下按宿主调度渲染,用逐轨在场判据判 Output 的输出。判据、退出码与局限见它自己的 [README](../tests/macos/auhost/README.md)。它是近似测试:runner 上的 `AUHostingService` 不等于 Logic。

先按第 5 节把三个 `.component` 装好,然后:

```bash
cmake -S tests/macos/auhost -B build-auhost -DCMAKE_BUILD_TYPE=Release
cmake --build build-auhost --parallel
./build-auhost/scvb_auhost --self-test                                        # 假插件跑整条调度与判据,不需要装任何 component
./build-auhost/scvb_auhost --mode inproc --scenario ls1 --out-dir auhost-out
./build-auhost/scvb_auhost --mode oop    --scenario ls4 --out-dir auhost-out  # --oop 的写法:--mode oop
./build-auhost/scvb_auhost --mode inproc --scenario ls1 --negative-control --out-dir auhost-out
```

- **进程外就是 `--mode oop`**(进程内是 `--mode inproc`);场景是 `ls1`(稳态回放)与 `ls4`(Cycle 加混合提前量)。CI 里这两种模式 × 两个场景各跑一遍,外加一步 `--negative-control`(宿主故意不调用 lane 1 的 Input,判据必须红在 lane 1 且只红在 lane 1)。
- 退出码:`0` 判据全过(负对照下是「负对照成立」);`1` 判据或进程判据不过;`2` **没测成**(组件没注册、实例化 / 配置 / Initialize 失败、预热等不到、用法错)。`2` 先查第 5 节的 AU 安装与注册,不是判据红。
- 输出在 `--out-dir` 下:`<mode>-<scenario>.kv`(全部数字)、`.md`(小结)、`-frames.csv`(逐帧逐轨判定)。只记录组件名与版本、进程名与系统服务路径,不记录任何用户目录。
- 渲染在独立线程上按**墙钟实时**推进(SCVB 有 500 ms 停滞这类墙钟门限),LS-1 约 6 s、LS-4 约 10 s 的真实时间;跑的时候机器别同时做别的重活。
- CI 入口是 `macos-oop.yml` 的 `scvb-auhost` job(由 `build-vst3.yml` 的 `macos-oop` caller 调用,测的是同一个 run 里 `macos-build` 上传的 bundle)。`macos-26` runner 只在 `workflow_dispatch` 时加入矩阵,PR 与 push 只跑 `macos-15`。

## 9. CI 里对应的 job(速查)

| job | 在哪 | 什么时候跑 |
| --- | --- | --- |
| `macos-build`(→ `build-macos.yml` 的 `build-and-validate-macos`) | `build-vst3.yml` 的 caller | PR(碰了 native 路径)、push→`dev`、dispatch;**tag 上一律跳过** |
| `macos-oop`(→ `macos-oop.yml` 的 `scvb-auhost`) | `build-vst3.yml` 的 caller,`needs` `macos-build` | 同上;`macos-26` 只在 dispatch |
| `build-mac`(→ `build-macos.yml` 的 release 档) | `release.yml` | 推 `v*` tag |

两个 mac job 都不是 required check,mac 红不挡 Windows 出包。`build-and-validate-macos` 的超时是 75 分钟(整个 job 实测约 39 分钟,其中 ctest 约 27 分钟)。发版相关的 mac 部分见 [RELEASE.md](RELEASE.md) 的「macOS 包」一节。
