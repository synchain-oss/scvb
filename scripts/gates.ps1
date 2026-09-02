<#
.SYNOPSIS  SCVB 本地质量门禁 —— 提子 PR 前必须全绿。
.DESCRIPTION
  档位(06 §5.1):
    pwsh scripts/gates.ps1                 # 全量(含 GUI pluginval)
    pwsh scripts/gates.ps1 -PluginOnly     # gate 1-7,跳过真机 GUI(gate 8)
    pwsh scripts/gates.ps1 -Quick          # 跳过 pluginval(gate 7/8),快速回环
  所有 cmake/ctest/pluginval 路径基于 -BuildDir(默认 build),并行 agent 靠它隔离构建目录。
  [R4/J56] gate 3b(gitleaks)/ 3c(reuse lint)与 check-spdx.ps1 已由 T01d 接入(06 §5.1)。
  [SL-265] gate 3j(check-privacy)= 公开仓隐私门禁,与 3b 同族:gitleaks 只管 secrets,
  个人身份信息(代号禁词/本机路径/个人邮箱域/主机名)由 3j 管;CI 侧对应 compliance.yml 两步。
  [SL-267] gate 3k(check-font-names)= 字体保留名门禁(OFL-1.1 §3),与 3c/3j 同属合规族:
  断言分发的 woff2 `name` 表与进包文本资源的字体栈都不含上游 RFN;
  CI 侧同样落在 compliance.yml(自测 + 扫描两步)。
  [SL-277/J96] **锁纪律**:gate 1-5 完全不持锁,多个 agent 可以同时 configure/build;
  gate 6/7/8 由本脚本自己用命名互斥体 `SCVB-ipc-tests` 全机串行(共享内存段名全机唯一)。
  调用方**不要**再在外面把整条 gates 包进目录锁 —— 那会把编译也串起来,正是本卡要拆掉的。
  等锁有 30 分钟上界:超时(或互斥体建不出来)→ 判负并**跳过** gate 6/7/8 不执行,
  绝不无锁硬跑 —— 无锁跑会去抢隔壁持锁 agent 的共享内存段,让那一侧收到查不出的假红。
  逃生口 `-NoIpcLock` 只在确认无并行 agent 时用。
.EXAMPLE   pwsh scripts/gates.ps1
.EXAMPLE   pwsh scripts/gates.ps1 -PluginOnly -BuildDir build-T15
#>
param(
  [switch]$Quick,
  [switch]$PluginOnly,
  [string]$Config = 'Release',
  [string]$JucePath = $env:JUCE_PATH,
  [string]$BuildDir = 'build',
  [string]$PluginvalExe = $env:PLUGINVAL_EXE,
  # [SL-277] CMake 生成器。默认空 = 沿用 CMake 在本机的默认选择(Windows 上是 Visual
  # Studio 生成器),与本卡之前的行为逐字一致。
  # 传 'Ninja Multi-Config' 可复现 CI 侧的构建(CI 自 [J96] 起用它 + sccache)。
  # **不做自动探测**:`ninja` 在 PATH 上但当前 shell 没有 vcvars 时,`-G Ninja` 会
  # 因为找不到 cl.exe 直接配置失败 —— 自动探测会把所有 agent 的 gate 4 一起变红,
  # 而他们什么都没改。所以要用就显式传,并且在 Developer Command Prompt 里跑。
  [string]$Generator = $env:SCVB_CMAKE_GENERATOR,
  # [SL-277] 逃生口:确认本机没有第二个 agent 在跑 gate 6/7/8 时才用。
  # 平时**不要**加 —— 关掉互斥后并行跑出来的红大概率是抢共享内存段,不是回归。
  [switch]$NoIpcLock
)

$ErrorActionPreference = 'Continue'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$ClangFormatVersion = '18.1.8'
$juceVersion = (Get-Content .juce-version -Raw).Trim()
$pluginvalVersion = (Get-Content .pluginval-version -Raw).Trim()

$results = New-Object System.Collections.Generic.List[object]
$script:fatal = $false

function Set-Gate {
  param([string]$Name, [bool]$Pass)
  if (-not $Pass) { $script:fatal = $true }
  $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
  $color = if ($Pass) { 'Green' } else { 'Red' }
  $results.Add([pscustomobject]@{ Gate = $Name; Result = $tag })
  Write-Host ("[{0}] {1}" -f $tag, $Name) -ForegroundColor $color
}

function Set-Skip {
  param([string]$Name)
  $results.Add([pscustomobject]@{ Gate = $Name; Result = 'SKIP' })
  Write-Host ("[SKIP] {0}" -f $Name) -ForegroundColor Yellow
}

# ---- IPC 测试锁(只包 gate 6/7/8;[SL-277]/[J96] 拆锁)-----------------------
# **为什么只包 6/7/8**:gate 4(configure)/ 5(build)只读写各自的 `-BuildDir`,
# 并行 agent 之间零共享状态;真正需要互斥的是**跨进程共享内存段** —— 段名前缀
# `SynchainSCVB.v1.` 是全机唯一的(docs/IPC_CONTRACT.md),ctest 里的 ipc 套件与
# pluginval 加载插件时都会去开同名段,两份同时跑必然互相踩,红得很像回归。
# 改造前的做法是各 agent 在**脚本外面**把整条 gates 包进一把目录锁,于是 20 分钟的
# 编译也被串行化了:四个 agent 排队等一个人编译,而它们本来完全可以并行编。
#
# **为什么用命名互斥体而不是目录锁**:内核对象在进程退出时**必然**释放(哪怕被
# kill、哪怕崩溃),所以没有 owner.txt、没有孤儿判定、也没有「等超时后覆写别人的锁」
# 这条已经实伤过人的路径。目录锁那套协议仍然可以留给「不经 gates.ps1 手跑 ctest」
# 的场景,但经 gates.ps1 的路径不再需要它。
#
# **只用 `Local\`,不设 Global 降级**(PR#176 复审采纳)。曾经写成「先试 `Global\`,
# 失败退 `Local\`」,那是一个**静默失去互斥**的洞:`Global\` 创建失败的现实原因不是
# 缺 SeCreateGlobalPrivilege(交互登录用户一般都有),而是**已存在的同名 Global 对象
# 的 DACL 拒绝当前 token** —— 提权终端里的 agent A 先建了 `Global\`,普通终端里的
# agent B 抛 UnauthorizedAccessException 被 catch 吃掉、退到 `Local\`,于是 A 持
# Global、B 持 Local,**两把不同的锁**,共享内存段照抢,而日志里只有一行黄字。
# 本项目的并行形态就是同一用户登录会话下的多个 agent 终端,`Local\` 本来就够;
# 去掉那一档,混合作用域的洞就不存在了。
# 建不出来时**判负**(见调用处的 Set-Gate),绝不静默继续 —— 并发假红最难查的
# 就是「以为有锁,其实没有」。
function New-ScvbMutex {
  param([string]$Name, [string]$Tag)
  try { return New-Object System.Threading.Mutex($false, "Local\$Name") }
  catch {
    Write-Host ("  [{0}] 互斥体 Local\{1} 创建失败({2}):{3}" -f $Tag, $Name, $_.Exception.GetType().Name, $_.Exception.Message) -ForegroundColor Red
    return $null
  }
}

