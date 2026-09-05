<#
.SYNOPSIS  gates.ps1 里「降级/放行的计数必须接进汇总标签」那一族,改用 PowerShell AST 判(pwsh 7)。
.DESCRIPTION
  [SL-330c] 病灶不是某一条判据写错了,是**这一族一直跑在文本级正则上**。
  `check-gates-visibility.mjs` 的 ② 段(PS 侧)为了钉住「计数落进 `-f` 的实参」,
  在同一条手拼正则上压了**四层**补丁,每一层都是被复审实测逼出来的:
    · 只认单行(三处 `\s` 换成 `[^\S\n]`,否则断在 `=` / `-f` 前后的拆行照绿);
    · 格式串不得跨引号(`[^'\n]*`,否则贪婪吃到本行最后一个引号、把 `#` 一并吞下);
    · 尾部排掉 `#`(否则行尾注释里的计数名被当成实参);
    · 行首锚 `^` + `m`(否则引擎挪到行尾注释里整句引用的那条赋值上重新起头)。
  四层补丁守的是**同一件事**:「这个赋值语句的右边,是不是一个 `-f` 表达式,实参里有没有那个计数」。
  那是一个**语法**问题,不是文本问题 —— 语法问题交给语法分析器,四层补丁一次全部作废,
  而且拆行、换引号、加注释都不再假红。

  同一条推理适用于**运算符的大小写敏感性**:文本级要穷举 `-match` / `-notmatch` / `-replace` /
  `-split` 以及它们的 `-i…` 显式形态,而在 AST 里那**就是一个枚举值**(`Imatch` / `Cmatch` …),
  写不出「漏了一种写法」这种错。

  还有一层收益在**代跑链**上:计数口径(`$markerCount`)的正反例此前拿 **JS 正则**代跑
  PowerShell 的模式,而 [SL-322] 为「两个引擎不同义」记过三条边(量词里的花括号被 `-f` 当成
  格式规格、`{0}` 换几次、运算符的大小写默认)。本脚本在 **pwsh 里用真 `-cmatch` 跑**,
  那条代跑链连同它的三条边一起消失。

  判据(任一不满足即判负;**不写条数** —— 手抄的数在这一族里已经漂过四次):
    · 每一对(标签, 计数)都要有**至少一处赋值**,其右侧是 `-f`(Format)二元表达式,
      且 `-f` 的实参里出现那个计数变量,**并且格式串里有对应下标的 `{N}`** ——
      PowerShell 对多余实参静默忽略,少了这一半,「拼进去了但不显形」会全绿(复审第 1 轮);
    · 计数口径 `$markerCount` 有**一处定义**,模式里除 `{0}` 外不得有花括号
      (`-f` 会把 `{0,2}` 读成格式规格,得到一条永不命中的正则 ⇒ 有降级也恒报 0);
    · 该口径**实跑正反例**:真信号行(带前导空格的标记行)必须命中,成功路径的散文必须不命中;
    · **承接计数的那条赋值**(`$draftsAllow +=` / `$warnLines =`)里要用那份口径,
      不是「源码里某处用过就行」—— 后者在多一个用点之后就会被别处撑绿(复审第 1 轮);
    · 凡是拿这份口径或裸标记去匹配的地方,运算符必须是**大小写敏感**的那一支;
      `Select-String` 的 `-CaseSensitive` 按 **AST 参数**判(认前缀缩写 `-Case`,
      也认 `-CaseSensitive:$false`),不按文本找。

  边界(照实说):
    · 本脚本只管 **PowerShell 那一侧**。`smoke-*.mjs` 的出口(① 段)、`format.yml` 的 rc=3 分支
      (③ 段)、gate 3i 那圈脚本的散文守卫(§④)仍在 `check-gates-visibility.mjs` 里;
      「存在性」那一档(`$rc -eq 3` / `[FLAKY-SKIP]` 字样 / 三个计数器的初始化与自增)也留在那边 ——
      它们从没因为文本级而失效过,搬过来只是换个地方数。**哪几条在哪一侧**以本段为准。
    · 它**不验证运行时真打出了那行**;那由 SL-297 的删除式实跑覆盖。本脚本只保证**接线还在**。
    · 只用 **pwsh 7** 校验:Windows PowerShell 5.1 的分析器在这份源码上会报假错。
