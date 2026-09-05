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
    ⚠ **边界:守卫只判「有没有」,不判极性、也不判调用点落在哪一支。** 只要祖先 `if` 的**任一分支
      条件**提到了这个命令/变量就算守住 —— 所以本判据保证的是「调用点周围有针对同一命令的存在性
      判断」,**不保证「它落在命令存在的那一支」**。有意取宽:极性分析(`-not $x` / `$null -eq $x`
      / 落在 ElseClause)容易误报,而误报会让人关掉门禁。要收紧就得连「从哪个 clause 上来的」一起记。
    ⚠ **边界:名字式判定依赖 `Get-Command` 的解析结果,而解析结果与机器/平台有关。** 判成外部的是
      「Application / ExternalScript / 解析不到」——于是 Windows 上同时解析出 Alias + Application 的
      短名(`sort` / `where` / `more` / `tee`)算**入面**,而 Linux 上 pwsh 不定义 `ls`/`cat` 这些别名,
      同名调用会从 Alias 变成 Application、同样入面。两个方向都是 fail-closed。
      ⚠ **但 fail-closed 到了 CI 上就是硬红**:`Get-CimInstance` 这类 Windows-only cmdlet 在
      Linux 的 pwsh 上解析不到,曾把 docs-truth 判红(#217)。所以「解析不到」那一档再切一刀:
      `<批准动词>-<名词>`(按 `Get-Verb` 这张**机器自带**的表)当 cmdlet 排掉;
      `clang-format` / `npx` / `pipx` 不受影响 —— `clang` 不是批准动词。
    ⚠ **边界:脚本块/路径这两处判定是启发式**(前者认 `ScriptBlockExpressionAst` 的文本形态,后者认
      `Join-Path`/`.exe`/盘符/斜杠)。裸 `/` 偏宽,任何 RHS 带斜杠的赋值都会被记成 pathLike ——
      只在 `& $var` 时起作用,方向偏红。另有一处 fail-open:同名变量若**先**赋成脚本块、**后**赋成
      路径,`scriptBlockVars` 会优先命中、那处 `& $var` 被静默排除;`gates.ps1` 今天没有这种写法。
    ⚠ **边界:豁免清单与判据面下界都只对默认目标(`gates.ps1`)生效。** 三条豁免的指纹全指向
      它的 ③ 类调用点,下界 20 也是按它的体量取的;`-Path` 指别的文件时豁免关掉、下界降为 1,并打一行 INFO ——
      `-Path` 指别的文件时**豁免不生效、下界从 20 降为 1**(只挡「一处都没扫到」),
      那时本脚本只回答「这个文件里的外部调用有没有守卫」,不回答「豁免是否还准」。
      「是不是默认目标」按**解析后的路径**比,所以 `-Path scripts/gates.ps1` 与不传 `-Path` 等价。
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

# PowerShell 自带的批准动词表(约 100 个)。用它把「解析不到的 cmdlet/函数」与「解析不到的
# 外部命令」分开 —— 见 Get-GatesGuardReport 里那段 ⚠。**这是机器提供的表,不是手写清单**;
# 手写清单正是本卡在根除的东西。
$script:ApprovedVerbs = @((Get-Verb).Verb)

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
# 从一次 `Get-Command …` 调用里取出**它在问哪个命令**。
# 只认两种形态:`-Name <x>`,以及第一个**位置**参数;跳过所有「参数 + 它的值」对。
# 认不出就返回 $null —— 调用方据此不记守卫(宁可多报一条红,也不记成一个错的名字)。
# ─────────────────────────────────────────────────────────────────────────────
function Get-GcTargetName {
  param($GcAst)
  $els = @($GcAst.CommandElements | Select-Object -Skip 1)
  for ($i = 0; $i -lt $els.Count; $i++) {
    $el = $els[$i]
    if ($el -is [System.Management.Automation.Language.CommandParameterAst]) {
      # 冒号形式 `-Name:reuse` 的值挂在 `.Argument` 上、**不占独立元素**。不认它的话,
      # `-Name:` 那一支会走空、落回后面的位置扫描,把 `-ErrorAction` 的值 `SilentlyContinue`
      # 当成命令名返回 —— 正是 ⑫ 要根除的形态换了个入口,而且**违反本函数自己写的不变量**
      # (认不出要返回 $null,不记错名字)。#217 复审指出。
      if ($el.ParameterName -like 'Name*') {
        if ($el.Argument -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
          return $el.Argument.Value
        }
        if ($null -eq $el.Argument -and $i + 1 -lt $els.Count -and
          $els[$i + 1] -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
          return $els[$i + 1].Value
        }
        return $null   # 明明写了 -Name 却认不出它的值:不猜
      }
      # 非 Name 的参数:冒号形式自带值,不吃下一个元素;分离形式吃掉下一个元素。
      # ⚠ 开关参数(`-All npx`)在分离形式下会把位置参数误吃掉 ⇒ 返回 $null ⇒ 守卫认不出 ⇒
      #   多一条红。方向 fail-closed、符合本函数的不变量,代价是排查成本;要分辨开关与
      #   取值参数就得引一份参数名清单,而「不靠名字清单」正是本卡的立意。
      if ($null -eq $el.Argument) { $i++ }
      continue
    }
    if ($el -is [System.Management.Automation.Language.StringConstantExpressionAst]) { return $el.Value }
  }
  return $null
}

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
      # ⚠ 取「第一个不以 `-` 开头的字符串常量」会把**参数值**也算进候选:`-ErrorAction` 自己是
      #   `CommandParameterAst`(躲过了),但它的值 `SilentlyContinue` 是个 StringConstantExpressionAst。
      #   于是 `Get-Command -ErrorAction SilentlyContinue reuse` 会被记成 reuse 守卫盯着
      #   「SilentlyContinue」,真正的 `reuse lint` 判无守卫 —— 方向偏红,但报错会指向一处
      #   **明明写了守卫**的调用点。所以跳过紧跟在参数后面的那个值。
      $target = Get-GcTargetName $gc[0]
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
        $t = Get-GcTargetName $c
        if ($t) { $out += $t }
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
      # ⚠ 「解析不到 ⇒ 算外部」这一档是 fail-closed(`pipx` / `reuse` 没装的机器上正靠它把它们
      #   留在判据面里),但**跨平台时它会反咬**:`Get-CimInstance` 是 Windows-only cmdlet,
      #   Linux 的 pwsh 解析不到 ⇒ 被判成「没有守卫的外部命令」⇒ **docs-truth 当场红**
      #   (#217 实测:`gates.ps1:180/189/1031` 三处)。本机绿、CI 红,而 CI 上一条误报红
      #   就是门禁坏了 —— 边界段原来写「顶多多一条误报的红」,那句话轻描淡写了。
      #   切法**不引名字清单**(那正是本卡的立意):用 PowerShell **自带**的批准动词表
      #   `Get-Verb`,把 `<批准动词>-<名词>` 形态的当 cmdlet 排掉。实测切得干净:
      #   Get-CimInstance / Start-Process / Set-Gate 是;clang-format / npx / pipx 不是
      #   (`clang` 不在批准动词表里),所以判据面不缩水。
      if ($resolved.Count -eq 0) {
        $parts = $name -split '-', 2
        if ($parts.Count -eq 2 -and $script:ApprovedVerbs -contains $parts[0]) { continue }
      }
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
  # 格数**由机器自己数**,不再手写:同一个数我在 SL-329 写错过一次(「26 处」),这里又写错一次
  # (13 → 15 是拍脑袋加出来的,真实断言 16 条)。凡是「加一格却忘了改数」都不该有机会发生 ——
  # 走这个 helper 就不可能漏计。
  $cells = 0
  $check = {
    param([string]$Name, [bool]$Ok)
    $script:cells++
    if (-not $Ok) { $script:fails += $Name }
  }
  # 夹具里凡是用到 `Set-Gate` 的,都要**在夹具里定义它** —— 真 gates.ps1 里它是本文件定义的函数,
  # 夹具不定义,判据就会(正确地)把它当成一处没有守卫的外部命令,于是自测红在夹具而不是判据上。
  $defSetGate = 'function Set-Gate { param($a, $b) }' + [Environment]::NewLine

  # ① 无守卫的外部调用 ⇒ 必须被抓
  $f1 = "npx --yes prettier --check ." + [Environment]::NewLine
  $r1 = Get-GatesGuardReport -Source $f1
  & $check ('① 无守卫的 npx 没被抓到(实得 {0} 条)' -f @($r1.Unguarded).Count) (@($r1.Unguarded).Count -eq 1)

  # ② 有守卫(变量形态)⇒ 不得误报
  $f2 = $defSetGate + '$npxCmd = Get-Command npx -ErrorAction SilentlyContinue' + [Environment]::NewLine +
  'if (-not $npxCmd) { Set-Gate ' + "'x'" + ' $false } else { npx --yes prettier --check . }'
  $r2 = Get-GatesGuardReport -Source $f2
  & $check '② 有守卫的 npx 被误判成无守卫' (@($r2.Unguarded).Count -eq 0)

  # ③ 有守卫(内联形态)⇒ 不得误报
  $f3 = $defSetGate + 'if (Get-Command reuse -ErrorAction SilentlyContinue) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r3 = Get-GatesGuardReport -Source $f3
  & $check '③ 内联 Get-Command 守卫没被认出来' (@($r3.Unguarded).Count -eq 0)

  # ④ **别人的守卫不算数**:npx 落在 $nodeCmd 的 if 里必须仍判无守卫
  $f4 = '$nodeCmd = Get-Command node -ErrorAction SilentlyContinue' + [Environment]::NewLine +
  'if ($nodeCmd) { npx --yes prettier --check . }'
  $r4 = Get-GatesGuardReport -Source $f4
  & $check '④ npx 借用 $nodeCmd 的守卫被放过了(命令名没对上就不该算)' (@($r4.Unguarded).Count -eq 1)

  # ⑤ 本文件自己定义的函数不得混进外部清单
  $f5 = 'function Set-Gate { param($a, $b) }' + [Environment]::NewLine + 'Set-Gate ' + "'x'" + ' $true'
  $r5 = Get-GatesGuardReport -Source $f5
  & $check ('⑤ 本地函数被当成外部命令(实得 {0} 处)' -f @($r5.Sites).Count) (@($r5.Sites).Count -eq 0)

  # ⑥ 脚本块变量的 `& $var` 不是外部命令
  $f6 = '$cb = { param($x) $x }' + [Environment]::NewLine + '& $cb 1'
  $r6 = Get-GatesGuardReport -Source $f6
  & $check '⑥ 脚本块闭包被当成外部命令' (@($r6.Sites).Count -eq 0)

  # ⑦ 变量式外部调用要认守卫(Test-Path $var 形态)
  $f7 = $defSetGate + '$exe = Join-Path $root ' + "'x.exe'" + [Environment]::NewLine +
  'if (Test-Path $exe) { & $exe --version } else { Set-Gate ' + "'x'" + ' $false }'
  $r7 = Get-GatesGuardReport -Source $f7
  & $check '⑦ 变量式调用的 Test-Path 守卫没被认出来' (@($r7.Unguarded).Count -eq 0)

  # ⑧ 豁免要按**文本指纹**、且陈旧条目判负
  $f8 = 'cmake --version'
  $r8 = Get-GatesGuardReport -Source $f8 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  & $check '⑧ 豁免没生效(该处应从 Unguarded 移进 Exempted)' ((@($r8.Unguarded).Count -eq 0) -and (@($r8.Exempted).Count -eq 1))
  $r8b = Get-GatesGuardReport -Source $f8 -Exemptions @(@{ Snippet = 'ctest --preset'; Reason = 'x' })
  & $check '⑧b 陈旧豁免没被抓出来' (@($r8b.StaleExemptions).Count -eq 1)

  # ⑨ 豁免不按名字:同名但**另一处**调用不得白拿豁免
  $f9 = 'cmake --version' + [Environment]::NewLine + 'cmake --build $dir'
  $r9 = Get-GatesGuardReport -Source $f9 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  & $check '⑨ 同名的另一处调用跟着白拿了豁免(豁免必须按文本指纹)' (@($r9.Unguarded).Count -eq 1)

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
  & $check '⑩ 多结果解析时调用点被静默移出判据面(fail-open)' (@($r10.Sites).Count -eq 1)
  # ⑩b 反向:解析成 Cmdlet(单结果)仍要被排除,别把上面那条改成「什么都算外部」
  $fakeCmdlet = { param($n) @([pscustomobject]@{ CommandType = 'Cmdlet' }) }
  $r10b = Get-GatesGuardReport -Source 'Write-Host hi' -Resolver $fakeCmdlet
  & $check '⑩b Cmdlet 被当成外部命令(判据面被撑宽)' (@($r10b.Sites).Count -eq 0)

  # ⑪ **豁免要钉命中数**(#217 复审【重要】2):同一段文本出现两处时必须判负,
  #    否则新写的第二处会白拿豁免。
  $f11 = 'cmake --version' + [Environment]::NewLine + 'cmake --version'
  $r11 = Get-GatesGuardReport -Source $f11 -Exemptions @(@{ Snippet = 'cmake --version'; Hits = 1; Reason = 'x' })
  & $check '⑪ 同文本第二处白拿了豁免(豁免必须钉命中数)' (@($r11.StaleExemptions).Count -eq 1)
  & $check '⑪b 命中数对不上时不该再豁免任何一处' (@($r11.Exempted).Count -eq 0)

  # ⑫ `Get-Command` 的**参数值**不得被当成命令名(#217 复审【建议】)。
  #    `-ErrorAction SilentlyContinue reuse` 这种写法下,旧的「第一个不以 - 开头的字符串常量」
  #    会把守卫记成盯着「SilentlyContinue」,于是真正的 `reuse lint` 判无守卫 —— 报错指向一处
  #    明明写了守卫的调用点。
  $f12 = $defSetGate +
  'if (Get-Command -ErrorAction SilentlyContinue reuse) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r12 = Get-GatesGuardReport -Source $f12
  & $check '⑫ Get-Command 的参数值被当成了命令名(守卫认不出来)' (@($r12.Unguarded).Count -eq 0)
  # ⑫b `-Name` 具名形态也要认
  $f12b = $defSetGate +
  'if (Get-Command -Name reuse -ErrorAction SilentlyContinue) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r12b = Get-GatesGuardReport -Source $f12b
  & $check '⑫b -Name 具名形态的守卫认不出来' (@($r12b.Unguarded).Count -eq 0)

  # ⑬ 冒号形式 `-Name:reuse`(#217 复审):旧写法在这条路径上返回 'SilentlyContinue',
  #    守卫认不出、且记了一个**错的名字** —— 违反 Get-GcTargetName 自己写的不变量。
  $f13 = $defSetGate +
  'if (Get-Command -Name:reuse -ErrorAction SilentlyContinue) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r13 = Get-GatesGuardReport -Source $f13
  & $check '⑬ 冒号形式 -Name:reuse 的守卫认不出来(且会记成错的名字)' (@($r13.Unguarded).Count -eq 0)
  # ⑬b 认不出时必须**返回 $null / 不记守卫**,而不是记一个错名字后放行别的调用点。
  $f13b = $defSetGate +
  'if (Get-Command -ErrorAction:SilentlyContinue -Name:reuse) { reuse lint } else { Set-Gate ' + "'x'" + ' $false }'
  $r13b = Get-GatesGuardReport -Source $f13b
  & $check '⑬b 冒号形式的非 Name 参数把值吃错了' (@($r13b.Unguarded).Count -eq 0)

  # ⑭ **跨平台**:解析不到的 Windows-only cmdlet 不得入面,而解析不到的外部命令仍要入面。
  #    用「什么都解析不到」的假解析器模拟 Linux 上跑 —— #217 就是在 ubuntu 的 docs-truth 上
  #    被 `Get-CimInstance` 三处照红的,本机永远复现不出来。
  $fakeNone = { param($n) @() }
  $r14 = Get-GatesGuardReport -Source 'Get-CimInstance Win32_Process -Filter "x"' -Resolver $fakeNone
  & $check '⑭ 解析不到的 <批准动词>-<名词>(Windows-only cmdlet)被当成外部命令 —— 跨平台会假红' (@($r14.Sites).Count -eq 0)
  # ⑭b 反向:解析不到的**外部命令**必须仍在面内,别把 ⑭ 改成「带连字符就放过」
  $r14b = Get-GatesGuardReport -Source 'clang-format --dry-run --Werror x.cpp' -Resolver $fakeNone
  & $check '⑭b 解析不到的外部命令(clang-format)被放过了 —— 判据面缩水' (@($r14b.Sites).Count -eq 1)

  if ($fails.Count -gt 0) {
    Write-Host '  [FAIL] check-gates-guards --self-test:' -ForegroundColor Red
    $fails | ForEach-Object { Write-Host ('    ' + $_) -ForegroundColor Red }
    exit 1
  }
  # 只报**数**,不再手抄一份「都测了些什么」的枚举:上一版那份枚举漏了新加的 ⑬/⑬b,
  # 而更上一版是格数漂了 —— 同一个病第三次。要知道测了哪几格,读上面那些 `& $check` 的名字,
  # 那是唯一真源。
  Write-Host ('  check-gates-guards -SelfTest:{0} 格全过(逐格名字见本脚本 selfTest 段的 $check 调用)' -f $cells)
  exit 0
}

