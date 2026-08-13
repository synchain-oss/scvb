<#
.SYNOPSIS  断言 docs/constitution/ 三份只读副本与 masterPlan/constitution/ 原件逐字节一致(仅允许文件头多一行只读注记)。
.DESCRIPTION
  比对方式:sha256。repo 副本 = 头注行 + 原件逐字节;本脚本剥离 repo 副本第一行后与 masterPlan 原件比对。
  宪法升版必须同步本比对(07 T01 / 06 §3.4:docs/constitution/ 是 review bot prompt 明文要读的冻结契约副本)。
.EXAMPLE    pwsh scripts/check-constitution-sync.ps1
.EXAMPLE    pwsh scripts/check-constitution-sync.ps1 -MasterPlanDir C:\path\to\masterPlan\constitution
#>
param(
  [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$MasterPlanDir = $env:SCVB_MASTERPLAN_CONSTITUTION
)

$ErrorActionPreference = 'Stop'

$files = @('ADR.md', 'params-v0.md', 'ipc-contract-v0.md')

if (-not $MasterPlanDir) {
  $candidates = @(
    (Join-Path $RepoRoot '..\SCVB\masterPlan\constitution'),
    (Join-Path $RepoRoot '..\masterPlan\constitution')
  )
  $MasterPlanDir = $candidates | Where-Object { Test-Path (Join-Path $_ 'ADR.md') } | Select-Object -First 1
}

if (-not $MasterPlanDir -or -not (Test-Path (Join-Path $MasterPlanDir 'ADR.md'))) {
  Write-Host '找不到 masterPlan/constitution 原件(已尝试相对路径)。' -ForegroundColor Yellow
  Write-Host '请用 -MasterPlanDir 指定,或设置环境变量 SCVB_MASTERPLAN_CONSTITUTION。' -ForegroundColor Yellow
  exit 1
}

$fail = $false
foreach ($f in $files) {
  $repoFile = Join-Path $RepoRoot (Join-Path 'docs\constitution' $f)
  $masterFile = Join-Path $MasterPlanDir $f

  if (-not (Test-Path $repoFile)) {
    Write-Host ("FAIL    {0} (repo 副本不存在)" -f $f) -ForegroundColor Red
    $fail = $true
    continue
  }
  if (-not (Test-Path $masterFile)) {
    Write-Host ("FAIL    {0} (masterPlan 原件不存在)" -f $f) -ForegroundColor Red
    $fail = $true
    continue
  }

  $repoBytes = [System.IO.File]::ReadAllBytes($repoFile)
  $masterBytes = [System.IO.File]::ReadAllBytes($masterFile)

  # 剥离 repo 副本的第一行(头注行,以第一个 LF 结尾)
  $nl = [System.Array]::IndexOf($repoBytes, [byte]10)
  if ($nl -lt 0) {
    Write-Host ("FAIL    {0} (repo 副本缺少头注换行)" -f $f) -ForegroundColor Red
    $fail = $true
    continue
  }
  $body = New-Object byte[] ($repoBytes.Length - $nl - 1)
  [System.Array]::Copy($repoBytes, $nl + 1, $body, 0, $body.Length)

  $sha = [System.Security.Cryptography.SHA256]::Create()
  $repoHash = [System.BitConverter]::ToString($sha.ComputeHash($body)).Replace('-', '').ToLowerInvariant()
  $masterHash = [System.BitConverter]::ToString($sha.ComputeHash($masterBytes)).Replace('-', '').ToLowerInvariant()

  if ($repoHash -eq $masterHash) {
    Write-Host ("PASS    {0}" -f $f) -ForegroundColor Green
  }
  else {
    Write-Host ("FAIL    {0} (repo 副本与 masterPlan 原件不一致,宪法升版必须同改)" -f $f) -ForegroundColor Red
    $fail = $true
  }
}

if ($fail) {
  Write-Host 'constitution sync: FAIL' -ForegroundColor Red
  exit 1
}
Write-Host 'constitution sync: 全部 PASS' -ForegroundColor Green
exit 0