.EXAMPLE   pwsh scripts/check-gates-visibility.ps1
.EXAMPLE   pwsh scripts/check-gates-visibility.ps1 -SelfTest
#>
param(
  [string]$Path,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# 标记名一律**拼装**:本文件将来若进了「散文里不许出现方括号标记」那道守卫的执行面
# (SL-334 记着要把 .ps1 族收进去),连续字面量就是它第一个咬住的东西。
$script:MarkOpen = [char]91   # [
$script:MarkClose = [char]93  # ]
function New-Mark { param([string]$Name) $script:MarkOpen + $Name + $script:MarkClose }

# 单引号。**定义在脚本顶部**,不在 -SelfTest 块里:生产路径的失败文案也要拼它,
# 而那条路径只有判负才走到、自测又不覆盖它 —— 第一版定义在自测块里,生产文案里的引号
# 被吞成空串,没有 StrictMode 所以连报错都没有(复审第 1 轮实测点出)。
$script:Quote = [char]39

# 大小写**敏感**的匹配类运算符。AST 把敏感性放进枚举名里(`C…` / `I…`),
# 所以这里列的是枚举值,不是源码里的写法 —— 文本级那版要穷举 `-i?(?:not)?match` 之类的
# 拼写变体,而拼写变体列不全正是它栽过的地方。
$script:CaseSensitiveOps = @('Cmatch', 'Cnotmatch', 'Creplace', 'Csplit', 'Clike', 'Cnotlike')
$script:MatchFamilyOps = @(
  'Imatch', 'Cmatch', 'Inotmatch', 'Cnotmatch',
  'Ireplace', 'Creplace', 'Isplit', 'Csplit',
  # [SL-330c 复审第 1 轮] `-like` 一族。AST 消掉的是「**拼写变体**列不全」那一类
  # (`-imatch` 与 `-match` 在这里是同一个枚举名),消不掉「**运算符家族**列不全」那一类 ——
  # 后者正是自测 ㉕ 找出来的洞(当时漏的是 `-split`)。`$_ -like "*[WARN]*"` 是很自然的
  # 下一个写法(不用写转义的方括号,比 `-cmatch '\[WARN\]'` 顺手),漏掉它就等于这条边没收。
  'Ilike', 'Clike', 'Inotlike', 'Cnotlike'
)

function Get-VisibilityReport {
  <#
    生产路径与 -SelfTest **共用这一个函数**。自测若走另一份实现,绿的就只是那一份。
    入参给 -Source(字符串)或 -ScriptPath(文件),两条路都走同一个分析器。
  #>
  param(
    [string]$Source,
    [string]$ScriptPath,
    # 要断言的(标签, 计数)对。生产路径传三对;自测按格传。
    [object[]]$Pairs = @(),
    # 要断言的「标记 → 承接计数的变量」对。生产路径传两对;自测按格传。
    [object[]]$MarkerSinks = @()
  )

  $tokens = $null
  $errors = $null
  if ($ScriptPath) {
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$tokens, [ref]$errors)
  }
  else {
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($Source, [ref]$tokens, [ref]$errors)
  }

  $rep = [ordered]@{
    ParseErrors  = @($errors)
    Pairs        = @()   # 每对一条:Label / Count / Ok / Sites
    Pattern      = $null # $markerCount 的模式(读不到则 $null)
    PatternDefs  = 0     # 定义处数:一处才算「一处定义」
    BraceLeft    = $null # 抠掉 {0} 之后还剩的花括号(有就是病灶)
    MarkerUses   = @()   # 每个标记一条:Mark / Found / CaseSensitive
    EchoFound    = $false
    EchoCaseOk   = $false
    Offenders    = @()   # 大小写不敏感地匹配标记的地方
    SelectString = @()   # Select-String 的模式碰了标记却不是大小写敏感的那一支
    MissingPlaceholder = @() # 计数进了 -f 的实参,但格式串里没有对应的 {N}(实参被静默忽略)
  }
  if (@($errors).Count -gt 0) { return [pscustomobject]$rep }

  # ── 判据一:计数落进标签的 `-f` 实参 ────────────────────────────────────────
  # 形状是实测出来的(gates.ps1 那三处):
  #   AssignmentStatementAst
  #     .Left  = VariableExpressionAst      ($smokeLabel)
  #     .Right = CommandExpressionAst
  #       .Expression = BinaryExpressionAst  Operator = Format
  #         .Left  = StringConstantExpressionAst  ('{0}(!{1} …)')
  #         .Right = ArrayLiteralAst              ($smokeLabel, $smokeFlaky)
  # 只问「右侧是不是 Format 表达式、实参里有没有那个计数」——
  # 拆行、换引号、行尾注释、缩进,一概与判据无关。
  $assignments = @($ast.FindAll({
        param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst]
      }, $true))

  foreach ($pair in $Pairs) {
    $label = [string]$pair.Label
    $count = [string]$pair.Count
    $sites = @()
    foreach ($a in $assignments) {
      $left = $a.Left
      if ($left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
      if ($left.VariablePath.UserPath -ne $label) { continue }
      $expr = $a.Right
      if ($expr -is [System.Management.Automation.Language.CommandExpressionAst]) { $expr = $expr.Expression }
      if ($expr -isnot [System.Management.Automation.Language.BinaryExpressionAst]) { continue }
      if ("$($expr.Operator)" -ne 'Format') { continue }
      # 实参按**顶层元素**取:`-f` 的实参列表是 ArrayLiteralAst 的 Elements
      # (只有一个实参时它不是数组,那就是它自己)。要的是「计数是第几个实参」,
      # 而不只是「计数出现在子树里」—— 下面那条占位符判据要拿这个下标去对。
      $items = @()
      if ($expr.Right -is [System.Management.Automation.Language.ArrayLiteralAst]) {
        $items = @($expr.Right.Elements)
      }
      else { $items = @($expr.Right) }
      $idx = -1
      for ($i = 0; $i -lt $items.Count; $i++) {
        $hasCount = @($items[$i].FindAll({
              param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] -and
              $n.VariablePath.UserPath -eq $count
            }, $true)).Count -gt 0
        if ($hasCount) { $idx = $i; break }
      }
      if ($idx -lt 0) { continue }
      # ⚠ [SL-330c 复审第 1 轮] 光有实参**不够**:PowerShell 的 `-f` 对**多余实参静默忽略**
      #   (实测 `'{0}(x)' -f 'A','B'` → `A(x)`,不报错;反过来占位多于实参才抛)。于是
      #     `$smokeLabel = '{0}(没跑成)' -f $smokeLabel, $smokeFlaky`   ← `{1}` 被删了
      #   让计数**根本不显形**,而只问「实参里有没有它」的判据全绿 —— 与「计数退回滚屏」
      #   是同一个失效形态,触发它只需要有人重写文案时顺手删掉 `{1}` 而没删实参。
      #   所以还要问:格式串里**有没有 `{<这个实参的下标>}`**。
      $fmt = ''
      if ($expr.Left -is [System.Management.Automation.Language.StringConstantExpressionAst] -or
        $expr.Left -is [System.Management.Automation.Language.ExpandableStringExpressionAst]) {
        $fmt = $expr.Left.Value
      }
      $placeholders = @([regex]::Matches($fmt, '\{(\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
      if ($placeholders -notcontains $idx) {
        $rep.MissingPlaceholder += [pscustomobject]@{
          Label = $label; Count = $count; Index = $idx
          Line  = $a.Extent.StartLineNumber; Format = $fmt
        }
        continue
      }
      $sites += $a.Extent.StartLineNumber
    }
    $rep.Pairs += [pscustomobject]@{
      Label = $label; Count = $count; Ok = (@($sites).Count -gt 0); Sites = $sites
    }
  }

  # ── 判据二:计数口径只有一处定义,且模式里除 `{0}` 外没有花括号 ──────────────
  $defs = @()
  foreach ($a in $assignments) {
    $left = $a.Left
    if ($left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
    if ($left.VariablePath.UserPath -ne 'markerCount') { continue }
    $expr = $a.Right
    if ($expr -is [System.Management.Automation.Language.CommandExpressionAst]) { $expr = $expr.Expression }
    if ($expr -is [System.Management.Automation.Language.StringConstantExpressionAst]) { $defs += $expr.Value }
  }
  $rep.PatternDefs = @($defs).Count
  if (@($defs).Count -ge 1) {
    $rep.Pattern = $defs[0]
    $stripped = $defs[0].Replace('{0}', '')
    if ($stripped -match '[{}]') { $rep.BraceLeft = $stripped }
  }

  # ── 判据三:用点走那份口径,且运算符大小写敏感 ──────────────────────────────
  # `$_ -cmatch ($markerCount -f 'WARN')` 的形状:BinaryExpressionAst,右边是括号里的
  # 另一个 Format 表达式。这里问的是**运算符枚举**,不是源码怎么写的。
  $binaries = @($ast.FindAll({
        param($n) $n -is [System.Management.Automation.Language.BinaryExpressionAst]
      }, $true))

  $mentionsMarker = {
    param($node)
    @($node.FindAll({
          param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] -and
          $n.VariablePath.UserPath -eq 'markerCount'
        }, $true)).Count -gt 0
  }

  # ⚠ [SL-330c 复审第 1 轮] 钉的是「**承接计数的那条语句**里走了共享口径」,不是
  #   「源码里**某处**有一个敏感匹配用了它」。第一版写成了后者 —— 那比它替掉的文本级判据
  #   (`/\$draftsAllow\s*\+=[^\n]*\$markerCount\s+-f\s+'ALLOW'/`)**弱一档**,是搬家路上掉的牙:
  #   今天每个标记恰好只有一个用点,所以实扫结果一样;可只要将来多出第二个用点,把真正
  #   计数那一行改回裸字面量(`$draftsAllow += @($out | ? { $_ -cmatch '\[ALLOW\]' }).Count`),
  #   `Found` / `CaseSensitive` 仍由另一个用点撑着 ⇒ 判据全绿,而它数的是散文 ⇒ **0 条也报 1**。
  #   所以沿用 `Pairs` 那套「左边必须是那个变量」的做法:从**那条赋值语句的右子树**里找。
  foreach ($sink in $MarkerSinks) {
    $mark = [string]$sink.Mark
    $var = [string]$sink.Sink
    $found = $false
    $caseOk = $false
    foreach ($a in $assignments) {
      $left = $a.Left
      if ($left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
      if ($left.VariablePath.UserPath -ne $var) { continue }
      foreach ($b in @($a.FindAll({
              param($n) $n -is [System.Management.Automation.Language.BinaryExpressionAst]
            }, $true))) {
        if ("$($b.Operator)" -notin $script:MatchFamilyOps) { continue }
        if (-not (& $mentionsMarker $b.Right)) { continue }
        # 实例化的是哪个标记:`-f` 的实参里那个字符串常量。
        $args1 = @($b.Right.FindAll({
              param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst]
            }, $true) | ForEach-Object { $_.Value })
        if ($args1 -notcontains $mark) { continue }
        $found = $true
        if ("$($b.Operator)" -in $script:CaseSensitiveOps) { $caseOk = $true }
      }
    }
    $rep.MarkerUses += [pscustomobject]@{
      Mark = $mark; Sink = $var; Found = $found; CaseSensitive = $caseOk
    }
  }

  # 裸标记回显那一处:它**不走** `$markerCount`(是字面量),所以按标记名生成的断言钉不住它。
  # ⚠ 判「这个字面量是不是在匹配标记」要**拿它去匹配**,不能比子串:源码里写的是
  #   `'\[ALLOW\]|\[BASE\]'`,方括号是转义过的,`[ALLOW]` 根本不是它的子串
  #   (第一版就是这么写的,自测第 ⑱ ⑲ 两格当场红 —— 那两格接住了它)。
  $allow = New-Mark 'ALLOW'
  $base = New-Mark 'BASE'
  $warn = New-Mark 'WARN'
  $hits = {
    param([string]$Pattern, [string]$Text)
    try { return [bool]($Text -cmatch $Pattern) } catch { return $false }
  }
  foreach ($b in $binaries) {
    if ("$($b.Operator)" -notin $script:MatchFamilyOps) { continue }
    if ($b.Right -isnot [System.Management.Automation.Language.StringConstantExpressionAst]) { continue }
    $v = $b.Right.Value
    if (-not ((& $hits $v $allow) -and (& $hits $v $base))) { continue }
    $rep.EchoFound = $true
    if ("$($b.Operator)" -in $script:CaseSensitiveOps) { $rep.EchoCaseOk = $true }
  }

  # ── 判据四(反向):没有任何一处拿大小写不敏感的运算符去匹配标记 ────────────
  foreach ($b in $binaries) {
    if ("$($b.Operator)" -notin $script:MatchFamilyOps) { continue }
    if ("$($b.Operator)" -in $script:CaseSensitiveOps) { continue }
    $touchesMarker = (& $mentionsMarker $b.Right)
    $touchesLiteral = $false
    if ($b.Right -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
      $v = $b.Right.Value
      $touchesLiteral = (& $hits $v $allow) -or (& $hits $v $base) -or (& $hits $v $warn)
    }
    if ($touchesMarker -or $touchesLiteral) {
      $rep.Offenders += [pscustomobject]@{
        Line = $b.Extent.StartLineNumber; Op = "$($b.Operator)"; Text = $b.Extent.Text
      }
    }
  }
  # `Select-String` 是命令不是运算符,单独一条:它默认同样不区分大小写。
  foreach ($c in @($ast.FindAll({
          param($n) $n -is [System.Management.Automation.Language.CommandAst]
        }, $true))) {
    if ($c.GetCommandName() -ne 'Select-String') { continue }
    $txt = $c.Extent.Text
    # ⚠ **只管走 `$markerCount` 的那些**,不管裸字面量的 `Select-String -Pattern '\[WARN\]'`。
    #   这条边界是**照搬**文本级那一版的口径,不是我这一版收窄了:gate 3g 那处纯回显就是
    #   裸字面量写法(它只回显、不计数,所以大小写不敏感不会让汇总表多报一档)。
    #   第一版我写成「碰到标记就管」,实扫当场红在那一处 —— 那是**扩面**,不是搬家,
    #   本卡的面是「把同一批判据从文本级搬到语法级」。要不要连它一起收紧是另一张卡。
    #   下面自测有一格钉着这条边界,免得它退化成一句没人验的散文。
    if (-not (& $mentionsMarker $c)) { continue }
    # ⚠ [SL-330c 复审第 1 轮] 这里原来是 `$txt -notmatch '-CaseSensitive'` ——
    #   **整份「上 AST」的脚本里唯一剩下的文本级判据**,而且正好栽在本文件头注嘲笑文本级的
    #   那个点上(拼写变体列不全),两个方向都漏,两条都实测过:
    #     · **假绿**:`-CaseSensitive:$false` 里含着 `-CaseSensitive` 这几个字 ⇒ 判据说它敏感,
    #       实际不敏感;
    #     · **假红**:PowerShell 允许**参数名前缀缩写**,`Select-String` 的参数里 `Ca` 开头只有
    #       `CaseSensitive`,所以 `-Case` 在运行时完全等价 ⇒ 文本判据判红一个正确写法。
    #   AST 里两件事都是现成的:`CommandParameterAst.ParameterName` 给的是**写下的那个前缀**
    #   (`-Case` → `Case`),`.Argument` 给的是冒号形态的实参(`-CaseSensitive:$false` → `$false`)。
    $csParam = $null
    foreach ($el in $c.CommandElements) {
      if ($el -isnot [System.Management.Automation.Language.CommandParameterAst]) { continue }
      $pn = $el.ParameterName
      if ($pn.Length -eq 0) { continue }
      # 前缀匹配:写下的名字要是 `CaseSensitive` 的前缀(大小写不敏感 —— 参数名本身就不敏感)。
      if ('CaseSensitive'.StartsWith($pn, [System.StringComparison]::OrdinalIgnoreCase)) {
        $csParam = $el
        break
      }
    }
    $sensitive = $false
    if ($null -ne $csParam) {
      # 没有冒号实参 = 开关打开;有实参则要它不是 `$false`。
      $arg = $csParam.Argument
      if ($null -eq $arg) { $sensitive = $true }
      elseif ($arg.Extent.Text -notmatch '^\$false$') { $sensitive = $true }
    }
    if (-not $sensitive) {
      $rep.SelectString += [pscustomobject]@{ Line = $c.Extent.StartLineNumber; Text = $txt }
    }
  }

  return [pscustomobject]$rep
}

function Test-MarkerPattern {
  <#
    判据三的另一半:**实跑**正反例。此前这一步拿 JS 正则代跑 PowerShell 的模式,
    而 [SL-322] 为「两个引擎不同义」记过三条边。这里在 pwsh 里用真 `-cmatch` 跑,
    连同 `-f` 的实例化一起 —— 代跑链没有了,也就没有「两边不同义」这个类别。
    返回失败说明;全过返回 $null。
  #>
  param([string]$Pattern, [string]$Mark, [string]$Good, [string]$Bad)
  $re = $Pattern -f $Mark
  if (-not ($Good -cmatch $re)) {
    return ('计数口径 /{0}/ 数不到真信号行「{1}」—— 有降级也恒报 0,而恒 0 连删除式都照不出来' -f $re, $Good)
  }
  if ($Bad -cmatch $re) {
    return ('计数口径 /{0}/ 把成功路径的散文「{1}」也数进去了 —— 没有降级也会在汇总表里报出一档' -f $re, $Bad)
  }
  return $null
}

# ═════════════════════════════════════════════════════════════════════════════
if ($SelfTest) {
  # 夹具一律**拼装**,并且喂给**生产路径同一个函数**。
  $fails = @()
  $cells = 0
  $check = {
    param([string]$Name, [bool]$Ok)
    $script:cells++
    if (-not $Ok) { $script:fails += $Name }
  }
  $nl = [Environment]::NewLine
  $q = $script:Quote   # 自测里的简写,真源在脚本顶部(生产路径也要用它)
  $pair = @(@{ Label = 'lbl'; Count = 'cnt' })
  $goodPat = '^\s*\[{0}\]'

  # ① 计数落进 `-f` 实参 ⇒ 认得出
  $f1 = '$lbl = ' + $q + '{0}(!{1})' + $q + ' -f $lbl, $cnt'
  $r1 = Get-VisibilityReport -Source $f1 -Pairs $pair
  & $check '① 计数落在 -f 实参里却没认出来' ($r1.Pairs[0].Ok)

  # ② **拆成多行**照样认得出 —— 文本级那版为此压了「只认单行」那层补丁
  $f2 = '$lbl =' + $nl + '  ' + $q + '{0}(!{1})' + $q + ' -f' + $nl + '    $lbl, $cnt'
  $r2 = Get-VisibilityReport -Source $f2 -Pairs $pair
  & $check '② 拆成多行的赋值被判成「没拼进标签」(文本级那版正是在这里假红)' ($r2.Pairs[0].Ok)

  # ③ 行尾注释里整句引用一条真赋值 ⇒ 不得放过(注释根本不进 AST)
  $f3 = '$lbl = $suffixed   # ' + '$lbl = ' + $q + '{0}(!{1})' + $q + ' -f $lbl, $cnt'
  $r3 = Get-VisibilityReport -Source $f3 -Pairs $pair
  & $check '③ 行尾注释里引用的赋值被当成真接线' (-not $r3.Pairs[0].Ok)

  # ④ 计数只在**条件**里被读、真正拼进标签的串不带它 ⇒ 判负
  $f4 = 'if ($cnt -gt 0) { $lbl = ' + $q + '{0}(x)' + $q + ' -f $lbl }'
  $r4 = Get-VisibilityReport -Source $f4 -Pairs $pair
  & $check '④ 计数只出现在条件里也被算作「拼进标签」' (-not $r4.Pairs[0].Ok)

  # ⑤ 计数退回滚屏(Write-Host 里有 -f 与计数,但没赋回标签)⇒ 判负
  $f5 = 'Write-Host (' + $q + '{0}(!{1})' + $q + ' -f $lbl, $cnt)'
  $r5 = Get-VisibilityReport -Source $f5 -Pairs $pair
  & $check '⑤ 计数只进了 Write-Host 也被算作「拼进标签」' (-not $r5.Pairs[0].Ok)

  # ⑥ 右边不是 `-f` 而是字符串拼接 ⇒ 判负(标签里确实没有格式化实参这一说)
  $f6 = '$lbl = $lbl + $cnt'
  $r6 = Get-VisibilityReport -Source $f6 -Pairs $pair
  & $check '⑥ 右侧不是 -f 表达式却被算作「拼进标签」' (-not $r6.Pairs[0].Ok)

  # ⑦ 变量改名(`$cntX`)⇒ 判负,不得命中前缀
  $f7 = '$lbl = ' + $q + '{0}(!{1})' + $q + ' -f $lbl, $cntX'
  $r7 = Get-VisibilityReport -Source $f7 -Pairs $pair
  & $check '⑦ 计数改名成 $cntX 仍被算作命中(文本级那版靠末尾 \b 才挡住)' (-not $r7.Pairs[0].Ok)

  # ⑧ 口径定义读得出
  $f8 = '$markerCount = ' + $q + $goodPat + $q
  $r8 = Get-VisibilityReport -Source $f8
  & $check '⑧ 读不到 $markerCount 的一处定义' ($r8.PatternDefs -eq 1 -and $r8.Pattern -eq $goodPat)

  # ⑨ 模式里除 `{0}` 外还有花括号 ⇒ 判负
  $f9 = '$markerCount = ' + $q + '^\s{0,2}\[{0}\]' + $q
  $r9 = Get-VisibilityReport -Source $f9
  & $check '⑨ 模式里的 {0,2} 没被抓到(-f 会把它读成格式规格,得到一条永不命中的正则)' ($null -ne $r9.BraceLeft)

  # ⑩ 口径实跑:真信号行命中、成功散文不命中
  $goodLine = '  ' + (New-Mark 'WARN') + ' 本机没有 grep,跳过这一档。'
  $badLine = '① 手写用例全过;另见 ' + (New-Mark 'WARN') + ' 那一档…'
  & $check '⑩ 正确口径在实跑里没通过' ($null -eq (Test-MarkerPattern -Pattern $goodPat -Mark 'WARN' -Good $goodLine -Bad $badLine))

  # ⑪ 裸标记口径(不吃前导空格)⇒ 实跑必须判负(它会把散文数进去)
  & $check '⑪ 裸标记口径没被实跑判负(0 条也会恒报 1)' ($null -ne (Test-MarkerPattern -Pattern '\[{0}\]' -Mark 'WARN' -Good $goodLine -Bad $badLine))

  # ⑫ 只写 `^`(不吃前导空格)⇒ 实跑必须判负(有降级也恒报 0)
  & $check '⑫ 不吃前导空格的口径没被实跑判负(有降级也恒报 0)' ($null -ne (Test-MarkerPattern -Pattern '^\[{0}\]' -Mark 'WARN' -Good $goodLine -Bad $badLine))

  # ⑬ 用点走口径且大小写敏感,而且**长在承接计数的那条赋值里** ⇒ 认得出
  $sinkW = @(@{ Mark = 'WARN'; Sink = 'warnLines' })
  $f13 = '$warnLines = @($out | Where-Object { $_ -cmatch ($markerCount -f ' + $q + 'WARN' + $q + ') })'
  $r13 = Get-VisibilityReport -Source $f13 -MarkerSinks $sinkW
  & $check '⑬ -cmatch 的用点没被认出来' ($r13.MarkerUses[0].Found -and $r13.MarkerUses[0].CaseSensitive)

  # ⑭ 用点退回 `-match` ⇒ 反向判据必须抓到(AST 里它是另一个枚举值)
  $f14 = '$x | Where-Object { $_ -match ($markerCount -f ' + $q + 'WARN' + $q + ') }'
  $r14 = Get-VisibilityReport -Source $f14 -Marks @('WARN')
  & $check '⑭ -match 的用点没被反向判据抓到' (@($r14.Offenders).Count -eq 1 -and -not $r14.MarkerUses[0].CaseSensitive)

  # ⑮ 显式不敏感形态 `-imatch` ⇒ 同样抓到(文本级那版要专门补一条 `-i?` 才挡得住)
  $f15 = '$x | Where-Object { $_ -imatch ($markerCount -f ' + $q + 'WARN' + $q + ') }'
  $r15 = Get-VisibilityReport -Source $f15 -Marks @('WARN')
  & $check '⑮ -imatch 没被抓到' (@($r15.Offenders).Count -eq 1)

  # ⑯ `-notmatch`(反过来滤掉标记行)⇒ 同样抓到
  $f16 = '$x | Where-Object { $_ -notmatch ($markerCount -f ' + $q + 'WARN' + $q + ') }'
  $r16 = Get-VisibilityReport -Source $f16 -Marks @('WARN')
  & $check '⑯ -notmatch 没被抓到' (@($r16.Offenders).Count -eq 1)

  # ⑰ `-replace` / `-split` 一族 ⇒ 同样抓到
  $f17 = '$x = $y -replace ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r17 = Get-VisibilityReport -Source $f17 -Marks @('WARN')
  & $check '⑰ -replace 没被抓到' (@($r17.Offenders).Count -eq 1)

  # ⑱ 裸标记回显那一处:大小写敏感 ⇒ 认得出
  $echoPat = '\' + $script:MarkOpen + 'ALLOW\' + $script:MarkClose + '|\' + $script:MarkOpen + 'BASE\' + $script:MarkClose
  $f18 = '$x | Where-Object { $_ -cmatch ' + $q + $echoPat + $q + ' }'
  $r18 = Get-VisibilityReport -Source $f18
  & $check '⑱ ALLOW+BASE 的裸回显没被认出来' ($r18.EchoFound -and $r18.EchoCaseOk)

  # ⑲ 回显那一处退回 `-match` ⇒ 抓到(小写标记会回显却不计数)
  $f19 = '$x | Where-Object { $_ -match ' + $q + $echoPat + $q + ' }'
  $r19 = Get-VisibilityReport -Source $f19
  & $check '⑲ 回显那处的 -match 没被抓到' ($r19.EchoFound -and -not $r19.EchoCaseOk -and @($r19.Offenders).Count -eq 1)

  # 20. `Select-String -Pattern` 且没带 -CaseSensitive ⇒ 抓到(它是命令,不是运算符)
  $f20 = '$x | Select-String -Pattern ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r20 = Get-VisibilityReport -Source $f20
  & $check '⑳ Select-String -Pattern 没被抓到' (@($r20.SelectString).Count -eq 1)

  # 21. 带 -CaseSensitive 的 Select-String ⇒ 不得误报
  $f21 = '$x | Select-String -CaseSensitive -Pattern ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r21 = Get-VisibilityReport -Source $f21
  & $check '㉑ 带 -CaseSensitive 的 Select-String 被误报' (@($r21.SelectString).Count -eq 0)

  # 22. 与标记无关的 `-match` ⇒ 不得误报(反向判据只管标记那一族)
  $f22 = 'if ($nv -match ' + $q + '^v(\d+)\.' + $q + ') { }'
  $r22 = Get-VisibilityReport -Source $f22
  & $check '㉒ 与标记无关的 -match 被误报成违规' (@($r22.Offenders).Count -eq 0)

  # 23. **边界格**:裸字面量的 `Select-String -Pattern '\[WARN\]'` **有意不管**。
  #   gate 3g 那处纯回显正是这个写法(只回显、不计数),文本级那一版也只管走 `$markerCount`
  #   的用点。这一格不是「判据的能力」,是**判据的边界** —— 钉住它,免得下一个人把这条
  #   边界当成漏洞顺手「补全」,那会让实扫在一处有意放过的地方变红。
  $f23 = '$x | Select-String -Pattern ' + $q + '\[WARN\]' + $q
  $r23 = Get-VisibilityReport -Source $f23
  & $check '㉓ 裸字面量的 Select-String 被管了 —— 那超出本判据的面(见 Select-String 那段注释)' (@($r23.SelectString).Count -eq 0)

  # ── 下面两格是**删除式扫出来的**:拆掉判据里对应的那一处,上面 23 格一格都没红 ──
  #   记在这里不只是补两格,也是记一次「自测看着齐、其实有洞」。两个洞同一个形状:
  #   **判据里有一个条件,而没有任何夹具区分它在与不在**。
  # 24. 计数确实拼进了某个 `-f`,但**赋给的是别的变量** ⇒ 必须判负。
  #     拆掉「左边必须是那个标签」那一行之后,上面 23 格全绿。
  $f24 = '$other = ' + $q + '{0}(!{1})' + $q + ' -f $lbl, $cnt'
  $r24 = Get-VisibilityReport -Source $f24 -Pairs $pair
  & $check '㉔ 计数拼进了别的变量,也被算作「落进标签」' (-not $r24.Pairs[0].Ok)

  # 25. `-split` 一族也要被反向判据抓到。匹配家族在这里是**一张枚举清单**,
  #     清单漏一项就漏一族;拆掉 `Isplit`/`Csplit` 之后上面 23 格全绿(⑰ 只钉了 `-replace`)。
  $f25 = '$x = $y -split ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r25 = Get-VisibilityReport -Source $f25 -Marks @('WARN')
  & $check '㉕ -split 没被抓到(匹配家族的枚举清单漏了一项)' (@($r25.Offenders).Count -eq 1)

  # ── 下面五格是**复审第 1 轮**逼出来的,每格对应一条新判据或一处补齐 ──
  # 26. `-f` 的实参里有计数,但**格式串里没有对应的 `{N}`** ⇒ 判负。
  #     实测:PowerShell 对多余实参静默忽略(`'{0}(x)' -f 'A','B'` → `A(x)`),
  #     所以这个写法让计数根本不显形,而只问「实参里有没有」的判据全绿。
  $f26 = '$lbl = ' + $q + '{0}(没跑成)' + $q + ' -f $lbl, $cnt'
  $r26 = Get-VisibilityReport -Source $f26 -Pairs $pair
  & $check '㉖ 格式串里删掉了 {1} 却留着实参,仍被算作「落进标签」' (-not $r26.Pairs[0].Ok)

  # 27. 承接计数的那条赋值**没走**共享口径(改回裸字面量),而别处有一个合规用点
  #     ⇒ 必须判负。第一版写成「源码里某处有就行」,这一格当时是绿的(搬家掉的牙)。
  $f27 = '$warnLines = @($out | Where-Object { $_ -cmatch ' + $q + '\[WARN\]' + $q + ' })' + $nl +
  '$other = @($out | Where-Object { $_ -cmatch ($markerCount -f ' + $q + 'WARN' + $q + ') })'
  $r27 = Get-VisibilityReport -Source $f27 -MarkerSinks $sinkW
  & $check '㉗ 承接计数那条改回裸字面量,却被别处的合规用点撑绿了' (-not $r27.MarkerUses[0].Found)

  # 28. `Select-String -CaseSensitive:$false` ⇒ 必须抓到(文本级那版会因为字面含
  #     `-CaseSensitive` 而**假绿**)。
  $f28 = '$x | Select-String -CaseSensitive:$false -Pattern ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r28 = Get-VisibilityReport -Source $f28
  & $check '㉘ -CaseSensitive:$false 被当成了大小写敏感' (@($r28.SelectString).Count -eq 1)

  # 29. `Select-String -Case`(参数名前缀缩写,运行时等价)⇒ 不得误报。
  #     文本级那版会因为 `-notmatch '-CaseSensitive'` 命中而**假红**一个正确写法。
  $f29 = '$x | Select-String -Case -Pattern ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r29 = Get-VisibilityReport -Source $f29
  & $check '㉙ -Case(合法的前缀缩写)被误报成不敏感' (@($r29.SelectString).Count -eq 0)

  # 30. `-like` 一族也要被反向判据抓到(㉕ 同款:枚举清单漏一项就漏一族)。
  $f30 = '$x = $y -like ($markerCount -f ' + $q + 'WARN' + $q + ')'
  $r30 = Get-VisibilityReport -Source $f30
  & $check '㉚ -like 没被抓到(匹配家族的枚举清单漏了一族)' (@($r30.Offenders).Count -eq 1)

  # 31. 右侧是**字符串拼接**而不是 `-f`,但左边那截长得像格式串 ⇒ 仍要判负。
  #     ⑥ 那一格(`$lbl = $lbl + $cnt`)挡不住这个:它的左截不是字符串常量,
  #     于是占位符那一半会替 `-f` 那一半把它拦下,`-f` 判据被**兜住而不是被验证**。
  #     删除式里「拆掉 Format 检查」这一注入原来因此全绿 —— 这一格专门区分那两半。
  $f31 = '$lbl = ' + $q + '{0}(!{1})' + $q + ' + $cnt'
  $r31 = Get-VisibilityReport -Source $f31 -Pairs $pair
  & $check '㉛ 字符串拼接(不是 -f)也被算作「落进标签」' (-not $r31.Pairs[0].Ok)

  if (@($fails).Count -gt 0) {
    Write-Host 'check-gates-visibility.ps1 --self-test 失败:' -ForegroundColor Red
    foreach ($f in $fails) { Write-Host ('  ' + $f) -ForegroundColor Red }
    exit 1
  }
  Write-Host ('check-gates-visibility.ps1 -SelfTest 通过:{0} 格' -f $cells) -ForegroundColor Green
  exit 0
}

# ═════════════════════════════════════════════════════════════════════════════
# 生产路径
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Path) { $Path = Join-Path $RepoRoot (Join-Path 'scripts' 'gates.ps1') }
if (-not (Test-Path -LiteralPath $Path)) {
  Write-Host ('  [FAIL] 找不到 {0}' -f $Path) -ForegroundColor Red
  exit 1
}

