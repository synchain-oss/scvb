# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS  SCVB 发布打包的唯一真源(06 §3.8 / 07 T40):把三个 .vst3 bundle + 合规文件组打成一个 zip,
           另出独立的 .sha256 与 package-summary.md。release.yml 的 Package 步只调用本脚本。
.DESCRIPTION
  06 §3.8 六条硬要求在本脚本里的落点(按条号):
    ① 文件名由版本号算出(-Version,缺省读 CMakeLists.txt 的 project(SCVB VERSION x.y.z)),不写字面量。
    ② 按目录枚举 *.vst3 bundle,断言**恰好 3 个**且名字恰好是 SCVB Input / Output / Monitor
       (06 原文写两个,[J75] T45 加了 Monitor;[J137] 定为三个 bundle 同一个 zip 发布,Monitor 是
       可选安装,INSTALL.txt 里标为可选)。打包保住 `<name>.vst3/Contents/...` 层级。
    ③ .sha256 为独立文件(sha256sum 格式,`sha256sum -c` 可直接校验);package-summary.md 含
       version / zipFileName / sizeBytes / sha256 / releaseDate。
    ④ 生成 INSTALL.txt:安装路径、九条使用规则的前 3 条(从用户手册的生成区原样取,不在这里抄第二份)、
       未签名插件的「解除锁定 / SmartScreen」步骤(U13)、精确到 tag 的源码声明。
    ⑤ zip 根目录带 LICENSE.txt(= 仓库 LICENSE,GPLv3 全文)、THIRD-PARTY-NOTICES.md、LICENSES/ 全部许可证
       全文、third_party/notices/ 全部上游版权 / 许可声明原文(与仓库同一相对路径,所以 NOTICES 里写的
       `third_party/notices/...` 在解压目录里原样可查);THIRD-PARTY-NOTICES.md「随二进制分发」表点名的每个
       许可证 + 本项目的 GPL-3.0-or-later 都必须在 LICENSES/ 里有全文,缺就红(演练 tag 用
       -AllowMissingLicenseTexts 降为警告);反过来 LICENSES/ 里的每份全文也必须被这张表(或
       GPL-3.0-or-later)点名,多出来就红,演练 tag 也不放行。THIRD-PARTY-NOTICES.md 全文里点名的每个声明文件路径
       (`third_party/notices/<文件>` / `LICENSES/<文件>`)都必须是仓库里存在、且在上面打包范围内的文件,
       否则红,演练 tag 也不放行。
       U2 裁定不附 LICENSE-EXCEPTION.md,所以没有它。
    ⑥ 打包后重新打开 zip 断言:三个 bundle 的 DLL 条目、上面每个合规文件、INSTALL.txt 的源码声明行都在;
       THIRD-PARTY-NOTICES.md 点名的每个声明文件路径在 zip 里都有同名条目;每个条目解出来的字节与源文件
       逐一比哈希;根目录不许有清单外的东西(third_party/ 下只许有 notices/)。
  确定性:条目按序数排序,时间戳统一取 SOURCE_DATE_EPOCH / HEAD 提交时间(都取不到才用 1980-01-01),
  所以同一份输入在同一运行时下重跑,zip 的 sha256 不变。不同 .NET 运行时(PS 5.1 与 7)的 deflate
  实现不同,跨运行时不保证字节一致。
  [B 线 M10] macOS 包由 scripts/package-macos.sh 打,但合规判据与 INSTALL.txt 的规则原文**仍只在本脚本**:
  它调 `-Preflight -PreflightOut <文件>`,本脚本把 preflight 核过的结果(版本 / tag、要打进 zip 的
  LICENSES/ 与 third_party/notices/ 文件、NOTICES 点名的声明文件路径、九条规则的前 3 条 en/zh)写成机读清单,
  sh 只读这份清单,不在 sh 里重写第二套解析。不传 -PreflightOut 时行为与输出逐字不变。
.EXAMPLE   pwsh scripts/package.ps1 -Version 1.2.3 -BuildDir build -OutDir dist
.EXAMPLE   pwsh scripts/package.ps1 -Version 0.0.0-dryrun -BuildDir D:\artifacts -OutDir D:\out
.EXAMPLE   pwsh scripts/package.ps1 -Preflight -PreflightOut /tmp/scvb-preflight.tsv
#>
param(
  # 缺省 = CMakeLists.txt 的 project(SCVB VERSION x.y.z)。tag 带 -rc.N / -beta.N 时由 release.yml 传整串(不含 v)。
  [string]$Version,
  # 缺省 = "v$Version"。INSTALL.txt 的源码 URL 与手册链接都钉在这个 tag 上。
  [string]$Tag,
  [string]$BuildDir = 'build',
  [string]$OutDir = 'dist',
  # 缺省 = 仓库 HEAD。只写进 INSTALL.txt / summary 作溯源,不参与判定。
  [string]$SourceCommit,
  # 只给演练 tag(v0.0.0-test)用:THIRD-PARTY-NOTICES 点名的许可证在 LICENSES/ 里缺全文时降为 [WARN]
  # 并记进 summary,好让流水线演练不被合规缺口卡住。正式版 / rc / beta 不传,缺就红。
  [switch]$AllowMissingLicenseTexts,
  # 只跑不依赖构建产物的检查就退出(见下方 Preflight 段)。
  [switch]$Preflight,
  # 只能配合 -Preflight:把 preflight 核过的结果写成机读清单(格式见下方 Preflight 段),给 package-macos.sh 用。
  [string]$PreflightOut
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $PSScriptRoot 'lib/release-version.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$RepoSlug = 'synchain-oss/scvb'
$ExpectedBundles = @('SCVB Input.vst3', 'SCVB Monitor.vst3', 'SCVB Output.vst3')

function Fail([string]$msg) {
  Write-Host "package.ps1: $msg" -ForegroundColor Red
  exit 1
}

# 相对路径按调用者的当前位置解析(PS 5.1 / 7 都认,绝对路径原样返回)。
function Resolve-Full([string]$p) {
  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p)
}

