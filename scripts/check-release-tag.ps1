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

  # tag, cmake, 期望 Ok, 期望 Kind, 期望 Version, 期望 Prerelease(放行的格才比对后三项)。
  # Prerelease 决定 release.yml 建草稿时勾不勾 pre-release,所以每个放行格都钉住它。
  $cases = @(
    @('v1.2.3', '1.2.3', $true, 'final', '1.2.3', $false),
    @('v1.2.3-rc.1', '1.2.3', $true, 'rc', '1.2.3-rc.1', $true),
    @('v1.2.3-rc.12', '1.2.3', $true, 'rc', '1.2.3-rc.12', $true),
    # beta 与 rc 同一条规则(B 线 M15):X.Y.Z 等于 CMake 版本、勾 pre-release。
    # 第一格在 M15 之前的期望是「拒」(当时 beta 不是发版通道),这是规格变更,不是放宽判据。
    @('v1.2.3-beta.1', '1.2.3', $true, 'beta', '1.2.3-beta.1', $true),
    @('v1.2.3-beta.12', '1.2.3', $true, 'beta', '1.2.3-beta.12', $true),
    @('v0.10.0-beta.1', '0.10.0', $true, 'beta', '0.10.0-beta.1', $true),
    @('v0.0.0-test', '1.2.3', $true, 'test', '0.0.0-test', $true),
    @('v0.0.0-test.2', '1.2.3', $true, 'test', '0.0.0-test.2', $true),
    @('v1.2.4', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-rc.1', '1.2.4', $false, $null, $null, $null),
    @('v1.2.3-test', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-rc', '1.2.3', $false, $null, $null, $null),
    @('v0.0.0-beta', '1.2.3', $false, $null, $null, $null),
    @('v0.0.0-rc.1', '1.2.3', $false, $null, $null, $null),
    # beta 反例:版本不一致;0.0.0 不享受演练 tag 的「不比对版本」;序号缺失 / 多段 / 大写 / 别的预发布名。
    @('v1.2.3-beta.1', '1.2.4', $false, $null, $null, $null),
    @('v0.10.0-beta.1', '0.9.0', $false, $null, $null, $null),
    @('v0.0.0-beta.1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta.', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta.1.2', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta.x', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-BETA.1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-Beta.1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-alpha.1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-rc.1-beta.1', '1.2.3', $false, $null, $null, $null),
    @('v1.2.3-beta.1+build.5', '1.2.3', $false, $null, $null, $null),
    @("v1.2.3-beta.1`n", '1.2.3', $false, $null, $null, $null),
    @('1.2.3', '1.2.3', $false, $null, $null, $null),
    @('v1.2', '1.2', $false, $null, $null, $null),
    @('v1.2.3 ', '1.2.3', $false, $null, $null, $null),
    @("v1.2.3`n", '1.2.3', $false, $null, $null, $null),
    @('v3.22', '3.22', $false, $null, $null, $null)
  )
  foreach ($c in $cases) {
    $r = Test-ScvbReleaseTag $c[0] $c[1]
    $bad = ($r.Ok -ne $c[2]) -or ($c[2] -and (($r.Kind -ne $c[3]) -or ($r.Version -ne $c[4]) -or ($r.Prerelease -ne $c[5])))
    if ($bad) {
      Write-Host ("  [FAIL] tag='{0}' cmake={1}: Ok={2} Kind={3} Version={4} Prerelease={5} ({6})" -f $c[0], $c[1], $r.Ok, $r.Kind, $r.Version, $r.Prerelease, $r.Message) -ForegroundColor Red
      $fail++
    } else {
      if ($c[2]) { Write-Host ("  [ok] tag='{0}' cmake={1} -> Ok={2} {3} prerelease={4}" -f $c[0], $c[1], $r.Ok, $r.Kind, $r.Prerelease) }
      else { Write-Host ("  [ok] tag='{0}' cmake={1} -> Ok={2}" -f $c[0], $c[1], $r.Ok) }
    }
  }
  if ($fail -gt 0) { Write-Host "check-release-tag self-test: $fail 格失败" -ForegroundColor Red; exit 1 }
  Write-Host "check-release-tag self-test: 全部 $($cases.Count + 2) 格通过" -ForegroundColor Green
  exit 0
}

if (-not $Tag) { Write-Host 'check-release-tag: 缺 -Tag' -ForegroundColor Red; exit 1 }
$CMakeFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt'
$cmakeVersion = Get-ScvbCMakeVersion ([IO.File]::ReadAllText($CMakeFile))
$r = Test-ScvbReleaseTag $Tag $cmakeVersion
if (-not $r.Ok) { Write-Host "check-release-tag: $($r.Message)" -ForegroundColor Red; exit 1 }
Write-Host "check-release-tag: $($r.Message)"
if ($env:GITHUB_OUTPUT) {
  # 显式无 BOM:PS 5.1 的 Out-File -Encoding utf8 会带 BOM。
  $out = "version=$($r.Version)`nkind=$($r.Kind)`nprerelease=$(([string]$r.Prerelease).ToLowerInvariant())`n"
  [IO.File]::AppendAllText($env:GITHUB_OUTPUT, $out, (New-Object System.Text.UTF8Encoding($false)))
}
exit 0