# 等锁**必须有上界**(PR#176 复审采纳)。这把锁现在包着 gate 8 的 GUI pluginval,
# 而 `--timeout-ms` 只管单个测试项 —— 进程本身卡在模态框或崩溃对话框上时它不受约束。
# 无超时的 `WaitOne()` 会让其余 agent 在「等待 Local\...」那一行之后静静挂几个小时,
# 零输出。改造前的目录锁至少有「超时 + 接管」那条路(那条路自身实伤过人,删掉是对的),
# 现在换成:**宁可红,不要无限挂** —— 超时返回 $false,调用处把对应 gate 判负。
# 返回值就是「有没有真的拿到」,调用处一律要判,别丢。
# `SCVB_MUTEX_WAIT_MINUTES` 只为**验证这条判据**而存在(反向验证:外面另起一个进程占住
# 互斥体,把上界调到 1 分钟,就能在一分钟内看到 6/7/8 判负而不是等半小时)。平时不要设 ——
# 调小不会削弱互斥(拿不到锁一律判负 + 跳过,绝不无锁跑),只会让正常排队更容易被判负。
$script:ScvbMutexWaitMinutes = if ($env:SCVB_MUTEX_WAIT_MINUTES) { [int]$env:SCVB_MUTEX_WAIT_MINUTES } else { 30 }

function Wait-ScvbMutex {
  param($Mutex, [string]$Name, [string]$Tag, [int]$TimeoutMinutes = $script:ScvbMutexWaitMinutes)
  # 时间戳带毫秒:并发验证时,「谁在什么时刻拿到/放开」这条证据只能来自进程**内部**的
  # 时钟。外面用管道加时间戳靠不住 —— pwsh 往管道写是块缓冲的,读到的时刻会晚于打印
  # 时刻,两条流的偏移量还不一样,拿它对拍会看出根本不存在的重叠。
  Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 等待 Local\{2}(上界 {3} 分钟)..." -f $Tag, (Get-Date).ToUniversalTime(), $Name, $TimeoutMinutes) -ForegroundColor Yellow
  $got = $false
  try { $got = $Mutex.WaitOne([TimeSpan]::FromMinutes($TimeoutMinutes)) }
  catch [System.Threading.AbandonedMutexException] {
    # 前一个持有者进程异常退出。锁已经归我们了,只是说明上一次跑得不干净。
    $got = $true
    Write-Host ("  [{0}] 前一持有者异常退出(AbandonedMutex),已接管" -f $Tag) -ForegroundColor Yellow
  }
  if (-not $got) {
    Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 等待 Local\{2} 超过 {3} 分钟仍未获得,判负" -f $Tag, (Get-Date).ToUniversalTime(), $Name, $TimeoutMinutes) -ForegroundColor Red
    Write-Host ("  [{0}] 提示:多半有卡死的 pluginval / ctest 进程还占着锁,查一下再重跑。" -f $Tag) -ForegroundColor Yellow
    return $false
  }
  Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 已获得 Local\{2}" -f $Tag, (Get-Date).ToUniversalTime(), $Name) -ForegroundColor Green
  return $true
}

function Enter-ScvbIpcLock {
  if ($NoIpcLock) {
    Write-Host '  [ipc-lock] -NoIpcLock:跳过 IPC 测试锁(仅限确认无并行 agent 时)' -ForegroundColor Yellow
    return $null
  }
  $m = New-ScvbMutex -Name 'SCVB-ipc-tests' -Tag 'ipc-lock'
  if ($null -eq $m) {
    # 判负而不是静默放行:拿不到锁就等于没有并发保护,后面 gate 6/7/8 的结果不可信。
    Set-Gate 'IPC 测试锁(gate 6/7/8 互斥)' $false
    return $null
  }
  if (-not (Wait-ScvbMutex -Mutex $m -Name 'SCVB-ipc-tests' -Tag 'ipc-lock')) {
    # 等超时 = 同样没有并发保护,与建不出来同一档处理。句柄没拿到锁,直接 Dispose。
    Set-Gate 'IPC 测试锁(gate 6/7/8 互斥)' $false
    $m.Dispose()
    return $null
  }
  return $m
}

function Exit-ScvbIpcLock {
  param($Mutex)
  if ($null -eq $Mutex) { return }
  try { $Mutex.ReleaseMutex() } catch { Write-Host ("  [ipc-lock] 释放异常:{0}" -f $_.Exception.Message) -ForegroundColor Yellow }
  $Mutex.Dispose()
  Write-Host ("  [ipc-lock] {0:HH:mm:ss.fff}Z 已释放" -f (Get-Date).ToUniversalTime()) -ForegroundColor Green
}

# ---- 定位 pluginval ----
if (-not $PluginvalExe) {
  $PluginvalExe = (Get-Command pluginval -ErrorAction SilentlyContinue).Source
}
if (-not $PluginvalExe) {
  $candidate = Join-Path $RepoRoot '..\tools\pluginval\pluginval.exe'
  if (Test-Path $candidate) { $PluginvalExe = $candidate }
}

# ==================================================================
Write-Host '=== Gate 1: 依赖预检 ==='
# ==================================================================
$ok = $true

$cmakeVer = ((& cmake --version 2>$null | Select-Object -First 1) -replace 'cmake version ', '')
if (-not $cmakeVer) { Write-Host '  cmake 未找到' -ForegroundColor Red; $ok = $false }
else { Write-Host ("  cmake {0}" -f $cmakeVer) }

$pfx86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$vswhere = Join-Path $pfx86 'Microsoft Visual Studio\Installer\vswhere.exe'
$msvc = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $msvc) { Write-Host '  MSVC(C++ 工具)未找到' -ForegroundColor Red; $ok = $false }
else { Write-Host ("  MSVC {0}" -f $msvc) }

