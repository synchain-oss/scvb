<#
.SYNOPSIS  一键构建 SCVB 并可选安装进系统 VST3 目录,供 DAW 实测(D3 通道 1,06 §6.1)。
.EXAMPLE   pwsh scripts/build.ps1 -Install
.EXAMPLE   pwsh scripts/build.ps1 -Config Debug -Target Output -NoInstall
.EXAMPLE   pwsh scripts/build.ps1 -BuildDir build-T15
#>
param(
  [ValidateSet('Release','Debug','RelWithDebInfo')][string]$Config = 'Release',
  [ValidateSet('All','Input','Output','Monitor','Core','Tests')][string]$Target = 'All',
  [string]$JucePath = $env:JUCE_PATH,
  [string]$BuildDir = 'build',
  [switch]$Install,
  [switch]$Clean,
  [switch]$SkipTests,
  [switch]$OpenFolder
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

# 版本单一真源:JUCE / pluginval 版本只从仓库根版本文件读(12 §6.3 / 06 §3.1 步骤 0),脚本内不出现字面量。
$juceVersion = (Get-Content .juce-version -Raw).Trim()
$pluginvalVersion = (Get-Content .pluginval-version -Raw).Trim()

function Fail([string]$msg) {
  Write-Host "依赖预检失败: $msg" -ForegroundColor Red
  Write-Host '请先修复环境再运行构建。' -ForegroundColor Red
  exit 1
}

Write-Host '== 依赖预检 =='

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { Fail '未找到 cmake(需 >= 3.22,可 winget install Kitware.CMake)' }
$cmakeVer = ((& cmake --version | Select-Object -First 1) -replace 'cmake version ', '')
Write-Host ("cmake        {0}" -f $cmakeVer)

$pfx86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$vswhere = Join-Path $pfx86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail '未找到 vswhere(MSVC 未安装)' }
$msvc = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $msvc) { Fail "未找到带 C++ 工具的 MSVC(安装 'Desktop development with C++')" }
Write-Host ("MSVC         {0}" -f $msvc)

if (-not $JucePath) { Fail '未设置 -JucePath 或环境变量 JUCE_PATH' }
if (-not (Test-Path (Join-Path $JucePath 'CMakeLists.txt'))) { Fail "JUCE_PATH 指向的目录不是 JUCE 检出: $JucePath" }
$juceTag = (& git -C $JucePath describe --tags 2>$null)
if ($juceTag -and ($juceTag.Trim() -ne $juceVersion)) {
  Write-Host ("警告: JUCE tag '{0}' 与 .juce-version '{1}' 不一致" -f $juceTag, $juceVersion) -ForegroundColor Yellow
}
Write-Host ("JUCE         {0}(要求 {1})" -f $JucePath, $juceVersion)

$freeGB = [math]::Round((Get-PSDrive -Name ((Split-Path -Qualifier $RepoRoot).TrimEnd(':'))).Free / 1GB, 1)
if ($freeGB -lt 5) { Fail ("磁盘空间不足(剩余 {0} GB,需 >= 5 GB)" -f $freeGB) }
Write-Host ("磁盘空间     {0} GB 可用" -f $freeGB)
Write-Host ("pluginval    要求 {0}" -f $pluginvalVersion)

# ---- 清理(可选)----
if ($Clean -and (Test-Path $BuildDir)) {
  Get-ChildItem $BuildDir -Exclude packages | Remove-Item -Recurse -Force
  Write-Host "已清理 $BuildDir(保留 packages 缓存)"
}

# ---- 配置 ----
Write-Host '== 配置 CMake =='
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& cmake -S . -B $BuildDir "-DCMAKE_BUILD_TYPE=$Config" "-DSCVB_BUILD_TESTS=ON" "-DJUCE_PATH=$JucePath"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 构建 ----
Write-Host '== 构建 =='
& cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 单测(默认开;失败不阻止产物生成,但黄字警告)----
if (-not $SkipTests) {
  Write-Host '== ctest =='
  & ctest --test-dir $BuildDir -C $Config --output-on-failure --no-tests=error
  if ($LASTEXITCODE -ne 0) {
    Write-Host '警告: ctest 失败(产物已生成,见下方路径)。' -ForegroundColor Yellow
  }
}

# ---- 定位三个 .vst3([J75] 增 SCVB Monitor) ----
$bundles = Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vst3' -Directory | Sort-Object Name
$sw.Stop()

Write-Host ''
Write-Host '== 产物 =='
if ($bundles.Count -eq 0) {
  Write-Host '未找到 .vst3 bundle(构建可能失败或目标名改变)。' -ForegroundColor Red
}
else {
  foreach ($b in $bundles) {
    Write-Host ("  {0}" -f $b.FullName)
  }
}
$projLine = (Select-String -Path CMakeLists.txt -Pattern 'project\(SCVB VERSION' | Select-Object -First 1).Line
$projVer = (($projLine -split 'VERSION')[1]).Trim().TrimEnd(')')
Write-Host ("版本: {0}" -f $projVer)
Write-Host ("构建耗时: {0:N1} 秒" -f $sw.Elapsed.TotalSeconds)

# ---- 安装(可选)----
if ($Install) {
  $systemVst3 = Join-Path $env:ProgramFiles 'Common Files\VST3'
  $userVst3 = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
  $dest = $systemVst3
  if (-not (Test-Path $systemVst3)) {
    $dest = $userVst3
    Write-Host "无管理员权限或系统 VST3 目录不可用,回退到用户级: $dest" -ForegroundColor Yellow
  }
  foreach ($b in $bundles) {
    Copy-Item $b.FullName -Destination $dest -Recurse -Force
    Write-Host ("已安装: {0}" -f (Join-Path $dest $b.Name))
  }
}

if ($OpenFolder -and $bundles.Count -gt 0) {
  Start-Process explorer.exe $bundles[0].Directory.FullName
}