$PAIRS = @(
  @{ Label = 'smokeLabel'; Count = 'smokeFlaky'; Why = 'gate 3e 的 rc=3(浏览器在但没连上)' },
  @{ Label = 'parityLabel'; Count = 'draftsAllow'; Why = 'gate 3i 的 ALLOW 放行' },
  @{ Label = 'parityLabel'; Count = 'parityWarn'; Why = 'gate 3i 的 WARN 降级' }
)
# 标记 → **承接计数的那个变量**。判据钉的是那条赋值语句,不是「源码里某处」——
# 理由写在 Get-VisibilityReport 里那段 ⚠ 旁边(复审第 1 轮:搬家掉了这颗牙)。
$MARKER_SINKS = @(
  @{ Mark = 'ALLOW'; Sink = 'draftsAllow' },
  @{ Mark = 'WARN'; Sink = 'warnLines' }
)

$rep = Get-VisibilityReport -ScriptPath $Path -Pairs $PAIRS -MarkerSinks $MARKER_SINKS
$bad = @()

if (@($rep.ParseErrors).Count -gt 0) {
  Write-Host ('  [FAIL] {0} 解析不了({1} 处语法错)' -f (Split-Path -Leaf $Path), @($rep.ParseErrors).Count) -ForegroundColor Red
  Write-Host ('    第一处:{0}' -f $rep.ParseErrors[0].Message) -ForegroundColor Red
  exit 1
}

