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

  判据(任一不满足即判负;**不写条数** —— 手抄的数在本卡里已经漂过三次):
    · **每个外部命令调用点**都要落在**针对同一个命令**的存在性守卫的 if/else 作用域内;
    · **豁免必须显式且带理由**。首选形态是**行内标记** —— 贴在被检文件的调用点旁,跟着代码走;
      行尾标记只绑本行,独占一行的标记只绑下一行,一条标记只豁免一处,理由不得为空。
      外部 `$Exemptions` 清单是**兼容通道**(给不便改的被检文件),按文本指纹 + 钉命中数,今天为空。
    · **豁免不得孤悬/陈旧**:标记附近已经没有「无守卫的外部调用」即判负;清单条目命中数对不上
      (0 处=陈旧、多于预期=被撑宽)同样判负 —— 否则豁免会随代码漂移而悄悄失去精度。

  **输出纪律(不是判据,不影响退出码)**:每次运行把**被豁免的那几处逐条打出来**。
  不查的东西最该显形 —— 只报一个数的话,谁悄悄多加一条标记就多一处不查,而输出上只是数字加一。
  这条写在判据表外,是因为那段 `Write-Host` 从不影响 `$bad`,「任一不满足即判负」对它不成立
  (#219 第 3 轮:我把它写进判据表,同时让「三条」变成了四条 —— 在修头注漂移的那个 commit 里)。

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
      同名调用会从 Alias 变成 Application、同样入面。两个方向都是 fail-closed,
      而 `gates.ps1` 今天并没有用到这些短名,所以这条边界对判据面没有实际影响
      (不写具体处数:那种数会随 `gates.ps1` 漂,而脚本每次运行都会自己打出来)。
      ⚠ **但 fail-closed 到了 CI 上就是硬红**:`Get-CimInstance` 这类 Windows-only cmdlet 在
      Linux 的 pwsh 上解析不到,曾把 docs-truth 判红(#217)。所以「解析不到」那一档再切一刀:
      `<批准动词>-<名词>`(按 `Get-Verb` 这张**机器自带**的表)当 cmdlet 排掉;
      `clang-format` / `npx` / `pipx` 不受影响 —— `clang` 不是批准动词。
      ⚠ **这一刀的另一侧是 fail-open,必须写出来**:批准动词表里有 `Update` / `Install` / `Add` /
      `Select` / `Start` / `Test` / `New` / `Set` / `Copy` / `Move` / `Sync` / `Format` / `Search` /
      `Send` / `Split` / `Join` / `Merge` / `Push` / `Register` / `Restart` …… 于是任何**外部可执行体**
      只要名字长成 `<这些动词>-<名词>`、且在当前机器上解析不到,就会被当 cmdlet **排出判据面**。
      `gates.ps1` 今天没有这种调用,但别把这一刀读成「只排 cmdlet」——它排的是「形态像 cmdlet 的、
      解析不到的名字」,两者不是一回事。
    ⚠ **边界:脚本块/路径这两处判定是启发式**(前者认 `ScriptBlockExpressionAst` 的文本形态,后者认
      `Join-Path`/`.exe`/盘符/斜杠)。裸 `/` 偏宽,任何 RHS 带斜杠的赋值都会被记成 pathLike ——
      只在 `& $var` 时起作用,方向偏红。另有一处 fail-open:同名变量若**先**赋成脚本块、**后**赋成
      路径,`scriptBlockVars` 会优先命中、那处 `& $var` 被静默排除;`gates.ps1` 今天没有这种写法。
    ⚠ **边界:只有判据面下界对默认目标(`gates.ps1`)特殊,豁免不再特殊。** 豁免现在是**行内标记**,
      贴在被检文件的调用点旁,于是**跟着文件走** —— `-Path` 指哪个文件就读哪个文件的标记,不需要
      「只对默认目标生效」那一档(那是外部清单时代的补丁:清单为 `gates.ps1` 写,套到别的文件上会
      整份判成陈旧)。外部 `$Exemptions` 清单今天是**空的**,通道留着给不便改的被检文件。
      下界仍然特殊:20 是按 `gates.ps1` 的体量取的,`-Path` 指别的文件时降为 1(只挡「一处都没扫到」),
      并打一行 INFO。「是不是默认目标」按**解析后的路径**比,所以 `-Path scripts/gates.ps1`
      与不传 `-Path` 等价。
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
# 今天是**空的**:三条 ③ 类豁免已搬进 `gates.ps1` 各调用点旁的行内标记(SL-331 后续批)。
# 留着这条通道是因为 `-Path` 可以指别的文件,而那些文件未必方便改;用法见上面。
$Exemptions = @()

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

  # 变量名用 `$psTokens`:同一函数里 `$tokens` 已经是「守卫记号数组」(见下面 $condTokens 那段)。
  # 撞名今天靠语句顺序活着 —— 标记收集恰好在守卫循环之前跑完;谁把它挪到后面,
  # `$_.Kind -eq 'Comment'` 就恒不命中 ⇒ **所有行内标记静默失效**(#219 复审)。
  $psTokens = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseInput($Source, [ref]$psTokens, [ref]$null)

  # 行内豁免标记。形态(必须**紧跟注释起头**,不能夹在句子中间):
  #     # 后接方括号 gates-guard-exempt 方括号,再接非空理由
  # 位置**互斥**,不是「两个都随便」:**行尾标记(该行还有代码)只绑本行**,
  # **独占一行的标记只绑下一行**。写成「同一行或紧邻上一行」会让人以为可以随手选一个,
  # 而那正是 #219 形态 B 的来处(一条行尾标记有两个候选,工具静默挑一个)。
  # 「紧跟注释起头」这条不是排版洁癖 —— 本脚本的头注、以及别处的散文都会**提到**这个标记名,
  # 若允许它出现在句子中间,任何一句解释都会变成一条豁免(本仓「扫描器入库才炸」那一族:
  # 判据被自己的文档喂出假绿)。所以头注里提到它时一律不放在注释开头。
  $srcLines = $Source -split "`r?`n"
  $markerRe = '^\s*#\s*\[gates-guard-exempt\]\s*(\S.*)$'
  $markers = @()
  foreach ($t in @($psTokens | Where-Object { $_.Kind -eq 'Comment' })) {
    $m = [regex]::Match($t.Text, $markerRe)
    if ($m.Success) {
      # `Standalone` = 这条注释**独占一行**(行首到注释起点之间只有空白)。
      # 归属因此没有歧义:**行尾标记只绑本行,独占行标记只绑下一行**。
      # 不这么分的话,「同一行或下一行」对一条行尾标记是**两个候选**,工具会静默挑一个 ——
      # 作者若是写给下一行的,本行那处就被悄悄豁免了(#219 复审的形态 B,实测复现)。
      $lineText = ($srcLines[$t.Extent.StartLineNumber - 1])
      $markers += , [pscustomobject]@{
        Line       = $t.Extent.StartLineNumber
        Reason     = $m.Groups[1].Value.Trim()
        Standalone = ($lineText -match '^\s*#')
        Used       = $false
      }
    }
  }

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
      # ⚠ 顺序:**先看 pathLike,再看脚本块**(#217 复审指出的一处 fail-open)。同名变量若
      #   先被赋成脚本块、后被赋成路径,旧写法让 `scriptBlockVars` 先命中,那处 `& $var`
      #   被**静默排除** —— 判据面悄悄少一处,而这正是本卡在治的方向。两边都命中时按外部算
      #   (fail-closed:顶多多一条红,不会漏)。
      if (-not $pathLikeVars.Contains($vn)) {
        if ($scriptBlockVars.Contains($vn)) { continue }        # 只当过闭包,不是外部命令
        continue                                                # 认不出是可执行体,不判
      }
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
        # ⚠ `Id` 用 AST 的 **extent 起始偏移**,每个调用点唯一。原来拿 `行号|文本` 当键,
        #   **同一行、同文本**的两处调用(`cmake --version; cmake --version`)键会撞:
        #   第一处消费掉标记并把键写进集合,第二处虽然没拿到标记,却因为键已在集合里而
        #   一并被滤出 Unguarded ⇒ **一个标记吞两处**。#219 复审实测(rc 本该是 1,实得 0)。
        Id      = $c.Extent.StartOffset
        Name    = $label
        Line    = $c.Extent.StartLineNumber
        Text    = ($c.Extent.Text -replace '\s+', ' ')
        Guarded = $guarded
        GuardBy = $by
      })
  }

  # 豁免匹配。用**调用点的 extent 偏移**当键去重(每处唯一),不靠对象同一性 ——
  # PSCustomObject 的 `-contains` 走类型比较,在这里会抛 "Argument types do not match"。
  $exemptedKeys = New-Object 'System.Collections.Generic.HashSet[string]'
  $stale = @()
  $unguarded = @($sites.ToArray() | Where-Object { -not $_.Guarded })
  # ⚠ 豁免要钉**命中几处**,不能只钉「至少一处」(#217 复审【重要】):只钉「至少一处」的话,
  #   明天谁在别处再写一处**同文本、无守卫**的调用,`-like` 会把它一并吞掉 —— 新调用点白拿豁免、
  #   门禁全绿,而它恰恰会踩回 `$LASTEXITCODE` 沿用上一条的老坑。豁免会随代码**自动变宽**,
  #   而这正是本卡开篇批判的「清单悄悄失去精度、还成为下一个人的依据」。
  # ---- 行内标记(首选形态)----
  # 标记贴在调用点旁,于是它**跟着代码走**:调用行被改、被挪、被删,标记跟着一起动,
  # 不会像外部清单那样「指纹失配 ⇒ 报陈旧豁免」而不是「你改了一处豁免点」。
  # 一个标记只豁免**一处** —— 这不是「天然」的,是**两条保证换来的**:`Id` 用 extent 偏移
  # (治同行同文本的键撞车)+ `Standalone` 归属(治行尾/独占行的候选歧义)。有这两条才不必再钉
  # `Hits`;把它写成「天然」会让下一个人拿它当现成理由,而那两条一旦被改掉,理由就没了。
  foreach ($u in $unguarded) {
    $mk = @($markers | Where-Object {
        -not $_.Used -and (
          ((-not $_.Standalone) -and $_.Line -eq $u.Line) -or   # 行尾标记:只绑本行
          ($_.Standalone -and $_.Line -eq ($u.Line - 1))        # 独占行标记:只绑下一行
        )
      })
    if ($mk.Count -gt 0) {
      $mk[0].Used = $true
      [void]$exemptedKeys.Add([string]$u.Id)
    }
  }
  # 孤悬标记 = 那一处要么已经加了守卫、要么被删/挪走了 —— 两种都该把标记一起处理掉。
  # 不判负的话,标记会随代码漂成一句无人负责的注释(与外部清单的「陈旧豁免」同一条理由)。
  foreach ($mk in @($markers | Where-Object { -not $_.Used })) {
    $stale += , [pscustomobject]@{
      Kind = 'marker'; Snippet = ('行内标记 @{0}' -f $mk.Line); Reason = $mk.Reason; Want = 1; Got = 0
    }
  }

  # ---- 外部清单(兼容路径)----
  # 标记搬进被检文件之后,`$Exemptions` 今天是空的;这条留着是因为 `-Path` 可以指别的文件,
  # 而那些文件未必方便改。用法不变:按文本指纹 + 钉命中数。
  foreach ($ex in $Exemptions) {
    $want = if ($null -ne $ex.Hits) { [int]$ex.Hits } else { 1 }
    $hit = @($unguarded | Where-Object { $_.Text -like ('*' + $ex.Snippet + '*') })
    if ($hit.Count -ne $want) {
      $stale += , [pscustomobject]@{ Kind = 'list'; Snippet = $ex.Snippet; Reason = $ex.Reason; Want = $want; Got = $hit.Count }
      continue
    }
    foreach ($h in $hit) { [void]$exemptedKeys.Add([string]$h.Id) }
  }
  # ⚠ 键要显式转 [string] 再喂 HashSet.Contains:PowerShell 会把 `-f` 的结果按 PSObject 传进去,
  #   `HashSet[string].Contains` 于是抛 "Argument types do not match"(而且报在下面构造对象那一行,
  #   与真正出错的表达式对不上,查起来很费劲)。
  $stillUnguarded = @($unguarded | Where-Object { -not $exemptedKeys.Contains([string]$_.Id) })
  $exempted = @($unguarded | Where-Object { $exemptedKeys.Contains([string]$_.Id) })

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
# [SL-337] 第二道判据:**注释里不得复述截断值名单**。
#
# 它防的形态(#222 复审第 4 轮实证):`gates.ps1` 里「哪几道闸用 `-Last 30`、哪道用 20」
# 这份事实,曾同时躺在**三处**注释里。三份当时全为真 —— 而只要有人调一个截断值,
# 改漏哪一份都不会有人发现,因为注释不会被执行。同一张卡上这句话已经被咬过三轮:
# 先是「与 3b 逐字相同」本身是假的,再是「三处」被那张卡自己的改动作废,最后才看清
# 病根不是某个字面量写错,而是**它在复述一份分散的事实**。
#
# 判据形态:**一条注释里同时出现 `-Last <数>` 与「另一道闸的档号」⇒ 判负。**
#   · 只判「值 × 闸」这个**组合**,不是见 `-Last` 就红 —— 单说一个值(「此前这里是
#     `-Last 40`」)是本地事实,不是名单;单提一道闸更不是。会漂的恰恰是把两者绑起来的
#     那句话,因为它断言的是**别处**代码今天取什么值。
#   · 档号**从被检文件自己的 `Set-Gate` 字面量里取**,不写手写清单 —— 手写清单正是本族
#     缺陷本身(同 `$script:ApprovedVerbs` 用 `Get-Verb` 那一处的理由)。只认带字母的
#     档号(`3b`/`3c`/`5b`…)与 `gate <数>` 两种形态:纯数字的 `3`、`5` 在散文里满地都是,
#     拿它当档号会把判据变成噪音源,而**噪音判据等于没有判据**。
#   · 注释从 **PowerShell 自己的 tokenizer** 取,不按行首 `#` 切:字符串里的 `#` 不是注释,
#     行尾注释也要收得上。档号同样走 AST(`Set-Gate` 调用的字面量实参)—— 两半必须同源,
#     否则字符串里一句 `Set-Gate '9z …'` 就能注入一个幻影档号(#225 复审)。
#
# ⚠ **这一刀的另一侧是 fail-open,必须写出来**(照本文件隔壁那道的规矩):
#   ① **只认 `-Last` 这个字面 token**。被复述的事实是「哪道闸截到多少行」,它完全可以不带
#      `-Last` 写出来(「与 3c 同值,末 30 行」),那样这道判据看不见。收得更宽要付的代价是
#      「30」这种裸数字满地都是 ⇒ 噪音判据等于没有判据,所以这一档**有意留在 fail-open 侧**,
#      靠真源那一段把名单收成一份来兜。
#   ② **档号取不到的 `Set-Gate` 形态**:变量标签(`Set-Gate $smokeLabel`)与表达式实参
#      (`Set-Gate ("3e …" -f …)`)都拿不到字面量,那几道闸的档号不在判据面里。
#   两条都靠 `Test-GatesLastFloor` 的下界兜底 —— 下界挡不住「少了一个档号」,但挡得住
#   「档号集合整体塌掉」那一档,而后者才是让判据静默恒真的那种失效。
#
# 豁免:**唯一真源那一段**在自己的注释里挂 `[gates-last-source]`,豁免范围是**它所在的
# 那一整段连续注释**。与本文件 `[gates-guard-exempt]` 同款:豁免长在被豁免的东西旁边,
# 不另立清单;而且标记必须**紧跟注释起头**(允许前置 `[SL-xxx]` 这类标签),句子中间
# 提到标记名不算 —— 否则本脚本的头注、以及任何解释这个标记的散文都能喂出一条假绿
# (本仓「扫描器入库才炸」那一族,见自测 ⑳)。
#
# 返回:@{ Comments; GateIds; Sanctioned; Offending }
# ─────────────────────────────────────────────────────────────────────────────
function Get-GatesLastListReport {
  param([Parameter(Mandatory)][string]$Source)

  $tokens = $null
  $errs = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseInput($Source, [ref]$tokens, [ref]$errs)
  $srcLines = $Source -split "`r?`n"

  # 行号 -> @{ Text; WholeLine }。**行尾注释与整行注释要分开**(#225 复审):下面「一整段
  # 连续注释」的爬行只认整行注释,否则形如 `$x = 1   # 说明` 的代码行会把它两侧的注释**桥接**
  # 成一段,豁免范围凭空跨过一段代码 —— 而 gates.ps1 里行尾注释很常见。
  $commentByLine = @{}
  foreach ($t in $tokens) {
    if ($t.Kind -ne [System.Management.Automation.Language.TokenKind]::Comment) { continue }
    $ln = $t.Extent.StartLineNumber
    $before = ''
    if ($ln -ge 1 -and $ln -le $srcLines.Count) {
      $col = $t.Extent.StartColumnNumber
      $line = [string]$srcLines[$ln - 1]
      if ($col -ge 1 -and ($col - 1) -le $line.Length) { $before = $line.Substring(0, $col - 1) }
    }
    $commentByLine[$ln] = @{ Text = [string]$t.Text; WholeLine = [string]::IsNullOrWhiteSpace($before) }
  }

  # 档号:从 **AST 里的 `Set-Gate` 调用**取第一个字面量实参,不走原始文本正则(#225 复审:
  # 注释走 tokenizer、档号走裸正则是**两条不同源的路** —— 字符串或注释里写一句
  # `Set-Gate '9z …'` 会注入一个幻影档号)。只留**带字母**的档号,理由见头注。
  # ⚠ **取不到的形态**:`Set-Gate $someLabel`(变量标签)与 `Set-Gate ("3e …" -f …)`(表达式)
  #   都拿不到字面量。这是**已知的 fail-open**:某道闸若只用变量标签设,它的档号就悄悄退出
  #   判据面。挡这一档的不是这里,而是调用点那条**下界**(见 Test-GatesLastFloor)。
  $gateIds = New-Object System.Collections.Generic.HashSet[string]
  if ($null -ne $ast) {
    $setGateCalls = @($ast.FindAll({ param($n)
          $n -is [System.Management.Automation.Language.CommandAst] -and
          $n.GetCommandName() -eq 'Set-Gate'
        }, $true))
    foreach ($c in $setGateCalls) {
      if ($c.CommandElements.Count -lt 2) { continue }
      $arg = $c.CommandElements[1]
      if ($arg -isnot [System.Management.Automation.Language.StringConstantExpressionAst]) { continue }
      $lead = [regex]::Match([string]$arg.Value, '^\s*(\d+[a-zA-Z])(?![0-9A-Za-z])')
      if ($lead.Success) { [void]$gateIds.Add($lead.Groups[1].Value.ToLowerInvariant()) }
    }
  }

  # 标记 -> 它所在的那一整段**整行**注释。
  # 与 `[gates-guard-exempt]` **同款的两条硬要求**(#225 复审指出原先都没有):
  #   · 标记后面必须跟**非空理由** —— 裸一行标记不该豁免整段;
  #   · 标记**大小写敏感**(那一族走 `[regex]::Match` 默认敏感,两个标记不该两种口径)。
  # 标记必须紧跟注释起头(允许前置 `[SL-xxx]` 这类标签),句子中间提到不算。
  $markedLines = @()
  foreach ($ln in $commentByLine.Keys) {
    if (-not $commentByLine[$ln].WholeLine) { continue }
    $body = ([string]$commentByLine[$ln].Text).TrimStart('#').Trim()
    while ($body -match '^\[([^\]]+)\]\s*') {
      $tag = $Matches[1]
      $rest = $body.Substring($Matches[0].Length)
      if ($tag -ceq 'gates-last-source') {
        if (-not [string]::IsNullOrWhiteSpace($rest)) { $markedLines += $ln }
        break
      }
      $body = $rest
    }
  }
  $blocks = @()
  foreach ($ln in ($markedLines | Sort-Object -Unique)) {
    $lo = $ln
    while ($commentByLine.ContainsKey($lo - 1) -and $commentByLine[$lo - 1].WholeLine) { $lo-- }
    $hi = $ln
    while ($commentByLine.ContainsKey($hi + 1) -and $commentByLine[$hi + 1].WholeLine) { $hi++ }
    $blocks += [pscustomobject]@{ Marker = [int]$ln; Lo = [int]$lo; Hi = [int]$hi }
  }
  $sanctionedLines = New-Object System.Collections.Generic.HashSet[int]
  foreach ($b in $blocks) { for ($i = $b.Lo; $i -le $b.Hi; $i++) { [void]$sanctionedLines.Add([int]$i) } }

  $offending = @()
  $sanctioned = @()
  foreach ($ln in ($commentByLine.Keys | Sort-Object)) {
    $text = [string]$commentByLine[$ln].Text
    if (-not [regex]::IsMatch($text, '-Last\s+\d+')) { continue }
    $hits = @()
    foreach ($g in $gateIds) {
      # ⚠ **不用 `\b`**(#225 复审):.NET 的 `\w` 覆盖 `\p{L}`,汉字是词字符,于是 `\b3b\b`
      #   在「与3b同值」上**不匹配** —— 判据会静默依赖「拉丁 token 两侧留空格」这条没人管的
      #   排版约定。改成只排拉丁数字与字母的环视,和中日韩字符相邻时照样成立。
      if ([regex]::IsMatch($text, ('(?i)(?<![0-9A-Za-z])' + [regex]::Escape($g) + '(?![0-9A-Za-z])'))) { $hits += $g }
    }
    if ([regex]::IsMatch($text, '(?i)(?<![0-9A-Za-z])gate\s*\d+')) { $hits += 'gate N' }
    if ($hits.Count -eq 0) { continue }
    $row = [pscustomobject]@{ Line = [int]$ln; Text = $text.Trim(); Gates = (($hits | Sort-Object -Unique) -join '/') }
    if ($sanctionedLines.Contains([int]$ln)) { $sanctioned += $row } else { $offending += $row }
  }

  # **孤悬的标记要判负**(#225 复审):守卫那道对 `StaleExemptions` 判红,理由是「豁免会随
  # 代码漂移而悄悄失去精度」。这道没有对偶的话,真源名单哪天被挪走/删掉,标记还留在原地,
  # 它所在那一整段就成了一块**永久免检区**,谁往里补一句复述都照绿。
  $stale = @()
  foreach ($b in $blocks) {
    $covered = @($sanctioned | Where-Object { $_.Line -ge $b.Lo -and $_.Line -le $b.Hi })
    if ($covered.Count -eq 0) {
      $stale += [pscustomobject]@{ Line = $b.Marker; Text = ([string]$commentByLine[$b.Marker].Text).Trim() }
    }
  }

  return @{
    Comments   = $commentByLine.Count
    GateIds    = @($gateIds)
    Sanctioned = @($sanctioned)
    Offending  = @($offending)
    Stale      = @($stale)
  }
}

