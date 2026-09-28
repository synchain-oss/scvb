# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS  发版 tag 门禁:tag 与 CMakeLists.txt 的 project(SCVB VERSION x.y.z) 是否对应(release.yml 第一步)。
.DESCRIPTION
  判据在 scripts/lib/release-version.ps1(与 package.ps1 共用)。通过时若有 $env:GITHUB_OUTPUT,
  写出 version / kind / prerelease 给后续 job 用。
  -SelfTest 用内置夹具跑判据本身:其中一格就是骨架版 release.yml 的真实 bug
  (第 1 行 cmake_minimum_required(VERSION 3.22) 被当成版本),判据退化回「取第一条 VERSION」必红。
.EXAMPLE   pwsh scripts/check-release-tag.ps1 -Tag v1.2.3
.EXAMPLE   pwsh scripts/check-release-tag.ps1 -SelfTest
#>
param(
  [string]$Tag,
  [string]$CMakeFile,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'lib/release-version.ps1')

if ($SelfTest) {
  $fail = 0
  # 与仓库 CMakeLists.txt 开头同形:cmake_minimum_required 在前、project 在后、中间隔着别的行。
  $cm = "cmake_minimum_required(VERSION 3.22)`n`nset(CMAKE_CXX_STANDARD 17)`n# project(SCVB VERSION 9.9.9)`nproject(SCVB VERSION 1.2.3)`n"
  $parsed = Get-ScvbCMakeVersion $cm
  if ($parsed -ne '1.2.3') { Write-Host "  [FAIL] CMake 解析:期望 1.2.3,得到 '$parsed'" -ForegroundColor Red; $fail++ }
  else { Write-Host '  [ok] CMake 解析跳过 cmake_minimum_required 与被注释的 project()' }
  $none = Get-ScvbCMakeVersion "cmake_minimum_required(VERSION 3.22)`nproject(Other VERSION 1.0.0)`n"
  if ($null -ne $none) { Write-Host "  [FAIL] 非 SCVB 的 project() 不该被认:得到 '$none'" -ForegroundColor Red; $fail++ }
  else { Write-Host '  [ok] 非 SCVB 的 project() 不认' }

  # tag, cmake, 期望 Ok, 期望 Kind, 期望 Version
  $cases = @(
    @('v1.2.3', '1.2.3', $true, 'final', '1.2.3'),
    @('v1.2.3-rc.1', '1.2.3', $true, 'rc', '1.2.3-rc.1'),
    @('v1.2.3-rc.12', '1.2.3', $true, 'rc', '1.2.3-rc.12'),
    @('v0.0.0-test', '1.2.3', $true, 'test', '0.0.0-test'),
    @('v0.0.0-test.2', '1.2.3', $true, 'test', '0.0.0-test.2'),
    @('v1.2.4', '1.2.3', $false, $null, $null),
    @('v1.2.3-rc.1', '1.2.4', $false, $null, $null),
    @('v1.2.3-test', '1.2.3', $false, $null, $null),
    @('v1.2.3-beta.1', '1.2.3', $false, $null, $null),
    @('v1.2.3-rc', '1.2.3', $false, $null, $null),
    @('v0.0.0-beta', '1.2.3', $false, $null, $null),
    @('v0.0.0-rc.1', '1.2.3', $false, $null, $null),
    @('1.2.3', '1.2.3', $false, $null, $null),
    @('v1.2', '1.2', $false, $null, $null),
    @('v1.2.3 ', '1.2.3', $false, $null, $null),
    @('v3.22', '3.22', $false, $null, $null)
  )
  foreach ($c in $cases) {
    $r = Test-ScvbReleaseTag $c[0] $c[1]
    $bad = ($r.Ok -ne $c[2]) -or ($c[2] -and (($r.Kind -ne $c[3]) -or ($r.Version -ne $c[4])))
    if ($bad) {
      Write-Host ("  [FAIL] tag='{0}' cmake={1}: Ok={2} Kind={3} Version={4} ({5})" -f $c[0], $c[1], $r.Ok, $r.Kind, $r.Version, $r.Message) -ForegroundColor Red
      $fail++
    } else {
      Write-Host ("  [ok] tag='{0}' cmake={1} -> Ok={2} {3}" -f $c[0], $c[1], $r.Ok, $r.Kind)
    }
  }
  if ($fail -gt 0) { Write-Host "check-release-tag self-test: $fail 格失败" -ForegroundColor Red; exit 1 }
  Write-Host "check-release-tag self-test: 全部 $($cases.Count + 2) 格通过" -ForegroundColor Green
  exit 0
}

if (-not $Tag) { Write-Host 'check-release-tag: 缺 -Tag' -ForegroundColor Red; exit 1 }
if (-not $CMakeFile) { $CMakeFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt' }
$cmakeVersion = Get-ScvbCMakeVersion ([IO.File]::ReadAllText($CMakeFile))
$r = Test-ScvbReleaseTag $Tag $cmakeVersion
if (-not $r.Ok) { Write-Host "check-release-tag: $($r.Message)" -ForegroundColor Red; exit 1 }
Write-Host "check-release-tag: $($r.Message)"
if ($env:GITHUB_OUTPUT) {
  "version=$($r.Version)" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "kind=$($r.Kind)" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "prerelease=$(([string]$r.Prerelease).ToLowerInvariant())" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
exit 0