foreach ($p in $rep.Pairs) {
  $why = (@($PAIRS | Where-Object { $_.Label -eq $p.Label -and $_.Count -eq $p.Count })[0]).Why
  if (-not $p.Ok) {
    $bad += ('`${0}` 没有落在任何一处 `${1} = ' + $script:Quote + '…' + $script:Quote + ' -f …` 的实参里({2})—— ' +
      '计数只会出现在滚屏里,而跑完 gates 的人看的是汇总表:这正是 SL-297 要堵的那个洞原样复现。') -f
    $p.Count, $p.Label, $why
  }
}

# 计数进了实参、格式串里却没有对应的占位符 —— 单独一条,别并进上面那句:
# 上面那句说的是「没拼进标签」,而这里是「拼进去了但不显形」,两种真因不同、修法也不同。
foreach ($m in $rep.MissingPlaceholder) {
  $bad += ('第 {0} 行:`${1}` 是 `-f` 的第 {2} 个实参,而格式串 /{3}/ 里没有 `{{{2}}}` —— ' +
    'PowerShell 对多余实参**静默忽略**,所以这个计数根本不会出现在汇总标签里。' +
    '要么把占位符加回去,要么把这个实参删掉(别留一个不显形的计数)。') -f
  $m.Line, $m.Count, $m.Index, $m.Format
}

