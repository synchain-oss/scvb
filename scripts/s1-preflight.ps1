# SPDX-License-Identifier: GPL-3.0-or-later
# s1-preflight.ps1 —— S1 交付前置检查(v6):s1 单元测试 + 免 DAW 压测,CI/本地通用。
#   本地交付前固定跑:  pwsh scripts/s1-preflight.ps1
#   CI(开源后):        pwsh scripts/s1-preflight.ps1 -SkipBuild -Ci
#   压测场景 = scvb_stress(mono2stereo/flip/outputcheck/churn),每场景独立进程,
#   崩溃 = 0xC0000005 退出码(复现 v1..v5 的 Cubase freeze/导出崩溃);WER LocalDumps
#   转储 + scvb journal 自动归档到 -DumpDir 下的 run-<时间戳> 目录。
#   产物:preflight-result.json(机器可读)+ 人类可读摘要;退出码 = 失败数(0 = 全绿)。
[CmdletBinding()]
param(
  [string]$BuildDir = "build-T02",
  [switch]$SkipBuild,
  [switch]$UnitTestsOnly,
  [switch]$StressOnly,
  [switch]$Ci
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDirAbs = Join-Path $repoRoot $BuildDir
$dumpBase = Join-Path $env:LOCALAPPDATA "SynchainSCVB\stress-dumps"
New-Item -ItemType Directory -Force -Path $dumpBase | Out-Null
$runDir = Join-Path $dumpBase ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

function Write-Step([string]$msg) { if (-not $Ci) { Write-Host ("[preflight] " + $msg) } }

$failures = 0
$results = @()

# ---------- 1) 构建 ----------
if (-not $SkipBuild -and -not $UnitTestsOnly -and -not $StressOnly) {
  Write-Step "build (Release): stress + unit tests + plugins"
  if ([string]::IsNullOrEmpty($env:JUCE_PATH)) { throw "需要 JUCE_PATH 环境变量(指向 JUCE 源码根目录)" }
  cmake --build $buildDirAbs --target scvb_stress scvb_s1_tests SCVBInput_VST3 SCVBOutput_VST3 --config Release
  if ($LASTEXITCODE -ne 0) { throw "构建失败(exit=$LASTEXITCODE)" }
}

# ---------- 2) 单元测试 ----------
if (-not $StressOnly) {
  Write-Step "unit tests: scvb_s1_tests"
  $testExe = Get-ChildItem -Path $buildDirAbs -Recurse -Filter scvb_s1_tests.exe | Where-Object { $_.FullName -match "\\Release\\" } | Select-Object -First 1
  if ($null -eq $testExe) { throw "未找到 scvb_s1_tests.exe(先构建或检查 -BuildDir)" }
  $unitOut = Join-Path $runDir "unit-tests.out.txt"
  $p = Start-Process -FilePath $testExe.FullName -Wait -PassThru -RedirectStandardOutput $unitOut -RedirectStandardError (Join-Path $runDir "unit-tests.err.txt")
  if ($p.ExitCode -ne 0) {
    $failures++
    $results += [pscustomobject]@{ suite = "unit"; status = ("FAIL(exit=" + $p.ExitCode + ")"); log = $unitOut }
    Write-Host "UNIT TESTS FAILED (exit=$($p.ExitCode))"
  } else {
    $results += [pscustomobject]@{ suite = "unit"; status = "PASS"; log = $unitOut }
    Write-Step "unit tests: PASS"
  }
}

# ---------- 3) 压测(每场景独立进程) ----------
if (-not $UnitTestsOnly) {
  Write-Step "stress: configuring WER LocalDumps for scvb_stress.exe"
  $key = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\scvb_stress.exe"
  New-Item -Path $key -Force | Out-Null
  Set-ItemProperty -Path $key -Name DumpFolder -Type ExpandString -Value $runDir
  Set-ItemProperty -Path $key -Name DumpType -Type DWord -Value 2
  Set-ItemProperty -Path $key -Name DumpCount -Type DWord -Value 50

  $stressExe = Get-ChildItem -Path $buildDirAbs -Recurse -Filter scvb_stress.exe | Where-Object { $_.FullName -match "\\Release\\" } | Select-Object -First 1
  if ($null -eq $stressExe) { throw "未找到 scvb_stress.exe(先构建或检查 -BuildDir)" }
  $scenarios = @("mono2stereo", "flip", "outputcheck", "churn")
  foreach ($s in $scenarios) {
    Write-Step ("stress scenario: " + $s)
    $outFile = Join-Path $runDir ("stress-" + $s + ".out.txt")
    $p = Start-Process -FilePath $stressExe.FullName -ArgumentList $s -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError (Join-Path $runDir ("stress-" + $s + ".err.txt"))
    $code = $p.ExitCode
    if ($code -eq 0) {
      $status = "PASS"
      $results += [pscustomobject]@{ scenario = $s; status = $status; exitCode = 0; log = $outFile }
    } else {
      $failures++
      if ($code -ge 0xC0000000 -and $code -le 0xCFFFFFFF) { $status = ("CRASH(0x" + $code.ToString("X8") + ")") } else { $status = ("FAIL(exit=" + $code + ")") }
      $results += [pscustomobject]@{ scenario = $s; status = $status; exitCode = $code; log = $outFile }
    }
    Write-Step ("  -> " + $status)
  }
}

# ---------- 4) 收集 journal + 转储 ----------
$journal = Join-Path $env:LOCALAPPDATA "SynchainSCVB\logs\scvb-journal.txt"
if (Test-Path $journal) { Copy-Item -LiteralPath $journal -Destination (Join-Path $runDir "scvb-journal.txt") -Force }
$dmpCount = @(Get-ChildItem -Path $runDir -Filter *.dmp -ErrorAction SilentlyContinue).Count

# ---------- 5) 汇总 ----------
$summary = [ordered]@{
  timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
  runDir = $runDir
  failures = $failures
  dumps = $dmpCount
  results = $results
}
$json = ConvertTo-Json -InputObject $summary -Depth 6
$json | Set-Content -LiteralPath (Join-Path $runDir "preflight-result.json") -Encoding UTF8

if ($Ci) {
  Write-Output $json
} else {
  Write-Host ""
  foreach ($r in $results) { Write-Host ("  " + ($r | Format-Table -AutoSize | Out-String).Trim()) }
  Write-Host ("[preflight] failures=" + $failures + " dumps=" + $dmpCount + " runDir=" + $runDir)
  if ($failures -eq 0) { Write-Host "[preflight] ALL PASS" } else { Write-Host "[preflight] FAILED" }
}
exit $failures