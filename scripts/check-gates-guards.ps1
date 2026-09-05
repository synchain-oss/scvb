<#
.SYNOPSIS  断言 gates.ps1 里每个外部命令调用点都落在某个存在性守卫的 if/else 作用域内(读 AST,不靠名字清单)。
.DESCRIPTION
  [SL-331] 病灶:#211(gate 5b)与 #214(整族收)两次都栽在同一件事上 —— PowerShell 找不到
  外部命令时抛 CommandNotFoundException,默认 `ErrorActionPreference=Continue` 下**不更新**
  `$LASTEXITCODE`,它保留**上一条外部命令**的值。于是「跑完看 `$LASTEXITCODE` 判 PASS/FAIL」
  的调用点在命令缺席时**沿用上一条的 0 判绿**:判据一行没跑过,汇总表却写着 PASS。

  #214 把当时的 6 处补齐了,但那一轮的收全**靠的是人肉扫 + 一份手抄清单**,而且**漏了两次**:
    · 第一遍扫 `& <命令>` 这个**语法形态**,漏掉了裸调用(`npx …` / `pipx run …`);
    · 第二遍补扫裸调用时用**手打的命令名清单**,`npx` / `pipx` 两个名字不在里面。
  两次漏的方式不同 —— 所以问题不是「下次更仔细」,而是**按名字列清单这件事本身不可靠**。
  本脚本就是那份清单的执行者:清单不再由人维护,由 AST 现算。

  判据(三条,任一不满足即判负):
    · **每个外部命令调用点**都要落在**针对同一个命令**的存在性守卫的 if/else 作用域内;
    · **豁免必须显式且带理由**(见下面 $Exemptions),按调用点的**文本指纹**匹配,不按名字;
    · **豁免不得陈旧**:清单里有一条匹配不到任何调用点,即判负(否则守卫会随代码漂移而失效)。

  「外部命令」怎么认(**不靠名字清单**):
    · 名字式调用 —— `Get-Command` 解析成 `Application` / `ExternalScript` 的,或**根本解析不到**的。
      ⚠ `ExternalScript` 那一档不能省:本机 `npx` 解析到的是 `npx.ps1`(**不是** Application),
        只按 Application 过滤它会从清单里消失 —— #214 收口时我照着自己写的配方跑,就这么又漏了一次。
      ⚠ 「解析不到」也算外部,是**故意 fail-closed**:`pipx` / `reuse` 没装的机器上正是靠这一档
        把它们留在判据面里 —— 那恰恰是本族缺陷的核心场景。代价是本文件自己定义的函数也会落进来,
        所以下面先用 `FunctionDefinitionAst` 把它们从**同一棵 AST** 里排掉,而不是收紧这一档。
    · 变量式调用(`& $var`)—— 变量赋值右值里含 `Get-Command`、或是路径/字符串的,算外部;
      右值是**脚本块**(`$enumLeftover = { … }`)的不算,那是本文件自己的闭包。
    ⚠ **边界:经 cmdlet 间接启动的外部命令不在面内。** `Start-Process -FilePath $nodeCmd.Source`
      (`gates.ps1:836`)这种由 cmdlet 代启的,AST 里是一次 `Start-Process` 调用、不是外部命令调用点,
      本判据看不见。那一处本身没问题(`$nodeCmd` 已有守卫),但「不靠名字清单」这个卖点在这条上
      **确有缺口** —— 照实写出来,别让读者以为它是全称保证。

  「守卫」怎么认:某个祖先 `IfStatementAst` 的**任一分支条件**里
    · 出现 `Get-Command <本命令>`(内联形态,如 gate 3c 的 `if (Get-Command reuse …)`),或
    · 引用了**由 `Get-Command <本命令>` 赋值**的变量(如 `$pwshCmd` / `$nodeCmd`),或
    · (变量式调用)引用了**被调用的那个变量本身**(如 `Test-Path $vswhere`)。
  **要求命令名相符**,不接受「别人的守卫」:`npx` 落在 `if ($nodeCmd)` 里不算数 —— npx 随 node 装,
  但只装 node 而 npm 的 shim 没进 PATH 时 `$nodeCmd` 为真而 `npx` 不在,#214 为这条单独判了 npx。