function Get-Sha256([string]$path) {
  return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-StreamSha256([System.IO.Stream]$s) {
  $h = [System.Security.Cryptography.SHA256]::Create()
  try { return ([BitConverter]::ToString($h.ComputeHash($s)) -replace '-', '').ToLowerInvariant() }
  finally { $h.Dispose() }
}

# 从用户手册的「硬约束」生成区取第 1..3 条原文(markdown 加重与反引号剥掉)。
# 手册是九条的唯一真源(en 由 gen-hard-rules.mjs 从 docs/hard-rules.i18n.json 生成),本脚本不存副本。
function Get-FirstThreeRules([string]$mdPath, [string]$heading) {
  $lines = [IO.File]::ReadAllLines($mdPath, [Text.Encoding]::UTF8)
  $in = $false
  $rules = New-Object System.Collections.Generic.List[string]
  $open = $false   # 当前是否有一条规则还在收续行
  foreach ($l in $lines) {
    if ($l -eq $heading) { $in = $true; continue }
    if (-not $in) { continue }
    if ($l -match '^## ') { break }
    if ($l -match '^>\s*([0-9]+)\.\s+(.+)$') {
      $n = [int]$Matches[1]
      if ($n -eq 4) { break }   # 第 4 条开头 = 第 3 条已收完(含续行)
      if ($n -ne $rules.Count + 1) { Fail "$mdPath 的「$heading」小节规则编号不连续(期望 $($rules.Count + 1),读到 $n)" }
      $rules.Add($Matches[2]); $open = $true
      continue
    }
    # 续行:同一段引用里不带编号的 `> 文本`,拼到上一条后面 —— 生成器哪天改成折行输出,
    # 这里不会只取到半句安全提示。`>` 空行或引用结束即收口。
    if ($open -and $l -match '^>\s+(\S.*)$') { $rules[$rules.Count - 1] += ' ' + $Matches[1]; continue }
    # 规则开始之后,本小节里再出现没被上面两支消费的非空引用行(例如 `>` 空行之后的续文),
    # 说明规则文本的形态超出了本解析器的理解 —— 判红,不静默截断安全提示。
    if ($rules.Count -gt 0 -and $l -match '^>\s*\S') { Fail "$mdPath 的「$heading」小节有一行引用没法归到任何一条规则:$l" }
    $open = $false
  }
  if ($rules.Count -ne 3) { Fail "$mdPath 的「$heading」小节里没读到前 3 条规则(读到 $($rules.Count) 条)" }
  # 剥加重后,中文句号 / 分号后面那个原本隔开 `**` 的空格会悬空,顺手收掉(英文不含这两个字符)。
  return , @($rules | ForEach-Object { (($_ -replace '\*\*', '') -replace '`', '') -replace '(?<=[。；])[ ]+', '' })
}

# 清单只描述「preflight 核过的东西」;不带 -Preflight 时整条打包流程照跑、清单却没人写,调用方会读到旧文件。
if ($PreflightOut -and -not $Preflight) { Fail '-PreflightOut 只能与 -Preflight 一起用' }

# ── 版本与 tag ────────────────────────────────────────────────────────────────
$cmakeVersion = Get-ScvbCMakeVersion ([IO.File]::ReadAllText((Join-Path $RepoRoot 'CMakeLists.txt')))
if (-not $cmakeVersion) { Fail 'CMakeLists.txt 里找不到 project(SCVB ... VERSION x.y.z)' }
if (-not $Version) { $Version = $cmakeVersion }
# 版本串会进文件名与 URL:只放行 semver 形态,挡住路径分隔符与空白。
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?\z') {
  Fail "版本号形态不对:'$Version'(应为 x.y.z 或 x.y.z-<预发布标识>,不带前导 v)"
}
if (-not $Tag) { $Tag = "v$Version" }
if ($Tag -ne "v$Version") { Fail "tag '$Tag' 与版本 '$Version' 不对应(应为 v$Version)" }
$coreVersion = ($Version -split '-', 2)[0]
if ($coreVersion -ne $cmakeVersion) {
  # 不判红:tag 与 CMake 一致性由 release.yml 的 check-release-tag.ps1 管(它对 v0.0.0-test 有专门放行);
  # 本地拿假版本号试打包也走到这里。只显形。
  Write-Host "[WARN] 版本 $Version 与 CMakeLists.txt 的 $cmakeVersion 不一致(本地试打包 / 测试 tag 属预期)" -ForegroundColor Yellow
}

