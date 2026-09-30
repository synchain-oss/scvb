<#
.SYNOPSIS  拿 gates 用的那把命名互斥,再跑给定命令 —— 手跑 ctest / 冒烟一律经这里。
.DESCRIPTION
  [SL-311] 为什么需要它:`scripts/gates.ps1` 自己在 gate 3e 与 gate 6/7/8 两段持
  `Local\SCVB-ipc-tests`([SL-301]),所以**经 gates 跑**是安全的。危险的恰恰是排障时
  「只跑那一个测试」—— 越想省时间越会裸跑,而排障期正是反复起停测试进程的时候。

  裸跑的实伤(2026-09-04 实测):`ctest -R scvb_host_tests` 不拿互斥,host 套件会建**全机
  唯一**的 viz 共享段,于是同机另一份 gates 里那条「viz 段不存在时只读方拿到 kFailed」的
  前置断言恒红 —— 对方多跑了三轮才查出不是自己的问题。省下的几分钟由别人以假红付账。

  这把锁是 `System.Threading.Mutex` 的**内核对象**,与 gates 用的是同一个名字、同一把;
  它**没有持有者身份**(这是有意的:[SL-301] 裁过,加 owner 文件就回到目录锁那套已经
  实伤过人的形态)。所以本脚本只负责「排队 → 跑 → 一定释放」,不写 owner、不接管、不超时抢占。

  ⚠ **不要**用它去包整条 `gates.ps1`:gates 自己会取这把锁,外面再包一层会把
  configure/build 也串起来 —— 那正是 [SL-301] 拆开的东西(CLAUDE.md §2 锁纪律)。
.PARAMETER Command
  要跑的命令(字符串,交给 pwsh -Command)。退出码原样透传。
.PARAMETER WaitMinutes
  等锁上界,默认 30 —— 与 gates 里由段预算推出来的默认值同量级。超时**不跑**并以 99 退出:
  宁可不跑,也不要无锁跑进去抢别人的共享段。
.EXAMPLE   pwsh scripts/with-ipc-lock.ps1 -Command 'ctest --test-dir build -C Release -R scvb_host_tests'
.EXAMPLE   pwsh scripts/with-ipc-lock.ps1 -Command 'node web-preview/tests/smoke-monitor-page.mjs'
#>
#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Command,
    [int]$WaitMinutes = 30
)

$ErrorActionPreference = 'Stop'
$name = 'Local\SCVB-ipc-tests'

function Write-Stamp {
    param([string]$Text, [string]$Color = 'Cyan')
    # 时间戳带毫秒且用 UTC:并发排障时「谁在什么时刻拿到/放开」这条证据要能与 gates 的
    # `[ipc-lock]` 行逐行对齐,而 gates 打的就是 UTC。两边不同基准就对不上账。
    Write-Host ("  [with-ipc-lock] {0}Z {1}" -f (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff'), $Text) -ForegroundColor $Color
}

$createdNew = $false
# 与 gates 逐字同名、同作用域(`Local\`)。**不设 `Global\` 降级**:`Global\` 建失败的现实
# 原因是已存在的同名对象 DACL 拒绝当前 token,退到 `Local\` 会变成两把不同的锁,
# 于是双方都以为自己持锁 —— 静默失去互斥,比没有锁更坏([SL-301] 头注同款理由)。
$mutex = New-Object System.Threading.Mutex($false, $name, [ref]$createdNew)

$rc = 1
$held = $false
try {
    Write-Stamp ("等待 {0}(上界 {1} 分钟)..." -f $name, $WaitMinutes)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $held = $mutex.WaitOne([TimeSpan]::FromMinutes($WaitMinutes))
    }
    catch [System.Threading.AbandonedMutexException] {
        # 上一个持有者进程死了却没释放。互斥体已经归我们,但对方**留下的机器状态未知**
        # (半跑完的测试、没退的 Chrome、写了一半的共享段)。拿是拿到了,必须出声。
        $held = $true
        Write-Stamp '⚠ 上一个持有者异常退出未释放(AbandonedMutex);锁已归本进程,但机器状态可能是脏的 —— 结果存疑就先查残留进程' 'Yellow'
    }
    if (-not $held) {
        Write-Stamp ("超时({0} 分钟)未取得互斥 —— **不跑**。同机多半有另一份 gates 在 3e 或 6/7/8 段。" -f $WaitMinutes) 'Red'
        exit 99
    }
    Write-Stamp ("已获得(等锁 {0:N1} 秒)" -f $sw.Elapsed.TotalSeconds) 'Green'

    & pwsh -NoProfile -Command $Command
    $rc = $LASTEXITCODE
}
finally {
    if ($held) {
        # 释放必须在 finally:被 Ctrl-C 或命令抛异常时,不释放就把整机的 gates 都挂在这上面。
        try { $mutex.ReleaseMutex() } catch {}
        Write-Stamp '已释放'
    }
    $mutex.Dispose()
}

Write-Stamp ("被包命令退出码 = {0}" -f $rc)
exit $rc
