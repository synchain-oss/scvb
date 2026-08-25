<#
.SYNOPSIS  SCVB WebView2 环境自查 —— 插件窗口打不开时,第一件要跑的东西。
.DESCRIPTION
  SCVB 的插件界面跑在 Microsoft Edge WebView2 里。窗口打不开时,故障可能在四个地方:
  运行时没装 / 运行时太旧 / user-data 目录不可写 / 宿主挡住了 msedgewebview2.exe 子进程。
  本脚本把这四条一次查完并给出结论,不改任何东西(纯只读,唯一的写动作是在自己的
  user-data 目录里建一个探针文件再删掉)。

  为什么不只查一处:「设置 → 应用」里看不到 WebView2 条目**不等于**没装 ——
  per-machine 安装常常不在应用列表里显示,而 Edge 浏览器本身也可能让某些探测手段
  误报「有」。故这里双通道查:注册表(三个位置)+ WebView2 Loader 报告的版本。

.EXAMPLE   pwsh scripts/check-webview2.ps1
.EXAMPLE   powershell -ExecutionPolicy Bypass -File 诊断.ps1
#>

$ErrorActionPreference = 'Continue'

# SCVB 要求的 WebView2 Runtime 主版本下限。真源 = src/plugin-common/PlatformWebView.h 的
# kMinRuntimeMajor(那里写了推导:JUCE 8.0.8 硬依赖的 WebView2 首发 GA 接口集)。
$MinMajor = 86
$DownloadUrl = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703'

# Evergreen Runtime 在 EdgeUpdate 下的固定产品 GUID(微软文档公布值,三个位置同一个)。
$RuntimeGuid = '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'

function Write-Head([string]$text) {
  Write-Host ''
  Write-Host ("=== {0} ===" -f $text) -ForegroundColor Cyan
}

function Get-PvFrom([string]$path) {
  try {
    $item = Get-ItemProperty -LiteralPath $path -ErrorAction Stop
    if ($item.pv) { return [string]$item.pv }
  }
  catch { }
  return $null
}

