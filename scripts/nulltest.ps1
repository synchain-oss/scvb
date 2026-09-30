<#
.SYNOPSIS  scvb_nulltest 薄封装 —— 定位 wav、读宿主 pan law 记录、调用工具、追加 nulltest-log.md。
.DESCRIPTION
  10-validation §4.2:找到两个 wav、读宿主 pan law 记录、调用 scvb_nulltest、把结果追加到
  docs/validation/audio/nulltest-log.md(含素材 sha256 指纹)。并行 agent 用 -BuildDir 隔离构建目录。
.EXAMPLE    pwsh scripts/nulltest.ps1 ref_A.wav test_B.wav -PanLawDb -3.01 -Align -BuildDir build-T01c
#>
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Ref,
  [Parameter(Mandatory = $true, Position = 1)][string]$Test,
  [double]$PanLawDb = 0.0,
  [switch]$Align,
  [string]$BuildDir = 'build',
  [string]$Config = 'Release',
  [string]$Json = ''
)

$ErrorActionPreference = 'Continue'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

# 1) 定位 scvb_nulltest.exe(并行 agent 靠 -BuildDir 隔离;优先按配置目录,失败再回退递归查找)。
$exe = $null
$candidates = @(
  (Join-Path $BuildDir "tests\tools\$Config\scvb_nulltest.exe"),
  (Join-Path $BuildDir 'tests\tools\scvb_nulltest.exe'),
  (Join-Path $BuildDir "$Config\tests\tools\scvb_nulltest.exe")
)
foreach ($c in $candidates) {
  if (Test-Path $c) { $exe = $c; break }
}
if (-not $exe) {
  $found = Get-ChildItem -Path $BuildDir -Recurse -Filter 'scvb_nulltest.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { $exe = $found.FullName }
}
if (-not $exe) {
  Write-Host "未找到 scvb_nulltest.exe —— 先在 $BuildDir 构建 SCVB_BUILD_TOOLS 目标。" -ForegroundColor Red
  exit 1
}

# 2) pan law 补偿:宿主 pan law 记录是衰减(负 dB),对 test(B)施加的补偿增益 = 取反。
$gainDb = -$PanLawDb

# 3) 调用工具。
$toolArgs = @($Ref, $Test)
if ($gainDb -ne 0.0) { $toolArgs += '--gain-db'; $toolArgs += ([string]$gainDb) }
if ($Align) { $toolArgs += '--align' }
if ($Json) { $toolArgs += '--json'; $toolArgs += $Json }

Write-Host ("scvb_nulltest: {0}" -f $exe)
Write-Host ("宿主 pan law {0} dB -> 补偿 --gain-db {1}" -f $PanLawDb, $gainDb)
$out = & $exe @toolArgs 2>&1
$exitCode = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($exitCode -ne 0) {
  Write-Host ("scvb_nulltest 失败 (exit {0})。" -f $exitCode) -ForegroundColor Red
  exit $exitCode
}

# 4) 追加 nulltest-log.md(含素材 sha256 指纹)。
$logDir = Join-Path $RepoRoot 'docs\validation\audio'
$logFile = Join-Path $logDir 'nulltest-log.md'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
if (-not (Test-Path $logFile)) {
  Set-Content -Path $logFile -Encoding UTF8 -Value @(
    '# null test 日志',
    '',
    '每次 null test 的结果与素材指纹(10-validation §4.2)。由 scripts/nulltest.ps1 追加。',
    ''
  )
}

$shaRef = (Get-FileHash -Path $Ref -Algorithm SHA256).Hash.ToLowerInvariant()
$shaTest = (Get-FileHash -Path $Test -Algorithm SHA256).Hash.ToLowerInvariant()
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("## $stamp")
$lines.Add('')
$lines.Add("- ref: $Ref (sha256 $shaRef)")
$lines.Add("- test: $Test (sha256 $shaTest)")
$lines.Add("- 宿主 pan law: $PanLawDb dB(补偿 --gain-db $gainDb;对齐: $Align)")
$lines.Add('')
$lines.Add('结果:')
foreach ($ln in @($out)) { $lines.Add("    $ln") }
$lines.Add('')

Add-Content -Path $logFile -Encoding UTF8 -Value $lines
Write-Host ("已追加日志: {0}" -f $logFile)
exit 0