# ─────────────────────────────────────────────────────────────────────────────
# 分隔符用 `/`:接进 CI 的 docs-truth(ubuntu)之后,`'scripts\gates.ps1'` 在 Linux 上是一个
# **带反斜杠的字面文件名**,`Test-Path` 直接不成立 —— 判据一行没跑就退 1(方向偏红,但报的是
# 「找不到被检文件」,查起来会以为是路径传错)。`/` 在 Windows 上同样可用。
if (-not $Path) { $Path = Join-Path $RepoRoot 'scripts/gates.ps1' }
if (-not (Test-Path $Path)) {
  Write-Host ('  [FAIL] 找不到被检文件:{0}' -f $Path) -ForegroundColor Red
  exit 1
}
# ⚠ 「是不是默认目标」要按**解析后的路径**比,不能按「有没有传 `-Path`」判(#217 复审【重要】):
#   `-Path scripts/gates.ps1` 指的就是默认那个文件,而按前者判会把豁免整份关掉 ⇒ 三处 ③ 类
#   调用点变成 Unguarded ⇒ 对**默认目标本身**假红,还点名让人去给它们加守卫,而它们本来就是
#   清单里写着理由的豁免。接 CI 时在 workflow 里显式写 `-Path scripts/gates.ps1` 是很自然的写法,
#   这条路径下一个 commit 就会踩到。
$defaultTarget = Join-Path $RepoRoot 'scripts/gates.ps1'
$isDefaultPath = $false
if (Test-Path $defaultTarget) {
  $isDefaultPath = ((Resolve-Path -LiteralPath $Path).Path -eq (Resolve-Path -LiteralPath $defaultTarget).Path)
}

