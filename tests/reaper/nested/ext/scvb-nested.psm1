# SPDX-License-Identifier: GPL-3.0-or-later
#
# SCVB's own part of the REAPER tests, as a reaper-harness extension:
#   Preflight     another host with SCVB open would share our segments (they are named by group only): skip the suite.
#   scvb-session  opens a project and hands it to a Node script that drives SCVB's own windows over WebView2's remote
#                 debugging port and REAPER over the file RPC in scvb-nested.lua (see session/lib.mjs).
#
# scvb-session starts REAPER itself instead of through Ctx.Api.LuaStep: the harness watchdog treats every visible
# dialog-class window as a stuck dialog, and REAPER's floating FX windows are dialog-class windows. It takes the same
# locks, uses the same private configuration directory and ends only the process it started.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:AllowedKeys = 'layout', 'script', 'timeoutSec', 'sr', 'block', 'mode', 'args'
$script:ExtDir = $PSScriptRoot

function Test-ScvbSegments {
    foreach ($g in 1..8) {
        $name = "Local\SynchainSCVB.v1.g$g.registry"
        try {
            $m = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting($name, [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
            $m.Dispose()
            "another host has SCVB group $g open"
        } catch [IO.FileNotFoundException] { }
    }
}

function Invoke-ScvbSession {
    param($Ctx, $Scenario)
    $raw = $Scenario.raw
    $root = Split-Path (Split-Path $Ctx.Driver -Parent) -Parent
    Import-Module (Join-Path $root 'src/Guard.psm1') -ErrorAction Stop
    Import-Module (Join-Path $root 'src/Locks.psm1') -ErrorAction Stop
    Import-Module (Join-Path $root 'src/Host.psm1') -ErrorAction Stop

    $id = $Scenario.id
    $layout = & $Ctx.Api.Layout $Ctx $raw['layout']
    $sr = $raw.Contains('sr') ? [int]$raw['sr'] : 48000
    $block = $raw.Contains('block') ? [int]$raw['block'] : 512
    $timeout = $raw.Contains('timeoutSec') ? [double]$raw['timeoutSec'] : 300
    $run = & $Ctx.Api.NewRun -Ctx $Ctx -Name $id -Layout $layout -Sr $sr -Block $block -Mode ($raw.Contains('mode') ? [string]$raw['mode'] : 'online')
    $rpc = Join-Path $Ctx.Work "$id-rpc"
    $null = New-Item -ItemType Directory -Force -Path $rpc
    $outFile = Join-Path $rpc 'outcome.json'
    $resultFile = Join-Path $rpc 'driver.result'
    $jobFile = Join-Path $rpc 'driver.job.lua'
    $exts = (@($Ctx.LuaExtensions) | ForEach-Object { "[==[$_]==]" }) -join ', '
    [IO.File]::WriteAllText($jobFile, "return { step = 'scvb_session', result = [==[$resultFile]==], project = [==[$($run.Project)]==], rpc = [==[$rpc]==], extensions = { $exts } }`n")

    $port = 9400 + (Get-Random -Minimum 0 -Maximum 400)
    $sessionDir = Join-Path (Split-Path $script:ExtDir -Parent) 'session'
    $script = Join-Path $sessionDir $raw['script']
    $nodeArgs = @($script, '--rpc', $rpc, '--port', $port, '--outcome', $outFile, '--project', $run.Project, '--wav', $run.Wav, '--work', $Ctx.Work)
    if ($raw.Contains('args')) { $nodeArgs += @($raw['args']) }

    $lock = Enter-HarnessLocks -Names $Ctx.Locks -TimeoutSeconds $Ctx.LockWaitSec
    if (-not $lock) { return & $Ctx.Api.Outcome INCOMPLETE 'the GUI locks were not available in time; REAPER was not started' }
    $proc = $null; $node = $null
    try {
        $psi = [Diagnostics.ProcessStartInfo]::new($Ctx.Exe)
        $psi.Arguments = (@('-nosplash', '-newinst', '-cfgfile', (Get-HarnessCfgArgument), "`"$($Ctx.Driver)`"") -join ' ')
        $psi.UseShellExecute = $false
        $psi.WorkingDirectory = $Ctx.Work
        $psi.Environment['REAPER_HARNESS_JOB'] = $jobFile
        $psi.Environment['WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS'] = "--remote-debugging-port=$port --remote-allow-origins=*"
        $proc = [Diagnostics.Process]::Start($psi)
        $ni = [Diagnostics.ProcessStartInfo]::new('node')
        foreach ($a in $nodeArgs) { $ni.ArgumentList.Add([string]$a) }
        $ni.UseShellExecute = $false
        $ni.WorkingDirectory = $Ctx.Work
        $ni.Environment['SCVB_SESSION_LOG'] = (Join-Path $rpc 'session.log')
        $node = [Diagnostics.Process]::Start($ni)

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $why = $null
        $nodeGone = $null
        while (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 250
            if ($sw.Elapsed.TotalSeconds -ge $timeout) { $why = "timed out after $timeout s and was ended"; break }
            if ($node.HasExited) {
                if (-not $nodeGone) { $nodeGone = $sw.Elapsed.TotalSeconds }
                # The session ended with its quit op; REAPER then only has the save prompt left.
                if ($sw.Elapsed.TotalSeconds - $nodeGone -ge 20) { $why = 'REAPER did not quit after the session ended'; break }
            }
            foreach ($row in [HarnessWin]::Windows([uint32]$proc.Id)) {
                $f = $row -split "`t", 4
                if ($f[1] -ne '1' -or $f[2] -cne '#32770') { continue }
                if ($f[3] -ceq 'REAPER Query') { $null = [HarnessWin]::Press([long]$f[0], 7) }
                elseif ($f[3] -imatch '^About REAPER') { $why = 'stuck on the evaluation licence reminder'; break }
            }
            if ($why) { break }
        }
        if ($why) {
            try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch { }
            $null = $proc.WaitForExit(5000)
        }
        if (-not $node.WaitForExit(30000)) { try { Stop-Process -Id $node.Id -Force -ErrorAction Stop } catch { } }
        if ($why) { return & $Ctx.Api.Outcome INCOMPLETE $why }
        if (-not (Test-Path -LiteralPath $outFile)) { return & $Ctx.Api.Outcome INCOMPLETE 'the session script wrote no outcome' }
        $o = Get-Content -Raw -LiteralPath $outFile | ConvertFrom-Json
        & $Ctx.Api.Outcome $o.status (& $Ctx.Api.HidePaths $o.detail)
    } finally {
        foreach ($p in $proc, $node) { if ($p -and -not $p.HasExited) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { } } }
        # HasExited turns true before the process leaves the process list; the end of the round counts what is left.
        if ($proc) {
            $procId = $proc.Id
            $proc.Dispose()
            $gone = [Diagnostics.Stopwatch]::StartNew()
            while ((Get-Process -Id $procId -ErrorAction SilentlyContinue) -and $gone.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 50 }
        }
        Exit-HarnessLocks $lock
    }
}

function Register-HarnessExtension {
    @{
        Name      = 'scvb-nested'
        Kinds     = @{
            'scvb-session' = @{
                Validate = {
                    param($Item)
                    foreach ($k in $Item.get_Keys()) { if ($k -cnotin $script:AllowedKeys) { "unknown key '$k'" } }
                    if (-not $Item.Contains('layout')) { "'layout' is required" }
                    if (-not $Item.Contains('script')) { "'script' is required" }
                }
                Run      = { param($Ctx, $Scenario) Invoke-ScvbSession $Ctx $Scenario }
            }
        }
        Preflight = { param($Config) Test-ScvbSegments }
    }
}

Export-ModuleMember -Function Register-HarnessExtension