if (-not $JucePath -or -not (Test-Path (Join-Path $JucePath 'CMakeLists.txt'))) {
  Write-Host '  JUCE_PATH 未设置或无效' -ForegroundColor Red; $ok = $false
}
else {
  $juceTag = (& git -C $JucePath describe --tags 2>$null)
  if ($juceTag -and ($juceTag.Trim() -ne $juceVersion)) {
    Write-Host ("  JUCE tag '{0}' 与 .juce-version '{1}' 不一致" -f $juceTag, $juceVersion) -ForegroundColor Red
    $ok = $false
  }
  else { Write-Host ("  JUCE {0}" -f $juceVersion) }
}

$cfVer = ((& clang-format --version 2>$null) -join ' ')
if ($cfVer -notmatch '18\.1\.8') {
  Write-Host ("  clang-format 18.1.8 未找到(当前: {0})" -f $cfVer) -ForegroundColor Red
  $ok = $false
}
else { Write-Host ("  clang-format {0}" -f $ClangFormatVersion) }

if (-not $PluginvalExe -or -not (Test-Path $PluginvalExe)) {
  Write-Host ("  pluginval 未找到(要求 {0};设 PLUGINVAL_EXE 或放 tools\pluginval\pluginval.exe)" -f $pluginvalVersion) -ForegroundColor Yellow
}
else { Write-Host ("  pluginval {0}(要求 {1})" -f $PluginvalExe, $pluginvalVersion) }

# 宪法只读副本同步(06 §3.4 / 07 T01,进 gate 1)
& pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts\check-constitution-sync.ps1') -RepoRoot $RepoRoot
if ($LASTEXITCODE -ne 0) { $ok = $false }

Set-Gate '1 依赖预检' $ok

# ==================================================================
Write-Host '=== Gate 2: clang-format (18.1.8) ==='
# ==================================================================
$files = @(git ls-files '*.h' '*.hpp' '*.cpp' '*.cc' | Where-Object { $_ -notmatch '^third_party/' })
$cf = $true
if ($files.Count -eq 0) {
  Write-Host '  未发现 C++ 源文件' -ForegroundColor Red
  $cf = $false
}
else {
  $cfOut = (& clang-format --dry-run --Werror --style=file $files 2>&1)
  if ($LASTEXITCODE -ne 0) {
    Write-Host '  clang-format 差异:' -ForegroundColor Red
    $cfOut | ForEach-Object { Write-Host ("  " + $_) }
    $cf = $false
  }
}
Set-Gate '2 clang-format' $cf

# ==================================================================
Write-Host '=== Gate 3: prettier (--check .) ==='
# ==================================================================
# 版本钉死到补丁号,且与 .github/workflows/format.yml 的 prettier 步、scripts/format.ps1 的
# prettier --write **三处同步改**(后者是本 gate 的配对写入器):`@3` 是浮动
# major,本地与 CI 会在不同时间各自解析出不同的 prettier —— 同一份代码「本地绿、CI 红」,
# 而中间没有任何东西变过。
$pp = (npx --yes prettier@3.9.6 --check . 2>&1)
Set-Gate '3 prettier' ($LASTEXITCODE -eq 0)

# ==================================================================
Write-Host '=== Gate 3b: gitleaks (密钥扫描) ==='
# ==================================================================
$gitleaksVer = (Get-Content .gitleaks-version -Raw).Trim()
$GitleaksExe = (Get-Command gitleaks -ErrorAction SilentlyContinue).Source
if (-not $GitleaksExe) {
  $candidate = Join-Path $RepoRoot '..\tools\gitleaks\gitleaks.exe'
  if (Test-Path $candidate) { $GitleaksExe = $candidate }
}
if (-not $GitleaksExe -or -not (Test-Path $GitleaksExe)) {
  Write-Host ("  gitleaks 未找到(要求 {0};设 PATH 或放 tools\gitleaks\gitleaks.exe)" -f $gitleaksVer) -ForegroundColor Red
  Set-Gate '3b gitleaks' $false
}
else {
  $gl = (& $GitleaksExe detect --no-git --redact --config .gitleaks.toml 2>&1)
  if ($LASTEXITCODE -ne 0) { $gl | Select-Object -Last 20 | ForEach-Object { Write-Host ("  " + $_) } }
  Set-Gate '3b gitleaks' ($LASTEXITCODE -eq 0)
}

# ==================================================================
Write-Host '=== Gate 3j: check-privacy (公开仓隐私门禁) ==='
# ==================================================================
# [SL-265] 与 gitleaks 同族但管的是**另一半**:gitleaks 只认 secrets(密钥/令牌),
# 个人身份信息(代号禁词 / 本机路径 / 个人邮箱域 / 主机名)它一条都不拦,故单列一关。
# **先自检再扫**:本门禁的失效模式是「静默放行」—— 针被改坏或豁免表被放宽后扫描照样退 0,
# 门禁看着绿其实什么都没拦。自检红 = 门禁自己坏了,比扫描结果更要紧。
# 与 .github/workflows/compliance.yml 的两步逐字同参。
$privacyOk = $false
if (Get-Command node -ErrorAction SilentlyContinue) {
  $pvSelf = (& node scripts/check-privacy.mjs --self-test 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $pvSelf | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Red }
    Write-Host '  自检失败 = 门禁本身坏了(不是仓里有命中)' -ForegroundColor Red
  }
  else {
    $pv = (& node scripts/check-privacy.mjs 2>&1)
    $privacyOk = ($LASTEXITCODE -eq 0)
    if ($privacyOk) { $pv | Select-Object -Last 1 | ForEach-Object { Write-Host ("  " + $_) } }
    else { $pv | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Red } }
  }
}
else {
  Write-Host '  node 未找到(需 Node >= 18)' -ForegroundColor Red
}
Set-Gate '3j check-privacy' $privacyOk

# ==================================================================
Write-Host '=== Gate 3c: reuse lint (REUSE 合规) ==='
# ==================================================================
if (Get-Command reuse -ErrorAction SilentlyContinue) {
  $rl = (& reuse lint 2>&1)
  $reuseExit = $LASTEXITCODE
}
else {
  $rl = (pipx run reuse lint 2>&1)
  $reuseExit = $LASTEXITCODE
}
if ($reuseExit -ne 0) { $rl | Select-Object -Last 30 | ForEach-Object { Write-Host ("  " + $_) } }
Set-Gate '3c reuse lint' ($reuseExit -eq 0)

# ==================================================================
Write-Host '=== check-spdx(源文件 SPDX 头,06 §5.1 gate 3c 注)==='
# ==================================================================
& pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts\check-spdx.ps1')
Set-Gate 'check-spdx' ($LASTEXITCODE -eq 0)