Write-Host 'SCVB WebView2 环境自查' -ForegroundColor White
Write-Host ("时间: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Write-Host ("系统: {0}" -f [System.Environment]::OSVersion.VersionString)

# ---------------------------------------------------------------- ① 注册表三处
Write-Head '1. 注册表(Evergreen Runtime 安装记录)'

$regPaths = [ordered]@{
  'HKLM 64 位视图下的 32 位分支(per-machine,最常见)' = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$RuntimeGuid"
  'HKLM 原生分支(per-machine,32 位系统/部分部署)'    = "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$RuntimeGuid"
  'HKCU(per-user 安装)'                              = "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$RuntimeGuid"
}

$regVersions = @()
foreach ($label in $regPaths.Keys) {
  $pv = Get-PvFrom $regPaths[$label]
  if ($pv) {
    Write-Host ("  [找到] {0}" -f $label)
    Write-Host ("         版本 {0}" -f $pv) -ForegroundColor Green
    $regVersions += $pv
  }
  else {
    Write-Host ("  [没有] {0}" -f $label) -ForegroundColor DarkGray
  }
}

# ---------------------------------------------------------------- ② Loader 通道
Write-Head '2. WebView2 Loader 报告的版本(插件实际走的这条)'

# 插件调的是 WebView2Loader 的 GetAvailableCoreWebView2BrowserVersionString。PowerShell 里
# 直接 P/Invoke 它需要 loader DLL 在 PATH 上,不可靠;改用它同源的事实依据:Runtime 安装
# 目录下的 msedgewebview2.exe 文件版本。两者一致时结论可信,不一致要写进报告。
$loaderVersion = $null
# ${env:ProgramFiles(x86)} 必须用花括号——名字里有括号,写成 $env:ProgramFiles(x86) 会被
# 解析成「$env:ProgramFiles 后面跟一个调用」,静默拿到错的路径。
$exeCandidates = @(
  "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application",
  "$env:ProgramFiles\Microsoft\EdgeWebView\Application",
  "$env:LOCALAPPDATA\Microsoft\EdgeWebView\Application"
) | Where-Object { $_ -and (Test-Path $_) }

foreach ($dir in $exeCandidates) {
  $exe = Get-ChildItem -Path $dir -Filter 'msedgewebview2.exe' -Recurse -ErrorAction SilentlyContinue |
  Select-Object -First 1
  if ($exe) {
    $loaderVersion = $exe.VersionInfo.ProductVersion
    Write-Host ("  [找到] {0}" -f $exe.FullName)
    Write-Host ("         版本 {0}" -f $loaderVersion) -ForegroundColor Green
    break
  }
}
if (-not $loaderVersion) {
  # 兜底:装在非标准位置时,从正在跑的进程反查可执行文件路径。
  $running = Get-Process -Name 'msedgewebview2' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($running -and $running.Path) {
    $loaderVersion = $running.MainModule.FileVersionInfo.ProductVersion
    Write-Host ("  [找到] {0}(经运行中的进程反查)" -f $running.Path)
    Write-Host ("         版本 {0}" -f $loaderVersion) -ForegroundColor Green
  }
}
if (-not $loaderVersion) {
  Write-Host '  [没有] 找不到 msedgewebview2.exe(Evergreen Runtime 的主程序)' -ForegroundColor Yellow
  Write-Host '         若第 1 步查到了版本,这里没找到通常只是装在非标准位置,不算问题。' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- ③ 结论
Write-Head '3. 结论'

$allVersions = @($regVersions + $loaderVersion) | Where-Object { $_ } | Select-Object -Unique
$best = $null
$bestMajor = -1
foreach ($v in $allVersions) {
  $m = 0
  if ($v -match '^(\d+)\.') { $m = [int]$Matches[1] }
  if ($m -gt $bestMajor) { $bestMajor = $m; $best = $v }
}

$runtimeOk = $false
if (-not $best) {
  Write-Host '  ✗ 没有检测到 WebView2 Evergreen Runtime。' -ForegroundColor Red
  Write-Host ('    请安装后重开 DAW:{0}' -f $DownloadUrl) -ForegroundColor Yellow
  Write-Host '    注意:「设置 → 应用」里看不到条目不代表没装,但这里三处都没有就是真没装。'
}
elseif ($bestMajor -lt $MinMajor) {
  Write-Host ('  ✗ Runtime 版本 {0} 低于 SCVB 要求的主版本 {1}。' -f $best, $MinMajor) -ForegroundColor Red
  Write-Host ('    请升级后重开 DAW:{0}' -f $DownloadUrl) -ForegroundColor Yellow
}
else {
  Write-Host ('  ✓ Runtime {0} 达标(要求主版本 ≥ {1})。' -f $best, $MinMajor) -ForegroundColor Green
  $runtimeOk = $true
}

if ($allVersions.Count -gt 1) {
  Write-Host ('  注:检测到多个版本记录({0})—— 一般是 per-machine 与 per-user 各装了一份,不影响使用。' -f ($allVersions -join ', ')) -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------- ④ user-data 目录
Write-Head '4. SCVB 的 user-data 目录(可写性)'

# 真源 = src/plugin-common/PlatformWebView.cpp 的 userDataFolderRoot()。
$udfRoot = Join-Path $env:LOCALAPPDATA 'Synchain\SCVB\WebView2'
Write-Host ("  位置: {0}" -f $udfRoot)

$udfOk = $false
try {
  if (-not (Test-Path $udfRoot)) { New-Item -ItemType Directory -Force -Path $udfRoot -ErrorAction Stop | Out-Null }
  $probe = Join-Path $udfRoot '.selfcheck-probe'
  Set-Content -LiteralPath $probe -Value 'ok' -ErrorAction Stop
  Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
  Write-Host '  ✓ 目录可建、可写。' -ForegroundColor Green
  $udfOk = $true
}
catch {
  Write-Host ('  ✗ 目录不可写:{0}' -f $_.Exception.Message) -ForegroundColor Red
  Write-Host '    常见成因:企业安全策略锁了 %LOCALAPPDATA%、杀毒软件拦截、磁盘满。' -ForegroundColor Yellow
}

$stale = @(Get-ChildItem -Path $udfRoot -Directory -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
  Write-Host ("  现有 {0} 个实例目录(名字里的 p<数字> 是创建它的进程 PID):" -f $stale.Count)
  $stale | Select-Object -First 8 | ForEach-Object { Write-Host ("    {0}" -f $_.Name) -ForegroundColor DarkGray }
  Write-Host '    DAW 全关之后这些目录可以整个删掉,插件会重建。' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- ⑤ 子进程
Write-Head '5. WebView2 子进程(宿主有没有挡住它)'

$procs = @(Get-Process -Name 'msedgewebview2' -ErrorAction SilentlyContinue)
if ($procs.Count -gt 0) {
  Write-Host ("  ✓ 当前有 {0} 个 msedgewebview2.exe 在跑 —— 子进程链没被挡。" -f $procs.Count) -ForegroundColor Green
}
else {
  Write-Host '  · 当前没有 msedgewebview2.exe 在跑。' -ForegroundColor DarkGray
  Write-Host '    如果这是在「DAW 已打开、插件窗口已打开」的状态下跑的,那就是关键线索:' -ForegroundColor Yellow
  Write-Host '    WebView2 子进程根本没起来(宿主沙箱/杀毒/组策略拦截),请把本报告发回。' -ForegroundColor Yellow
}

# ---------------------------------------------------------------- 汇总
Write-Head '汇总'
Write-Host ("  Runtime      : {0}" -f $(if ($runtimeOk) { "OK ($best)" } elseif ($best) { "太旧 ($best)" } else { '未安装' }))
Write-Host ("  user-data 目录: {0}" -f $(if ($udfOk) { 'OK' } else { '不可写' }))
Write-Host ("  子进程        : {0}" -f $(if ($procs.Count -gt 0) { "运行中 ($($procs.Count))" } else { '未运行' }))
Write-Host ''
Write-Host '把上面整屏内容截图或复制发回给开发,即可定位问题。' -ForegroundColor White

if ($runtimeOk -and $udfOk) { exit 0 } else { exit 1 }
