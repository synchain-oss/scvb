<#
.SYNOPSIS  web-preview 一键静态服务器(T28)—— 固定端口,打印可点 URL。
.DESCRIPTION
  以**仓库根**为站点根起一个纯静态 HTTP 服务(System.Net.HttpListener,零依赖、免管理员)。
  站点根必须是仓库根而不是 web-preview/:壳页用 `../web/output/index.html` 引用真源
  (06 §6.2 硬约束 1「web/ 是唯一真源」),两棵目录必须落在同一个源下才能既解析路径、
  又保持同源(mock 注入靠的就是同源 iframe,见 web-preview/shell.js 文件头)。

  为什么不能 file:// 直开:ES module 在 file:// 下被 CORS 拒绝;且 Chromium 把每个
  file:// 文档当不透明源,iframe 立刻变跨源,`frame.contentWindow.__SCVB_MOCK__` 直接
  SecurityError。所以预览**只有**这一条入口。

  Cache-Control: no-store 是刻意的:①预览页永远拿最新的 web/ 真源,免得改完灰模看不到;
  ②让 iframe 每次装载都真的走网络,把 shell.js 注入泵的时间窗口保持在宽档(见该文件头)。
.PARAMETER Port
  固定端口,默认 8823。被占用时脚本直接报错退出,不静默换端口——换了端口就没法把
  URL 写进文档/任务卡里当固定入口了。
.PARAMETER Open
  起服后用默认浏览器打开导航页。
.PARAMETER Inject
  **方案 C 兜底,默认关闭**(统筹批准)。开启后:凡请求 web/output/index.html 或
  web/input/index.html,服务器在**传输途中**于 </head> 前插一段 classic script,
  它从 `window.parent.__SCVB_PREVIEW_MOCK__`(shell.js 挂的)取 mock 并写进本文档的
  `window.__SCVB_MOCK__`。classic script 在解析期同步执行,恒早于 deferred 的
  `<script type="module" src="./app.js">` —— 时序确定,不依赖 shell.js 的重注入泵。

  红线自查:磁盘上的 web/ **一个字节没动**(改的只是这一次响应体),
  插入的是 6 行引导代码而非任何 UI 代码,`web/` 仍是唯一真源(06 §6.2 硬约束 1)。
  代价:开着它时预览只能经本脚本打开,别的静态服务器起不来同样效果 —— 所以默认关。
  什么时候开:shell.js 的方案 A/B 都被真机实测判死(壳页工具条转红 + console 诊断)时。
.PARAMETER SmokeTest
  自检档:起服 → 自己发三条请求(导航页 / shell.js / 真源 Output 页)→ 打印结果 → 停服退出。
  用于「serve.ps1 实跑一次能起服务并打印 URL(起完即停)」这条自检,不进任何运行时路径。
  与 -Inject 同用时会额外断言引导 script 确实出现在响应体里。
.EXAMPLE   pwsh web-preview/serve.ps1
.EXAMPLE   pwsh web-preview/serve.ps1 -Open
.EXAMPLE   pwsh web-preview/serve.ps1 -SmokeTest
.EXAMPLE   pwsh web-preview/serve.ps1 -Inject      # A/B 注入都失灵时的兜底
#>
param(
  [int]$Port = 8823,
  [switch]$Open,
  [switch]$Inject,
  [switch]$SmokeTest
)

$ErrorActionPreference = 'Stop'

# 站点根 = 仓库根(本脚本在 <repo>/web-preview/ 下)
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BaseUrl = "http://127.0.0.1:$Port"
$EntryUrl = "$BaseUrl/web-preview/index.html"

# MIME 表 —— 只列本仓库真的会被预览页拉到的类型;缺项回落 octet-stream。
# .js 用 text/javascript(RFC 9239 的现行注册名);错的 MIME 会让 <script type="module"> 被拒。
$MimeTypes = @{
  '.html'  = 'text/html; charset=utf-8'
  '.htm'   = 'text/html; charset=utf-8'
  '.js'    = 'text/javascript; charset=utf-8'
  '.mjs'   = 'text/javascript; charset=utf-8'
  '.css'   = 'text/css; charset=utf-8'
  '.json'  = 'application/json; charset=utf-8'
  '.map'   = 'application/json; charset=utf-8'
  '.svg'   = 'image/svg+xml'
  '.woff2' = 'font/woff2'
  '.woff'  = 'font/woff'
  '.png'   = 'image/png'
  '.jpg'   = 'image/jpeg'
  '.jpeg'  = 'image/jpeg'
  '.gif'   = 'image/gif'
  '.ico'   = 'image/x-icon'
  '.md'    = 'text/markdown; charset=utf-8'
  '.txt'   = 'text/plain; charset=utf-8'
}

