<#
.SYNOPSIS  断言 ctest 真正吃的那份清单里,每个 add_test 都带 TIMEOUT 属性(读生成物 CTestTestfile.cmake)。
.DESCRIPTION
  [SL-325] 病灶:「七套全部有 TIMEOUT 属性」在 SL-320 收口时是**当下事实,没有任何执行者**。
  新加一个测试目标、忘了写 `set_tests_properties(... TIMEOUT ...)`,它会静默退回 CTest 默认的
  1500s —— 而这条退化在本机与 CI 上**都不会红**:命令行 `--timeout 300` 只对没有属性的测试生效,
  于是新测试「有个上界」看起来是对的,只是那个上界在出包硬门上是 1500s(gates 那边 300s)。
  同一个「上界」再次分叉成两个真源,正是 SL-320 修掉的那个洞的复发形态。

  **为什么读生成物,不读 CMakeLists**:属性可以写在任何一个 `CMakeLists.txt` 里
  (`scvb_ipc_tests` 的 900 就在 `tests/ipc/CMakeLists.txt`,不在 `tests/CMakeLists.txt`),
  也可以由 `SCVB_BUILD_IPC_TESTS` 这类开关决定进不进集合。手边那个 CMakeLists 回答不了
  「谁有属性」——`build*/**/CTestTestfile.cmake` 才是 ctest 真正吃的那份。本仓在
  `tests/CMakeLists.txt` 与 `scripts/gates.ps1` 两处注释里都写过这句话,这个脚本是它的执行者。

  判负口径(三条都要,缺一条门禁就会静默变绿):
    · 有 add_test 没有 TIMEOUT   ⇒ 判负,点名;
    · 一个 CTestTestfile 都找不到 ⇒ 判负(不是跳过:没配置过的构建目录会让判据恒真);
    · 找到文件但一个测试都没有   ⇒ 判负(空集合让「全部都有属性」恒真)。

  排除 `_deps/`:那是 FetchContent 拉下来的第三方(Catch2),它有没有 TIMEOUT 不归本仓管,
  也改不动。今天它一个 add_test 都没有;写死排除是为了「哪天它有了」不会变成一条改不掉的红。
