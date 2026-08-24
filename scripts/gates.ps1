<#
.SYNOPSIS  SCVB 本地质量门禁 —— 提子 PR 前必须全绿。
.DESCRIPTION
  档位(06 §5.1):
    pwsh scripts/gates.ps1                 # 全量(含 GUI pluginval)
    pwsh scripts/gates.ps1 -PluginOnly     # gate 1-7,跳过真机 GUI(gate 8)
    pwsh scripts/gates.ps1 -Quick          # 跳过 pluginval(gate 7/8),快速回环
  所有 cmake/ctest/pluginval 路径基于 -BuildDir(默认 build),并行 agent 靠它隔离构建目录。
  [R4/J56] gate 3b(gitleaks)/ 3c(reuse lint)与 check-spdx.ps1 已由 T01d 接入(06 §5.1)。
.EXAMPLE   pwsh scripts/gates.ps1
.EXAMPLE   pwsh scripts/gates.ps1 -PluginOnly -BuildDir build-T15
#>
param(
  [switch]$Quick,
  [switch]$PluginOnly,
  [string]$Config = 'Release',
  [string]$JucePath = $env:JUCE_PATH,
  [string]$BuildDir = 'build',
  [string]$PluginvalExe = $env:PLUGINVAL_EXE
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
$pp = (npx --yes prettier@3 --check . 2>&1)
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
  $smokeOk = $true
  foreach ($f in $smokeFiles) {
    $out = (& node $f.FullName 2>&1)
    if ($LASTEXITCODE -ne 0) {
      $smokeOk = $false
      Write-Host ("  {0}:" -f $f.Name) -ForegroundColor Red
      $out | Select-String -Pattern '\[FAIL\]' | Select-Object -First 20 |
      ForEach-Object { Write-Host ("  " + $_) }
    }
  }
  Set-Gate ('3e web smoke({0} 套,node {1})' -f $smokeFiles.Count, $nv) $smokeOk
}

# ==================================================================
Write-Host ('=== Gate 4: 配置 (BuildDir={0}) ===' -f $BuildDir)
# ==================================================================
$cfg = (& cmake -S . -B $BuildDir "-DCMAKE_BUILD_TYPE=$Config" "-DSCVB_BUILD_TESTS=ON" "-DJUCE_PATH=$JucePath" 2>&1)
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

# ==================================================================
Write-Host '=== Gate 6: ctest ==='
# ==================================================================
$ct = (& ctest --test-dir $BuildDir -C $Config --output-on-failure --no-tests=error 2>&1)
if ($LASTEXITCODE -ne 0) { $ct | Select-Object -Last 40 | ForEach-Object { Write-Host ("  " + $_) } }
Set-Gate '6 ctest' ($LASTEXITCODE -eq 0)

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
    # [issue #24] 只统计正式插件的两个 bundle;其它 spike(s2/s3)共享构建目录的产物不再干扰计数。
    # [issue #24] 再按路径收窄到 src/input、src/output(生产插件目录,同名 bundle 不重复计数)。
    $bundles = @(Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vst3' -Directory | Where-Object { $_.Name -in @('SCVB Input.vst3', 'SCVB Output.vst3') -and $_.FullName -match '[\\/]src[\\/](input|output)[\\/]' })
    $pv = $true
    if ($bundles.Count -ne 2) {
      Write-Host ("  期望 2 个 .vst3 bundle(SCVB Input / SCVB Output),实际 {0} 个" -f $bundles.Count) -ForegroundColor Red
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
    $mutex = New-Object System.Threading.Mutex($false, 'Global\SCVB-pluginval-gui')
    Write-Host '  等待 GUI pluginval 全局互斥体...' -ForegroundColor Yellow
    $null = $mutex.WaitOne()
    Write-Host '  已获得 GUI pluginval 互斥体' -ForegroundColor Green
    $pv = $true
    try {
      $logDir = Join-Path $BuildDir 'pluginval-gui-logs'
      New-Item -ItemType Directory -Force $logDir | Out-Null
      # [issue #24] 再按路径收窄到 src/input、src/output(生产插件目录,同名 bundle 不重复计数)。
    $bundles = @(Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vst3' -Directory | Where-Object { $_.Name -in @('SCVB Input.vst3', 'SCVB Output.vst3') -and $_.FullName -match '[\\/]src[\\/](input|output)[\\/]' })
      if ($bundles.Count -ne 2) {
        Write-Host ("  期望 2 个 .vst3 bundle,实际 {0} 个" -f $bundles.Count) -ForegroundColor Red
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