if ($rep.PatternDefs -ne 1) {
  $bad += ('`$markerCount` 的定义有 {0} 处(要求恰好 1 处)—— 两处计数共用一份口径,' +
    '抄成两份就会只改一份') -f $rep.PatternDefs
}
elseif ($null -ne $rep.BraceLeft) {
  $bad += ('`$markerCount` 的模式里除 `{{0}}` 外还有花括号(抠掉 `{{0}}` 后剩 /{0}/)—— ' +
    'PowerShell 的 `-f` 会把 `{{0,2}}` 这类量词读成格式规格,得到一条永不命中的正则,' +
    '于是有降级也恒报 0;要写量词请改用不带花括号的等价形态') -f $rep.BraceLeft
}
else {
  foreach ($mark in @($MARKER_SINKS | ForEach-Object { $_.Mark })) {
    $good = '  ' + (New-Mark $mark) + ' 真信号行'
    $prose = '成功路径的散文里提了一嘴 ' + (New-Mark $mark) + ' 那一档'
    $msg = Test-MarkerPattern -Pattern $rep.Pattern -Mark $mark -Good $good -Bad $prose
    if ($msg) { $bad += $msg }
  }
}

foreach ($u in $rep.MarkerUses) {
  if (-not $u.Found) {
    $bad += ('没有一处按 `$markerCount -f {1}{0}{1}` 实例化的匹配 —— 口径又被抄了一份字面量,' +
      '两份就会只改一份') -f $u.Mark, $script:Quote
  }
  elseif (-not $u.CaseSensitive) {
    $bad += ('`{0}` 的匹配不是大小写敏感的那一支 —— 小写标记会被 gates 数进去而守卫扫不到,' +
      '两边各自绿、数对不上') -f $u.Mark
  }
}