# ==================================================================
Write-Host '=== Gate 3d: 设计盒真源(design-box.js -> DesignBox.h 对拍)==='
# ==================================================================
# 设计盒常量唯一真源 = web/shared/design-box.js;生成物 src/core/DesignBox.h 必须逐字节一致
# (01 §6.1 / 05 §1.2;消除 Bridge 双处硬编码技术债)。--check 重生成并对拍,漂移即红。
#
# ⚠ 先判 python 在不在,理由同 Gate 3e 的 node 守卫(PR#64 评审【建议】):
# 命令不存在时 PowerShell 抛 CommandNotFoundException 而**不更新 $LASTEXITCODE**,
# 它保留上一条外部命令的 0 ⇒ 对拍一次没跑却判绿。误报绿比硬失败危险得多。
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Write-Host '  python 未找到(gen-design-box.py 需要)' -ForegroundColor Red
  Set-Gate '3d 设计盒真源' $false
}
else {
  $designBox = (& python scripts\gen-design-box.py --check 2>&1)
  if ($LASTEXITCODE -ne 0) { $designBox | ForEach-Object { Write-Host ("  " + $_) } }
  Set-Gate '3d 设计盒真源' ($LASTEXITCODE -eq 0)
}

# ==================================================================
Write-Host '=== Gate 3e: web smoke(web-preview/tests/*.mjs)==='
# ==================================================================
# web-preview/tests 是 UI 侧**唯一**的行为门禁(纯函数 + mock 端到端 + 源码级
# 纪律断言),T31–T36 六张卡的回归保护全压在上面。与 .github/workflows/format.yml
# 的 web-smoke job 同口径。零依赖:不装 npm 包直接 node 跑;每套退出码 0 = 全绿,
# 非 0 会逐条打印 [FAIL]。不提前 break —— 一次跑完好看全所有红项。
#
# ⚠ **必须先判 node 在不在**(PR#64 评审【重要】):PowerShell 找不到外部命令时抛
# CommandNotFoundException,默认 ErrorActionPreference=Continue 下**不更新**
# $LASTEXITCODE —— 它会保留上一条外部命令(check-spdx)的 0,于是「一套都没跑」
# 被判成全绿。**误报绿比硬失败危险得多**。口径照 3b/3c(gitleaks/reuse)。
# CI 侧由 actions/setup-node 钉死版本,无此风险;本守卫只为本地。
$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
$nodeMajor = 0
if ($nodeCmd) {
  $nv = (& node --version 2>&1) -as [string]      # 形如 v22.5.0
  if ($nv -match '^v(\d+)\.') { $nodeMajor = [int]$Matches[1] }
}
$smokeDir = Join-Path $RepoRoot 'web-preview\tests'
$smokeFiles = @(Get-ChildItem -Path $smokeDir -Filter 'smoke-*.mjs' -ErrorAction SilentlyContinue)
if (-not $nodeCmd) {
  # 用到 node 内建 fetch 与全局 WebSocket,故要求 ≥ 22(与 CI 的 node-version 一致)
  Write-Host '  node 未找到(要求 >= 22)' -ForegroundColor Red
  Set-Gate '3e web smoke' $false
}
elseif ($nodeMajor -lt 22) {
  Write-Host ("  node 版本过低: {0}(要求 >= 22:内建 fetch / 全局 WebSocket)" -f $nv) -ForegroundColor Red
  Set-Gate '3e web smoke' $false
}
elseif ($smokeFiles.Count -eq 0) {
  Write-Host '  未发现 web smoke(web-preview/tests/smoke-*.mjs)' -ForegroundColor Red
  Set-Gate '3e web smoke' $false
}
else {
  # 退出码约定(T46 起):0 = 全绿;1 = 有断言失败;**2 = 缺可选外部依赖,本机跑不了**。
  # 会回 2 的是**四套**页面级冒烟(都要一个无头 Chrome/Edge):smoke-monitor-page.mjs、
  # smoke-output-stale-page.mjs(SL-177 过期提示,04 §4.5)、smoke-output-dist-page.mjs
  # (SL-203 分布图补间)与 smoke-seg-restore-page.mjs(SL-242 段级「恢复自动」的作用域)。
  # 执行面按 glob 自动收,加一套不必改这里 —— 这行只是给人读的。
  # 为什么单列一档:
  # 把「本机没装浏览器」和「页面真的坏了」都判成红,等于逼每个只改 C++ 的人装浏览器,
  # 或者反过来诱导谁把这套从门禁里摘掉 —— 两条都比一条 SKIP 差。**但绝不静默**:
  # 打印 SKIP 行并计数,总结里带上,免得「一套没跑」看起来和「跑过了」一样。
  $smokeOk = $true
  $smokeSkipped = 0
  foreach ($f in $smokeFiles) {
    $out = (& node $f.FullName 2>&1)
    $rc = $LASTEXITCODE
    if ($rc -eq 2) {
      $smokeSkipped++
      Write-Host ("  [SKIP] {0}:缺可选外部依赖(见该脚本文件头)" -f $f.Name) -ForegroundColor Yellow
      $out | Select-String -Pattern '^❌' | Select-Object -First 2 |
      ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
    }
    elseif ($rc -ne 0) {
      $smokeOk = $false
      Write-Host ("  {0}:" -f $f.Name) -ForegroundColor Red
      $out | Select-String -Pattern '\[FAIL\]' | Select-Object -First 20 |
      ForEach-Object { Write-Host ("  " + $_) }
    }
  }
  $smokeLabel = '3e web smoke({0} 套,node {1})' -f $smokeFiles.Count, $nv
  if ($smokeSkipped -gt 0) {
    $smokeLabel = '3e web smoke({0} 套 −{1} SKIP,node {2})' -f $smokeFiles.Count, $smokeSkipped, $nv
  }
  Set-Gate $smokeLabel $smokeOk
}

