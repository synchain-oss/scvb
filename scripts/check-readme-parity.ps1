<#
.SYNOPSIS  SCVB 的双语文档对等门禁 —— 一次跑完 README 与 USER_GUIDE 两对。
.DESCRIPTION
  这是 07(T39b/B05 的 gates 行)与 06 CI 按名字调用的入口。实际的比对逻辑在
  scripts/check-doc-parity.ps1(12 §1.2 的参数化重写版),本脚本只负责把 12 §1.2
  指定的**两对**文件喂给它:

    README.md            <-> README.zh-CN.md
    docs/USER_GUIDE.md   <-> docs/USER_GUIDE.zh-CN.md

  为什么留这个名字:07 的卡面与 06 的 CI 都写作 check-readme-parity.ps1,而 12 §1.2
  把脚本重写并改名为 check-doc-parity.ps1(参数化 + 覆盖 USER_GUIDE)。两边都按各自
  的名字调用,所以保留本文件作为唯一入口,逻辑不重复实现 —— 两份实现才是漂移的来源。
.EXAMPLE   pwsh scripts/check-readme-parity.ps1
#>
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$checker = Join-Path $PSScriptRoot 'check-doc-parity.ps1'

$pairs = @(
  @{ A = 'README.md'; B = 'README.zh-CN.md' },
  @{ A = 'docs/USER_GUIDE.md'; B = 'docs/USER_GUIDE.zh-CN.md' }
)

$failed = 0
foreach ($p in $pairs) {
  & $checker -A $p.A -B $p.B
  if ($LASTEXITCODE -ne 0) { $failed++ }
}

if ($failed -gt 0) {
  Write-Host ("check-readme-parity: {0} 对文档结构不对等。" -f $failed) -ForegroundColor Red
  exit 1
}

Write-Host 'check-readme-parity: 2 对双语文档结构对等。' -ForegroundColor Green
exit 0
