<#
.SYNOPSIS  宿主卡死时抓一份完整内存转储(线程栈定谳用)。
.DESCRIPTION
  SCVB v5.1 真机出现「快速切换 tab 后插件与宿主整体卡死、只能杀进程」。卡死 ≠ 崩溃:
  没有崩溃就没有 WER 记录,事后什么都查不到。要定谳只有一条路 —— **在还卡着的时候**
  把宿主进程的全部线程栈抓下来。

  本脚本用 Windows 自带的 DbgHelp!MiniDumpWriteDump 直接写转储,**不需要装 procdump
  或任何调试器**。目标进程处于「无响应」状态也照抓不误(转储是操作系统从外部做的,
  不需要目标进程配合)。

  纯读取:除了写出那个 .dmp 文件之外,不改目标进程、不改系统任何设置。

.EXAMPLE   powershell -ExecutionPolicy Bypass -File .\collect-hang-dump.ps1
           # 列出候选宿主进程,选一个抓

.EXAMPLE   powershell -ExecutionPolicy Bypass -File .\collect-hang-dump.ps1 -ProcessId 12345
           # 直接抓指定 PID

           注:交付给用户的测试包里本文件改名为 **采集转储.ps1**(中文名对非开发用户更好认),
           内容不变 —— INSTALL.txt 与终验清单里写的都是那个名字。

.NOTES
  · 必须存成 **UTF-8 with BOM**(理由同 诊断.ps1:Windows PowerShell 5.1 无 BOM 时
    按 ANSI 读,中文注释会让整个脚本语法解析失败)。
  · 完整转储的大小 ≈ 目标进程的内存占用,Cubase 大工程可能几个 GB。磁盘要够。
  · 需要与目标进程相同的权限。宿主以管理员运行时,本脚本也要用管理员开的终端跑。
#>
param(
    [int]$ProcessId = 0,
    [string]$OutDir = [Environment]::GetFolderPath('Desktop')
)

$ErrorActionPreference = 'Stop'

function Write-Head($t) { Write-Host ""; Write-Host "== $t ==" -ForegroundColor Cyan }

Write-Head "SCVB 卡死转储采集"

# ---- 1. 选目标 --------------------------------------------------------------
# 候选 = 常见宿主。名字里带这些词的都列出来,由用户选(不猜、不自动挑)。
$patterns = @('Cubase', 'Nuendo', 'reaper', 'Studio One', 'Ableton', 'FL64', 'Bitwig', 'Samplitude')

if ($ProcessId -le 0) {
    $cands = Get-Process | Where-Object {
        $n = $_.ProcessName
        $patterns | Where-Object { $n -like "*$_*" }
    } | Sort-Object -Property WorkingSet64 -Descending

    if (-not $cands) {
        Write-Host "没找到常见宿主进程。" -ForegroundColor Yellow
        Write-Host "请在任务管理器里看一眼宿主的 PID,然后重跑:" -ForegroundColor Yellow
        Write-Host "    powershell -ExecutionPolicy Bypass -File .\采集转储.ps1 -ProcessId <PID>"
        exit 2
    }

    Write-Host "找到这些进程:"
    $i = 0
    foreach ($p in $cands) {
        $i++
        # Responding=False 就是任务管理器里的「无响应」——卡死时正是它
        $state = if ($p.Responding) { "响应正常" } else { "**无响应(就是它)**" }
        $mb = [math]::Round($p.WorkingSet64 / 1MB)
        Write-Host ("  [{0}] PID {1,-8} {2,-18} 内存 {3,6} MB   {4}" -f $i, $p.Id, $p.ProcessName, $mb, $state)
    }

    if ($cands.Count -eq 1) {
        $ProcessId = $cands[0].Id
        Write-Host ""
        Write-Host "只有一个,直接抓 PID $ProcessId。"
    }
    else {
        Write-Host ""
        $sel = Read-Host "抓哪一个?输编号(1-$($cands.Count))"
        $idx = 0
        if (-not [int]::TryParse($sel, [ref]$idx) -or $idx -lt 1 -or $idx -gt $cands.Count) {
            Write-Host "输入不是有效编号,退出。" -ForegroundColor Red
            exit 2
        }
        $ProcessId = $cands[$idx - 1].Id
    }
}

try { $proc = Get-Process -Id $ProcessId }
catch {
    Write-Host "PID $ProcessId 不存在(是不是已经被杀掉了?)" -ForegroundColor Red
    exit 2
}

# ---- 2. 写转储 --------------------------------------------------------------
Write-Head "写转储"

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outFile = Join-Path $OutDir ("SCVB-hang-{0}-{1}-{2}.dmp" -f $proc.ProcessName, $ProcessId, $stamp)

Add-Type -Namespace ScvbDump -Name Native -MemberDefinition @'
[DllImport("dbghelp.dll", SetLastError = true)]
public static extern bool MiniDumpWriteDump(
    IntPtr hProcess, uint processId, Microsoft.Win32.SafeHandles.SafeFileHandle hFile,
    int dumpType, IntPtr exceptionParam, IntPtr userStreamParam, IntPtr callbackParam);
'@

# 0x00000002 = MiniDumpWithFullMemory。线程栈其实用不上全内存,但卡死这种一次性现场
# 宁可多抓:少抓了要用户再复现一次,而这个 bug 本来就不好复现。
$MiniDumpWithFullMemory = 0x00000002

$fs = $null
try {
    $fs = [System.IO.File]::Create($outFile)
    Write-Host "目标:PID $ProcessId ($($proc.ProcessName))"
    Write-Host "输出:$outFile"
    Write-Host "进行中…(几个 GB 的工程可能要一两分钟,别关窗口)"

    $ok = [ScvbDump.Native]::MiniDumpWriteDump(
        $proc.Handle, [uint32]$ProcessId, $fs.SafeFileHandle,
        $MiniDumpWithFullMemory, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)

    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $fs.Close(); $fs = $null

    if (-not $ok) {
        Write-Host ""
        Write-Host "写转储失败(Win32 错误码 $err)。" -ForegroundColor Red
        if ($err -eq 5) {
            Write-Host "错误 5 = 权限不足:宿主多半是以管理员身份运行的。" -ForegroundColor Yellow
            Write-Host "请用**管理员**身份开一个终端,再跑一次这个脚本。" -ForegroundColor Yellow
        }
        Write-Host ""
        Write-Host "退路(任务管理器,一样能用):" -ForegroundColor Yellow
        Write-Host "  Ctrl+Shift+Esc → 详细信息 → 找到宿主进程 → 右键 →「创建转储文件」"
        Write-Host "  完成后它会告诉你 .dmp 存在哪儿,把那个路径发回来即可。"
        Remove-Item $outFile -ErrorAction SilentlyContinue
        exit 1
    }
}
finally {
    if ($fs) { $fs.Close() }
}

$size = [math]::Round((Get-Item $outFile).Length / 1MB)
Write-Host ""
Write-Host "完成:$outFile  (${size} MB)" -ForegroundColor Green
Write-Host ""
Write-Host "接下来:" -ForegroundColor Cyan
Write-Host "  1) 这个文件通常很大,别直接发聊天 —— 压缩后传网盘,把**链接**发回来;"
Write-Host "  2) 顺手说一句当时你在做什么(切了哪几个 tab、开关拧到哪一档),两句话就行;"
Write-Host "  3) 现在可以去任务管理器把宿主结束掉了。"