if (-not $rep.EchoFound) {
  $bad += 'ALLOW+BASE 的裸标记回显不见了 —— 这一圈脚本的成功输出不再按标记回显'
}
elseif (-not $rep.EchoCaseOk) {
  $bad += 'ALLOW+BASE 的回显不是大小写敏感的那一支 —— 小写标记会被回显却不计数,滚屏里有一行、汇总表里 0 处'
}

foreach ($o in $rep.Offenders) {
  $bad += ('第 {0} 行用大小写不敏感的 `{1}` 匹配标记:{2}' -f $o.Line, $o.Op, $o.Text)
}
foreach ($s in $rep.SelectString) {
  $bad += ('第 {0} 行的 `Select-String` 没带 `-CaseSensitive`:{1}' -f $s.Line, $s.Text)
}

if (@($bad).Count -gt 0) {
  Write-Host ('  [FAIL] gates 显形接线(PowerShell 侧){0} 条不成立:' -f @($bad).Count) -ForegroundColor Red
  foreach ($b in $bad) { Write-Host ('    ' + $b) -ForegroundColor Red }
  Write-Host '    口径与边界见本文件头注;JS 侧那三段(冒烟出口 / CI 分支 / 散文守卫)在 check-gates-visibility.mjs。' -ForegroundColor Yellow
  exit 1
}

Write-Host ('  gates 显形接线(PowerShell 侧)通过:{0} 对计数都落在 -f 实参里**且格式串有对应占位符**,计数口径一处定义、实跑正反例通过,承接计数的那两条赋值都走那份口径,标记匹配一律大小写敏感(Select-String 按 AST 参数判)。' -f @($rep.Pairs).Count)
exit 0