$src = Get-Content -LiteralPath $Path -Raw
foreach ($ex in $Exemptions) {
  if ([string]::IsNullOrWhiteSpace($ex.Reason)) {
    Write-Host ('  [FAIL] 豁免条目缺理由:{0} —— 豁免必须写清为什么' -f $ex.Snippet) -ForegroundColor Red
    exit 1
  }
}

# 豁免清单是**为 gates.ps1 写的**(三条指纹全指向它的 ③ 类调用点)。`-Path` 指别的文件时
# 无条件套上去,三条会**全部**判成「陈旧」——文档说 `-Path` 可指别的文件,而实际一用就假红。
# 所以豁免与下界一样,只在检默认文件时生效;检别的文件时**明说**这一点,别让人以为已经全查了。
$activeExemptions = if ($isDefaultPath) { $Exemptions } else { @() }
if (-not $isDefaultPath) {
  Write-Host ('  [INFO] -Path 指向 {0}(非默认目标):豁免清单**不生效**,判据面下界从 20 降为 1(只挡「一处都没扫到」)。' -f (Split-Path -Leaf $Path)) -ForegroundColor Yellow
}
$report = Get-GatesGuardReport -Source $src -Exemptions $activeExemptions

# 判据面塌了 = 判据近乎恒真。只挡 0 是不够的:分析被改坏到只剩三五处也照样绿,
# 而删除式测的是「拆掉守卫会红」,**测不出「面缩水了」**。所以钉一个下界。
# 数不写死成「今天几处」——那种数会过期,而且会成为下一个人的依据(本卡上一版就把
# `gates.ps1` 旧注释里的名字式处数抄了过来,而本脚本还多收了变量式调用那一族)。
# 下界取一个**明显低于现状、又高于「分析塌掉」**的数,只用来照出塌方,不用来对账。
# ⚠ 下界**只在检默认文件(gates.ps1)时生效**。`-Path` 是公开参数、边界里也写了「可指别的文件」,
#   对一个更小的脚本无条件套 20 会假红 —— 判据把自己的适用面写宽了,却按最宽那份的体量收口。
$floor = if ($isDefaultPath) { 20 } else { 1 }   # 非默认目标也至少要有 1 处,否则空集合恒真
if (@($report.Sites).Count -lt $floor) {
  if ($isDefaultPath) {
    Write-Host ('  [FAIL] 只扫到 {0} 处外部命令调用点(下界 {1})—— 判据面塌了,不是代码变干净了' -f
      @($report.Sites).Count, $floor) -ForegroundColor Red
    Write-Host '    多半是 AST 分析或分类被改坏:判据面一小,门禁就近乎恒真,而删除式照不出来。' -ForegroundColor Yellow
  }
  else {
    # 非默认目标只挡「一处都没扫到」,而那多半是**文件传错了**,不是判据坏了 ——
    # 照搬默认目标那句「判据面塌了」会让人去查判据,查错方向。
    Write-Host ('  [FAIL] {0} 里一处外部命令调用点都没有 —— 确认是不是传错了文件' -f (Split-Path -Leaf $Path)) -ForegroundColor Red
    Write-Host '    (空集合会让「每处都有守卫」恒真,所以这里判负而不是安静通过。)' -ForegroundColor Yellow
  }
  exit 1
}

$bad = $false
if (@($report.StaleExemptions).Count -gt 0) {
  $bad = $true
  Write-Host '  [FAIL] 豁免命中数与清单对不上:' -ForegroundColor Red
  $report.StaleExemptions | ForEach-Object {
    # `$hit` 是从**无守卫**的调用点里筛的,所以 Got=0 有两个成因,而「被加上守卫了」在这个仓里
    # 更常见(谁给 `cmake --version` 包一层 if 就会触发)。只写「已经没有对应调用点」会让人
    # 去 gates.ps1 里找一行**还在**的代码。
    $why = if ($_.Got -eq 0) { '陈旧 —— 那处调用点要么被删了,要么**已经加上守卫**(两种都该删掉这条豁免)' } else { '被撑宽 —— 代码里多出了同文本的调用点,新的那处会白拿豁免' }
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