# ---- 方案 C(-Inject)用的引导片段 --------------------------------------------
# 只做一件事:把壳页 realm 里的 mock 取过来挂到本文档的 window.__SCVB_MOCK__。
# 裸开真源页面时 window.parent === window,取到 undefined,整段成为 no-op(不影响
# 「灰模在无后端时也不炸」这条 T27b 验收)。
$InjectMarker = '__SCVB_PREVIEW_MOCK__'
$InjectSnippet = @'
<script>
    /* [web-preview serve.ps1 -Inject] 传输途中插入的引导片段(方案 C);磁盘上的 web/ 未被修改。
       classic script 在解析期同步执行,恒早于 deferred 的 app.js,故 window.__SCVB_MOCK__
       在 createBridge() 求值时必已就位。取件口由 web-preview/shell.js 挂出。 */
    try {
        if (window.parent !== window && window.parent.__SCVB_PREVIEW_MOCK__) {
            window.__SCVB_MOCK__ = window.parent.__SCVB_PREVIEW_MOCK__;
        }
    } catch (e) {
        console.warn("[web-preview -Inject] 取 mock 失败(多半是跨源):", e);
    }
</script>
'@

# -Inject 只作用于这两条真源页面路径,别的文件一律原样透传。
$InjectTargets = @('/web/output/index.html', '/web/input/index.html')

