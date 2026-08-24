<#
.SYNOPSIS  双语文档的结构对等检查(12 §1.2)。
.DESCRIPTION
  比对两份文档的标题**序列**:只比 (层级, 序号),不比标题文本 —— 两边本来就是两种语言。
  任何一处层级不同或数量不同即非零退出,并指出是第几个标题、两边各在哪一行。

  为什么是这个形状(12 §1.2 列出的 v1 三个确定性缺陷):
    ① 旧版把 ```bash 围栏块里的 `# 注释` 当成标题。README 的安装/构建/快速上手三节必然带
       命令块,中英两版的注释数几乎不可能一致 —— 于是这个 required 检查会在无人改标题时红,
       或者在真漏了一节时因两边偏差恰好抵消而绿。本版**剔围栏**。
    ② 旧版只比数量、不比顺序、不检层级,`##` 改成 `###` 不会被发现。本版**比序列**。
    ③ 旧版只覆盖 README,而体量最大、承载九条安全红字的 USER_GUIDE 双语对零机检。
       本版**参数化**,由调用方指定文件对;check-readme-parity.ps1 一次跑两对。
.EXAMPLE   pwsh scripts/check-doc-parity.ps1 -A README.md -B README.zh-CN.md
.EXAMPLE   pwsh scripts/check-doc-parity.ps1 -A docs/USER_GUIDE.md -B docs/USER_GUIDE.zh-CN.md
#>
param(
  [Parameter(Mandatory)][string]$A,
  [Parameter(Mandatory)][string]$B
)

$ErrorActionPreference = 'Stop'

function Get-HeadingSeq([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) {
    Write-Error "找不到文件:$path"
    exit 1
  }
  $inFence = $false
  $seq = @()
  $ln = 0
  foreach ($line in (Get-Content -LiteralPath $path)) {
    $ln++
    # 围栏开合(``` 或 ~~~):围栏内的 `# 注释` 不是标题
    if ($line -match '^\s*(```|~~~)') { $inFence = -not $inFence; continue }
    if ($inFence) { continue }
    if ($line -match '^(#{1,3})\s') {
      $seq += [pscustomobject]@{ Level = $Matches[1].Length; Line = $ln; Text = $line.Trim() }
    }
  }
  return , $seq
}

$sa = Get-HeadingSeq $A
$sb = Get-HeadingSeq $B

$n = [Math]::Max($sa.Count, $sb.Count)
for ($i = 0; $i -lt $n; $i++) {
  $hasA = $i -lt $sa.Count
  $hasB = $i -lt $sb.Count
  $la = if ($hasA) { $sa[$i].Level } else { '(缺)' }
  $lb = if ($hasB) { $sb[$i].Level } else { '(缺)' }
  if ($la -ne $lb) {
    $whereA = if ($hasA) { "行 $($sa[$i].Line):$($sa[$i].Text)" } else { '(没有第 {0} 个标题)' -f ($i + 1) }
    $whereB = if ($hasB) { "行 $($sb[$i].Line):$($sb[$i].Text)" } else { '(没有第 {0} 个标题)' -f ($i + 1) }
    Write-Host "结构不对等 @第 $($i + 1) 个标题" -ForegroundColor Red
    Write-Host "  $A  层级=$la  $whereA"
    Write-Host "  $B  层级=$lb  $whereB"
    Write-Host "  两份文档的 h1/h2/h3 数量、顺序、层级必须逐个一致(12 §1.2);标题文本不比。"
    exit 1
  }
}

Write-Host ("[OK] {0} <-> {1}:{2} 个标题,层级序列一致" -f $A, $B, $sa.Count) -ForegroundColor Green
exit 0
