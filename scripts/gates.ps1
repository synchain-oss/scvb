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
  [SL-277/J96] + [SL-301] **锁纪律**:gate 1-5 里**只有 3e 的页面级冒烟那一趟持锁**,
  configure/build 完全不持锁(多个 agent 可以同时编译);
  gate 3e 与 gate 6/7/8 由本脚本自己用命名互斥体 `SCVB-ipc-tests` 全机串行
  ——**两段分别持、之间放开**([SL-301];3f..5 的 configure/build 不持锁,可并行)。
  调用方**不要**再在外面把整条 gates 包进目录锁 —— 那会把编译也串起来,正是本卡要拆掉的。
  等锁上界**由 3e 的段预算推出、不写字面量**(默认预算 480s ⇒ 30 分钟;见 `$script:ScvbMutexWaitMinutes`
  处的推导)。超时(或互斥体建不出来)→ 锁那一行判负,该段 **一概不执行**
  (本档位下本来要跑的记 FAIL、本来就跳过的仍记 SKIP),整条 gates 以 1 退出。
  绝不无锁硬跑 —— 无锁跑会去抢隔壁持锁 agent 的共享内存段,让那一侧收到查不出的假红。
  逃生口 `-NoIpcLock` 只在确认无并行 agent 时用。
.EXAMPLE   pwsh scripts/gates.ps1
.EXAMPLE   pwsh scripts/gates.ps1 -PluginOnly -BuildDir build-T15
#>
#Requires -Version 7.0
# 本脚本一直是 pwsh 7 跑的,但那是**隐含前提**;PR#182 复审建议显式化。两处依赖它:
#   · gate 3e 的 `$proc.Kill($true)`(连进程树)是 .NET Core 3.0+ 的重载;
#   · gate 3e 读重定向输出用的 `Get-Content` 默认 UTF-8 —— Windows PowerShell 5.1 下
#     默认 ANSI,捞出来给人看的中文 `[FAIL]` 行会是乱码。
# 与其在编码上打补丁,不如把前提写在门口。

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
  # [SL-277] 逃生口:确认本机没有第二个 agent 在跑 gate 3e / 6/7/8 时才用。
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

# 「哪个档位跑哪几道」的**单一真源**([SL-277] PR#176 复审采纳)。gate 7/8 的正常路径
# 与「拿不到 IPC 锁」的判负路径都读这两个变量 —— 各写各的 `if ($Quick …)` 就会在改档位时
# 只改一处,失效形态是汇总表把 SKIP/FAIL 记反:退出码不受影响,所以没人会立刻发现。
# gate 6(ctest)在所有档位都跑,不需要这样的开关。
$runGate7 = -not $Quick
$runGate8 = -not ($Quick -or $PluginOnly)

# ---- IPC 测试锁(两段:gate 3e 与 gate 6/7/8;[SL-277]/[J96] 拆锁、[SL-301] 纳入 3e)----
# **为什么不包 4/5**:gate 4(configure)/ 5(build)只读写各自的 `-BuildDir`,
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