if (-not $SourceCommit) {
  $SourceCommit = ''
  try { $SourceCommit = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim() } catch { $SourceCommit = '' }
}

# 条目时间戳:SOURCE_DATE_EPOCH > HEAD 提交时间 > 1980-01-01(zip 的 DOS 时间下限)。
$epoch = $null
if ($env:SOURCE_DATE_EPOCH -match '^[0-9]+$') { $epoch = [long]$env:SOURCE_DATE_EPOCH }
if ($null -eq $epoch) {
  try {
    $ct = (& git -C $RepoRoot log -1 --format=%ct 2>$null | Out-String).Trim()
    if ($ct -match '^[0-9]+$') { $epoch = [long]$ct }
  } catch { $epoch = $null }
}
if ($null -eq $epoch) { $epoch = 315532800 }
$entryTime = [DateTimeOffset]::FromUnixTimeSeconds($epoch)

$zipName = "SCVB-v$Version-win64.zip"

# ── ⑤ 合规文件 ────────────────────────────────────────────────────────────────
$licenseDir = Join-Path $RepoRoot 'LICENSES'
# LICENSES/ 下的全部文件(不限扩展名)都进 zip。
$licenseFiles = @(Get-ChildItem -LiteralPath $licenseDir -File)
foreach ($f in @('LICENSE', 'THIRD-PARTY-NOTICES.md')) {
  if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $f) -PathType Leaf)) { Fail "仓库根缺 $f" }
}
# third_party/notices/ 下的全部文件(JUCE 内置库与 WebView2 loader 的上游版权 / 许可声明原文,例如
# HarfBuzz 的逐行版权只写在 harfbuzz.COPYING 里)都进 zip,zip 内路径与仓库相同。
# 键 = zip 内路径,值 = 源文件全路径。
$noticesDir = Join-Path $RepoRoot 'third_party\notices'
if (-not (Test-Path -LiteralPath $noticesDir -PathType Container)) { Fail '仓库缺 third_party/notices/ 目录' }
$noticesRoot = (Get-Item -LiteralPath $noticesDir).FullName.TrimEnd('\', '/')
$noticeFiles = @{}
foreach ($f in @(Get-ChildItem -LiteralPath $noticesRoot -Recurse -File)) {
  $noticeFiles[('third_party/notices/' + $f.FullName.Substring($noticesRoot.Length).TrimStart('\', '/').Replace('\', '/'))] = $f.FullName
}
if ($noticeFiles.Count -eq 0) { Fail 'third_party/notices/ 下没有任何文件' }
# 「随二进制分发」表里每个许可证(第 3 列开头的 SPDX 标识)都必须在 LICENSES/ 里有全文
# (文件名 = <SPDX>.<任意扩展名>);再加本项目自己的 GPL-3.0-or-later。表读不出行即判红,不当成「没有依赖」。
$notices = [IO.File]::ReadAllLines((Join-Path $RepoRoot 'THIRD-PARTY-NOTICES.md'), [Text.Encoding]::UTF8)
$spdxIds = New-Object System.Collections.Generic.List[string]
$spdxIds.Add('GPL-3.0-or-later')
$inDist = $false; $rows = 0; $col = -1
foreach ($l in $notices) {
  if ($l -match '^## ') { $inDist = $l.StartsWith('## 随二进制分发'); continue }
  if (-not $inDist -or $l -notmatch '^\|') { continue }
  $cells = $l.Split('|')
  # 按表头定位「许可证」列,不写死下标。
  if ($col -lt 0) {
    for ($c = 0; $c -lt $cells.Count; $c++) { if ($cells[$c].Trim().StartsWith('许可证')) { $col = $c } }
    if ($col -lt 0) { Fail "THIRD-PARTY-NOTICES.md「随二进制分发」表的表头里找不到「许可证」列:$l" }
    continue
  }
  if ($cells[1].Trim() -match '^-+$') { continue }
  if ($cells.Count -le $col) { Fail "THIRD-PARTY-NOTICES.md「随二进制分发」表有一行列数不够:$l" }
  $rows++
  $m = [regex]::Match($cells[$col].Trim(), '^([A-Za-z0-9][A-Za-z0-9.+-]*)')
  if (-not $m.Success) { Fail "THIRD-PARTY-NOTICES.md 的「随二进制分发」表里有一行读不出 SPDX 标识:$l" }
  if (-not $spdxIds.Contains($m.Groups[1].Value)) { $spdxIds.Add($m.Groups[1].Value) }
}
if ($rows -eq 0) { Fail 'THIRD-PARTY-NOTICES.md 里没读到「随二进制分发」表的任何一行(标题或表格式变了?)' }
$missingLicenseTexts = @($spdxIds | Where-Object { $id = $_; -not ($licenseFiles | Where-Object { $_.BaseName -eq $id }) })
if ($missingLicenseTexts.Count -gt 0) {
  $msg = "LICENSES/ 缺这些许可证的全文(THIRD-PARTY-NOTICES.md 的「随二进制分发」表点名了它们):" + ($missingLicenseTexts -join ', ')
  if ($AllowMissingLicenseTexts) { Write-Host "[WARN] $msg —— 演练模式放行,记进 package-summary.md" -ForegroundColor Yellow }
  else { Fail $msg }
}
# [SL-571] 反方向:LICENSES/ 里的每份全文,都得有「随二进制分发」表的一行(或本项目自己的 GPL-3.0-or-later)
# 点名它。上面那道只管「表里点名了就得有全文」;删掉表里一行却留着全文、或只放全文不登记组件,原先都静默通过。
# 两道合起来(不带 -AllowMissingLicenseTexts 时),LICENSES/ 的份数就钉在表上,不写字面量。区分大小写(List.Contains 是序数比较):文件名要与表里的
# SPDX 标识逐字相同。演练 tag 也不放行 —— 多出来的全文不是缺口,是登记错了。
$unlistedLicenseTexts = @($licenseFiles | Where-Object { -not $spdxIds.Contains($_.BaseName) } | ForEach-Object { $_.Name })
if ($unlistedLicenseTexts.Count -gt 0) {
  Fail ("LICENSES/ 里有这些全文,THIRD-PARTY-NOTICES.md 的「随二进制分发」表却没有一行点名它们(补上表里那一行,或删掉全文):" + ($unlistedLicenseTexts -join ', '))
}
# THIRD-PARTY-NOTICES.md 全文(不止「随二进制分发」表:HarfBuzz 的指向写在「版权行」一节)里点名的每个
# 声明文件路径 —— `third_party/notices/<文件>` 与 `LICENSES/<文件>`,以 / 结尾的目录引用不算 —— 都必须是
# 本次要打进 zip 的文件。NOTICES 进 zip 后,这些路径就是用户手里唯一的指引,指向包里没有的文件等于没给。
# 打包后 ⑥ 再对 zip 本身判一次。演练 tag 也不放行:缺的不是全文,是 NOTICES 自己许诺的原文。
# 两道核对都**区分大小写**:路径要在解压目录里原样可查,不能只在不分大小写的文件系统上才对得上。
# 路径字符里含 `/`:third_party/notices/ 是递归打包的,子目录里的文件也要能被点名、被核对。
$citedNoticePaths = New-Object System.Collections.Generic.List[string]
foreach ($m in [regex]::Matches(($notices -join "`n"), '(?<![\w./-])((?:third_party/notices|LICENSES)/[A-Za-z0-9._+/-]+)')) {
  $p = $m.Groups[1].Value.TrimEnd('.')   # 句末句点不属于路径
  if ($p.EndsWith('/')) { continue }      # 「目录/...」这类省略写法剥完句点只剩目录,不是文件引用
  if (-not $citedNoticePaths.Contains($p)) { $citedNoticePaths.Add($p) }
}
# 一个都读不出来说明写法变了(比如改成了别的路径前缀),不当成「没有引用」静默放行。
if ($citedNoticePaths.Count -eq 0) { Fail 'THIRD-PARTY-NOTICES.md 里没读到任何 third_party/notices/<文件> 或 LICENSES/<文件> 形态的路径(写法变了?)' }
foreach ($p in $citedNoticePaths) {
  # @{} 的 ContainsKey 与 -eq 都不分大小写,这里一律用 -ccontains / -ceq。
  $packed = ([string[]]@($noticeFiles.Keys)) -ccontains $p
  if (-not $packed -and $p -cmatch '^LICENSES/([^/]+)$') {
    $name = $Matches[1]
    $packed = [bool]($licenseFiles | Where-Object { $_.Name -ceq $name })
  }
  if ($packed) { continue }
  if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $p) -PathType Leaf)) { Fail "THIRD-PARTY-NOTICES.md 点名了 $p,仓库里没有这个文件" }
  Fail "THIRD-PARTY-NOTICES.md 点名了 $p,它不在打包清单里(只打 LICENSES/ 顶层文件与 third_party/notices/ 全部文件;大小写须与实际文件名一致)"
}

# ── ④ INSTALL.txt ─────────────────────────────────────────────────────────────
$sourceUrl = "https://github.com/$RepoSlug/tree/$Tag"
$sourceLine = "Corresponding source for this exact build: $sourceUrl"
$rulesEn = Get-FirstThreeRules (Join-Path $RepoRoot 'docs/USER_GUIDE.md') '## Hard rules'
$rulesZh = Get-FirstThreeRules (Join-Path $RepoRoot 'docs/USER_GUIDE.zh-CN.md') '## 硬约束'
$guideEn = "https://github.com/$RepoSlug/blob/$Tag/docs/USER_GUIDE.md"
$guideZh = "https://github.com/$RepoSlug/blob/$Tag/docs/USER_GUIDE.zh-CN.md"
$vstDir = 'C:\Program Files\Common Files\VST3\'
$commitLine = if ($SourceCommit) { "Built from commit: $SourceCommit" } else { $null }

$install = New-Object System.Collections.Generic.List[string]
$install.AddRange([string[]]@(
  "SCVB (Synchain Vocal Balancer) $Version - Windows x64 (VST3)",
  '',
  $sourceLine
))
if ($commitLine) { $install.Add($commitLine) }
$install.AddRange([string[]]@(
  '',
  '== English ==',
  '',
  'What is in this zip',
  '  SCVB Input.vst3     required - goes on every vocal track',
  '  SCVB Output.vst3    required - goes on the vocal bus',
  '  SCVB Monitor.vst3   optional - read-only window for watching a whole group; install it only if you want it',
  '  LICENSE.txt, THIRD-PARTY-NOTICES.md, LICENSES\   licence texts',
  '  third_party\notices\   original copyright and licence notices of the third-party code built into the plugins',
  '  INSTALL.txt         this file',
  'Input and Output are a pair and share one version number. Install both from the same zip.',
  '',
  'Requirements: Windows 10 1809+ or Windows 11 (x64), a 64-bit VST3 host,',
  'and the Microsoft WebView2 Evergreen Runtime (usually already present).',
  '',
  'Install',
  '  1. Check the download. In PowerShell:',
  "       Get-FileHash .\$zipName -Algorithm SHA256",
  "     The hash must match $zipName.sha256 and the SHA-256 in the GitHub Release notes.",
  '     If it does not match, do not install it.',
  '  2. Unblock the zip BEFORE you extract it. SCVB is not code-signed, and Windows marks files',
  '     downloaded from the internet. Right-click the zip > Properties > General > tick "Unblock" > OK.',
  '     (No "Unblock" box means there is nothing to do.) Or in PowerShell:',
  "       Unblock-File .\$zipName",
  '     If you already extracted and copied the plugins, unblock them in place from an',
  '     administrator PowerShell:',
  "       Get-ChildItem '$($vstDir)SCVB *.vst3' -Recurse | Unblock-File",
  "  3. Extract, then copy each whole .vst3 folder (not just the file inside it) into",
  "       $vstDir",
  '     Windows will ask for administrator permission for that folder.',
  '  4. Rescan plugins in your DAW.',
  '',
  'If Windows SmartScreen or your browser warns about an unknown or unrecognised publisher,',
  'that is because the plugins are not code-signed. Check the SHA-256 first; then choose',
  '"More info" > "Run anyway" (or "Keep" in the browser). You can also build SCVB yourself',
  'from the source link above.',
  '',
  'Read before first use - the first 3 of the nine usage rules (the plugin shows all nine on',
  'first launch; breaking any of them gives silence, wrong panning or failed analysis):'
))
for ($i = 0; $i -lt 3; $i++) { $install.Add(("  {0}. {1}" -f ($i + 1), $rulesEn[$i])) }
$install.AddRange([string[]]@(
  "All nine rules: $guideEn",
  '',
  'Licence: SCVB is free software under the GNU GPL v3 or later (LICENSE.txt).',
  'Third-party components and their licences: THIRD-PARTY-NOTICES.md, LICENSES\ and third_party\notices\.',
  "Issues: https://github.com/$RepoSlug/issues",
  '',
  '== 中文 ==',
  '',
  'zip 里有什么',
  '  SCVB Input.vst3     必装 —— 插在每条人声轨上',
  '  SCVB Output.vst3    必装 —— 插在人声总线上',
  '  SCVB Monitor.vst3   可选 —— 只读的整组观察窗,需要才装',
  '  LICENSE.txt、THIRD-PARTY-NOTICES.md、LICENSES\   许可证全文',
  '  third_party\notices\   编进插件的第三方代码的上游版权与许可声明原文',
  '  INSTALL.txt         本文件',
  'Input 与 Output 是一对,共用一个版本号,请从同一个 zip 里一起安装。',
  '',
  '系统要求:Windows 10 1809+ 或 Windows 11(x64)、64 位 VST3 宿主、',
  'Microsoft WebView2 Evergreen Runtime(通常系统已自带)。',
  '',
  '安装',
  '  1. 校验下载。在 PowerShell 里运行:',
  "       Get-FileHash .\$zipName -Algorithm SHA256",
  "     结果必须与 $zipName.sha256 以及 GitHub Release 正文里的 SHA-256 一致,对不上就不要安装。",
  '  2. 解压**之前**先解除锁定。SCVB 没有代码签名,Windows 会给从网上下载的文件打标记。',
  '     右键 zip > 属性 > 常规 > 勾选「解除锁定」> 确定(没有这个勾选框就说明无需处理)。',
  '     或者在 PowerShell 里运行:',
  "       Unblock-File .\$zipName",
  '     如果已经解压并复制过了,用管理员身份打开 PowerShell 就地解除:',
  "       Get-ChildItem '$($vstDir)SCVB *.vst3' -Recurse | Unblock-File",
  '  3. 解压,把每个 .vst3 **整个文件夹**(不是里面的单个文件)复制到',
  "       $vstDir",
  '     复制到该目录需要管理员权限,按系统提示确认即可。',
  '  4. 在 DAW 里重新扫描插件。',
  '',
  '如果 Windows SmartScreen 或浏览器提示「未知发布者」,原因是插件没有代码签名。请先核对 SHA-256,',
  '再点「更多信息」>「仍要运行」(浏览器里选「保留」)。你也可以按上面的源码链接自行构建。',
  '',
  '首次使用前必读 —— 九条使用规则的前 3 条(插件首次启动会完整弹出九条;违反任何一条都会导致',
  '静音、声像位置错误或分析失效):'
))
for ($i = 0; $i -lt 3; $i++) { $install.Add(("  {0}. {1}" -f ($i + 1), $rulesZh[$i])) }
$install.AddRange([string[]]@(
  "完整九条:$guideZh",
  '',
  '许可证:SCVB 以 GNU GPL v3 或更高版本发布(LICENSE.txt);第三方组件及其许可证见',
  'THIRD-PARTY-NOTICES.md、LICENSES\ 与 third_party\notices\。',
  "问题反馈:https://github.com/$RepoSlug/issues",
  ''
))
# 中文一段里的 ** 是写给自己看的强调,txt 不渲染 markdown,落盘前剥掉。
$installText = (($install.ToArray() -join "`r`n") -replace '\*\*', '')

# -Preflight:只做不依赖构建产物的检查(版本 / tag、许可证全文覆盖、NOTICES 点名的声明文件、
# INSTALL.txt 的规则提取)就退出。
# release.yml 的 verify-tag 在 20 分钟的构建之前先跑它,这几类问题不必等构建完才红。
# -PreflightOut:[B 线 M10] 把上面核过的结果写成机读清单给 scripts/package-macos.sh。格式:UTF-8 无 BOM、LF,
# 每行 `键<TAB>值`,同名键按出现顺序累积;首行 `format<TAB>scvb-package-preflight/1`(形态变了先改这个号,
# sh 只认它认得的号)。值里不许有 TAB / CR / LF(规则原文是单行拼好的,这里再拦一道,免得 sh 读错行)。
# 清单项与本脚本自己打 Windows 包时用的是同一批变量:mac 包的合规文件集合、NOTICES 点名路径与规则原文
# 都从这里来,不在 sh 里另写一套。文件清单按序数排序,输出与运行环境无关。
if ($Preflight) {
  if ($PreflightOut) {
    $outFull = Resolve-Full $PreflightOut
    $outParent = Split-Path -Parent $outFull
    if (-not (Test-Path -LiteralPath $outParent -PathType Container)) { Fail "-PreflightOut 的目录不存在:$outParent" }
    $manifest = New-Object System.Collections.Generic.List[string]
    $addKv = {
      param([string]$k, [string]$v)
      if ($v -match "[`t`r`n]") { Fail "preflight 清单的 $k 值里有 TAB / 换行,清单格式容不下:$v" }
      $manifest.Add("$k`t$v")
    }
    & $addKv 'format' 'scvb-package-preflight/1'
    & $addKv 'version' $Version
    & $addKv 'tag' $Tag
    & $addKv 'cmakeVersion' $cmakeVersion
    & $addKv 'sourceCommit' $SourceCommit
    & $addKv 'sourceUrl' $sourceUrl
    & $addKv 'sourceLine' $sourceLine
    & $addKv 'guideEn' $guideEn
    & $addKv 'guideZh' $guideZh
    & $addKv 'issuesUrl' "https://github.com/$RepoSlug/issues"
    foreach ($r in $rulesEn) { & $addKv 'ruleEn' ($r -replace '\*\*', '') }
    foreach ($r in $rulesZh) { & $addKv 'ruleZh' ($r -replace '\*\*', '') }
    foreach ($id in $spdxIds) { & $addKv 'spdx' $id }
    $lic = [string[]]@($licenseFiles | ForEach-Object { 'LICENSES/' + $_.Name })
    [Array]::Sort($lic, [StringComparer]::Ordinal)
    foreach ($p in $lic) { & $addKv 'license' $p }
    $ntc = [string[]]@($noticeFiles.Keys)
    [Array]::Sort($ntc, [StringComparer]::Ordinal)
    foreach ($p in $ntc) { & $addKv 'notice' $p }
    $cit = [string[]]@($citedNoticePaths)
    [Array]::Sort($cit, [StringComparer]::Ordinal)
    foreach ($p in $cit) { & $addKv 'cited' $p }
    foreach ($id in $missingLicenseTexts) { & $addKv 'missingLicenseText' $id }
    [IO.File]::WriteAllText($outFull, (($manifest.ToArray() -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
  }
  Write-Host "package.ps1: preflight OK(version $Version, tag $Tag, 许可证 $($spdxIds.Count) 个已核,LICENSES/ 全文 $($licenseFiles.Count) 份,NOTICES 点名的声明文件 $($citedNoticePaths.Count) 个已核,规则 en/zh 各 3 条)"
  exit 0
}

$buildFull = Resolve-Full $BuildDir
$outFull = Resolve-Full $OutDir
if (-not (Test-Path -LiteralPath $buildFull -PathType Container)) { Fail "BuildDir 不存在:$buildFull" }

# ── ② 枚举 bundle ─────────────────────────────────────────────────────────────
$bundles = @(Get-ChildItem -LiteralPath $buildFull -Recurse -Directory -Filter '*.vst3')
if ($bundles.Count -ne 3) {
  $bundles | ForEach-Object { Write-Host "  found: $($_.FullName)" }
  Fail "期望恰好 3 个 .vst3 bundle 目录(SCVB Input / Output / Monitor),实际 $($bundles.Count) 个。BuildDir 里混着多套构建(如 Debug + Release)时请指到单一配置的目录。"
}
# 两侧用同一个序数比较器排序,不依赖常量表的书写顺序,也不依赖 locale。
$names = [string[]]@($bundles | ForEach-Object { $_.Name })
[Array]::Sort($names, [StringComparer]::Ordinal)
$expectedSorted = [string[]]@($ExpectedBundles)
[Array]::Sort($expectedSorted, [StringComparer]::Ordinal)
if (($names -join '|') -ne ($expectedSorted -join '|')) {
  Fail ("bundle 名字不对:实际 [{0}],期望 [{1}]" -f ($names -join ', '), ($ExpectedBundles -join ', '))
}
foreach ($b in $bundles) {
  $dll = Join-Path $b.FullName (Join-Path 'Contents\x86_64-win' $b.Name)
  if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { Fail "bundle 不完整,缺 $dll" }
}

# ── 组装条目清单:(zip 内路径, 源文件 或 $null=内存内容) ──────────────────────
$stage = Join-Path ([IO.Path]::GetTempPath()) ("scvb-package-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
  $installPath = Join-Path $stage 'INSTALL.txt'
  [IO.File]::WriteAllText($installPath, $installText, (New-Object System.Text.UTF8Encoding($true)))

  $entries = @{}   # zip 内路径 -> 源文件全路径
  foreach ($b in $bundles) {
    $root = $b.FullName.TrimEnd('\', '/')
    foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File)) {
      $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
      $entries[($b.Name + '/' + $rel)] = $f.FullName
    }
  }
  $entries['LICENSE.txt'] = (Join-Path $RepoRoot 'LICENSE')
  $entries['THIRD-PARTY-NOTICES.md'] = (Join-Path $RepoRoot 'THIRD-PARTY-NOTICES.md')
  foreach ($f in $licenseFiles) { $entries[('LICENSES/' + $f.Name)] = $f.FullName }
  foreach ($k in $noticeFiles.Keys) { $entries[$k] = $noticeFiles[$k] }
  $entries['INSTALL.txt'] = $installPath

  $keys = [string[]]@($entries.Keys)
  [Array]::Sort($keys, [StringComparer]::Ordinal)

  # ── 写 zip ──────────────────────────────────────────────────────────────────
  if (-not (Test-Path -LiteralPath $outFull)) { New-Item -ItemType Directory -Path $outFull | Out-Null }
  $zipPath = Join-Path $outFull $zipName
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  $fs = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
  try {
    $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
      foreach ($k in $keys) {
        $e = $zip.CreateEntry($k, [System.IO.Compression.CompressionLevel]::Optimal)
        $e.LastWriteTime = $entryTime
        $dst = $e.Open()
        try {
          $src = [IO.File]::OpenRead($entries[$k])
          try { $src.CopyTo($dst) } finally { $src.Dispose() }
        } finally { $dst.Dispose() }
      }
    } finally { $zip.Dispose() }
  } finally { $fs.Dispose() }

  # ── ⑥ 打包后断言:重新打开 zip,逐条比对 ────────────────────────────────────
  $zr = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
  try {
    $inZip = @{}
    foreach ($e in $zr.Entries) {
      if ($inZip.ContainsKey($e.FullName)) { Fail "zip 内条目重复:$($e.FullName)" }
      $s = $e.Open()
      try { $inZip[$e.FullName] = Get-StreamSha256 $s } finally { $s.Dispose() }
    }
    if ($inZip.Count -ne $keys.Count) { Fail "zip 条目数 $($inZip.Count) != 待打包文件数 $($keys.Count)" }
    foreach ($k in $keys) {
      if (-not $inZip.ContainsKey($k)) { Fail "zip 内缺条目:$k" }
      if ($inZip[$k] -ne (Get-Sha256 $entries[$k])) { Fail "zip 内条目与源文件字节不一致:$k" }
    }
    # NOTICES 点名的每个声明文件路径,在用户实际拿到的 zip 里逐个核对(打包前核的是仓库侧)。
    # 排在下面的必需条目清单之前:漏打 third_party/notices/ 时先报的是「哪条引用落空」。
    # 按条目原名区分大小写比对($inZip 是不分大小写的 @{},不能拿它的 ContainsKey)。
    $zipNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($e in $zr.Entries) { [void]$zipNames.Add($e.FullName) }
    foreach ($p in $citedNoticePaths) {
      if (-not $zipNames.Contains($p)) { Fail "THIRD-PARTY-NOTICES.md 点名的 $p 不在 zip 里(用户包里这条指引会落空;大小写须一致)" }
    }
    $required = @('LICENSE.txt', 'THIRD-PARTY-NOTICES.md', 'INSTALL.txt')
    foreach ($f in $licenseFiles) { $required += ('LICENSES/' + $f.Name) }
    foreach ($k in $noticeFiles.Keys) { $required += $k }
    foreach ($b in $ExpectedBundles) { $required += "$b/Contents/x86_64-win/$b" }
    foreach ($r in $required) { if (-not $inZip.ContainsKey($r)) { Fail "zip 内缺必需条目:$r" } }
    # 根目录白名单:三个 bundle 目录、LICENSES/、third_party/(其下只许 notices/)、三个根文件,别的都不该出现。
    $allowedTop = @($ExpectedBundles) + @('LICENSES', 'third_party', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.md', 'INSTALL.txt')
    foreach ($n in $inZip.Keys) {
      $top = ($n -split '/', 2)[0]
      if ($allowedTop -notcontains $top) { Fail "zip 根目录出现清单外的条目:$n" }
      if ($top -eq 'third_party' -and -not $n.StartsWith('third_party/notices/', [StringComparison]::Ordinal)) {
        Fail "zip 的 third_party/ 下出现 notices/ 以外的条目:$n"
      }
    }
    $installEntry = $zr.GetEntry('INSTALL.txt')
    $rd = New-Object System.IO.StreamReader($installEntry.Open(), [Text.Encoding]::UTF8)
    try { $installBack = $rd.ReadToEnd() } finally { $rd.Dispose() }
    if (-not ($installBack.Contains($sourceLine))) { Fail "INSTALL.txt 缺精确到 tag 的源码声明行:$sourceLine" }
  } finally { $zr.Dispose() }

  # ── ③ sha256 与 summary ──────────────────────────────────────────────────────
  $sha = Get-Sha256 $zipPath
  $size = (Get-Item -LiteralPath $zipPath).Length
  $shaPath = "$zipPath.sha256"
  [IO.File]::WriteAllText($shaPath, "$sha  $zipName`n", (New-Object System.Text.UTF8Encoding($false)))

  $releaseDate = [DateTime]::UtcNow.ToString('yyyy-MM-dd')
  $summary = New-Object System.Collections.Generic.List[string]
  $summary.AddRange([string[]]@(
    "# SCVB package summary",
    '',
    '| key | value |',
    '| --- | --- |',
    "| version | $Version |",
    "| tag | $Tag |",
    "| zipFileName | $zipName |",
    "| sizeBytes | $size |",
    "| sha256 | $sha |",
    "| releaseDate | $releaseDate (UTC) |",
    "| sourceCommit | $SourceCommit |",
    "| cmakeVersion | $cmakeVersion |",
    "| bundles | $($ExpectedBundles -join ', ') |",
    "| missingLicenseTexts | $(if ($missingLicenseTexts.Count -gt 0) { ($missingLicenseTexts -join ', ') + ' (pipeline test only - must be empty for a real release)' } else { 'none' }) |",
    "| thirdPartyNotices | $($noticeFiles.Count) files under third_party/notices/ |",
    "| citedNoticePaths | $($citedNoticePaths -join ', ') (every file path THIRD-PARTY-NOTICES.md cites is in the zip) |",
    '',
    "Corresponding source: $sourceUrl",
    '',
    '## zip contents',
    '',
    '| entry | sha256 |',
    '| --- | --- |'
  ))
  foreach ($k in $keys) { $summary.Add(("| ``{0}`` | {1} |" -f $k, $inZip[$k])) }
  $summaryPath = Join-Path $outFull 'package-summary.md'
  [IO.File]::WriteAllText($summaryPath, (($summary.ToArray() -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

  if ($env:GITHUB_OUTPUT) {
    # 显式无 BOM:PS 5.1 的 Out-File -Encoding utf8 会带 BOM。
    [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "zip=$zipPath`nsha256=$sha`n", (New-Object System.Text.UTF8Encoding($false)))
  }

  Write-Host "package.ps1: OK"
  Write-Host "  zip      $zipPath ($size bytes, $($keys.Count) entries)"
  Write-Host "  sha256   $sha"
  Write-Host "  summary  $summaryPath"
} finally {
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
exit 0