# ==================================================================
Write-Host '=== Gate 3f: 文档真源(九条红字生成物 + 双语结构对等)==='
# ==================================================================
# 12 §3.4 第 5 条把 gen-hard-rules --check 挂在「与 check-i18n.mjs 同一 gates 档」。
# 九条红字要落到 7 处(markdown ×4 + i18n ×3),手抄必漂,而条目数 == 9、grep「六条」、
# check-i18n 的 key 全等这三道既有机检只查得出**数量与标题**,查不出条目**文本**漂移
# —— 逐字节比对生成物是唯一查得出的那道。
# node 守卫同 Gate 3e:找不到 node 时 $LASTEXITCODE 会保留上一条外部命令的 0,
# 「一条都没跑」会被判成全绿,而误报绿比硬失败危险得多。
if (-not $nodeCmd) {
  Write-Host '  node 未找到(要求 >= 22)' -ForegroundColor Red
  Set-Gate '3f 文档真源' $false
}
else {
  $docsOk = $true

  $genOut = (& node scripts\gen-hard-rules.mjs --check 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $docsOk = $false
    Write-Host '  gen-hard-rules --check:' -ForegroundColor Red
    $genOut | ForEach-Object { Write-Host ("  " + $_) }
  }

  # guide.rule1..9 已由 gen-hard-rules 落地,不再需要 --skip-guide-rules 过渡开关(12 §3.4)。
  $i18nOut = (& node scripts\check-i18n.mjs 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $docsOk = $false
    Write-Host '  check-i18n:' -ForegroundColor Red
    $i18nOut | ForEach-Object { Write-Host ("  " + $_) }
  }

  $parityOut = (& pwsh -NoProfile -File scripts\check-readme-parity.ps1 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $docsOk = $false
    Write-Host '  check-readme-parity:' -ForegroundColor Red
    $parityOut | ForEach-Object { Write-Host ("  " + $_) }
  }

  Set-Gate '3f 文档真源' $docsOk
}

# ==================================================================
Write-Host '=== Gate 3g: IPC 契约文档对拍(IPC_CONTRACT.md <-> ipc-layout golden)==='
# ==================================================================
# #75(T39a)把 docs/IPC_CONTRACT.md 转正时,文档里的结构体偏移/大小与实现漂移,
# 靠人审来回修了三轮才收口 —— 人眼数不住 offset,「16..76」错成「16..80」评审
# 100% 看不出来。真源链是 代码 →(tests/core/test_ipc_layout.cpp 的编译期断言)→
# tests/golden/ipc-layout.txt → docs/IPC_CONTRACT.md;最后一环此前全靠人肉维护,
# 本 gate 就是把它机检起来(方向不可颠倒:以 golden 为真,文档是被检侧)。
# 顺带补 golden 的完备性洞:C++ 测试只查「golden 每行都对得上代码」,查不出
# 「代码新增字段而 golden 漏冻」,故脚本另读头文件比对字段集合与顺序。
#
# node 守卫同 Gate 3e/3f:找不到 node 时 $LASTEXITCODE 会保留上一条外部命令的 0,
# 「一条都没跑」会被判成全绿。误报绿比硬失败危险得多。
if (-not $nodeCmd) {
  Write-Host '  node 未找到(要求 >= 22)' -ForegroundColor Red
  Set-Gate '3g IPC 契约文档对拍' $false
}
else {
  # 脚本要求三侧文件齐备(缺一侧自己会硬失败),这里再做一次存在性守卫,
  # 好在文件被挪走时给出比 node 栈更直白的提示。
  $ipcParityInputs = @(
    'docs\IPC_CONTRACT.md',
    'tests\golden\ipc-layout.txt',
    'src\core\ipc\SegmentLayout.h',
    'src\core\ipc\CtrlPlane.h'
  )
  $ipcMissing = @($ipcParityInputs | Where-Object { -not (Test-Path (Join-Path $RepoRoot $_)) })
  if ($ipcMissing.Count -gt 0) {
    Write-Host ('  对拍输入缺失: {0}' -f ($ipcMissing -join ', ')) -ForegroundColor Red
    Set-Gate '3g IPC 契约文档对拍' $false
  }
  else {
    $ipcOut = (& node scripts\check-ipc-doc-parity.mjs --strict-missing 2>&1)
    $ipcOk = ($LASTEXITCODE -eq 0)
    # 非零才打全量;绿时也把 [WARN](文档未标注、无法机检的项)透出来,
    # 免得「机检覆盖不到的洞」悄悄扩大 —— 补齐后可给脚本加 --strict 收紧。
    if (-not $ipcOk) { $ipcOut | ForEach-Object { Write-Host ("  " + $_) } }
    else { $ipcOut | Select-String -Pattern '\[WARN\]' | ForEach-Object { Write-Host ("  " + $_) } }
    Set-Gate '3g IPC 契约文档对拍' $ipcOk
  }
}

# ==================================================================
Write-Host '=== Gate 3i: 桥面/曲线/设计盒 三方对拍(scripts/check-*-parity + design-box)==='
# ==================================================================
# [SL-258] 这三个脚本此前**没有任何执行者** —— 不在 CI、不在本 gates、不在 package.json。
# 于是 [SL-256] 给 check-bridge-parity 加的「已注册 handler ↔ manifest」断言,以及本卡把它
# 扩到 input/monitor 三侧的版本,都只有人手动 node 才会红。同一族的洞因此栽了两次
# (exportSuggestions 与 setGuideSeen:契约齐全、常量在、web 真调用,唯独没注册 handler),
# **即便门禁当时已经写好也照样栽**。判级理由与 Gate 3g 逐字同款:门禁没有执行者等于没有门禁。
# 三条本地实跑均 exit=0 后才接线(curve 最坏偏差 9.6e-5 dB / 容差 0.01 dB)。
if (-not $nodeCmd) {
  Write-Host '  node 不在 PATH —— 本 gate 无法执行(不是跳过,是判负:工具缺失不得计为通过)' -ForegroundColor Red
  Set-Gate '3i 桥面/曲线/设计盒对拍' $false
}
else {
  $parityOk = $true
  foreach ($sc in @('check-bridge-parity.mjs', 'check-curve-parity.mjs', 'check-design-box.mjs')) {
    $out = (& node (Join-Path 'scripts' $sc) 2>&1)
    if ($LASTEXITCODE -ne 0) {
      $parityOk = $false
      Write-Host ("  {0}:" -f $sc) -ForegroundColor Red
      $out | ForEach-Object { Write-Host ("  " + $_) }
    }
  }
  Set-Gate '3i 桥面/曲线/设计盒对拍' $parityOk
}

