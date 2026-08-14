<#
.SYNOPSIS  SCVB 源文件 SPDX 头门禁 —— 断言「能加头的」源文件首部含 SPDX-License-Identifier。
.DESCRIPTION
  遍历 git 追踪的 .cpp/.h/.hpp/.cc/.js(排除 web/js/juce/** 第三方与 third_party/**),
  断言每个文件首 5 行含 SPDX-License-Identifier(标识按 U1 = GPL-3.0-or-later,12 §1.3)。
  REUSE.toml 的 [[annotations]] path=["**"] 兜底只为让 reuse lint 可达,不是免加头的许可;
  能加头的源文件仍必须逐个加头,由本脚本单独把关(06 §5.1 gate 3c 注)。
.EXAMPLE   pwsh scripts/check-spdx.ps1
#>
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$LicenseId = 'GPL-3.0-or-later'
$tag = 'SPDX-License-Identifier'
$needle = "$tag" + ': ' + $LicenseId

$files = @(git ls-files '*.cpp' '*.h' '*.hpp' '*.cc' '*.js' |
  Where-Object { $_ -notmatch '^web/js/juce/' -and $_ -notmatch '^third_party/' })

if ($files.Count -eq 0) {
  Write-Error 'check-spdx: 未发现可加头的源文件'
  exit 1
}

$missing = @()
foreach ($f in $files) {
  $head = @(Get-Content -LiteralPath $f -TotalCount 5 -ErrorAction SilentlyContinue)
  $hit = @($head | Select-String -SimpleMatch $needle).Count -gt 0
  if (-not $hit) {
    $missing += $f
    Write-Host ("  [MISS] {0}" -f $f) -ForegroundColor Red
  }
}

if ($missing.Count -gt 0) {
  Write-Host ''
  Write-Host ("check-spdx: {0} 个源文件缺失 SPDX 头(要求 {1}):" -f $missing.Count, $LicenseId) -ForegroundColor Red
  $missing | ForEach-Object { Write-Host ("  " + $_) }
  exit 1
}

Write-Host ("check-spdx 通过: {0} 个源文件全部含 SPDX 头" -f $files.Count) -ForegroundColor Green
exit 0