.EXAMPLE    pwsh scripts/check-ctest-timeouts.ps1 -BuildDir build
.EXAMPLE    pwsh scripts/check-ctest-timeouts.ps1 -SelfTest
#>
param(
  [string]$BuildDir = 'build',
  [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# ─────────────────────────────────────────────────────────────────────────────
# 判据本体。**生产路径与 --SelfTest 共用这一个函数** —— 自测若走另一份实现,
# 它证明的是那一份能用,而不是门禁能用(本仓「测接线不只测零件」那一族)。
# ─────────────────────────────────────────────────────────────────────────────
function Get-CtestTimeoutReport {
  param([string[]]$Contents)

  $declared = New-Object 'System.Collections.Generic.HashSet[string]'
  $withTimeout = New-Object 'System.Collections.Generic.HashSet[string]'

  foreach ($text in $Contents) {
    foreach ($m in [regex]::Matches($text, 'add_test\(\[=\[([^\]]+)\]=\]')) {
      [void]$declared.Add($m.Groups[1].Value)
    }
    foreach ($m in [regex]::Matches($text, 'set_tests_properties\(\[=\[([^\]]+)\]=\]\s+PROPERTIES\s')) {
      $name = $m.Groups[1].Value
      # 属性尾巴要**按记号切**,不能拿 `TIMEOUT` 直接在整行上 grep:同一行里
      # `_BACKTRACE_TRIPLES` 的值是一串带引号的路径,路径里出现 `TIMEOUT` 三个字母
      # (某个 agent 的工作目录叫 `…/timeout-repro/…`)就会把一个**没有**上界的测试
      # 判成有 —— 门禁静默变绿。所以逐对读 `名 值`,只认落在**属性名**位上的那个词。
      $tail = $text.Substring($m.Index + $m.Length)
      $pos = 0
      while ($pos -lt $tail.Length) {
        $pair = [regex]::Match($tail.Substring($pos), '^\s*([A-Za-z_][A-Za-z0-9_]*)\s+("(?:[^"\\]|\\.)*"|[^\s)]+)')
        if (-not $pair.Success) { break }
        if ($pair.Groups[1].Value -eq 'TIMEOUT') { [void]$withTimeout.Add($name); break }
        $pos += $pair.Length
      }
    }
  }

  $missing = @($declared | Where-Object { -not $withTimeout.Contains($_) } | Sort-Object)
  return [pscustomobject]@{
    Declared    = @($declared | Sort-Object)
    WithTimeout = @($withTimeout | Sort-Object)
    Missing     = $missing
  }
}

# ─────────────────────────────────────────────────────────────────────────────
if ($SelfTest) {
  # 夹具**拼装**,不从仓里读、也不写进任何被扫描的路径:这个脚本自己不在扫描面里,
  # 但夹具里那段「缺 TIMEOUT 的 add_test」若落进真的 CTestTestfile,判据就该红。
  $q = [char]34
  $mk = {
    param([string]$name, [string]$props)
    ('add_test([=[{0}]=] "C:/x/{0}.exe")' -f $name) + "`n" +
    $(if ($props) { ('set_tests_properties([=[{0}]=] PROPERTIES  {1})' -f $name, $props) + "`n" } else { '' })
  }

  $fixOk      = (& $mk 'fix_has_timeout' ('TIMEOUT {0}30{0} _BACKTRACE_TRIPLES {0}C:/x/CMakeLists.txt;1;add_test;{0}' -f $q))
  $fixMissing = (& $mk 'fix_no_timeout'  ('WILL_FAIL {0}FALSE{0} _BACKTRACE_TRIPLES {0}C:/x/CMakeLists.txt;2;add_test;{0}' -f $q))
  # 第三格钉的是**记号切分**那一条:`TIMEOUT` 只出现在 `_BACKTRACE_TRIPLES` 的**值**里
  # (真实形态 —— 工作目录名里带这几个字母)。按整行 grep 会把它判成「有上界」。
  $fixInValue = (& $mk 'fix_timeout_in_path' ('WILL_FAIL {0}FALSE{0} _BACKTRACE_TRIPLES {0}C:/agents/TIMEOUT-repro/CMakeLists.txt;3;add_test;{0}' -f $q))

  $failures = @()
  $r1 = Get-CtestTimeoutReport -Contents @($fixOk)
  if ($r1.Missing.Count -ne 0) { $failures += ('有 TIMEOUT 的夹具被误判成缺失:{0}' -f ($r1.Missing -join ', ')) }
  $r2 = Get-CtestTimeoutReport -Contents @($fixMissing)
  if ($r2.Missing -notcontains 'fix_no_timeout') { $failures += '缺 TIMEOUT 的夹具没有被抓到 —— 判据没有牙' }
  $r3 = Get-CtestTimeoutReport -Contents @($fixInValue)
  if ($r3.Missing -notcontains 'fix_timeout_in_path') { $failures += 'TIMEOUT 出现在属性值里被当成属性名 —— 记号切分失效' }
  # 混合一格:两个夹具同时喂进去,只有该红的那个红(避免「一红全红」蒙混过关)。
  $r4 = Get-CtestTimeoutReport -Contents @($fixOk, $fixMissing)
  if (($r4.Missing -join ',') -ne 'fix_no_timeout') { $failures += ('混合夹具的缺失集合不对:[{0}]' -f ($r4.Missing -join ', ')) }

  if ($failures.Count -gt 0) {
    Write-Host '  [FAIL] check-ctest-timeouts --self-test:' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host ('    ' + $_) -ForegroundColor Red }
    exit 1
  }
  Write-Host '  check-ctest-timeouts --self-test:4 格全过(有属性不误报 / 缺属性抓得到 / 值里的 TIMEOUT 不算 / 混合只红该红的)'
  exit 0
}

# ─────────────────────────────────────────────────────────────────────────────
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
if (-not (Test-Path $buildPath)) {
  Write-Host ('  [FAIL] 构建目录不存在:{0} —— 这不是跳过:没有生成物就无从判定谁有上界' -f $buildPath) -ForegroundColor Red
  exit 1
}

$files = @(Get-ChildItem -Path $buildPath -Filter 'CTestTestfile.cmake' -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -notmatch '[\/]_deps[\/]' })
if ($files.Count -eq 0) {
  Write-Host ('  [FAIL] {0} 下找不到 CTestTestfile.cmake —— 先跑 configure(gate 4)' -f $buildPath) -ForegroundColor Red
  exit 1
}

$report = Get-CtestTimeoutReport -Contents @($files | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw })
if ($report.Declared.Count -eq 0) {
  Write-Host ('  [FAIL] 读了 {0} 份 CTestTestfile.cmake,一个 add_test 都没有 —— 空集合会让本判据恒真' -f $files.Count) -ForegroundColor Red
  exit 1
}
if ($report.Missing.Count -gt 0) {
  Write-Host ('  [FAIL] 下列测试没有 TIMEOUT 属性,会静默吃 CTest 默认的 1500s:') -ForegroundColor Red
  $report.Missing | ForEach-Object { Write-Host ('    ' + $_) -ForegroundColor Red }
  Write-Host '    修法:在它的 add_test 旁写 set_tests_properties(<名> PROPERTIES TIMEOUT <秒>),' -ForegroundColor Yellow
  Write-Host '    数按 max(最慢观测 x 3, 30s) 取(口径见 tests/CMakeLists.txt 里 scvb_tests 上方那段)。' -ForegroundColor Yellow
  exit 1
}

Write-Host ('  ctest 上界属性完备:{0} 套全部有 TIMEOUT({1})' -f $report.Declared.Count, ($report.Declared -join '、'))
exit 0