# ─────────────────────────────────────────────────────────────────────────────
# [SL-337 / #225 复审] **判据面下界**。与隔壁 `$floor` 同一条理由:判据面一小,门禁就近乎
# 恒真,而删除式照不出来 —— 它测的是「拆掉判据会红」,测不出「面缩水了」。
# 这道判据的塌缩路径比隔壁多:`Set-Gate` 改名、多包一层 helper、某道闸只用变量标签设、
# 或者档号提取被改坏,`$gateIds` 一空,整道判据当场恒真且**完全静默**。
# 两个下界都取「明显低于现状、又高于塌方」的数,只用来照出塌方,不用来对账。
# 返回 $null 表示没塌;否则返回一句可直接打给人看的话。
# ─────────────────────────────────────────────────────────────────────────────
function Test-GatesLastFloor {
  param([Parameter(Mandatory)]$Report, [switch]$DefaultTarget)
  # ⚠ **档号下界只对默认目标生效**。`-Path` 是公开参数,而「一个 `Set-Gate` 都没有」
  #   对别的脚本是**合法状态**、不是塌方—— 无条件套一个下界,犯的就是隔壁那道
  #   自己写过的「判据把自己的适用面写宽了,却按最宽那份的体量收口」。
  #   注释下界两边都留:非默认目标只挡「一条注释都没扫到」,而那多半是文件传错了。
  $cmFloor = if ($DefaultTarget) { 200 } else { 1 }
  if ($DefaultTarget -and @($Report.GateIds).Count -lt 5) {
    return ('只提取到 {0} 个档号(下界 5)—— Set-Gate 的形态多半被改了,判据面塌了' -f @($Report.GateIds).Count)
  }
  if ($Report.Comments -lt $cmFloor) {
    return ('只扫到 {0} 条注释(下界 {1})—— 注释提取多半被改坏了,判据面塌了' -f $Report.Comments, $cmFloor)
  }
  return $null
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

  # ⑮ **同名变量先赋脚本块、后赋路径**:那处 `& $var` 必须仍在判据面内(#217 复审指出的
  #    一处 fail-open —— 旧写法让 scriptBlockVars 先命中,调用点被静默排除)。
  $f15 = '$x = { param($a) $a }' + [Environment]::NewLine +
  '$x = Join-Path $root ' + "'tool.exe'" + [Environment]::NewLine + '& $x --version'
  $r15 = Get-GatesGuardReport -Source $f15
  & $check '⑮ 先脚本块后路径的同名变量被静默排出判据面(fail-open)' (@($r15.Sites).Count -eq 1)
  # ⑮b 反向:只当过闭包、从未赋成路径的变量仍**不得**入面,别把 ⑮ 改成「所有 & $var 都算外部」
  $f15b = '$cb = { param($a) $a }' + [Environment]::NewLine + '& $cb 1'
  $r15b = Get-GatesGuardReport -Source $f15b
  & $check '⑮b 纯闭包被当成外部命令(判据面被撑宽)' (@($r15b.Sites).Count -eq 0)

  # ---- 行内豁免标记(SL-331 后续批把 ③ 类三处从外部清单搬进了 gates.ps1 调用点旁)----
  $mkExempt = '# [gates-guard-exempt] 理由写在这里'
  # ⑯ 标记在**紧邻上一行** ⇒ 豁免
  $r16 = Get-GatesGuardReport -Source ($mkExempt + [Environment]::NewLine + 'cmake --version')
  & $check '⑯ 紧邻上一行的行内标记没生效' ((@($r16.Unguarded).Count -eq 0) -and (@($r16.Exempted).Count -eq 1))
  # ⑯b 标记在**同一行**(行尾)⇒ 同样豁免
  $r16b = Get-GatesGuardReport -Source ('cmake --version   ' + $mkExempt)
  & $check '⑯b 同一行行尾的行内标记没生效' (@($r16b.Unguarded).Count -eq 0)
  # ⑰ **理由为空**的标记不算标记 ⇒ 那一处仍判负(fail-closed:宁可红,也不接受一条没说理由的豁免)
  $r17 = Get-GatesGuardReport -Source ('# [gates-guard-exempt]' + [Environment]::NewLine + 'cmake --version')
  & $check '⑰ 没写理由的标记被当成了有效豁免' (@($r17.Unguarded).Count -eq 1)
  # ⑱ **孤悬标记**(附近没有无守卫调用点)⇒ 判负,免得它随代码漂成一句无人负责的注释
  $r18 = Get-GatesGuardReport -Source ($mkExempt + [Environment]::NewLine + '$x = 1')
  & $check '⑱ 孤悬的行内标记没被判负' (@($r18.StaleExemptions).Count -eq 1)
  # ⑲ 一个标记**只豁免一处**:同文本的第二处仍要判负
  $r19 = Get-GatesGuardReport -Source ($mkExempt + [Environment]::NewLine + 'cmake --version' + [Environment]::NewLine + 'cmake --version')
  & $check '⑲ 一个标记豁免了不止一处' (@($r19.Unguarded).Count -eq 1)
  # ⑳ 标记必须**紧跟注释起头**:夹在句子中间的提及不算 —— 否则本脚本的头注、以及任何解释
  #    这个标记的散文,都会变成一条豁免(本仓「扫描器入库才炸」那一族:判据被自己的文档喂出假绿)。
  $r20 = Get-GatesGuardReport -Source ('# 说明:标记形态是 [gates-guard-exempt] 理由' + [Environment]::NewLine + 'cmake --version')
  & $check '⑳ 句子中间提到标记名也被当成了豁免(文档能喂出假绿)' (@($r20.Unguarded).Count -eq 1)

  # ㉑ **一个标记只豁免一处**在「同一行两处同文本」上也要成立(#219 复审实测:旧的
  #    `行号|文本` 去重键在这里会撞,一个标记吞两处,rc 本该 1 却是 0)。
  $r21 = Get-GatesGuardReport -Source ($mkExempt + [Environment]::NewLine + 'cmake --version; cmake --version')
  & $check '㉑ 同一行的第二处调用跟着白拿了豁免(去重键撞了)' (@($r21.Unguarded).Count -eq 1)

  # ㉒ **正向格**:行尾标记绑本行、下一行那处判负。
  #    ⚠ 这一格与 ㉒b **钉不住** `Standalone` 那一刀(旧的 `-or` 逻辑在这两个输入上输出相同)——
  #    真正钉住它的是下面的 ㉓/㉓b。留着它们是为了「不误报」这一侧:改动不该让正常写法变红。
  $r22 = Get-GatesGuardReport -Source ('cmake --version   ' + $mkExempt + [Environment]::NewLine + 'ctest --preset x')
  & $check '㉒ 行尾标记绑错了行(下一行那处该判负)' (@($r22.Unguarded).Count -eq 1)
  # ㉒b **独占一行的标记只绑下一行**:本行没有调用点,不该白白孤悬判负
  $r22b = Get-GatesGuardReport -Source ($mkExempt + [Environment]::NewLine + 'ctest --preset x')
  & $check '㉒b 独占行标记没绑到下一行' ((@($r22b.Unguarded).Count -eq 0) -and (@($r22b.StaleExemptions).Count -eq 0))

  # ㉓ **真正钉住 `Standalone` 那一刀的一格**(#219 第 3 轮:原 ㉒/㉒b 没牙 —— 把那一刀改回旧的
  #    `-or` 逻辑,31 格仍全绿)。能区分新旧两套规则的输入,关键是让**行尾标记所在行的调用是
  #    「有守卫」的** —— 它因此不进 $unguarded,旧逻辑于是让**下一行**那处命中
  #    `$_.Line -eq ($u.Line - 1)`、白拿这条标记被静默豁免;新逻辑下行尾标记只绑本行,
  #    下一行拿不到 ⇒ 标记判为孤悬。旧逻辑 rc=0 / 新逻辑 rc=1,两侧实跑对过。
  $f23 = '$toolCmd = Get-Command tool -ErrorAction SilentlyContinue' + [Environment]::NewLine +
  'if ($toolCmd) { & tool --version }   ' + $mkExempt + [Environment]::NewLine +
  '& toolB --version'
  $r23 = Get-GatesGuardReport -Source $f23
  & $check '㉓ 行尾标记被下一行白拿了(旧的「同行或上一行」二选一逻辑)' (@($r23.Exempted).Count -eq 0)
  & $check '㉓b 该行尾标记本行无可豁免对象时应判孤悬' (@($r23.StaleExemptions).Count -eq 1)

  # ─── [SL-337] 第二道判据「注释里不得复述截断值名单」的自测格 ───────────────
  # 夹具全部**拼装**,零自我豁免:本文件自己的散文里也提这些字面量,若拿本文件当夹具,
  # 判据就会被自己的文档喂出结论(本仓「扫描器入库才炸」那一族)。
  $sg = "Set-Gate '3b gitleaks' `$ok" + [Environment]::NewLine + "Set-Gate '3c reuse lint' `$ok"
  $mkLast = '# [gates-last-source] 名单真源'

  # ㉔ **不误报**:注释里只有值、没有别的闸 ⇒ 不判负(单说本地事实不是名单)
  $l24 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# 此前这里是 -Last 40,没有独立理由')
  & $check '㉔ 只提一个值也被判成了名单(判据成了噪音源)' (@($l24.Offending).Count -eq 0)

  # ㉔b **代码不是注释**:真正的 `Select-Object -Last 30` 调用不该被算进来
  $l24b = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '$x | Select-Object -Last 30   # 3b 同款')
  & $check '㉔b 代码行里的 -Last 被当成了注释复述' (@($l24b.Offending).Count -eq 0)

  # ㉕ **正向**:值 × 带字母档号 同现 ⇒ 判负
  $l25 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# 与 3b 同值:这里也取 -Last 20')
  & $check '㉕ 值与档号同现没被判负(本判据的正主)' (@($l25.Offending).Count -eq 1)

  # ㉕b `gate <数>` 形态同样算档号(纯数字 3/5 不算,否则满地都是)
  $l25b = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# 与 gate 5(构建)同值,取 -Last 30')
  & $check '㉕b 「gate N」形态的档号没被认出来' (@($l25b.Offending).Count -eq 1)
  # ⚠ 夹具里必须真的有**纯数字档号的闸**(`3 prettier` / `5 构建`),否则这一格没牙:
  #   夹具里只放带字母的闸时,把「只认带字母档号」那一刀放宽掉,档号集合根本不会变,
  #   本格照样绿。这是反向注入实测出来的,不是想出来的(同族教训:#219 那两格无牙用例)。
  $sgNum = $sg + [Environment]::NewLine + "Set-Gate '3 prettier' `$ok" + [Environment]::NewLine + "Set-Gate '5 构建' `$ok"
  $l25c = Get-GatesLastListReport -Source ($sgNum + [Environment]::NewLine + '# 第 3 版把上界从 5 改成 -Last 30')
  & $check '㉕c 散文里的纯数字被当成了档号(噪音判据)' (@($l25c.Offending).Count -eq 0)

  # ㉖ **豁免**:带标记那一整段连续注释内不判负
  $l26 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + $mkLast + [Environment]::NewLine + '# 3b 是 -Last 20,其余四处 30')
  & $check '㉖ 带标记的真源段仍被判负' ((@($l26.Offending).Count -eq 0) -and (@($l26.Sanctioned).Count -eq 1))

  # ㉖b 豁免**只覆盖标记所在的那一段**:空行断开之后的第二处照样判负
  $l26b = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + $mkLast + [Environment]::NewLine +
    '# 3b 是 -Last 20,其余四处 30' + [Environment]::NewLine + '$x = 1' + [Environment]::NewLine +
    '# 抄一份:3c 也取 -Last 30')
  & $check '㉖b 标记把它那一段之外的复述也豁免了' (@($l26b.Offending).Count -eq 1)

  # ㉗ 句子中间提到标记名**不算**豁免(与 ⑳ 同族:文档不许喂出假绿)
  $l27 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine +
    '# 说明:真源段的标记形态是 [gates-last-source]' + [Environment]::NewLine + '# 3b 是 -Last 20')
  & $check '㉗ 句子中间提到标记名也豁免了(文档能喂出假绿)' (@($l27.Offending).Count -eq 1)

  # ㉘ **字符串里的井号不是注释**:tokenizer 与「行首 #」在这里分道扬镳
  $l28 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '$msg = "# 3b 是 -Last 20"')
  & $check '㉘ 字符串里的井号被当成了注释' (@($l28.Offending).Count -eq 0)

  # ㉙ 档号确实是**从 Set-Gate 提取的**,不是硬编码:抽掉 Set-Gate,同一条注释不再判负。
  #    ⚠ 这一格把**塌缩形态写成了期望值**(#225 复审)—— 它钉的「不是硬编码」是对的,
  #      但单有它等于给「档号集合塌了 ⇒ 整道判据恢复静默」背书。所以必须与
  #      ㉙b(下界)成对出现:一格证明不硬编码,另一格证明塌了会被照出来。
  $l29 = Get-GatesLastListReport -Source ('# 与 3b 同值:这里也取 -Last 20')
  & $check '㉙ 档号是硬编码的(抽掉 Set-Gate 后仍判负)' ((@($l29.Offending).Count -eq 0) -and (@($l29.GateIds).Count -eq 0))
  & $check '㉙b 档号集合塌掉了却没被下界拦住(判据静默恒真)' ($null -ne (Test-GatesLastFloor -Report $l29 -DefaultTarget))

  # ─── [#225 复审第 1 轮] 补的格 ────────────────────────────────────────────
  # ㉚ **裸标记不该豁免**:`[gates-guard-exempt]` 那一族强制非空理由(自测 ⑰),两个标记同款。
  $l30 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# [gates-last-source]' + [Environment]::NewLine + '# 3b 是 -Last 20')
  & $check '㉚ 没写理由的裸标记也豁免了整段' (@($l30.Offending).Count -eq 1)

  # ㉚b **孤悬标记判负**:标记还在、它那段里已经没有需要豁免的东西 ⇒ 一块永久免检区
  $l30b = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + $mkLast + [Environment]::NewLine + '# 这段已经不列名单了')
  & $check '㉚b 孤悬的真源标记没被判负(留下一块永久免检区)' (@($l30b.Stale).Count -eq 1)
  & $check '㉚b2 真源段还在用时被误判成孤悬' (@($l26.Stale).Count -eq 0)

  # ㉛ **行尾注释不该桥接两段**:`$x = 1   # 说明` 这样的代码行必须断开豁免范围。
  #    ⚠ ㉖b 用的是**不带**行尾注释的 `$x = 1`,照不出这一条(#225 复审实测)——
  #      两格的差别只有那句行尾注释,留着两格是因为它们分别钉「代码行断开」与「带注释的代码行也断开」。
  $l31 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + $mkLast + [Environment]::NewLine +
    '# 3b 是 -Last 20,其余四处 30' + [Environment]::NewLine + '$x = 1   # 顺带说明' + [Environment]::NewLine +
    '# 抄一份:3c 也取 -Last 30')
  & $check '㉛ 带行尾注释的代码行把两段注释桥接成了一段(豁免凭空跨过代码)' (@($l31.Offending).Count -eq 1)

  # ㉜ **标记大小写敏感**:与 [gates-guard-exempt] 同口径(SL-322 为标记族的大小写正反各钉过一格)
  $l32 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# [Gates-Last-Source] 名单真源' + [Environment]::NewLine + '# 3b 是 -Last 20')
  & $check '㉜ 大小写不同的标记也生效(与另一个标记两种口径)' (@($l32.Offending).Count -eq 1)

  # ㉝ **档号与汉字相邻时也要认出来**:.NET 的 \w 覆盖 \p{L},`\b3b\b` 在「与3b同值」上不匹配 ⇒
  #    判据会静默依赖「拉丁 token 两侧留空格」这条没人管的排版约定(#225 复审)。
  $l33 = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# 与3b同值,取 -Last 20')
  & $check '㉝ 档号与汉字相邻时没被认出来(判据靠排版约定活着)' (@($l33.Offending).Count -eq 1)
  # ㉝b 反侧仍要成立:`13b` 这种前后粘着数字/字母的不算档号
  $l33b = Get-GatesLastListReport -Source ($sg + [Environment]::NewLine + '# 版本 13b 的上界是 -Last 20')
  & $check '㉝b 粘在别的字母数字里的片段被当成了档号' (@($l33b.Offending).Count -eq 0)

  # ㉞ **档号走 AST,不走裸正则**:字符串里写一句 Set-Gate 不该注入幻影档号(#225 复审)
  $l34 = Get-GatesLastListReport -Source ('$msg = "Set-Gate ''9z 幻影'' $ok"' + [Environment]::NewLine + '# 与 9z 同值,取 -Last 20')
  & $check '㉞ 字符串里的 Set-Gate 注入了幻影档号' (@($l34.Offending).Count -eq 0)

  # ㉟ 下界:非默认目标降为 1,别对小脚本假红(与隔壁 $floor 同一条理由)
  # ⚠ 夹具里**不能有 Set-Gate**,否则这一格没牙:带上 `$sg` 时档号本就≥ 2,
  #   下界怎么写都不会红。能区分新旧两套的输入恰恰是「一个 Set-Gate 都没有」。
  $l35 = Get-GatesLastListReport -Source ('# 一条注释' + [Environment]::NewLine + '& some-tool --version')
  & $check '㉟ 对「没有 Set-Gate 的非默认目标」假红了' ($null -eq (Test-GatesLastFloor -Report $l35))
  & $check '㉟b 默认目标上档号塌成 0 却没红' ($null -ne (Test-GatesLastFloor -Report $l35 -DefaultTarget))
  # ㉟c **只有档号下界能挡住的那一档**:注释很多、档号为 0。
  #    ⚠ 没有这一格,拆掉档号下界也不会红 —— 上面那几格的夹具注释数本就 < 200,
  #      注释下界替它兼了底。这是反向注入实测出来的(删除式第 ⑤ 格当时是绿的)。
  $manyComments = (1..250 | ForEach-Object { '# 第 ' + $_ + ' 条' }) -join [Environment]::NewLine
  $l35c = Get-GatesLastListReport -Source $manyComments
  & $check '㉟c 档号塌成 0 而注释不少时,没有任何下界挡住' ($null -ne (Test-GatesLastFloor -Report $l35c -DefaultTarget))
  & $check '㉟d 同一输入在非默认目标上不该假红' ($null -eq (Test-GatesLastFloor -Report $l35c))

  # ㉟e **只有注释下界能挡住的那一档**:档号够多、注释几乎没有。
  #    ⚠ 没有这一格,把注释下界整刀删掉自测也不会红 —— 上面每一格的档号数都 < 5,
  #      在 -DefaultTarget 上档号那一刀总是先返回,替它兼了底。
  #      与 ㉟c 是**同一个洞的两个方向**:那一格没有档号、这一格没有注释,
  #      两格各自只能被一条下界拦住。注释提取一旦被改坏,`$commentByLine` 为空
  #      ⇒ `Offending` 恒为 0、判据静默全绿,而档号那一半走 AST 照样能取到十几个,
  #      档号下界拦不住。
  $sgFive = @('3b gitleaks', '3c reuse lint', '3d 设计盒真源', '3e web smoke', '3f 文档真源') |
  ForEach-Object { "Set-Gate '$_' `$ok" }
  $l35e = Get-GatesLastListReport -Source (($sgFive -join [Environment]::NewLine) + [Environment]::NewLine + '# 一条注释')
  & $check '㉟e 档号够多但注释塌成个位数时,没有任何下界挡住' ($null -ne (Test-GatesLastFloor -Report $l35e -DefaultTarget))
  & $check '㉟f 同一输入在非默认目标上假红了' ($null -eq (Test-GatesLastFloor -Report $l35e))

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