# [SL-301 复审] **「这一套起不起浏览器」只有一份判据**,两处共用(算等锁预算、实际分类)。
# 此前写了两遍同一条正则、靠人工同步 —— 漂了不会报错,只会让上界按错的套数算。
#
# 判据**有意取并集、偏向多判**,因为两种漂法的代价严重不对称:
#   · 多判(纯 node 被当成页面级)⇒ 多串行几十秒,**无害**;
#   · **漏判**(真起 Chrome 的被当成纯 node)⇒ **无锁跑起浏览器**,正是本卡要根除的形态,
#     而且**完全静默**。
# 所以宁可宽:`cdpConnect`(与 gate 3i 的 check-smoke-hygiene 同一判据,故本集合 ⊇ 它那一族)
# 或 `.on(`/`.once(` 挂 error 的浏览器句柄、或命令行里出现 `--headless`、或文件名 `-page.mjs`。
# 只认单一写法(比如只认 `chrome.on("error"`)会被 prettier 换引号、或新冒烟写成
# `browser.once('error'` 静默绕过 —— 那正是危险的那一侧。
function Test-ScvbPageSuite {
  param([string]$Path)
  if ($Path -like '*-page.mjs') { return $true }
  $t = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
  if ($null -eq $t) {
    # **读不到就当页面级**(复审指出:上一版这里 `return $false`,恰好倒向我自己在头注里
    # 点名的危险侧 —— 读不到文件时把它判成纯 node ⇒ **无锁跑起浏览器**,而且完全静默)。
    # 兜底必须与判据同向:多判只是多串几十秒,漏判是本卡白做。出声,别让它静默生效。
    Write-Host ("  [ipc-lock] 读不到 {0},保守当作页面级(持锁跑)" -f (Split-Path $Path -Leaf)) -ForegroundColor Yellow
    return $true
  }
  # 四条任一命中即算「起浏览器」。`cdpConnect` 与 gate 3i 的 check-smoke-hygiene 同判据,
  # 故本集合 ⊇ 它那一族;其余三条是往「宁可多判」偏的保险。
  if ($t -match 'function cdpConnect') { return $true }
  if ($t -match '--headless') { return $true }
  if ($t -match '\.(on|once)\(\s*["'']error') { return $true }
  return $false
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
# [SL-301] 等锁上界**由 3e 的实际预算推出来,不写字面量**。
#
# 为什么不能写死:等锁上界要盖住**持锁方最坏占住多久**,而那个量在本卡里改过两次口径,
# 每改一次,写死的常数就悄悄失配一次 —— 所以它必须是算出来的。
#
# 现在的口径(裁定 (a) 落地之后,**唯一有效的那个**):持锁方最坏就是 **3e 的段预算**
# (`$script:ScvbSmokeSegmentBudgetSec`,默认 480s)。到点即杀当前套、其余记 FAIL、立即放锁,
# 所以「页面级套数」与「每套上界 `SCVB_SMOKE_TIMEOUT_SEC`」**都不再决定持锁上限** —— 谁把
# 每套上界调到 600s 也撑不破这个封顶,因为先到的是预算。
#
# 于是:`ceil(段预算 × 1.5 / 60)`,再与 30 取大(6/7/8 段本身也要排队,历史持锁 9-10 分钟)。
# 默认 480s ⇒ ceil(12)=12,取大后 **30 分钟**,正是统筹裁定要留的值;把预算调大,上界自动跟上。
#
# (历史,仅供追溯、勿据以推理:(a) 之前这里算的是「页面级套数 × 每套上界」,
#  更早一版锁包着**整个 3e** 的 24 套、把 6×300s≈31 分钟错当上界写死了 45 —— 真值是
#  24×300s=120 分钟。两版口径都已被段预算取代。)
$script:ScvbSmokeSegmentBudgetSec = 480
if ($env:SCVB_SMOKE_SEGMENT_BUDGET_SEC) {
  $parsedSegBudget = 0
  if ([int]::TryParse($env:SCVB_SMOKE_SEGMENT_BUDGET_SEC, [ref]$parsedSegBudget) -and $parsedSegBudget -ge 10) {
    $script:ScvbSmokeSegmentBudgetSec = $parsedSegBudget
  }
  else {
    Write-Host ("  [WARN] SCVB_SMOKE_SEGMENT_BUDGET_SEC='{0}' 不是 >=10 的整数,回落到默认 480s" -f $env:SCVB_SMOKE_SEGMENT_BUDGET_SEC) -ForegroundColor Yellow
  }
}
# 解析与段预算那份**同形**(先给出默认、再试解析、坏值出声回落),不要两套风格:
# 裸 `[int]$env:...` 在 `=abc` 时抛在脚本顶层、**整条 gates 当场挂**,而 `=0` / 负数会被
# 悄悄收下,把等锁上界压成「几乎不等」—— 拿不到锁一律判负,于是并发时人人假红。
# `[ref]` 只能作用在**已存在**的变量上,所以这一行的 `= 0` 不是多余的初始化:
# 去掉它,设了这个环境变量的那一跑会以「[ref] cannot be applied…」在顶层直接死掉。
$script:ScvbMutexWaitMinutes = [Math]::Max(30, [Math]::Ceiling($script:ScvbSmokeSegmentBudgetSec * 1.5 / 60))
if ($env:SCVB_MUTEX_WAIT_MINUTES) {
  $parsedWaitMin = 0
  if ([int]::TryParse($env:SCVB_MUTEX_WAIT_MINUTES, [ref]$parsedWaitMin) -and $parsedWaitMin -ge 1) {
    $script:ScvbMutexWaitMinutes = $parsedWaitMin
  }
  else {
    Write-Host ("  [WARN] SCVB_MUTEX_WAIT_MINUTES='{0}' 不是 >=1 的整数,回落到推导值 {1} 分钟" -f $env:SCVB_MUTEX_WAIT_MINUTES, $script:ScvbMutexWaitMinutes) -ForegroundColor Yellow
  }
}

function Wait-ScvbMutex {
  param($Mutex, [string]$Name, [string]$Tag, [int]$TimeoutMinutes = $script:ScvbMutexWaitMinutes)
  # 时间戳带毫秒:并发验证时,「谁在什么时刻拿到/放开」这条证据只能来自进程**内部**的
  # 时钟。外面用管道加时间戳靠不住 —— pwsh 往管道写是块缓冲的,读到的时刻会晚于打印
  # 时刻,两条流的偏移量还不一样,拿它对拍会看出根本不存在的重叠。
  Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 等待 Local\{2}(上界 {3} 分钟)..." -f $Tag, (Get-Date).ToUniversalTime(), $Name, $TimeoutMinutes) -ForegroundColor Yellow
  # [SL-301] 排队要**显形**:只打「等待…」「已获得」两行,读日志的人看不出等了多久 ——
  # 而本卡把 3e 也纳入互斥面之后,排队会变成常态。等了 0 秒和等了 8 分钟必须一眼可分,
  # 否则就是本仓治了一整轮的那个形态(降级/排队发生了,但摘要里看不见)。
  # **拿不到持有者身份**:命名互斥体是内核对象,没有 owner 信息;要给出「持有者 X」
  # 得再引一个 owner 文件,那正是判例里实伤过人的目录锁形态,不做。所以只报时长。
  $waitSw = [System.Diagnostics.Stopwatch]::StartNew()
  $got = $false
  try { $got = $Mutex.WaitOne([TimeSpan]::FromMinutes($TimeoutMinutes)) }
  catch [System.Threading.AbandonedMutexException] {
    # 前一个持有者进程异常退出。锁已经归我们了,只是说明上一次跑得不干净。
    $got = $true
    Write-Host ("  [{0}] 前一持有者异常退出(AbandonedMutex),已接管" -f $Tag) -ForegroundColor Yellow
  }
  if (-not $got) {
    $waitSw.Stop()
    Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 等待 Local\{2} 超过 {3} 分钟仍未获得(实等 {4:N1} 秒),判负" -f $Tag, (Get-Date).ToUniversalTime(), $Name, $TimeoutMinutes, $waitSw.Elapsed.TotalSeconds) -ForegroundColor Red
    Write-Host ("  [{0}] 提示:多半有卡死的 pluginval / ctest 进程还占着锁,查一下再重跑。" -f $Tag) -ForegroundColor Yellow
    # [SL-301 裁定] 持锁方也可能在 3e 的页面级段(它现在也持这把锁)。等锁方的日志里
    # 看不到对方是谁,所以要指路 —— 否则又变成「查不到原因在别人的 web smoke 上」。
    Write-Host ("  [{0}] 也可能是持锁方正在 3e 的页面级段:看它日志里的 `[ipc-lock] … gate 3e(页面级)` 行" -f $Tag) -ForegroundColor Yellow
    return $false
  }
  $waitSw.Stop()
  Write-Host ("  [{0}] {1:HH:mm:ss.fff}Z 已获得 Local\{2}(等锁 {3:N1} 秒)" -f $Tag, (Get-Date).ToUniversalTime(), $Name, $waitSw.Elapsed.TotalSeconds) -ForegroundColor Green
  return $true
}

# [SL-301] 参数化段名:这把锁现在有**两个**持锁段(3e 与 6/7/8),判负信息必须说清
# 是**哪一段**没拿到锁 —— 否则汇总里只看到一条「IPC 测试锁」判负,读的人不知道
# 是 web smoke 那段还是 ctest 那段没跑。
function Enter-ScvbIpcLock {
  param([string]$Segment = 'gate 6/7/8')
  if ($NoIpcLock) {
    Write-Host ("  [ipc-lock] -NoIpcLock:跳过 IPC 测试锁({0};仅限确认无并行 agent 时)" -f $Segment) -ForegroundColor Yellow
    return $null
  }
  $m = New-ScvbMutex -Name 'SCVB-ipc-tests' -Tag 'ipc-lock'
  if ($null -eq $m) {
    # 判负而不是静默放行:拿不到锁就等于没有并发保护,该段的结果不可信。
    Set-Gate ("IPC 测试锁({0} 互斥)" -f $Segment) $false
    return $null
  }
  if (-not (Wait-ScvbMutex -Mutex $m -Name 'SCVB-ipc-tests' -Tag 'ipc-lock')) {
    # 等超时 = 同样没有并发保护,与建不出来同一档处理。句柄没拿到锁,直接 Dispose。
    Set-Gate ("IPC 测试锁({0} 互斥)" -f $Segment) $false
    $m.Dispose()
    return $null
  }
  return $m
}

function Exit-ScvbIpcLock {
  param($Mutex, [string]$Segment = 'gate 6/7/8')
  if ($null -eq $Mutex) { return }
  try { $Mutex.ReleaseMutex() } catch { Write-Host ("  [ipc-lock] 释放异常:{0}" -f $_.Exception.Message) -ForegroundColor Yellow }
  $Mutex.Dispose()
  Write-Host ("  [ipc-lock] {0:HH:mm:ss.fff}Z 已释放({1})" -f (Get-Date).ToUniversalTime(), $Segment) -ForegroundColor Green
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
# ===== 持锁段①起点:gate 3e **页面级冒烟**持 IPC 测试锁(惰性取锁,[SL-301])=====
# ==================================================================
# **为什么 3e 也要这把锁**([SL-301] 实测):3e 起 6 个无头 Chrome(单跑峰值 15 个进程、
# 吃掉约 11 个核),而这份负载会**把同机另一个 agent 的 gate 6 拖红** —— 实测一次:
# `scvb_ipc_tests` 的「15 claimer 抢同一 channel ⇒ 恰好 1 个成功」在对方 3e 跑着时判红,
# 而那个 PR 只动 `tests/host/`。互斥体原来只串行化 6/7/8,**挡得住别人的 ctest、挡不住 3e**。
# 形态与真回归不可分,所以这不是「重试能兜住」的一档。
#
# **为什么分两段持、不是一路持到 gate 8**:取锁点前移到 3e 之后,若一路持到 8,
# `4 configure` / `5 build` 就一起进了锁 —— 实测持锁从 **9-10 分钟涨到约 36 分钟**,
# 而多出来的 22 分钟全是 build。要的不变式是「全机任一时刻只有一份 3e **或**一份 6/7/8」,
# 两段持锁**同样满足**(两段都要这把锁),而 build 保持并行。观测到的伤害是 3e↔gate 6,
# 不是 3e↔build:build 没有时序断言。
#
# 拿不到锁的处置与 6/7/8 **逐字同款**:判负 + 不执行。理由也同款 —— 无锁硬跑就是去抢
# 隔壁持锁 agent 的资源,让那一侧收到一个自己日志里查不到原因的假红。
# 惰性取锁:实际的 Enter 在下面循环里「跑到第一套页面级之前」才做,
# 于是纯 node 的那 18 套无锁先跑、可与别的 agent 并行。这三个变量是段内状态。
$script:SmokeLock = $null
$script:SmokeLockTaken = $false
$script:SmokeLockOk = $true   # 没到页面级就一直是「不需要锁」
$script:SmokeSegSw = $null    # 持锁段计时,取到锁那一刻才起
# 段预算只解析一份(见脚本顶部 `$script:ScvbSmokeSegmentBudgetSec` 的头注),这里直接用 ——
# 两份解析漂了会让「等锁上界」与「实际预算」脱钩,而且上一版两份的行为还不一致(一份出声一份静默)。
$smokeSegBudgetSec = $script:ScvbSmokeSegmentBudgetSec
$smokeBudgetHit = $false
$smokeNoLock = $false

# ==================================================================
Write-Host '=== Gate 3e: web smoke(web-preview/tests/*.mjs)==='
# ==================================================================
# web-preview/tests 是 UI 侧**唯一**的行为门禁(纯函数 + mock 端到端 + 源码级
# 纪律断言),T31–T36 六张卡的回归保护全压在上面。与 .github/workflows/format.yml
# 的 web-smoke job 同口径。零依赖:不装 npm 包直接 node 跑;每套退出码 0 = 全绿,
# 非 0 会逐条打印 [FAIL]。**单套判红不 break**,一次跑完看全所有红项;整段中止只有
# 两个出口,都不是「某一套红了」:拿不到 IPC 锁、以及 [SL-301] 的页面级段预算见底。
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
try {

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
  # 退出码约定(T46 起,[SL-297] 增第三档):0 = 全绿;1 = 有断言失败;
  # **2 = 缺可选外部依赖(本机没有浏览器)**;**3 = 浏览器在,但这一次没起来 / 没连上**。
  # 2 与 3 都**不判红**,区别只在摘要里怎么显形:2 = `[SKIP]`,3 = `[FLAKY-SKIP]` + 单独计数。
  # 分这两档的代价是实测出来的:压成一个码时,一台装着 Chrome 的机器上一次瞬时超时会让
  # 整套判据无声消失,而汇总行照写全 PASS(SL-293 多轮 gates 撞到三次,掉的套件每次不同)。
  # 会回 2 的是**六套**页面级冒烟(都要一个无头 Chrome/Edge):smoke-monitor-page.mjs、
  # smoke-output-stale-page.mjs(SL-177 过期提示,04 §4.5)、smoke-output-dist-page.mjs
  # (SL-203 分布图补间)、smoke-seg-restore-page.mjs(SL-242 段级「恢复自动」的作用域)、
  # smoke-ui-layout-page.mjs(SL-272/275/276/273 header 几何、建议表留白、重分析弹窗)
  # 与 smoke-seg-diff-fold-page.mjs(SL-274 diff 摘要折叠 + 泳道保底)。
  # 执行面按 glob 自动收,加一套不必改这里 —— 这行只是给人读的。
  # 为什么单列一档:
  # 把「本机没装浏览器」和「页面真的坏了」都判成红,等于逼每个只改 C++ 的人装浏览器,
  # 或者反过来诱导谁把这套从门禁里摘掉 —— 两条都比一条 SKIP 差。**但绝不静默**:
  # 打印 SKIP 行并计数,总结里带上,免得「一套没跑」看起来和「跑过了」一样。
  # ⚠ **每套都要有整体超时**([SL-287])。原来这里是裸的 `(& node $f.FullName 2>&1)` ——
  # 一套挂死,gate 3e 就停在那儿不动 —— SL-274 实测过一次:75 分钟零输出,
  # node 与 Chrome 都还活着。
  # ⚠ **[SL-301] 这段因果已经变了,别照旧说法推断锁的作用域。** SL-277 拆锁之后曾有一段
  # 时间 gate 1–5 完全不持锁,那时「一套挂死」的代价收窄成「**本轮** gates 停死」、不连累别人。
  # **现在 3e 自己持锁**(它的 Chrome 负载会把同机别人的 gate 6 拖红),所以「一套挂死」
  # **重新会堵住全场** —— 这条超时因此比那时更要紧,不是「照加」而是**承重**。
  # 最坏账现在由**段预算**封顶,不再是「套数 × 每套上界」:见 `$smokeSegBudgetSec` 的头注
  # (默认 480s)。别人可能等锁的上限 = 那个预算,而不是这条 300s 乘出来的数。
  #(此处原先写「6 × 300s ≈ 31 分钟、上界从 30 抬到 45」—— 两句都随裁定 (a) 作废:
  # 上界留 30 且改成跟着预算算,持锁方由预算封顶。**引章节名不引行号**:行号一改就漂,
  # 这正是上一轮被点名的那个失效形态。)
  # 页面级冒烟内部现在有 CDP 截止时间兜住「响应不回来」那一类,但兜不住「Chrome 根本没起来」
  # 「WebSocket 没连上」「页面永不 load」——那些卡在 CDP 之外,只有这一层能收。
  #
  # 上界取 300s:实测最慢的一套(seg-diff-fold)健康时跑 57s,其余 5–34s,**5 倍余量**;
  # 而代价上限从 75 分钟降到 5 分钟。
  # 超时后**判红并继续下一套**,不整段中止 —— 一次跑完看全所有红项。
  # (整段中止的两个出口在别处:拿不到锁、段预算见底;「某一套超时」从来不是其中之一。)
  # 用 Start-Process + WaitForExit 而不是 `&`:`&` 没有超时可言;`Kill($true)` 连子进程树
  # 一起收,否则被杀的只是 node,它起的 Chrome 会留下来(正是 SL-274 压住锁的那个形态)。
  $smokeOk = $true
  $smokeSkipped = 0
  $smokeFlaky = 0      # [SL-297] rc=3:浏览器在但没连上
  $smokeRanCount = 0   # [SL-301 复审] 实际跑到的套数(break 之后 < 总数,汇总不能说谎)
  $smokeHungByBudget = 0  # [SL-301 复审] `$smokeHung` 里属于「被段预算腰斩」的那部分
  $smokeHung = 0
  # `SCVB_SMOKE_TIMEOUT_SEC` 是给慢机器的口子(照 SCVB_MUTEX_WAIT_MINUTES 的先例),
  # 平时不用设。调大不会削弱任何判据 —— 超时只负责兜住挂死,不参与判对错。
  # **非法值要钳住**(PR#182 复审):`=abc` 会让 `[int]` 转换抛错、整条 gates 挂;
  # `=0` 或负数会让 `WaitForExit(0)` 立刻返回 false ⇒ **每一套都被判 [HUNG]**,
  # 而且看起来像真挂死。转换失败回默认值,并压一个 30s 下界。
  $smokeTimeoutSec = 300
  if ($env:SCVB_SMOKE_TIMEOUT_SEC) {
    $parsed = 0
    if ([int]::TryParse($env:SCVB_SMOKE_TIMEOUT_SEC, [ref]$parsed) -and $parsed -ge 30) {
      $smokeTimeoutSec = $parsed
    }
    else {
      Write-Host ("  [WARN] SCVB_SMOKE_TIMEOUT_SEC='{0}' 不是 >=30 的整数,回落到默认 300s" -f $env:SCVB_SMOKE_TIMEOUT_SEC) -ForegroundColor Yellow
    }
  }
  # [SL-301 复审] **持锁面收窄到真正起浏览器的那几套。**
  # 24 套里只有 6 套起无头 Chrome,另外 18 套是纯 node、零共享状态 —— 把它们也串行化
  # 是「持锁面比论据宽」:论据是「Chrome 负载拖红别人的 gate 6」,那 18 套一个 Chrome 都不起。
  # 分类判据见 `Test-ScvbPageSuite`(单一真源,有意取并集偏向多判 —— 漏判的代价是
  # 「无锁跑起浏览器」且完全静默,多判只是多串几十秒)。
  # 排序把纯 node 的排前面,**跑到第一套页面级时才取锁**,循环结束在 finally 里放 ——
  # 于是无锁那 18 套可以与别的 agent 并行。持锁段占住多久由**段预算**封顶
  # (`$script:ScvbSmokeSegmentBudgetSec`),不再是「套数 × 每套上界」——
  # 等锁上界正是从那个预算推出来的,两处口径同源。
  $pageSuites = @($smokeFiles | Where-Object { Test-ScvbPageSuite -Path $_.FullName })
  $pageNames = @($pageSuites | ForEach-Object { $_.Name })
  $smokeFiles = @($smokeFiles | Where-Object { $pageNames -notcontains $_.Name }) + $pageSuites
  Write-Host ("  [ipc-lock] 本轮 {0} 套里 {1} 套起浏览器(仅这几套持锁,其余 {2} 套无锁先跑)" -f $smokeFiles.Count, $pageSuites.Count, ($smokeFiles.Count - $pageSuites.Count)) -ForegroundColor Cyan
  # 判出 0 套页面级 ⇒ **整个 3e 无锁跑完**。四条判据取并集,归零几乎不可能,
  # 但「几乎不可能」正是不会有人盯着的那一格:真发生时唯一的痕迹是上面那行 Cyan,
  # 混在正常输出里没人会觉得不对。出声,与 `Test-ScvbPageSuite` 的兜底同向。
  $smokeZeroPage = ($pageSuites.Count -eq 0)
  if ($smokeZeroPage) {
    Write-Host '  [ipc-lock] ⚠ 判出 0 套页面级 —— 本轮 3e 将全程无锁。冒烟改名 / 判据失效都会长这样,先去核 Test-ScvbPageSuite 再信这个结果' -ForegroundColor Yellow
  }
  foreach ($f in $smokeFiles) {
    # 第一套页面级之前才取锁(惰性取锁);`$smokeLock` 在段外声明,finally 负责放。
    if (($pageNames -contains $f.Name) -and (-not $script:SmokeLockTaken)) {
      $script:SmokeLockTaken = $true
      $script:SmokeLock = Enter-ScvbIpcLock -Segment 'gate 3e(页面级)'
      $script:SmokeLockOk = ($null -ne $script:SmokeLock) -or $NoIpcLock
      if (-not $script:SmokeLockOk) {
        # 与 6/7/8 同款:拿不到锁 = 没有并发保护,页面级那几套**不执行**、整段判负。
        Write-Host '  [ipc-lock] 拿不到锁 ⇒ 页面级冒烟不执行(无锁硬跑会去抢隔壁持锁 agent 的机器)' -ForegroundColor Red
        $smokeOk = $false
        # 与超预算那条 break **同型,待遇要一致**:两条都让余套没跑,都必须在摘要里显形。
        # 复审点出上一版只有超预算进了标签 —— 同一个 commit 里两条同型路径不同待遇,
        # 正是「降级不可见」换个入口又回来。
        $smokeNoLock = $true
        break
      }
      # [SL-301 裁定(a)] **持锁段整体预算**从拿到锁的那一刻开始计。
      # ⚠ 只在**真的持着锁**时计:`-NoIpcLock` 下 `Enter` 返回 $null、根本没有锁,
      # 那就不存在「占着锁不放」这回事 —— 再用预算腰斩就是一条**凭空的假红**,
      # 而且超预算那行还会说一句不存在的「立即放锁」(复审点出)。
      if ($null -ne $script:SmokeLock) {
        $script:SmokeSegSw = [System.Diagnostics.Stopwatch]::StartNew()
      }
    }
    # [SL-301 裁定(a)] 预算兜的是**持锁方**,等锁上界兜的是**等锁方**,两者并存、不可互相替代:
    # 上界只保证「等的人不会被判负」,**不保证「等的人不用干等满」** —— 病态 3e 持锁
    # 6 × 每套上界时,全机其他 agent 照样干等。所以持锁方自己要有封顶。
    if ($script:SmokeLockTaken -and $script:SmokeLockOk -and $null -ne $script:SmokeSegSw) {
      $usedSec = [int]$script:SmokeSegSw.Elapsed.TotalSeconds
      if ($usedSec -ge $smokeSegBudgetSec) {
        # 预算已耗尽:**余套一律判负且不跑**,立刻跳出 —— 锁在 finally 里当场释放。
        Write-Host ("  [ipc-lock] 3e 页面级段超预算({0}s ≥ {1}s):余下的套不再执行,整段判负,立即放锁" -f $usedSec, $smokeSegBudgetSec) -ForegroundColor Red
        $smokeOk = $false
        $smokeBudgetHit = $true
        break
      }
    }
    $soPath = [System.IO.Path]::GetTempFileName()
    $sePath = [System.IO.Path]::GetTempFileName()
    # 路径要**自己加引号**(PR#182 复审):`Start-Process` 把 `-ArgumentList` 按空格拼成
    # 命令行、**不会**自动引号化,而旧写法 `& node $f.FullName` 是 PowerShell 直接传参、
    # 自带引号化。检出路径含空格时 node 会收到被拆词的路径,这一套直接跑不起来,
    # 报的还是「找不到文件」,不会有人想到是 gates 这一行的锅。
    $proc = Start-Process -FilePath $nodeCmd.Source -ArgumentList ('"{0}"' -f $f.FullName) `
      -NoNewWindow -PassThru -RedirectStandardOutput $soPath -RedirectStandardError $sePath
    # 单套的等待上界 = min(每套上界, 预算剩余)。这样「杀当前套」由同一条 WaitForExit 承担,
    # 不必另起一套超时机制;非持锁段(纯 node 那 18 套)不受预算约束,取值仍是每套上界。
    $waitSec = $smokeTimeoutSec
    if ($script:SmokeLockTaken -and $script:SmokeLockOk -and $null -ne $script:SmokeSegSw) {
      $remain = $smokeSegBudgetSec - [int]$script:SmokeSegSw.Elapsed.TotalSeconds
      if ($remain -lt $waitSec) { $waitSec = [Math]::Max(1, $remain) }
    }
    if ($proc.WaitForExit($waitSec * 1000)) {
      $rc = $proc.ExitCode
    }
    else {
      # 连进程树一起收:只杀 node 的话,它起的无头 Chrome 会活下来继续占资源。
      # `Kill($true)`(连进程树)是 .NET Core 3.0+ 的重载。回退到 `Kill()` 时**只杀 node,
      # 它起的 Chrome 会留下** —— 正是本段要根除的形态,所以回退必须出声,不能静默。
      try { $proc.Kill($true) }
      catch {
        Write-Host '         [WARN] 本机 pwsh 不支持 Kill($true)(需 .NET Core 3.0+),回退成只杀 node —— 它起的 Chrome 可能留下,手动查一下' -ForegroundColor Yellow
        try { $proc.Kill() } catch {}
      }
      # `Kill` 是**异步**的:句柄未必已关,紧跟着读重定向文件可能读到截断的输出 ——
      # 而这恰恰是最需要诊断信息的那条路径。等一个有界的短时,不会重新引入无限等。
      try { $null = $proc.WaitForExit(5000) } catch {}
      $rc = -1
      $smokeHung++
      $smokeOk = $false
      # 杀因要分清:**预算截断**与**真挂死**是两回事,而两者都走这条 kill 路径。
      # 只写「超时被杀」会让人去追一套其实没挂的冒烟(它只是被段预算腰斩了)。
      if ($waitSec -lt $smokeTimeoutSec) {
        Write-Host ("  [HUNG] {0}:被**段预算**腰斩(只等了 {1}s,每套上界是 {2}s)——它未必挂死,是 3e 页面级段的 {3}s 预算见底了" -f $f.Name, $waitSec, $smokeTimeoutSec, $smokeSegBudgetSec) -ForegroundColor Red
        # **在这里也置位**(复审第 5 轮):`$smokeBudgetHit` 原本只在循环**顶部**的预算检查里置真,
        # 而被腰斩的若是**最后一套**页面级,循环就此结束、那个检查再也不会执行 ⇒ 汇总把
        # 「预算被击穿」报成一次普通挂死。滚屏里说对了、摘要里说错了 —— 正是本卡在治的形态。
        $smokeBudgetHit = $true
        # 杀因分档要做全:并进 `$smokeHung` 一个数,摘要就只会写「N 套超时被杀」,
        # 与上面这行滚屏刚说的「它未必挂死」自相矛盾 —— 同一次运行里两个口径。
        $smokeHungByBudget++
      }
      else {
        Write-Host ("  [HUNG] {0}:超过 {1}s 未结束,已连进程树杀掉并判红" -f $f.Name, $smokeTimeoutSec) -ForegroundColor Red
        Write-Host '         (这一套内部的 CDP 截止时间没能兜住 ⇒ 多半卡在 CDP 之外:Chrome 没起来 / WebSocket 没连上 / 页面永不 load)' -ForegroundColor Yellow
      }
    }
    $out = @()
    foreach ($fp in @($soPath, $sePath)) {
      if (Test-Path $fp) { $out += (Get-Content -LiteralPath $fp -ErrorAction SilentlyContinue) }
    }
    Remove-Item -LiteralPath $soPath, $sePath -Force -ErrorAction SilentlyContinue
    $smokeRanCount++
    if ($rc -eq 2) {
      $smokeSkipped++
      Write-Host ("  [SKIP] {0}:缺可选外部依赖(见该脚本文件头)" -f $f.Name) -ForegroundColor Yellow
      $out | Select-String -Pattern '^❌' | Select-Object -First 2 |
      ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
    }
    elseif ($rc -eq 3) {
      # [SL-297] **浏览器在、这一次没跑成**。与 rc=2 分开的理由见那一族脚本的
      # `browserFailed()`:压成同一个码时,一台**装着 Chrome** 的机器上一次瞬时超时
      # 会让整套判据无声消失,而汇总行照写全 PASS(SL-293 多轮 gates 实测撞到三次,
      # 每次掉的套件还不一样,两套单独重跑都全绿 —— 丢的是运行机会,不是代码)。
      # **判定不变**:与 rc=2 一样不判红。把超时直接改成硬红在当前抖动率下会卡住所有人,
      # 那是第二步(重试/退避)的事,不在本卡。这里只负责让它**在摘要里显形**。
      $smokeFlaky++
      Write-Host ("  [FLAKY-SKIP] {0}:浏览器在,但这一次没起来 / 没连上 —— **本套没跑成**,不是缺依赖" -f $f.Name) -ForegroundColor Yellow
      $out | Select-String -Pattern '^❌' | Select-Object -First 2 |
      ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
    }
    elseif ($rc -ne 0) {
      $smokeOk = $false
      Write-Host ("  {0}:" -f $f.Name) -ForegroundColor Red
      # 捞 `[FATAL]` 与 `[FAIL]` 两种([SL-287])。页面级冒烟的 uncaughtException /
      # unhandledRejection 处理器打的是 `[FATAL]`,只捞 `[FAIL]` 的话「脚本自己炸了」
      # 会一行不显示 —— 而 [J96] 之后本机 gates 是子 PR 上唯一的编译/行为门,
      # 致命错误不进摘要等于没报。
      $out | Select-String -Pattern '\[FAIL\]|\[FATAL\]' | Select-Object -First 20 |
      ForEach-Object { Write-Host ("  " + $_) }
    }
  }
  # [SL-301 复审] **汇总不能说谎**:拿不到锁或超预算时会 `break`,实际跑到的套数 < 总数,
  # 而原来无论如何都写「24 套」—— 与本 gate 自己「降级要显形」的口径冲突。
  # 跑满时保持原样(不给正常路径添噪声),没跑满才写成 `N/总数 套跑到`。
  $smokeCountText =
    if ($smokeRanCount -lt $smokeFiles.Count) { '{0}/{1} 套跑到' -f $smokeRanCount, $smokeFiles.Count }
    else { '{0} 套' -f $smokeFiles.Count }
  $smokeLabel = '3e web smoke({0},node {1})' -f $smokeCountText, $nv
  if ($smokeSkipped -gt 0) {
    $smokeLabel = '3e web smoke({0} −{1} SKIP,node {2})' -f $smokeCountText, $smokeSkipped, $nv
  }
  # [SL-297] FLAKY-SKIP **必须进汇总标签**:跑完 gates 的人看的是这张表,不是往回滚屏。
  # 这一条正是本卡要堵的洞 —— 只在滚屏里打一行 `[FLAKY-SKIP]`、汇总仍写「24 套」,
  # 等于「没跑成」长得和「跑过了」一模一样。`!` 前缀与 [HUNG] 同款,一眼能看出是异常档。
  if ($smokeFlaky -gt 0) {
    $smokeLabel = '{0}(!{1} 套没跑成:浏览器在但没连上)' -f $smokeLabel, $smokeFlaky
  }
  # 挂死也要进摘要:跑完 gates 的人看的是汇总表,不是往回滚屏找那行 [HUNG]。
  # 两种杀因分开写:「真挂死」要有人去查,「被段预算腰斩」不用 —— 那一套多半没病,
  # 是这一段的时间用完了。合成一个数就是把前者的紧迫性摊薄、把后者的无辜抹掉。
  $smokeHungReal = $smokeHung - $smokeHungByBudget
  if ($smokeHungReal -gt 0) {
    $smokeLabel = '{0}(!{1} 套超时被杀)' -f $smokeLabel, $smokeHungReal
  }
  # [SL-301 裁定(a)] 超预算同理必须进摘要 —— 「余套没跑」与「都跑过了」不能长得一样。
  if ($smokeBudgetHit) {
    # 预算被击穿有两种后果,可能同时发生,也可能只发生一种:
    #   · 当前那一套被**腰斩**(kill 分支,`$smokeHungByBudget`);
    #   · 后面的套**根本没跑到**(循环顶部 break,`$smokeRanCount < 总数`)。
    # 一句话把发生了的都说出来。**「腰斩」只说一次** —— 早先拆成两个后缀连写会变成
    # 「(!1 套被段预算腰斩)(!……,末套被腰斩)」,同一件事说两遍。
    # 也不能写死「余套未跑」:预算若在**最后一套**上见底,24 套全都起过跑,
    # 那句话是往更糟的方向说谎。所以两半都由计数决定,一半不成立就不写。
    $budgetParts = @()
    if ($smokeHungByBudget -gt 0) { $budgetParts += ('{0} 套被腰斩' -f $smokeHungByBudget) }
    if ($smokeRanCount -lt $smokeFiles.Count) { $budgetParts += ('余 {0} 套未跑' -f ($smokeFiles.Count - $smokeRanCount)) }
    if ($budgetParts.Count -eq 0) { $budgetParts += '余套未跑' }
    $smokeLabel = '{0}(!页面级段超 {1}s 预算,{2})' -f $smokeLabel, $smokeSegBudgetSec, ($budgetParts -join '、')
  }
  # 地板也要进摘要,理由与上面三个降级逐字相同:跑完 gates 的人看的是这张表。
  # 而这一格是**汇总看起来最正常**的那一格 —— 全绿、无 SKIP、无 HUNG、无预算后缀,
  # 只有一行黄字。既然承认没人会盯滚屏,就不该把唯一的痕迹留在滚屏里。
  if ($smokeZeroPage) {
    $smokeLabel = '{0}(!0 套页面级,全程无锁)' -f $smokeLabel
  }
  if ($smokeNoLock) {
    $smokeLabel = '{0}(!拿不到 IPC 锁,页面级未跑)' -f $smokeLabel
  }
  Set-Gate $smokeLabel $smokeOk
}

}
finally {
  # [SL-301] **放锁前核一眼机器上还有没有无头 Chrome**(注意:核的是**机器状态**,
  # 不是「本段起的那些」—— 判据数不出父进程,见下面那条措辞注)。放了锁而 Chrome 还活着,
  # 下一个 agent 拿到锁开始跑 6/7/8,机器上却仍有上一份的 6 个 Chrome 在吃核 ——
  # 「放了锁但资源还占着」等于没放,本卡的不变式当场失效。
  # 正常路径下 3e 自己会收干净(每套跑完即退,超时那支走 `Kill($true)` 连进程树);
  # 这里只做**核对与兜底**:仍有残留就等一小会儿再报,让读日志的人看得见。
  # 只按「本机所有 chrome」计数,不去杀 —— 杀掉用户自己的浏览器是更坏的副作用,
  # 而 3e 的临时 user-data-dir 已由各套自己的 teardown 负责([SL-287])。
  if ($script:SmokeLockTaken -and $script:SmokeLockOk -and -not $NoIpcLock) {
    # **只数本套起的那些**,不数「本机所有 chrome」。第一版数了全部,于是把
    # **用户自己开着的浏览器**也算进来 —— 首次并发验收就打出「仍有 8 个 chrome」,
    # 而那 8 个多半是人在用的窗口:一条永远为真的警告等于没有警告。
    # 判据用**命令行**(`--headless` / 临时 user-data-dir 名里的 `scvb-`),不是进程名 ——
    # 与 SL-301 测量期清理残留时踩到的是同一条:按进程名一把抓会误伤用户的浏览器。
    $headless = @()
    try {
      $headless = @(Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" -ErrorAction Stop |
        Where-Object { $_.CommandLine -like '*--headless*' -or $_.CommandLine -like '*scvb-*' })
    }
    catch {
      # 取不到命令行(权限/CIM 不可用)时**不猜**:宁可不报,也不要拿「所有 chrome」冒充。
      Write-Host ("  [ipc-lock] 无法枚举 chrome 命令行({0}),跳过残留核对" -f $_.Exception.GetType().Name) -ForegroundColor Yellow
      $headless = $null
    }
    # 超预算那条路径上**照样核残留,只是不多睡那 3 秒** —— 复审点出我上一版把整块跳过了,
    # 而那恰恰是**残留最可能存在**的一条路径(套被腰斩,它自己的 teardown 没跑完)。
    # 「尽快交锁」省的是 3 秒的等待,不是省掉诊断。
    if ($null -ne $headless -and $headless.Count -gt 0) {
      if (-not $smokeBudgetHit) { Start-Sleep -Seconds 3 }
      # 第二次枚举也要守「不猜就出声」那条口径(复审指出第一版这里丢了它):
      # 用 SilentlyContinue 的话,CIM 这一刻恰好失败会静默变成「0 个残留」——
      # 那是**把查不到当成没有**,与本卡治的「降级不可见」是同一个毛病。
      try {
        $headless = @(Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" -ErrorAction Stop |
          Where-Object { $_.CommandLine -like '*--headless*' -or $_.CommandLine -like '*scvb-*' })
      }
      catch {
        Write-Host ("  [ipc-lock] 复核残留时枚举失败({0}),这一轮的残留数**未知**(不是 0)" -f $_.Exception.GetType().Name) -ForegroundColor Yellow
        $headless = $null
      }
      if ($null -ne $headless -and $headless.Count -gt 0) {
        # 措辞要能经得起追问(复审逐条点过):
        #   · 数的是**进程**不是浏览器实例 —— renderer / GPU / utility 子进程同样带 `--headless`,
        #     一个实例通常对应好几个进程;写成「N 个浏览器」会让人据此去推「漏了几套没收」而推错。
        #   · **不能断言是本轮哪一套**:这个判据数的是「全机所有带 `--headless`/`scvb-` 的 chrome」,
        #     它数不出父进程 —— 数到的完全可能是**上一个 agent 或前几轮**漏下来的。
        #     要真的只数自己起的,得从 `$proc.Id` 沿 `ParentProcessId` 递归收进程树;不做,
        #     但那就不能把话说死。(`*scvb-*` 还会匹配到别人 worktree 路径,同一回事。)
        Write-Host ("  [ipc-lock] 放 3e 锁时机器上仍有 {0} 个无头 chrome **进程**(含 renderer 等子进程,不等于 {0} 个浏览器实例;也可能含前几轮/别的 agent 的残留)—— 下一个拿到锁的 agent 会在仍被占着核的机器上跑 6/7/8。残留归 SL-287 那族的 teardown,本卡只报不判负" -f $headless.Count) -ForegroundColor Yellow
      }
    }
  }
  Exit-ScvbIpcLock -Mutex $script:SmokeLock -Segment 'gate 3e(页面级)'
}
# ===== 持锁段①终点:3f..5(configure/build)**不持锁**,可与别的 agent 并行 =====

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
Write-Host '=== Gate 3i: 桥面/曲线/设计盒/native 路径/冒烟写法/预写条目 对拍(scripts/check-*)==='
# ==================================================================
# [SL-258] 这三个脚本此前**没有任何执行者** —— 不在 CI、不在本 gates、不在 package.json。
# 于是 [SL-256] 给 check-bridge-parity 加的「已注册 handler ↔ manifest」断言,以及本卡把它
# 扩到 input/monitor 三侧的版本,都只有人手动 node 才会红。同一族的洞因此栽了两次
# (exportSuggestions 与 setGuideSeen:契约齐全、常量在、web 真调用,唯独没注册 handler),
# **即便门禁当时已经写好也照样栽**。判级理由与 Gate 3g 逐字同款:门禁没有执行者等于没有门禁。
# 三条本地实跑均 exit=0 后才接线(curve 最坏偏差 9.6e-5 dB / 容差 0.01 dB)。
# [SL-283] 第四条 check-native-paths.mjs 同理接进来:它与 format.yml 的 docs-truth 跑
# **逐字同一条命令**。本仓的纪律是「只挂在本地 gates 上等于没有执行者」,反过来同样成立 ——
# 只挂在 CI 上,改 NATIVE_RE 的人本地全绿、推上去才红。
# [SL-297] 第六条 check-gates-visibility.mjs:它读的是 **gates.ps1 自己**,断言 Gate 3e 的
# rc=3(浏览器在但没连上)有独立计数**且计数接进了汇总标签**。本卡修的洞就是「降级了但
# 摘要里看不见」,而那个修复自己也会被人删回去 —— 删掉标签里那段插值,`[FLAKY-SKIP]`
# 退回只在滚屏出现一行,洞原样复现而所有现有用例照绿。所以钉的是**接线**不是常量。
# 注意它在**没有 grep 的机器上**(Windows 裸装)会把引擎对拍降级成警告仍返回 0。
# [SL-286] 起那是**两档**(30 条手写用例 / 全仓真实路径);另外**没有 git 时**
# 「顶层条目全覆盖」**与「全仓路径引擎对拍」两档**一起降级(后者嵌在前者的代码块里,
# 拿不到全仓文件清单就无从对拍)。所以本地绿不等于这几档验过,它们以 CI(ubuntu)为准。
# [SL-295] 第七条 check-changelog-drafts.mjs:断言 CHANGELOG.md 注释块里没有「卡已经合了、
# 预写条目却还留着」的条目。它是这一圈里**唯一要读 git 历史**的一条 —— 已上线集合取自
# base 分支的提交标题(默认 `origin/feature/v1`,可用 SCVB_CHANGELOG_BASE 改)。取不到那个
# ref、或仓库是浅克隆时它**判负而不是跳过**:近乎空的已上线集合会让门禁永远绿,正是本仓
# 「SKIP 吞掉判据」那一族的形态。它的**自测**要单独跑一条(下面那圈只跑裸命令)。
if (-not $nodeCmd) {
  Write-Host '  node 不在 PATH —— 本 gate 无法执行(不是跳过,是判负:工具缺失不得计为通过)' -ForegroundColor Red
  Set-Gate '3i 桥面/曲线/设计盒/native 路径/冒烟写法/预写条目对拍' $false
}
else {
  $parityOk = $true
  $parityWarn = 0
  # [SL-295] 自测单独一条:实跑绿有两种可能 ——「块里真没有漏搬」和「判据被改坏了」,
  # 自测把后一种单独照出来。与 format.yml 的 docs-truth 两步逐字同款。
  $draftsSelfTest = (& node (Join-Path 'scripts' 'check-changelog-drafts.mjs') --self-test 2>&1)
  if ($LASTEXITCODE -ne 0) {
    $parityOk = $false
    Write-Host '  check-changelog-drafts.mjs --self-test:' -ForegroundColor Red
    $draftsSelfTest | ForEach-Object { Write-Host ("  " + $_) }
  }
  foreach ($sc in @('check-bridge-parity.mjs', 'check-curve-parity.mjs', 'check-design-box.mjs', 'check-native-paths.mjs', 'check-smoke-hygiene.mjs', 'check-gates-visibility.mjs', 'check-changelog-drafts.mjs')) {
    $out = (& node (Join-Path 'scripts' $sc) 2>&1)
    if ($LASTEXITCODE -ne 0) {
      $parityOk = $false
      Write-Host ("  {0}:" -f $sc) -ForegroundColor Red
      $out | ForEach-Object { Write-Host ("  " + $_) }
    }
    else {
      # [SL-283] **成功时也要把 [WARN] 行回显,并把「降级了几档」带进汇总表**。
      # check-native-paths 在没有 grep 的机器上会把引擎对拍降级成警告并**仍返回 0** ——
      # [SL-286] 起那是**两档**(30 条手写用例 / 全仓真实路径),各打一行;没有 git 时
      # 掉的是「顶层条目全覆盖」**与「全仓路径引擎对拍」两档,但只打一行**(后者嵌在
      # 前者的代码块里)—— 这就是下面那段说「行数是档数的下界」的来处。
      # 而 Windows 上 `grep` 通常不在 PATH 上
      # (Git for Windows 的 grep 只在 Git Bash 里),所以降级在本地是**常态而不是例外**。
      # 只回显还不够:`Set-Gate` 只有 PASS/FAIL 两态,汇总表里「降级过的一次」和「全跑过的
      # 一次」会长得一模一样,而跑完 gates 的人看汇总表的概率远高于往回滚二十屏找黄字。
      # 与 Gate 3e 同款处理(见那段 **但绝不静默** —— SKIP 计数要带进总结):把降级计数拼进
      # gate 名,让「有一档没跑」在汇总表里就与「跑过了」不同形。
      $warnLines = @($out | Where-Object { $_ -match '\[WARN\]' })
      if ($warnLines.Count -gt 0) {
        $parityWarn += $warnLines.Count
        $warnLines | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
      }
      # [SL-295] check-changelog-drafts 有两行**只在成功路径上**、却必须显形的输出,
      # 这一圈默认只回显 [WARN],会把它们整段吞掉:
      #   · `[ALLOW] #<号> 放行 —— <理由>` —— 豁免不显形就等于没有豁免纪律(脚本头注口径);
      #   · `[BASE] <base>@<sha> (<date>) —— N 条提交标题` —— 陈旧的 remote-tracking ref 会让
      #     门禁**静默变绿**,而这条降级**只在本地发生**(CI 那边每次都从远端重新 fetch,
      #     `fetch-depth: 0`;「新 clone」本身并不保证 base 新,保证它的是「这一次就是拉来的」)。
      #     放进成功消息又被成功分支吞掉,等于修在了唯一用不到的地方。
      # 两者都按**方括号标记**匹配,不按文案匹配:挂在散文上的话,一次很自然的措辞编辑就会让
      # 这行回显静默失效(#197 复审第 5 轮【建议】)。
      # 不并进 $parityWarn:它数的是「某档没跑」,放行与 base 戳都不是降级。
      @($out | Where-Object { $_ -match '\[ALLOW\]|\[BASE\]' }) |
        ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
    }
  }
  # 标签只说**数出来的东西**,不替别人的 `[WARN]` 下定义(PR#180 复审采纳)。
  # 这里数的单位是**行**,不是「档」。
  # [SL-286] 这段论证原先举的两个例子**都已被 SL-286 改掉**,留着会把人带向不存在的东西:
  #   · 原文说「check-native-paths 的降级是单次单行 console.warn」——它现在有**三个**降级点:
  #     ①「没 grep ⇒ 跳过 30 条手写用例对拍」②「没 grep ⇒ 全仓路径对拍也一并跳过」
  #     ③「没 git ⇒ 跳过顶层条目全覆盖」。但**单次运行最多打两行**:② 落在「git 可用」
  #     那一支里,与 ③ 互斥(没 git 时 ④ 整档不进,② 根本走不到)。**四种组合实测**
  #     (上一版只列了三种 —— 一张不完整却看着像穷举的表,和写死行号是同一类账):
  #       无 git 无 grep = 2 行   有 git 无 grep = 2 行
  #       无 git 有 grep = 1 行   两者都有       = 0 行
  #     **注意 1:1 并不成立**(SL-283 时成立,现在不了):没 git 时 ② 走不到、连一行都不打,
  #     但它确实**没跑**。逐格算「打几行 / 实际跳掉几档」:
  #       无 git 无 grep = 2 行 / 3 档   有 git 无 grep = 2 行 / 2 档
  #       无 git 有 grep = 1 行 / 2 档   两者都有       = 0 行 / 0 档
  #     即**行数是档数的下界,会少报**。这反而是「不能冒充降级档数」的又一条理由 ——
  #     标签只能说「上方有几处 WARN,自己看」。
  #   · 原文说 check-bridge-parity 的 `warn()` helper「已定义、当前零调用」,并举
  #     那一处「真跳过却裸 warns.push、这里数不到」当反例 —— 那四处**已改走 warn()**
  #     (全在 check-bridge-parity 的「四、事件载荷字段对拍」那一节里),裸 push 只剩
  #     helper 自己那一处,反例没了。
  #     ⚠ 这里**故意不写行号**:上一版那两个数就是这么失效的,而换成四个新数只是把时钟
  #     归零、耦合原样留着 —— 且它们指向**另一个文件**,那边任何增删都会让四个数一起漂,
  #     不会有人发现。同一条规矩见 check-smoke-hygiene.mjs 头注「别写死位置」。
  # 但**结论不变**,理由换成仍然成立的那条:`warn()` 的语义是「**通过但有提示**」,不等于
  # 「某档没跑」——今天这四处恰好都是「跳过」,谁哪天拿它发一条纯提示,写死「−N 档降级」
  # 就会对着提示喊降级。这个信号刚建立起来就是要让人信它,喊错一次,下次真降级也会被当噪声。
  # 所以这半句只能是「上方有几处 WARN,自己看」,不能冒充「降级档数」的权威计数。
  $parityLabel = '3i 桥面/曲线/设计盒/native 路径/冒烟写法/预写条目对拍'
  if ($parityWarn -gt 0) {
    $parityLabel = '3i 桥面/曲线/设计盒/native 路径/冒烟写法/预写条目对拍(上方 {0} 处 [WARN],逐条看清是不是「某档没跑」)' -f $parityWarn
  }
  Set-Gate $parityLabel $parityOk
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
$cfgArgs = @('-S', '.', '-B', $BuildDir, '-DSCVB_BUILD_TESTS=ON', "-DJUCE_PATH=$JucePath")
if ($Generator -like '*Multi-Config*') {
  # 多配置生成器下档位由 `--build --config` / `ctest -C` 选,`CMAKE_BUILD_TYPE` 会被忽略;
  # 传了只会让人以为它在起作用 —— CI 侧同理,那边也不传(build-vst3.yml 的 configure 步)。
  $cfgArgs += "-DCMAKE_CONFIGURATION_TYPES=$Config"
}
else {
  # **这条分支是按生成器名字分,不是按「是不是多配置」分**(PR#176 复审的一处校正):
  # 默认路径($Generator 为空)在 Windows 上落到 Visual Studio 生成器,那**也是**多配置的,
  # 于是照样拿到一个被忽略的 `CMAKE_BUILD_TYPE`。明知如此仍这么写,是因为默认路径要**逐字**
  # 保持本卡之前的行为(默认档位不该因为这条清理而变),而单配置生成器(`-G Ninja`、
  # Makefiles)确实需要它。想连默认路径一起清,得先把「默认生成器是什么」钉死,那是另一张卡。
  $cfgArgs += "-DCMAKE_BUILD_TYPE=$Config"
}
if ($Generator) {
  $cfgArgs = @('-G', $Generator) + $cfgArgs
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
$ipcLock = Enter-ScvbIpcLock -Segment 'gate 6/7/8'
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
  if ($runGate7) { Set-Gate '7 pluginval 非 GUI' $false } else { Set-Skip '7 pluginval 非 GUI' }
  if ($runGate8) { Set-Gate '8 pluginval 全量含 GUI' $false } else { Set-Skip '8 pluginval 全量含 GUI' }
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
if (-not $runGate7) {
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
if (-not $runGate8) {
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
  Exit-ScvbIpcLock -Mutex $ipcLock -Segment 'gate 6/7/8'
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