function Add-PreviewBootstrap {
  <# 往 HTML 文本的第一个 </head> 前插引导片段;找不到 </head> 就原样返回并告警。 #>
  param([string]$Html)

  $rx = [regex]::new('</head>', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
  if (-not $rx.IsMatch($Html)) {
    Write-Host '  [WARN] -Inject: 响应体里没找到 </head>,本次未注入' -ForegroundColor Yellow
    return $Html
  }
  return $rx.Replace($Html, ($InjectSnippet + '</head>'), 1)
}

function Get-SafeLocalPath {
  <#
    URL 路径 → 磁盘路径,并挡掉目录穿越(..%2f 之类)。
    做法:先 UrlDecode,再拼到站点根上做 GetFullPath 归一化,最后断言归一化后的
    绝对路径仍在站点根之内 —— 只信归一化结果,不信输入串长什么样。
  #>
  param([string]$UrlPath, [string]$Root)

  $decoded = [System.Uri]::UnescapeDataString($UrlPath)
  $decoded = $decoded.TrimStart('/')
  if ($decoded -eq '') { $decoded = 'web-preview/index.html' }
  $decoded = $decoded -replace '/', [System.IO.Path]::DirectorySeparatorChar

  $full = [System.IO.Path]::GetFullPath((Join-Path $Root $decoded))
  $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
  if (-not $full.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
    return $null
  }
  if ((Test-Path -LiteralPath $full -PathType Container)) {
    $full = Join-Path $full 'index.html'
  }
  return $full
}

function Send-StaticFile {
  <# 处理一条请求:命中就回文件,否则 404;返回状态码供日志/自检使用。 #>
  param([System.Net.HttpListenerContext]$Context, [string]$Root)

  $req = $Context.Request
  $res = $Context.Response
  $status = 200
  $bytes = $null

  try {
    $local = Get-SafeLocalPath -UrlPath $req.Url.AbsolutePath -Root $Root
    if ($null -ne $local -and (Test-Path -LiteralPath $local -PathType Leaf)) {
      $ext = [System.IO.Path]::GetExtension($local).ToLowerInvariant()
      $res.ContentType = if ($MimeTypes.ContainsKey($ext)) { $MimeTypes[$ext] } else { 'application/octet-stream' }
      $urlPath = $req.Url.AbsolutePath
      if ($Inject -and ($InjectTargets -contains $urlPath)) {
        # 方案 C:只改这一次响应体,磁盘文件不动(见 .PARAMETER Inject)。
        $html = [System.IO.File]::ReadAllText($local, [System.Text.Encoding]::UTF8)
        $bytes = [System.Text.Encoding]::UTF8.GetBytes((Add-PreviewBootstrap -Html $html))
      }
      else {
        $bytes = [System.IO.File]::ReadAllBytes($local)
      }
    }
    else {
      $status = 404
      $res.ContentType = 'text/plain; charset=utf-8'
      $bytes = [System.Text.Encoding]::UTF8.GetBytes("404 Not Found: $($req.Url.AbsolutePath)`n站点根 = $Root")
    }

    $res.StatusCode = $status
    # 预览服务器一律 no-store,理由见 .DESCRIPTION。
    $res.Headers['Cache-Control'] = 'no-store, must-revalidate'
    if ($req.HttpMethod -eq 'HEAD') {
      $res.ContentLength64 = $bytes.Length
    }
    else {
      $res.ContentLength64 = $bytes.Length
      $res.OutputStream.Write($bytes, 0, $bytes.Length)
    }
  }
  catch {
    $status = 500
    try {
      $res.StatusCode = 500
      $msg = [System.Text.Encoding]::UTF8.GetBytes("500 " + $_.Exception.Message)
      $res.ContentLength64 = $msg.Length
      $res.OutputStream.Write($msg, 0, $msg.Length)
    }
    catch { }
  }
  finally {
    try { $res.OutputStream.Close() } catch { }
  }

  $tone = if ($status -eq 200) { 'DarkGray' } else { 'Yellow' }
  Write-Host ("  {0} {1} {2}" -f $status, $req.HttpMethod, $req.Url.AbsolutePath) -ForegroundColor $tone
  return $status
}

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("$BaseUrl/")
try {
  $listener.Start()
}
catch {
  Write-Host ("serve.ps1: 端口 {0} 起不来 —— {1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
  Write-Host '  端口固定是刻意的(URL 要能写进文档当固定入口)。占用方多半是上一次没停掉的 serve.ps1;' -ForegroundColor Red
  Write-Host ('  查:Get-NetTCPConnection -LocalPort {0} | Select-Object OwningProcess' -f $Port) -ForegroundColor Red
  exit 1
}

Write-Host ''
Write-Host 'SCVB web-preview —— 静态预览服务器已启动' -ForegroundColor Green
Write-Host ("  站点根 : {0}" -f $RepoRoot)
Write-Host ("  导航页 : {0}" -f $EntryUrl) -ForegroundColor Cyan
Write-Host ("  Output : {0}/web-preview/output.html?fixture=fifteen-tracks" -f $BaseUrl) -ForegroundColor Cyan
Write-Host ("  Input  : {0}/web-preview/input.html?fixture=channel-conflict" -f $BaseUrl) -ForegroundColor Cyan
if ($Inject) {
  Write-Host '  -Inject : 已开(方案 C 兜底)—— 真源 HTML 在传输途中被插入引导 script;磁盘 web/ 未改动。' -ForegroundColor Yellow
}
Write-Host ''

if ($SmokeTest) {
  # 自检档:自己当客户端发三条请求,逐条同步收 context 并回包(单线程一收一发,不起后台)。
  $http = [System.Net.Http.HttpClient]::new()
  $paths = @(
    '/web-preview/index.html'
    '/web-preview/shell.js'
    '/web/output/index.html'
  )
  $fail = 0
  foreach ($p in $paths) {
    $pending = $http.GetAsync("$BaseUrl$p")
    $ctxTask = $listener.GetContextAsync()
    if (-not $ctxTask.Wait(10000)) {
      Write-Host ("  [TIMEOUT] {0}" -f $p) -ForegroundColor Red
      $fail++
      continue
    }
    [void](Send-StaticFile -Context $ctxTask.Result -Root $RepoRoot)
    $resp = $pending.GetAwaiter().GetResult()
    $body = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    $len = $body.Length
    $ok = $resp.IsSuccessStatusCode -and $len -gt 0
    # -Inject 开着时,真源页面的响应体里必须能看见引导片段的取件口(方案 C 生效自证)。
    if ($ok -and $Inject -and ($InjectTargets -contains $p)) {
      $ok = $body.Contains($InjectMarker)
      if (-not $ok) { Write-Host '    [FAIL] -Inject 已开但响应体里没有引导片段' -ForegroundColor Red }
    }
    if (-not $ok) { $fail++ }
    $okMark = if ($ok) { 'OK  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1} → {2} {3} chars ({4})" -f $okMark, $p, [int]$resp.StatusCode, $len, $resp.Content.Headers.ContentType)
  }
  $http.Dispose()
  $listener.Stop()
  $listener.Close()
  Write-Host ''
  if ($fail -gt 0) {
    Write-Host ("serve.ps1 -SmokeTest: {0} 条失败" -f $fail) -ForegroundColor Red
    exit 1
  }
  Write-Host 'serve.ps1 -SmokeTest: 起服 → 三条请求全绿 → 已停服' -ForegroundColor Green
  exit 0
}

if ($Open) { Start-Process $EntryUrl }
Write-Host 'Ctrl+C 停止。' -ForegroundColor DarkGray

try {
  while ($listener.IsListening) {
    # 用 GetContextAsync + 短等待轮询,而不是阻塞式 GetContext():
    # 阻塞在 .NET 调用里时 PowerShell 收不到 Ctrl+C,只能靠杀进程收场。
    $ctxTask = $listener.GetContextAsync()
    while (-not $ctxTask.Wait(200)) { }
    [void](Send-StaticFile -Context $ctxTask.Result -Root $RepoRoot)
  }
}
finally {
  try { $listener.Stop(); $listener.Close() } catch { }
  Write-Host 'serve.ps1: 已停止。' -ForegroundColor DarkGray
}