# 豁免**跟着被检文件走**(行内标记),所以不再需要「只对默认目标生效」那一档 ——
# 那是外部清单时代的补丁:清单是为 gates.ps1 写的,套到别的文件上会整份判成陈旧。
# 下界仍然只对默认目标生效(它是按 gates.ps1 的体量取的)。
if (-not $isDefaultPath) {
  Write-Host ('  [INFO] -Path 指向 {0}(非默认目标):判据面下界从 20 降为 1(只挡「一处都没扫到」);行内豁免标记照常生效。' -f (Split-Path -Leaf $Path)) -ForegroundColor Yellow
}
$report = Get-GatesGuardReport -Source $src -Exemptions $Exemptions

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
  Write-Host '  [FAIL] 豁免与代码对不上:' -ForegroundColor Red
  # 分两套文案:行内标记既没有「清单」也没有 `Hits`,照搬外部清单那句「请改准 Hits」
  # 会指向一个**不存在的旋钮**(#219 复审)。同一把尺子本文件下面为 Got=0 的两种成因用过。
  @($report.StaleExemptions | Where-Object { $_.Kind -eq 'marker' }) | ForEach-Object {
    Write-Host ('    {0}:孤悬的行内豁免标记 —— 附近已经没有「无守卫的外部调用」了' -f $_.Snippet) -ForegroundColor Red
    Write-Host ('      理由:' + $_.Reason) -ForegroundColor DarkGray
    Write-Host '      多半是那一处已经加上守卫、或被删/挪走了:把这条标记一并删掉或跟着挪。' -ForegroundColor Yellow
  }
  @($report.StaleExemptions | Where-Object { $_.Kind -ne 'marker' }) | ForEach-Object {
    # `$hit` 是从**无守卫**的调用点里筛的,所以 Got=0 有两个成因,而「被加上守卫了」在这个仓里
    # 更常见(谁给 `cmake --version` 包一层 if 就会触发)。只写「已经没有对应调用点」会让人
    # 去 gates.ps1 里找一行**还在**的代码。
    $why = if ($_.Got -eq 0) { '陈旧 —— 那处调用点要么被删了,要么**已经加上守卫**(两种都该删掉这条豁免)' } else { '被撑宽 —— 代码里多出了同文本的调用点,新的那处会白拿豁免' }
    Write-Host ('    {0}  期望 {1} 处、实得 {2} 处({3})' -f $_.Snippet, $_.Want, $_.Got, $why) -ForegroundColor Red
    Write-Host ('      理由:' + $_.Reason) -ForegroundColor DarkGray
  }
  if (@($report.StaleExemptions | Where-Object { $_.Kind -ne 'marker' }).Count -gt 0) {
    Write-Host '    (外部清单那几条)豁免随代码漂移而失效正是本判据要防的:请改准 Hits,或给新调用点加守卫。' -ForegroundColor Yellow
  }
}
if (@($report.Unguarded).Count -gt 0) {
  $bad = $true
  Write-Host ('  [FAIL] {0} 处外部命令调用点不在任何存在性守卫的 if/else 里:' -f @($report.Unguarded).Count) -ForegroundColor Red
  $report.Unguarded | ForEach-Object {
    Write-Host ('    {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $_.Line, $_.Text) -ForegroundColor Red
  }
  Write-Host '    修法:在调用外面加 `if (-not $<命令>Cmd) { Set-Gate … $false } else { … }`,' -ForegroundColor Yellow
  Write-Host '    并在调用前 `$global:LASTEXITCODE = 1`。' -ForegroundColor Yellow
  Write-Host '    确属「缺席即判负」(输出判据)的,加一条带理由的行内豁免标记:写在调用行**行尾**,' -ForegroundColor Yellow
  Write-Host '    或**独占上一行**(两种位置互斥:行尾绑本行、独占行绑下一行)。' -ForegroundColor Yellow
  Write-Host '    (形态见本脚本头注;理由不能空,标记孤悬会判负)。' -ForegroundColor Yellow
}
if ($bad) { exit 1 }

$byName = ($report.Sites | Group-Object Name | Sort-Object Name |
    ForEach-Object { '{0} {1}' -f $_.Name, $_.Count }) -join ' / '
Write-Host ('  gates 外部命令守卫完备:{0} 处调用、{1} 个名字,{2} 处按行内标记/清单豁免。' -f
  @($report.Sites).Count, @($report.Sites | Group-Object Name).Count, @($report.Exempted).Count)
Write-Host ('    分布:{0}' -f $byName)
# 把**被豁免的那几处逐条打出来**。豁免的本质是「这一处不查了」,而不查的东西最该显形 ——
# 只报一个数的话,谁悄悄多加一条标记就多一处不查,而输出上只是数字加一(#219 复审)。
foreach ($e in @($report.Exempted | Sort-Object Line)) {
  Write-Host ('    [豁免] {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $e.Line, $e.Text) -ForegroundColor DarkGray
}
# ─────────────────────────────────────────────────────────────────────────────
# [SL-337] 第二道判据:注释里不得复述截断值名单(形态与理由见上面那段头注)。
# 与守卫完备那道**分开报**:两道判的是两件事,压成一行会让人以为红的是守卫。
# ─────────────────────────────────────────────────────────────────────────────
$lastRep = Get-GatesLastListReport -Source $src
$lastFloorMsg = Test-GatesLastFloor -Report $lastRep -DefaultTarget:$isDefaultPath
if ($null -ne $lastFloorMsg) {
  Write-Host ('  [FAIL] 截断值名单判据:{0}' -f $lastFloorMsg) -ForegroundColor Red
  Write-Host '    判据面塌了不是「注释变干净了」 —— 档号集合与注释提取任一半塌掉,' -ForegroundColor Yellow
  Write-Host '    这道判据都会当场恒真且完全静默。塌的是哪一半,上面那句已经点名了。' -ForegroundColor Yellow
  exit 1
}
if (@($lastRep.Stale).Count -gt 0) {
  Write-Host ('  [FAIL] {0} 条孤悬的 [gates-last-source] 标记:' -f @($lastRep.Stale).Count) -ForegroundColor Red
  foreach ($st in $lastRep.Stale) {
    Write-Host ('    {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $st.Line, $st.Text) -ForegroundColor Red
  }
  Write-Host '    它那一整段里已经没有需要豁免的东西了 —— 留着就是一块永久免检区,' -ForegroundColor Yellow
  Write-Host '    谁往这段里补一句复述都照绿。名单挪走了就把标记跟着挪,或删掉。' -ForegroundColor Yellow
  exit 1
}
if (@($lastRep.Offending).Count -gt 0) {
  Write-Host ('  [FAIL] {0} 条注释在复述截断值名单(值 × 别的闸号同现):' -f @($lastRep.Offending).Count) -ForegroundColor Red
  foreach ($o in $lastRep.Offending) {
    Write-Host ('    {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $o.Line, $o.Text) -ForegroundColor Red
    Write-Host ('      点到的闸:{0}' -f $o.Gates) -ForegroundColor DarkGray
  }
  Write-Host '    修法:名单只留真源那一份(挂 [gates-last-source] 的那段),这里改成指路回去;' -ForegroundColor Yellow
  Write-Host '    或者干脆不提别的闸 —— 本地取什么值,读者看紧邻的那行代码即可。' -ForegroundColor Yellow
  Write-Host '    (为什么要有这道:同一份名单曾在本文件里躺着三份,#222 复审第 4 轮。)' -ForegroundColor Yellow
  exit 1
}
Write-Host ('  gates 注释未复述截断值名单:扫 {0} 条注释、{1} 个档号,真源段 {2} 条豁免。' -f
  $lastRep.Comments, @($lastRep.GateIds).Count, @($lastRep.Sanctioned).Count)
foreach ($sa in @($lastRep.Sanctioned)) {
  Write-Host ('    [真源] {0}:{1}  {2}' -f (Split-Path -Leaf $Path), $sa.Line, $sa.Text) -ForegroundColor DarkGray
}

exit 0
