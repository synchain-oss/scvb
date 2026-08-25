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
    # 用 Write-Host 而非 Write-Error:$ErrorActionPreference = 'Stop' 会把 Write-Error
    # 变成终止性错误,下一行的 exit 1 根本执行不到 —— 退出码仍非零,但走的是异常路径,
    # 打出来的是一段 .NET 堆栈而不是这句人话。
    Write-Host "找不到文件:$path" -ForegroundColor Red
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
  # 两侧一律转成字符串再比:某一侧「缺」时占位符是字符串,另一侧是 [int] 层级号。
  # PowerShell 会把右操作数强制成左操作数的类型,而 `-ne` 这类相等运算在转换失败时
  # **返回「不相等」而不是抛错**(只有 `-gt` 这类序运算才会抛 Cannot convert)——
  # 所以原写法在 pwsh 7.6.5 实测两个方向都正常打印诊断并 exit 1(见 PR 记录)。
  # 但这份「不抛」依赖的是相等运算特有的宽松语义:哪天有人把判据改成序比较,
  # 或者 Level 改成别的类型,失效方式会是当场抛错而不是漏判。统一成字符串比较后,
  # 两侧类型恒定,判据与运算符的选择解耦。
  $la = if ($hasA) { [string]$sa[$i].Level } else { '(缺)' }
  $lb = if ($hasB) { [string]$sb[$i].Level } else { '(缺)' }
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