.EXAMPLE    pwsh scripts/check-gates-guards.ps1
.EXAMPLE    pwsh scripts/check-gates-guards.ps1 -SelfTest
#>
param(
  [string]$Path,
  [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# ─────────────────────────────────────────────────────────────────────────────
# 豁免清单。**按文本指纹匹配,不按命令名** —— 按名字豁免的话,同名的新调用点会跟着白拿豁免。
# 每条必须有非空 `Reason`;匹配不到任何调用点的陈旧条目**判负**。
#
# 这三处是 #214 分类里的**③类:输出判据 + 缺席时已经判负**,方向偏红,包一层壳反而多余:
#   · `cmake --version`        —— 空输出 ⇒ `$ok = $false`;
#   · `clang-format --version` —— 空串不匹配 `18.1.8` ⇒ 判负;
#   · `git ls-files`           —— 空集合 ⇒ gate 2 判负。
# ⚠ 这份清单**暂居本文件**:豁免标记本该写在 `gates.ps1` 各调用点旁,但本卡与 seg-r4 的 SL-322
#   同文件冲突,统筹要求本卡先不动 `gates.ps1`。SL-322 合入后把标记搬进那边、这份清单相应缩短。
$Exemptions = @(
  @{ Snippet = 'cmake --version'; Hits = 1; Reason = '③类:输出判据 —— 空输出即 $ok = $false(gate 1),缺席方向偏红' }
  @{ Snippet = 'clang-format --version'; Hits = 1; Reason = '③类:输出判据 —— 空串不匹配 18.1.8 即判负(gate 1),缺席方向偏红' }
  @{ Snippet = 'git ls-files'; Hits = 1; Reason = '③类:输出判据 —— 空集合即 gate 2 判负,缺席方向偏红' }
)

# ─────────────────────────────────────────────────────────────────────────────
# 判据本体。**生产路径与 -SelfTest 共用这一个函数** —— 自测若走另一份实现,
# 它证明的是那一份能用,而不是门禁能用。
# 返回:@{ Sites = @(每个外部调用点); Guarded / Unguarded / Exempted / StaleExemptions }
# ─────────────────────────────────────────────────────────────────────────────
function Get-GatesGuardReport {
  # `$Resolver` 把「名字 -> 命令类型集合」这一层**注入进来**:生产路径用 Get-Command,
  # 自测可以喂一个返回多结果的假解析器 —— 否则「多结果」这一格只能靠本机 PATH 碰运气,
  # 而本判据的卖点之一正是「结论不随机器漂移」。
  param(
    [string]$Source,
    [object[]]$Exemptions = @(),
    [scriptblock]$Resolver = { param($n) @(Get-Command $n -ErrorAction SilentlyContinue) }
  )

  $ast = [System.Management.Automation.Language.Parser]::ParseInput($Source, [ref]$null, [ref]$null)

  # 本文件自己定义的函数 —— 从同一棵 AST 里收,不写名字清单。
  $localFuncs = @($ast.FindAll({ param($n)
        $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
      ForEach-Object { $_.Name })

  # 赋值分析:哪些变量是「Get-Command 出来的守卫变量」,哪些是脚本块。
  $guardVarToCmd = @{}   # $pwshCmd -> pwsh
  $scriptBlockVars = New-Object 'System.Collections.Generic.HashSet[string]'
  $pathLikeVars = New-Object 'System.Collections.Generic.HashSet[string]'
  foreach ($asn in $ast.FindAll({ param($n)
        $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)) {
    $lhs = $asn.Left
    if ($lhs -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
    $name = $lhs.VariablePath.UserPath
    $rhs = $asn.Right
    if ($rhs.Extent.Text -match '^\s*\{') { [void]$scriptBlockVars.Add($name); continue }
    $gc = @($rhs.FindAll({ param($n)
          $n -is [System.Management.Automation.Language.CommandAst] -and
          $n.GetCommandName() -eq 'Get-Command' }, $true))
    if ($gc.Count -gt 0) {
      $target = $null
      foreach ($el in $gc[0].CommandElements | Select-Object -Skip 1) {
        if ($el -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
          $el.Value -notmatch '^-') { $target = $el.Value; break }
      }
      if ($target) { $guardVarToCmd[$name] = $target }
      [void]$pathLikeVars.Add($name)
      continue
    }
    # 路径味道的赋值(Join-Path / 含盘符或斜杠的字符串)也当外部可执行体处理。
    if ($rhs.Extent.Text -match 'Join-Path|\.exe|[A-Za-z]:\\|/') { [void]$pathLikeVars.Add($name) }
  }

  # 某个 if 分支条件里出现的「守卫记号」:Get-Command 的目标名 + 被引用的变量名。
  # 用**普通数组**,不用 HashSet、也不做嵌套函数 —— PowerShell 从函数里 `return` 集合会把它
  # **展开**成管道元素,调用方拿到 object[] 后 `.Contains()` 走另一套类型比较、恒不命中。
  # 这一族坑(集合展开 / PSObject 包装 / List 进 pscustomobject)本卡已经踩到三次,索性都避开。
  $condTokens = {
    param($IfAst)
    $out = @()
    foreach ($clause in $IfAst.Clauses) {
      $cond = $clause.Item1
      foreach ($v in $cond.FindAll({ param($n)
            $n -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
        $out += ('$' + $v.VariablePath.UserPath)
      }
      foreach ($c in $cond.FindAll({ param($n)
            $n -is [System.Management.Automation.Language.CommandAst] -and
            $n.GetCommandName() -eq 'Get-Command' }, $true)) {
        foreach ($el in $c.CommandElements | Select-Object -Skip 1) {
          if ($el -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
            $el.Value -notmatch '^-') { $out += $el.Value; break }
        }
      }
    }
    return , @($out)
  }

  $sites = New-Object 'System.Collections.Generic.List[object]'
  foreach ($c in $ast.FindAll({ param($n)
        $n -is [System.Management.Automation.Language.CommandAst] }, $true)) {
    $name = $c.GetCommandName()
    $need = $null      # 这个调用点需要哪个「守卫记号」
    $label = $null

    if ($name) {
      if ($localFuncs -contains $name) { continue }
      # ⚠ 这里**不能**写 `$resolved.CommandType -notin 'Application','ExternalScript'`(#217 复审【重要】):
      #   `-in` / `-notin` 的左操作数是集合时,PowerShell **不做逐元素比较**,而是拿整个数组
      #   去和右侧每个元素 `-eq` —— 实测 `@('ExternalScript','Application') -notin
      #   'Application','ExternalScript'` 是 **True**。于是某台机器上 `npx` 同时解析出
      #   `npx.ps1` 与 `npx.cmd` 时,这个调用点会被**静默移出判据面**,而汇总照打「守卫完备」:
      #   判据没跑却记 PASS,正是本卡要根除的那一族,只是换了个位置。逐元素判。
      $resolved = @(& $Resolver $name)
      if ($resolved.Count -gt 0 -and
        -not ($resolved.CommandType -contains 'Application') -and
        -not ($resolved.CommandType -contains 'ExternalScript')) { continue }
      $need = $name
      $label = $name
    }
    else {
      # 变量式调用:`& $var …`
      $first = $c.CommandElements[0]
      if ($first -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
      $vn = $first.VariablePath.UserPath
      if ($scriptBlockVars.Contains($vn)) { continue }          # 本文件自己的闭包,不是外部命令
      if (-not $pathLikeVars.Contains($vn)) { continue }        # 认不出是可执行体,不判
      $need = '$' + $vn
      $label = '$' + $vn
    }

    # 往上找守卫
    $guarded = $false
    $by = $null
    $node = $c.Parent
    while ($node -ne $null) {
      if ($node -is [System.Management.Automation.Language.IfStatementAst]) {
        # ⚠ 这里**不能**再包 `@()`:脚本块里已经用 `, @($out)` 防了展开,外面再包一层就得到
        #   `@( @(…) )` —— `-contains` 于是拿整个内层数组去比,恒不命中(实测 tokens 打出来是
        #   `System.Object[]`)。防展开与再包一层,两个方向都会坏,只能二选一。
        $tokens = & $condTokens $node
        if ($tokens -contains $need) { $guarded = $true; $by = $need }
        else {
          # 守卫变量形态:条件里引用了某个 $xxxCmd,而它是 Get-Command <本命令> 出来的
          foreach ($t in $tokens) {
            if ($t -like '$*') {
              $gv = $t.Substring(1)
              if ($guardVarToCmd.ContainsKey($gv) -and $guardVarToCmd[$gv] -eq $need) {
                $guarded = $true; $by = $t; break
              }
            }
          }
        }
        if ($guarded) { break }
      }
      $node = $node.Parent
    }

    $sites.Add([pscustomobject]@{
        Name    = $label
        Line    = $c.Extent.StartLineNumber
        Text    = ($c.Extent.Text -replace '\s+', ' ')
        Guarded = $guarded
        GuardBy = $by
      })
  }

  # 豁免匹配(按文本指纹)。用 `行号|文本` 当键去重,不靠对象同一性 ——
  # PSCustomObject 的 `-contains` 走类型比较,在这里会抛 "Argument types do not match"。
  $exemptedKeys = New-Object 'System.Collections.Generic.HashSet[string]'
  $stale = @()
  $unguarded = @($sites.ToArray() | Where-Object { -not $_.Guarded })
  # ⚠ 豁免要钉**命中几处**,不能只钉「至少一处」(#217 复审【重要】):只钉「至少一处」的话,
  #   明天谁在别处再写一处**同文本、无守卫**的调用,`-like` 会把它一并吞掉 —— 新调用点白拿豁免、
  #   门禁全绿,而它恰恰会踩回 `$LASTEXITCODE` 沿用上一条的老坑。豁免会随代码**自动变宽**,
  #   而这正是本卡开篇批判的「清单悄悄失去精度、还成为下一个人的依据」。
  foreach ($ex in $Exemptions) {
    $want = if ($null -ne $ex.Hits) { [int]$ex.Hits } else { 1 }
    $hit = @($unguarded | Where-Object { $_.Text -like ('*' + $ex.Snippet + '*') })
    if ($hit.Count -ne $want) {
      $stale += , [pscustomobject]@{ Snippet = $ex.Snippet; Reason = $ex.Reason; Want = $want; Got = $hit.Count }
      continue
    }
    foreach ($h in $hit) { [void]$exemptedKeys.Add(('{0}|{1}' -f $h.Line, $h.Text)) }
  }
  # ⚠ 键要显式转 [string] 再喂 HashSet.Contains:PowerShell 会把 `-f` 的结果按 PSObject 传进去,
  #   `HashSet[string].Contains` 于是抛 "Argument types do not match"(而且报在下面构造对象那一行,
  #   与真正出错的表达式对不上,查起来很费劲)。
  $stillUnguarded = @($unguarded | Where-Object { -not $exemptedKeys.Contains([string]('{0}|{1}' -f $_.Line, $_.Text)) })
  $exempted = @($unguarded | Where-Object { $exemptedKeys.Contains([string]('{0}|{1}' -f $_.Line, $_.Text)) })

  # ⚠ `List[object]` 要先 `.ToArray()`:直接把 `@($list)` 放进 `[pscustomobject]@{}` 会抛
  #   "Argument types do not match",而错误报在构造那一行、不指向真正出问题的字段。
  $sitesArr = $sites.ToArray()

  return [pscustomobject]@{
    Sites           = $sitesArr
    Guarded         = @($sitesArr | Where-Object { $_.Guarded })
    Unguarded       = $stillUnguarded
    Exempted        = $exempted
    StaleExemptions = @($stale)
    LocalFuncs      = @($localFuncs)
  }
}

# ─────────────────────────────────────────────────────────────────────────────
if ($SelfTest) {
  # 夹具一律**拼装**,并且喂给**生产路径同一个函数**。
  $q = [char]34
  $fails = @()
  # 夹具里凡是用到 `Set-Gate` 的,都要**在夹具里定义它** —— 真 gates.ps1 里它是本文件定义的函数,
  # 夹具不定义,判据就会(正确地)把它当成一处没有守卫的外部命令,于是自测红在夹具而不是判据上。
  $defSetGate = 'function Set-Gate { param($a, $b) }' + [Environment]::NewLine

  # ① 无守卫的外部调用 ⇒ 必须被抓
  $f1 = "npx --yes prettier --check ." + [Environment]::NewLine
  $r1 = Get-GatesGuardReport -Source $f1
  if (@($r1.Unguarded).Count -ne 1) { $fails += ('① 无守卫的 npx 没被抓到(实得 {0} 条)' -f @($r1.Unguarded).Count) }

  # ② 有守卫(变量形态)⇒ 不得误报
  $f2 = $defSetGate + '$npxCmd = Get-Command npx -ErrorAction SilentlyContinue' + [Environment]::NewLine +
  'if (-not $npxCmd) { Set-Gate ' + "'x'" + ' $false } else { npx --yes prettier --check . }'
  $r2 = Get-GatesGuardReport -Source $f2
  if (@($r2.Unguarded).Count -ne 0) { $fails += ('② 有守卫的 npx 被误判成无守卫') }

  # ③ 有守卫(内联形态)⇒ 不得误报
  $f3 = $defSetGate + 'if (Get-Command reuse -ErrorAction SilentlyContinue) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r3 = Get-GatesGuardReport -Source $f3
  if (@($r3.Unguarded).Count -ne 0) { $fails += '③ 内联 Get-Command 守卫没被认出来' }

  # ④ **别人的守卫不算数**:npx 落在 $nodeCmd 的 if 里必须仍判无守卫
  $f4 = '$nodeCmd = Get-Command node -ErrorAction SilentlyContinue' + [Environment]::NewLine +
  'if ($nodeCmd) { npx --yes prettier --check . }'
  $r4 = Get-GatesGuardReport -Source $f4
  if (@($r4.Unguarded).Count -ne 1) { $fails += '④ npx 借用 $nodeCmd 的守卫被放过了(命令名没对上就不该算)' }

  # ⑤ 本文件自己定义的函数不得混进外部清单
  $f5 = 'function Set-Gate { param($a, $b) }' + [Environment]::NewLine + 'Set-Gate ' + "'x'" + ' $true'
  $r5 = Get-GatesGuardReport -Source $f5
  if (@($r5.Sites).Count -ne 0) { $fails += ('⑤ 本地函数被当成外部命令(实得 {0} 处)' -f @($r5.Sites).Count) }

  # ⑥ 脚本块变量的 `& $var` 不是外部命令
  $f6 = '$cb = { param($x) $x }' + [Environment]::NewLine + '& $cb 1'
  $r6 = Get-GatesGuardReport -Source $f6
  if (@($r6.Sites).Count -ne 0) { $fails += '⑥ 脚本块闭包被当成外部命令' }

  # ⑦ 变量式外部调用要认守卫(Test-Path $var 形态)
  $f7 = $defSetGate + '$exe = Join-Path $root ' + "'x.exe'" + [Environment]::NewLine +
  'if (Test-Path $exe) { & $exe --version } else { Set-Gate ' + "'x'" + ' $false }'
  $r7 = Get-GatesGuardReport -Source $f7
  if (@($r7.Unguarded).Count -ne 0) { $fails += '⑦ 变量式调用的 Test-Path 守卫没被认出来' }

  # ⑧ 豁免要按**文本指纹**、且陈旧条目判负
  $f8 = 'cmake --version'
  $r8 = Get-GatesGuardReport -Source $f8 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  if (@($r8.Unguarded).Count -ne 0 -or @($r8.Exempted).Count -ne 1) { $fails += '⑧ 豁免没生效' }
  $r8b = Get-GatesGuardReport -Source $f8 -Exemptions @(@{ Snippet = 'ctest --preset'; Reason = 'x' })
  if (@($r8b.StaleExemptions).Count -ne 1) { $fails += '⑧b 陈旧豁免没被抓出来' }

  # ⑨ 豁免不按名字:同名但**另一处**调用不得白拿豁免
  $f9 = 'cmake --version' + [Environment]::NewLine + 'cmake --build $dir'
  $r9 = Get-GatesGuardReport -Source $f9 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  if (@($r9.Unguarded).Count -ne 1) { $fails += '⑨ 同名的另一处调用跟着白拿了豁免(豁免必须按文本指纹)' }

  # ⑩ **多结果解析仍须算外部**(#217 复审【重要】1)。喂一个返回两条结果的假解析器 ——
  #    旧写法 `$resolved.CommandType -notin …` 在这一格上恒真、会把调用点静默移出判据面。
  $fakeMulti = {
    param($n)
    @(
      [pscustomobject]@{ CommandType = 'ExternalScript' }
      [pscustomobject]@{ CommandType = 'Application' }
    )
  }
  $r10 = Get-GatesGuardReport -Source 'npx --yes prettier --check .' -Resolver $fakeMulti
  if (@($r10.Sites).Count -ne 1) { $fails += '⑩ 多结果解析时调用点被静默移出判据面(fail-open)' }
  # ⑩b 反向:解析成 Cmdlet(单结果)仍要被排除,别把上面那条改成「什么都算外部」
  $fakeCmdlet = { param($n) @([pscustomobject]@{ CommandType = 'Cmdlet' }) }
  $r10b = Get-GatesGuardReport -Source 'Write-Host hi' -Resolver $fakeCmdlet
  if (@($r10b.Sites).Count -ne 0) { $fails += '⑩b Cmdlet 被当成外部命令(判据面被撑宽)' }

  # ⑪ **豁免要钉命中数**(#217 复审【重要】2):同一段文本出现两处时必须判负,
  #    否则新写的第二处会白拿豁免。
  $f11 = 'cmake --version' + [Environment]::NewLine + 'cmake --version'
  $r11 = Get-GatesGuardReport -Source $f11 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  if (@($r11.StaleExemptions).Count -ne 1) { $fails += '⑪ 同文本第二处白拿了豁免(豁免必须钉命中数)' }
  if (@($r11.Exempted).Count -ne 0) { $fails += '⑪b 命中数对不上时不该再豁免任何一处' }

  if ($fails.Count -gt 0) {
    Write-Host '  [FAIL] check-gates-guards --self-test:' -ForegroundColor Red
    $fails | ForEach-Object { Write-Host ('    ' + $_) -ForegroundColor Red }
    exit 1
  }
  Write-Host '  check-gates-guards -SelfTest:13 格全过(抓无守卫 / 两种守卫形态不误报 / 别人的守卫不算数 / 本地函数与脚本块不入面 / 变量式守卫 / 豁免按指纹 + 陈旧判负 / 同名不白拿 / 多结果仍算外部 / Cmdlet 不入面 / 同文本两处判负)'
  exit 0
}

# ─────────────────────────────────────────────────────────────────────────────
if (-not $Path) { $Path = Join-Path $RepoRoot 'scripts\gates.ps1' }
if (-not (Test-Path $Path)) {
  Write-Host ('  [FAIL] 找不到被检文件:{0}' -f $Path) -ForegroundColor Red
  exit 1
}

$src = Get-Content -LiteralPath $Path -Raw
foreach ($ex in $Exemptions) {
  if ([string]::IsNullOrWhiteSpace($ex.Reason)) {
    Write-Host ('  [FAIL] 豁免条目缺理由:{0} —— 豁免必须写清为什么' -f $ex.Snippet) -ForegroundColor Red
    exit 1
  }
}

$report = Get-GatesGuardReport -Source $src -Exemptions $Exemptions

if (@($report.Sites).Count -eq 0) {
  # 判据面为空 = 判据恒真。今天是 29 处;掉到 0 只可能是分析被改坏了,
  # 而「恒绿」连删除式都照不出来(0 == 0),所以这里判负,不安静通过。
  Write-Host '  [FAIL] 一个外部命令调用点都没扫到 —— 判据面为空即恒真' -ForegroundColor Red
  exit 1
}

$bad = $false
if (@($report.StaleExemptions).Count -gt 0) {
  $bad = $true
  Write-Host '  [FAIL] 豁免命中数与清单对不上:' -ForegroundColor Red
  $report.StaleExemptions | ForEach-Object {
    $why = if ($_.Got -eq 0) { '陈旧 —— 代码里已经没有对应调用点' } else { '被撑宽 —— 代码里多出了同文本的调用点,新的那处会白拿豁免' }
    Write-Host ('    {0}  期望 {1} 处、实得 {2} 处({3})' -f $_.Snippet, $_.Want, $_.Got, $why) -ForegroundColor Red
    Write-Host ('      理由:' + $_.Reason) -ForegroundColor DarkGray
  }
  Write-Host '    豁免随代码漂移而失效(变陈旧或被撑宽)正是本判据要防的:请改准 Hits,或给新调用点加守卫。' -ForegroundColor Yellow
}
if (@($report.Unguarded).Count -gt 0) {
  $bad = $true
  Write-Host ('  [FAIL] {0} 处外部命令调用点不在任何存在性守卫的 if/else 里:' -f @($report.Unguarded).Count) -ForegroundColor Red
  $report.Unguarded | ForEach-Object {
    Write-Host ('    {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $_.Line, $_.Text) -ForegroundColor Red
  }
  Write-Host '    修法:在调用外面加 `if (-not $<命令>Cmd) { Set-Gate … $false } else { … }`,' -ForegroundColor Yellow
  Write-Host '    并在调用前 `$global:LASTEXITCODE = 1`;确属「缺席即判负」的,加进本脚本的豁免清单并写理由。' -ForegroundColor Yellow
}
if ($bad) { exit 1 }

$byName = ($report.Sites | Group-Object Name | Sort-Object Name |
    ForEach-Object { '{0} {1}' -f $_.Name, $_.Count }) -join ' / '
Write-Host ('  gates 外部命令守卫完备:{0} 处调用、{1} 个名字,{2} 处按清单豁免。' -f
  @($report.Sites).Count, @($report.Sites | Group-Object Name).Count, @($report.Exempted).Count)
Write-Host ('    分布:{0}' -f $byName)
exit 0