# ==================================================================
Write-Host '=== Gate 3h: 字体子集覆盖(文案字符 <-> web/fonts/*.woff2 逐字对拍)==='
# ==================================================================
# [F12] web/fonts/README.md 早就写着「新增文案不重跑 fetch_fonts.py 就会上屏方块,而 CI 查不出」。
# 那句话在 2026-08-17 → 08-25 之间被兑现:i18n.js 连改四批词条、子集一次没重跑,feature/v1
# 主线带着几百个无字形字符(含「卡箍」这种正经词条)一路合入,八道门禁没有一道看得见。
# 人审 PR diff 永远发现不了「这个新汉字字体里没有」—— 只有逐字比对字符集与 cmap 查得出。
#
# CI 侧跑**同一条命令**:.github/workflows/format.yml 的 docs-truth job(与 3f/3g 同档)。
# 那边由 setup-python 钉版本 + pip 锁 fontTools/Brotli 补丁号;本地用开发机现装的。
#
# python 守卫同 Gate 3d:命令不存在时 PowerShell 抛 CommandNotFoundException 而**不更新**
# $LASTEXITCODE,它会保留上一条外部命令的 0,于是「一次没跑」被判成绿。
# 脚本自身也不静默:缺 fontTools/brotli 会以非零退出并打安装命令,不会假绿跳过。
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Write-Host '  python 未找到(check-font-coverage.py 需要,另需 fontTools + brotli)' -ForegroundColor Red
  Set-Gate '3h 字体子集覆盖' $false
}
else {
  $fontOut = (& python scripts\check-font-coverage.py 2>&1)
  $fontOk = ($LASTEXITCODE -eq 0)
  # 绿时也透出 [INFO](按设计走字体栈回退、以及上游家族本身没有的字形),
  # 免得那张白名单悄悄变长而无人过问;红时打全量,缺字逐个带码位。
  if (-not $fontOk) { $fontOut | ForEach-Object { Write-Host ("  " + $_) } }
  else { $fontOut | Select-String -Pattern '\[INFO\]|—' | ForEach-Object { Write-Host ("  " + $_) } }
  Set-Gate '3h 字体子集覆盖' $fontOk
}

# ==================================================================
Write-Host '=== Gate 3k: 字体保留名(woff2 name 表 <-> OFL-1.1 §3 RFN 断言)==='
# ==================================================================
# [SL-267] 与 3h 同吃 web/fonts/*.woff2,但问的是**另一个问题**:3h 问「字够不够」,
# 3k 问「名字能不能用」。OFL-1.1 §3 禁止 Modified Version 使用上游保留字体名(RFN),
# 而子集化就是修改;IBM Plex 两款的 RFN 是 "Plex" 一词本身,仓库带着这个违规转了 public。
# 判据落在字体 `name` 表而非文件名:文件改叫 ScvbSans.woff2 而 name 表里仍写 "IBM Plex Sans"
# 的话,装进系统字体册 / DevTools 字体面板 / PDF 导出读到的依然是上游名 —— 违规照旧,
# 而 diff 看着已经改完了。woff2 是 brotli 压缩的,grep 二进制不命中不等于名字已清除。
# 违规面还有第三处:进包的 CSS/JS 里的 `@font-face` family 与字体栈字面量 —— 只守 woff2 的话,
# 把 family 改回上游名而字体一字不动,解表照样全绿,而分发出去的 CSS 又在呈现 RFN。
# 故本门禁同时扫 web/ 下的 .css/.js/.html(vendored 的 web/js/juce/ 除外),
# 判据只落在字体名上下文(font-family: / font: 简写 / 驼峰 fontFamily / --ff-* 变量 /
# 含通用族关键字的字符串字面量含模板串 / src: local(...) 里的家族名),
# 不是整文件 grep —— RFN "Source" 是常用词,整文件扫会刷出几十条假红。
#
# **先自检再扫**,理由与 3j 逐字同款:本门禁的失效模式是「静默放行」——
# 署名豁免表被放宽、或子串比对被写成相等比对之后,扫描照样退 0。自检用仓内真字体就地
# 合成坏样例,验证「漏改的呈现名必红 / 署名记录不误伤 / 未登记字体必红」三档确实成立。
# python 守卫同 Gate 3d/3h:命令不存在时 $LASTEXITCODE 会保留上一条外部命令的 0。
# 与 .github/workflows/compliance.yml 的两步逐字同参。
$fontNameOk = $false
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Write-Host '  python 未找到(check-font-names.py 需要,另需 fontTools + brotli)' -ForegroundColor Red
}
else {
  $fnSelf = (& python scripts\check-font-names.py --self-test 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $fnSelf | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Red }
    Write-Host '  自检失败 = 门禁本身坏了(不是字体里有 RFN)' -ForegroundColor Red
  }
  else {
    $fnOut = (& python scripts\check-font-names.py 2>&1)
    $fontNameOk = ($LASTEXITCODE -eq 0)
    if ($fontNameOk) { $fnOut | Select-Object -Last 1 | ForEach-Object { Write-Host ("  " + $_) } }
    else { $fnOut | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Red } }
  }
}
Set-Gate '3k 字体保留名' $fontNameOk

# ==================================================================
# ---- gate 4/5 前置守卫:cmake 必须真实可执行 ----
# 不设守卫的后果不是「报错」,是**假绿**:外部命令不存在时 PowerShell 抛
# command-not-found,但 $LASTEXITCODE 保留上一条命令的旧值(通常 0),
# 于是 Set-Gate ($LASTEXITCODE -eq 0) 判 PASS —— 构建与测试一次没跑却全绿。
# 同族教训见 node(Gate 3e)/ gitleaks(Gate 3b)的 Get-Command 守卫。
# [SL-277] ctest 的守卫已随 gate 6 一起挪到下面的持锁段里 —— 拆锁后 gate 6 不再
# 与 gate 4/5 同处一个 if/else,守卫也得跟着走,否则「ctest 不在 PATH」这条会漏判。
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$ctestCmd = Get-Command ctest -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
  Write-Host '  cmake 不在 PATH —— gate 4/5 无法执行(不是跳过,是判负:工具缺失不得计为通过)' -ForegroundColor Red
  Write-Host '  提示:仓库自带 cmake 在 ..\tools\cmake-*-windows-x86_64\bin,加进 PATH 后重跑。' -ForegroundColor Yellow
  Set-Gate '4 配置' $false
  Set-Gate '5 构建' $false
}
else {

# ==================================================================
Write-Host ('=== Gate 4: 配置 (BuildDir={0}) ===' -f $BuildDir)
# ==================================================================
$cfgArgs = @('-S', '.', '-B', $BuildDir, "-DCMAKE_BUILD_TYPE=$Config", '-DSCVB_BUILD_TESTS=ON', "-DJUCE_PATH=$JucePath")
if ($Generator) {
  $cfgArgs = @('-G', $Generator) + $cfgArgs
  # 与 CI 的 configure 逐字对齐:多配置 Ninja 下只生成 Release 一档。
  if ($Generator -like 'Ninja Multi-Config*') { $cfgArgs += "-DCMAKE_CONFIGURATION_TYPES=$Config" }
  Write-Host ("  生成器:{0}(显式指定)" -f $Generator) -ForegroundColor Cyan
}
else {
  # [SL-277] 本地与 CI 的生成器**不同**,这不是等价关系,别当成等价的用:
  # CI 是 Ninja Multi-Config + sccache,本地默认是 Visual Studio 生成器。
  # 两者会在不同的地方红(add_custom_command 隐式依赖漏声明、生成物时序、PCH 行为),
  # 而 push→feature/** 的 CI 触发已在 [J96] 撤掉 —— Ninja 侧的错第一次被看见的时刻
  # 就是出包前那次 dispatch。改到构建系统的 PR 请打 `ci:full`,或在 Developer
  # Command Prompt 里跑 `-Generator "Ninja Multi-Config"` 先自己对一遍。
  Write-Host '  生成器:CMake 默认(CI 用的是 Ninja Multi-Config,二者不等价;见 CLAUDE.md §2)' -ForegroundColor DarkGray
}
$cfg = (& cmake @cfgArgs 2>&1)
if ($LASTEXITCODE -ne 0) { $cfg | ForEach-Object { Write-Host ("  " + $_) } }
Set-Gate '4 配置' ($LASTEXITCODE -eq 0)

# ==================================================================
Write-Host '=== Gate 5: 构建(/W4 零 warning)==='
# ==================================================================
$log = Join-Path $BuildDir 'build.log'
$build = (& cmake --build $BuildDir --config $Config --parallel 2>&1 | Tee-Object -FilePath $log)
$buildOk = ($LASTEXITCODE -eq 0)
$w = @()
if (Test-Path $log) {
  $w = @(Select-String -Path $log -Pattern 'warning C' | Where-Object { $_.Line -notmatch 'JUCE' -and $_.Line -notmatch '_deps' -and $_.Line -notmatch 'vcpkg' })
}
if (-not $buildOk) {
  Write-Host '  构建失败:' -ForegroundColor Red
  ($build | Select-Object -Last 30) | ForEach-Object { Write-Host ("  " + $_) }
}
if ($w.Count -gt 0) {
  Write-Host ("  MSVC 告警 {0} 条(ADR-011 要求 /W4 零 warning):" -f $w.Count) -ForegroundColor Red
  $w | ForEach-Object { Write-Host ("  " + $_.Line) }
  $buildOk = $false
}
Set-Gate '5 构建' $buildOk

} # end gate 4/5 守卫 else

# ==================================================================
# ===== 持锁段起点:gate 6 / 7 / 8 全程持 IPC 测试锁([SL-277]/[J96] 拆锁)=====
# 锁**只包这一段**。gate 4(configure)/ 5(build)在上面已经跑完并释放,理由见
# Enter-ScvbIpcLock 的头注。
# ==================================================================
$ipcLock = Enter-ScvbIpcLock
# 拿不到锁(建不出互斥体 / 等超时,两种都已在 Enter-ScvbIpcLock 里判负)时**不跑**
# 6/7/8,而不是无锁硬跑一遍(PR#176 复审采纳)。本进程反正注定 exit 1,自己没损失;
# 有损失的是**隔壁那个正老老实实持着锁跑的 agent** —— 它会被这一份无锁的 ipc 套件抢走
# 共享内存段、收到一个假红,而它自己的日志里什么异常都看不到。那正是本卡要根除的
# 失效类的镜像版(「以为别人有锁,其实没有」)。
# `-NoIpcLock` 不受影响:那是用户显式声明「本机没有第二个 agent」,$ipcLock 为 $null
# 是预期的,所以这里要 `-or $NoIpcLock` 而不是只判 $null。
$ipcLockOk = ($null -ne $ipcLock) -or $NoIpcLock
try {

if (-not $ipcLockOk) {
  Write-Host '=== Gate 6/7/8: 跳过执行,直接判负(没有 IPC 测试锁 = 没有并发保护)===' -ForegroundColor Red
  Set-Gate '6 ctest' $false
  # 档位照旧:`-Quick` / `-PluginOnly` 本来就不跑的那几道仍记 SKIP,不要因为锁的问题
  # 把它们写成 FAIL —— 汇总表要如实说「这一道压根没安排跑」还是「安排了但不可信」。
  # 判负的力度不受影响:锁那一行与 gate 6 已经让整条 gates 以 1 退出。
  if ($Quick) { Set-Skip '7 pluginval 非 GUI' } else { Set-Gate '7 pluginval 非 GUI' $false }
  if ($Quick -or $PluginOnly) { Set-Skip '8 pluginval 全量含 GUI' } else { Set-Gate '8 pluginval 全量含 GUI' $false }
}
else {

  # ==================================================================
  Write-Host '=== Gate 6: ctest ==='
  # ==================================================================
  if (-not $cmakeCmd -or -not $ctestCmd) {
    Write-Host '  cmake / ctest 不在 PATH —— gate 6 无法执行(不是跳过,是判负:工具缺失不得计为通过)' -ForegroundColor Red
    Write-Host '  提示:仓库自带 cmake 在 ..\tools\cmake-*-windows-x86_64\bin,加进 PATH 后重跑。' -ForegroundColor Yellow
    Set-Gate '6 ctest' $false
  }
  else {
    $ct = (& ctest --test-dir $BuildDir -C $Config --output-on-failure --no-tests=error 2>&1)
    if ($LASTEXITCODE -ne 0) { $ct | Select-Object -Last 40 | ForEach-Object { Write-Host ("  " + $_) } }
    Set-Gate '6 ctest' ($LASTEXITCODE -eq 0)
  }

# ==================================================================
# Gate 7: pluginval 非 GUI(与 CI 等价,06 §3.1)
# ==================================================================
if ($Quick) {
  Set-Skip '7 pluginval 非 GUI'
}
else {
  Write-Host '=== Gate 7: pluginval 非 GUI (strict 5) ==='
  if (-not $PluginvalExe -or -not (Test-Path $PluginvalExe)) {
    Write-Host '  pluginval 未找到' -ForegroundColor Red
    Set-Gate '7 pluginval 非 GUI' $false
  }
  else {
    $logDir = Join-Path $BuildDir 'pluginval-logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    # [issue #24] 只统计正式插件的三个 bundle([J75] 增 Monitor);其它 spike(s2/s3)共享构建目录的产物不再干扰计数。
    # [issue #24] 再按路径收窄到 src/input、src/output、src/monitor(生产插件目录,同名 bundle 不重复计数)。
    $bundles = @(Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vst3' -Directory | Where-Object { $_.Name -in @('SCVB Input.vst3', 'SCVB Output.vst3', 'SCVB Monitor.vst3') -and $_.FullName -match '[\\/]src[\\/](input|output|monitor)[\\/]' })
    $pv = $true
    if ($bundles.Count -ne 3) {
      Write-Host ("  期望 3 个 .vst3 bundle(SCVB Input / SCVB Output / SCVB Monitor),实际 {0} 个" -f $bundles.Count) -ForegroundColor Red
      $pv = $false
    }
    else {
      foreach ($b in $bundles) {
        & $PluginvalExe --strictness-level 5 --timeout-ms 60000 --skip-gui-tests --output-dir $logDir $b.FullName 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) {
          Write-Host ("  pluginval FAIL: {0}" -f $b.Name) -ForegroundColor Red
          $pv = $false
        }
        else { Write-Host ("  pluginval PASS: {0}" -f $b.Name) -ForegroundColor Green }
      }
    }
    Set-Gate '7 pluginval 非 GUI' $pv
  }
}

# ==================================================================
# Gate 8: pluginval 全量含 GUI(本地真机;全局互斥,06 §5.1)
# ==================================================================
if ($Quick -or $PluginOnly) {
  Set-Skip '8 pluginval 全量含 GUI'
}
else {
  Write-Host '=== Gate 8: pluginval 全量含 GUI (strict 5) ==='
  if (-not $PluginvalExe -or -not (Test-Path $PluginvalExe)) {
    Write-Host '  pluginval 未找到' -ForegroundColor Red
    Set-Gate '8 pluginval 全量含 GUI' $false
  }
  else {
    # [SL-277] 这把 GUI 专用互斥体保留:经 gates.ps1 的路径此时已经持着外层的
    # SCVB-ipc-tests(两把锁的获取顺序全脚本唯一 = 先 ipc 后 gui,不会死锁),
    # 但**不经 gates.ps1** 手跑 GUI pluginval 的场景只认得这一把,去掉就没保护了。
    #
    # PR#176 复审两处采纳:
    #  ① 原来是裸 `New-Object`,没有任何守卫。脚本顶部是 `$ErrorActionPreference =
    #     'Continue'`,所以构造失败时不中止:`$mutex` 留 $null → `$mutex.WaitOne()`
    #     报错继续 → `$pv` 保持 $true → pluginval 整段可能一次没跑,最后判 **PASS**。
    #     这就是 gate 4/5 守卫注释里反复说的那种**假绿**,只是从「$LASTEXITCODE 陈旧」
    #     换成了「在 $null 上调方法」。现在走 New-ScvbMutex,建不出来直接判负。
    #  ② 作用域由 `Global\` 改 `Local\`,与 IPC 锁同一档 —— 理由见 New-ScvbMutex 头注
    #     (提权终端先建 Global 会让普通终端拿不到,于是各持一把)。
    #     **过渡期注意**:本 PR 合并前,别的 worktree 里还是旧脚本(用 `Global\`),
    #     那期间两侧不互斥;gate 8 只在 feature→dev 收口跑,窗口很短,合并后即消失。
    #  ③ 等锁超时与建不出来同一档处理:`-and` 在 PowerShell 里短路,所以 $mutex 为
    #     $null 时不会去调 Wait-ScvbMutex。拿不到就判负、不跑 —— 与外层 IPC 锁
    #     ($ipcLockOk)一个道理:无锁硬跑会把隔壁 agent 的 GUI 会话搅了。
    $mutex = New-ScvbMutex -Name 'SCVB-pluginval-gui' -Tag 'gui-lock'
    $guiLockOk = ($null -ne $mutex) -and (Wait-ScvbMutex -Mutex $mutex -Name 'SCVB-pluginval-gui' -Tag 'gui-lock')
    if (-not $guiLockOk) {
      # 判负,**不是** return:本段处在脚本级 try/finally 里,`return` 会直接结束整个
      # 脚本 —— finally 照跑、但汇总表与 `exit 1` 全被跳过,进程以 0 退出 = 又一种假绿。
      if ($null -ne $mutex) { $mutex.Dispose() }   # 超时路径:句柄没拿到锁,不能 ReleaseMutex
      Set-Gate '8 pluginval 全量含 GUI' $false
    }
    else {
      $pv = $true
      try {
        $logDir = Join-Path $BuildDir 'pluginval-gui-logs'
        New-Item -ItemType Directory -Force $logDir | Out-Null
        # [issue #24] 再按路径收窄到 src/input、src/output、src/monitor(生产插件目录,同名 bundle 不重复计数)。
        $bundles = @(Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vst3' -Directory | Where-Object { $_.Name -in @('SCVB Input.vst3', 'SCVB Output.vst3', 'SCVB Monitor.vst3') -and $_.FullName -match '[\\/]src[\\/](input|output|monitor)[\\/]' })
        if ($bundles.Count -ne 3) {
          Write-Host ("  期望 3 个 .vst3 bundle,实际 {0} 个" -f $bundles.Count) -ForegroundColor Red
          $pv = $false
        }
        else {
          foreach ($b in $bundles) {
            & $PluginvalExe --strictness-level 5 --timeout-ms 60000 --output-dir $logDir $b.FullName 2>&1 | Out-Host
            if ($LASTEXITCODE -ne 0) {
              Write-Host ("  pluginval FAIL: {0}" -f $b.Name) -ForegroundColor Red
              $pv = $false
            }
            else { Write-Host ("  pluginval PASS: {0}" -f $b.Name) -ForegroundColor Green }
          }
        }
      }
      finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
      }
      Set-Gate '8 pluginval 全量含 GUI' $pv
    }
  }
}

} # end IPC 锁守卫 else

}
finally {
  # 无论 gate 6/7/8 里哪一步抛异常都必须归还 —— 但即便这里没跑到,互斥体也会在
  # 进程退出时由内核释放(这正是不用目录锁的理由)。
  Exit-ScvbIpcLock $ipcLock
}
# ===== 持锁段终点 =====

# ==================================================================
# 汇总(可直接粘进 PR 描述的表格)
# ==================================================================
Write-Host ''
Write-Host '| gate | 结果 |'
Write-Host '|---|---|'
foreach ($r in $results) {
  Write-Host ("| {0} | {1} |" -f $r.Gate, $r.Result)
}

if ($script:fatal) {
  Write-Host ''
  Write-Host 'gates: FAIL(存在失败项)' -ForegroundColor Red
  exit 1
}
Write-Host ''
Write-Host 'gates: 全部 PASS' -ForegroundColor Green
exit 0