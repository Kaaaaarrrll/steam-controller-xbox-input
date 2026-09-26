<#
    SteamPadWatchdog.ps1
    ---------------------------------------------------------------------------
    Keeps a Steam Controller (2026) usable as an Xbox pad inside the Windows 11
    Xbox full screen experience, and hands the controller over to Steam Input
    while a Steam game is running.

    Why this exists:
      * The 2026 Steam Controller has no XInput mode. The Xbox app only speaks
        XInput / Windows.Gaming.Input, so a translation layer is required.
      * SteamlessController provides that layer (raw HID -> virtual Xbox 360 pad
        via ViGEmBus), but it needs an exclusive handle on the controller, which
        Steam takes the moment Steam is running.
      * So: exactly one of {SteamlessController, Steam} may run at a time.
        This watchdog enforces that, and flips between them automatically.

    State machine (polled twice a second):
        No Steam game running  ->  bridge should be running
        Steam game running     ->  bridge stops (Steam Input owns the pad)

    Steam merely being OPEN is not a reason to stop. That is the "finished a
    game, back in the Xbox app" case, and the bridge is the only thing making
    the pad work there. Matches the app's own "Auto - Off ONLY while in Steam
    game" mode, which this used to override by killing the process under it.

    It never closes Steam. It used to, once Steam had been in the background
    for a while, and that reads from the outside as Steam quitting itself at
    random. Starting and stopping Steam is the user's business.

    Runs unattended from a logon scheduled task. Logs to
    %LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log
#>

[CmdletBinding()]
param(
    [string] $ConfigPath = "$env:LOCALAPPDATA\SteamPadBridge\config.json",
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- configuration
$Defaults = [ordered]@{
    BridgeExe               = ''      # full path to SteamlessController.exe
    SteamExe                = ''      # full path to steam.exe
    XboxAumid               = ''      # e.g. Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.App
    ControllerVid           = 'VID_28DE'
    PollMilliseconds        = 500
    BridgeStartDelaySeconds = 3       # let Steam fully release the HID handle first
    ReturnToXboxAfterGame   = $true   # bring the Xbox app back when a full screen game exits to the desktop
    LogLevel                = 'info'  # info | debug
}

function Read-Config {
    param([string] $Path)
    $cfg = [ordered]@{}
    foreach ($k in $Defaults.Keys) { $cfg[$k] = $Defaults[$k] }
    if (Test-Path -LiteralPath $Path) {
        try {
            $raw  = (Get-Content -LiteralPath $Path -Raw).TrimStart([char]0xFEFF)
            $json = $raw | ConvertFrom-Json
            foreach ($p in $json.PSObject.Properties) {
                if ($cfg.Contains($p.Name)) { $cfg[$p.Name] = $p.Value }
            }
        } catch {
            Write-Warning "Could not parse $Path ($($_.Exception.Message)); using defaults."
        }
    }
    return $cfg
}

$Config  = Read-Config -Path $ConfigPath
$LogDir  = Join-Path $env:LOCALAPPDATA 'SteamPadBridge\logs'
$LogFile = Join-Path $LogDir 'watchdog.log'
$null    = New-Item -ItemType Directory -Path $LogDir -Force

function Write-Log {
    param([string] $Message, [ValidateSet('info','warn','error','debug')][string] $Level = 'info')
    if ($Level -eq 'debug' -and $Config.LogLevel -ne 'debug') { return }
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level.ToUpper(), $Message
    try {
        if ((Test-Path $LogFile) -and ((Get-Item $LogFile).Length -gt 2MB)) {
            Move-Item $LogFile "$LogFile.1" -Force
        }
        Add-Content -LiteralPath $LogFile -Value $line -Encoding UTF8
    } catch { }
    Write-Verbose $line
}

# ------------------------------------------------------------ win32 interop
if (-not ('SteamPad.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace SteamPad {
    public static class Native {
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct MONITORINFO {
            public int cbSize; public RECT rcMonitor; public RECT rcWork; public int dwFlags;
        }

        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint flags);
        [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO mi);

        public static uint ForegroundPid() {
            IntPtr h = GetForegroundWindow();
            if (h == IntPtr.Zero) return 0;
            uint pid; GetWindowThreadProcessId(h, out pid); return pid;
        }

        // True when the foreground window covers its whole monitor - the shape
        // a full screen game has and an ordinary window does not. This is what
        // separates "a game just exited" from "I closed a chat window".
        public static bool ForegroundIsFullscreen() {
            IntPtr h = GetForegroundWindow();
            if (h == IntPtr.Zero) return false;
            RECT w;
            if (!GetWindowRect(h, out w)) return false;
            IntPtr mon = MonitorFromWindow(h, 2);   // MONITOR_DEFAULTTONEAREST
            if (mon == IntPtr.Zero) return false;
            MONITORINFO mi = new MONITORINFO();
            mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
            if (!GetMonitorInfo(mon, ref mi)) return false;
            return w.Left <= mi.rcMonitor.Left && w.Top <= mi.rcMonitor.Top
                && w.Right >= mi.rcMonitor.Right && w.Bottom >= mi.rcMonitor.Bottom;
        }
    }
}
'@
}

# Processes that are the desktop itself, never a game. Landing on one of these
# after something closed is what "I am back at the desktop" looks like.
$ShellProcesses = @(
    '', 'explorer', 'ApplicationFrameHost', 'ShellExperienceHost',
    'StartMenuExperienceHost', 'SearchHost', 'SearchApp', 'TextInputHost',
    'XboxPcApp', 'XboxPcAppFT', 'XboxPcTray', 'LockApp'
)

# Never treat these as "the game you were playing", even full screen: the Xbox
# UI itself runs full screen, and so does the bridge's own window on occasion.
$NeverGameProcesses = $ShellProcesses + @(
    'SteamlessController', 'steam', 'steamwebhelper', 'XboxGameBarWidgets', 'GameBar'
)

function Get-ForegroundProcessName {
    try {
        $pid32 = [SteamPad.Native]::ForegroundPid()
        if ($pid32 -eq 0) { return '' }
        $p = Get-Process -Id $pid32 -ErrorAction SilentlyContinue
        if ($p) { return $p.ProcessName } else { return '' }
    } catch { return '' }
}

function Get-ForegroundInfo {
    # Pid, name and whether it is full screen, read together so all three
    # describe the same instant.
    try {
        $pid32 = [SteamPad.Native]::ForegroundPid()
        if ($pid32 -eq 0) { return [pscustomobject]@{ Pid = 0; Name = ''; Fullscreen = $false } }
        $p = Get-Process -Id $pid32 -ErrorAction SilentlyContinue
        return [pscustomobject]@{
            Pid        = [int]$pid32
            Name       = $(if ($p) { $p.ProcessName } else { '' })
            Fullscreen = [SteamPad.Native]::ForegroundIsFullscreen()
        }
    } catch {
        return [pscustomobject]@{ Pid = 0; Name = ''; Fullscreen = $false }
    }
}

# ------------------------------------------------------------------- helpers
function Get-BridgeProcessName {
    if ([string]::IsNullOrWhiteSpace($Config.BridgeExe)) { return 'SteamlessController' }
    return [System.IO.Path]::GetFileNameWithoutExtension($Config.BridgeExe)
}

function Get-BridgeProcess { Get-Process -Name (Get-BridgeProcessName) -ErrorAction SilentlyContinue }
function Get-SteamProcess  { Get-Process -Name 'steam' -ErrorAction SilentlyContinue }

function Measure-Items {
    # Get-Process hands back a bare object for one match and an array for
    # several. Under Set-StrictMode -Version Latest a bare object has no .Count,
    # so counting one has to go through @(). Getting this wrong threw on every
    # single poll the moment Steam started - which meant the bridge was never
    # released, i.e. the exact both-own-the-pad state this whole thing prevents.
    param($InputObject)
    if ($null -eq $InputObject) { return 0 }
    return @($InputObject).Count
}

$script:LastPnpProbe  = [datetime]::MinValue
$script:LastPnpResult = $true

function Test-ControllerPresent {
    param([switch] $Force)
    # Get-PnpDevice -PresentOnly walks the whole device tree - ~400 ms on a
    # normal desktop, i.e. most of a 500 ms poll. Cache it the same way the game
    # probe below is cached; a controller does not come and go within 5 seconds.
    if (-not $Force -and ((Get-Date) - $script:LastPnpProbe).TotalSeconds -lt 5) {
        return $script:LastPnpResult
    }
    $script:LastPnpProbe = Get-Date
    try {
        $vid = $Config.ControllerVid
        $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like "*$vid*" -and $_.Status -eq 'OK' }
        $script:LastPnpResult = [bool]$dev
    } catch { $script:LastPnpResult = $true }   # if we cannot tell, do not block the bridge
    return $script:LastPnpResult
}

function Test-ViGEmBusPresent {
    # Without ViGEmBus the bridge starts happily and emits nothing at all.
    try { return (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\ViGEmBus') } catch { return $false }
}

function Test-XboxPadVisible {
    # ViGEmBus publishes an Xbox 360 pad (VID_045E&PID_028E). A real 360 pad
    # matches too, but for a diagnostic the useful question is simply whether
    # anything XInput-shaped is visible to the Xbox app at all.
    try {
        $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like '*VID_045E&PID_028E*' -and $_.Status -eq 'OK' }
        return [bool]$dev
    } catch { return $false }
}

$script:LastGameProbe  = [datetime]::MinValue
$script:LastGameResult = $false

function Test-SteamGameRunning {
    # Primary signal: Steam writes the running app id here (0 = no game).
    try {
        $v = Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name 'RunningAppID' -ErrorAction SilentlyContinue
        if ($v -and $v.RunningAppID -and [int]$v.RunningAppID -ne 0) { return $true }
    } catch { }

    # Backup signal (RunningAppID is unreliable for a handful of titles):
    # any process executing out of a steamapps\common folder. Throttled - WMI is
    # not something to run twice a second.
    if (((Get-Date) - $script:LastGameProbe).TotalSeconds -lt 3) { return $script:LastGameResult }
    $script:LastGameProbe = Get-Date
    try {
        $hit = Get-CimInstance Win32_Process -Filter "ExecutablePath LIKE '%\\steamapps\\common\\%'" -ErrorAction SilentlyContinue |
               Select-Object -First 1
        $script:LastGameResult = [bool]$hit
    } catch { $script:LastGameResult = $false }
    return $script:LastGameResult
}

function Stop-Bridge {
    $p = Get-BridgeProcess
    if (-not $p) { return }
    Write-Log "Stopping bridge ($(Measure-Items $p) process(es)) so Steam can claim the controller."
    foreach ($proc in $p) {
        try { $proc.CloseMainWindow() | Out-Null } catch { }
    }
    Start-Sleep -Milliseconds 400
    foreach ($proc in (Get-BridgeProcess)) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
}

$script:LastBridgeStart = [datetime]::MinValue

function Start-Bridge {
    if (Get-BridgeProcess) { return }
    # A bridge that dies on launch must not be respawned twice a second.
    if (((Get-Date) - $script:LastBridgeStart).TotalSeconds -lt 10) { return }
    if ([string]::IsNullOrWhiteSpace($Config.BridgeExe) -or -not (Test-Path -LiteralPath $Config.BridgeExe)) {
        Write-Log "BridgeExe not found: '$($Config.BridgeExe)'. Fix config.json." 'error'
        return
    }
    if (-not (Test-ControllerPresent)) {
        Write-Log "Controller ($($Config.ControllerVid)) not present yet; waiting." 'debug'
        return
    }
    Write-Log "Starting bridge: $($Config.BridgeExe)"
    $script:LastBridgeStart = Get-Date
    try {
        Start-Process -FilePath $Config.BridgeExe -WorkingDirectory (Split-Path -Parent $Config.BridgeExe)
    } catch {
        Write-Log "Failed to start bridge: $($_.Exception.Message)" 'error'
    }
}

function Show-XboxApp {
    param([string] $Reason)
    if ([string]::IsNullOrWhiteSpace($Config.XboxAumid)) { return }
    try {
        Start-Process 'explorer.exe' -ArgumentList ("shell:AppsFolder\{0}" -f $Config.XboxAumid)
        Write-Log "Brought the Xbox app back to the foreground ($Reason)."
    } catch {
        Write-Log "Could not relaunch the Xbox app: $($_.Exception.Message)" 'warn'
    }
}

# ------------------------------------------------------------------ self test
if ($SelfTest) {
    $bridge = Get-BridgeProcess
    $steam  = Get-SteamProcess
    [pscustomobject][ordered]@{
        ConfigPath        = $ConfigPath
        BridgeExe         = $Config.BridgeExe
        BridgeExeExists   = (-not [string]::IsNullOrWhiteSpace($Config.BridgeExe)) -and (Test-Path -LiteralPath ([string]$Config.BridgeExe))
        BridgeRunning     = [bool]$bridge
        SteamExe          = $Config.SteamExe
        SteamRunning      = [bool]$steam
        SteamGameRunning  = (Test-SteamGameRunning)
        ControllerPresent = (Test-ControllerPresent -Force)
        ViGEmBusPresent   = (Test-ViGEmBusPresent)
        XboxPadVisible    = (Test-XboxPadVisible)
        XboxAumid         = $Config.XboxAumid
        ForegroundApp     = (Get-ForegroundProcessName)
        ForegroundFullscr = ([SteamPad.Native]::ForegroundIsFullscreen())
        LogFile           = $LogFile
    } | Format-List
    return
}

# ------------------------------------------------------- decision (unit tested)
# Pure function: given the observed world, say what should happen next.
# Kept free of Windows-only cmdlets so it can be tested anywhere.
function Get-WatchdogDecision {
    param(
        [bool] $SteamRunning,
        [bool] $GameRunning,
        [bool] $BridgeRunning,
        [bool] $ControllerPresent
    )

    # A Steam GAME owns the controller: Steam Input claims it exclusively and the
    # bridge would fight it (SteamlessController #79/#80). Steam merely being OPEN
    # does not - and that case matters, because it is the one where you have
    # finished a game and gone back to the Xbox app, where the bridge is the only
    # thing making the pad work at all.
    #
    # This used to stop the bridge whenever steam.exe existed, which silently
    # overrode the app's own "Auto - Off ONLY while in Steam game" mode: the mode
    # would release the pad correctly and the watchdog would kill the process
    # underneath it, so the pad never came back until Steam was closed. Closing
    # Steam was then bolted on to compensate. Both halves are gone now.
    if ($GameRunning) {
        if ($BridgeRunning) {
            return [pscustomobject]@{ Action = 'StopBridge'; Reason = 'a Steam game is running and owns the controller' }
        }
        return [pscustomobject]@{ Action = 'None'; Reason = 'Steam Input is driving the pad' }
    }

    if (-not $BridgeRunning) {
        if (-not $ControllerPresent) {
            return [pscustomobject]@{ Action = 'None'; Reason = 'waiting for the controller to appear' }
        }
        $why = if ($SteamRunning) { 'Steam is open but no game - the bridge owns the pad' }
               else                { 'no Steam - the bridge should own the pad' }
        return [pscustomobject]@{ Action = 'StartBridge'; Reason = $why }
    }

    return [pscustomobject]@{ Action = 'None'; Reason = 'bridge is driving the pad' }
}

# Pure: should the Xbox app be brought back now that a full screen app is gone?
#
# Windows is supposed to return you to the home app when a game exits and often
# does not, which leaves you staring at the desktop with a controller in your
# hands. The full screen test is what keeps this from firing every time any
# window closes, and the shell test is what keeps it from yanking you out of
# whatever you deliberately switched to instead.
function Get-XboxReturnDecision {
    param(
        [bool] $Enabled,
        [bool] $HadFullscreenApp,
        [bool] $AppStillRunning,
        [bool] $ForegroundIsShell
    )
    if (-not $Enabled)           { return 'None' }
    if (-not $HadFullscreenApp)  { return 'None' }
    if ($AppStillRunning)        { return 'None' }
    if (-not $ForegroundIsShell) { return 'None' }
    return 'FocusXbox'
}

# Pure: what to do about a Steam process that is already up when we start.
# Steam already running simply means the bridge waits. This used to close it,
# on the reasoning that leftover Steam at logon was cruft - but the scheduled
# task also restarts this script after a crash, so "at logon" was never a safe
# assumption, and closing someone's Steam is not ours to do either way.
function Get-StartupAction {
    param([bool] $SteamRunning, [bool] $GameRunning)
    if (-not $SteamRunning) { return 'None' }
    if ($GameRunning)       { return 'AdoptGame' }
    return 'None'
}

# Loaded by the test harness: define everything, then stop before the loop.
if ($env:STEAMPAD_TEST_MODE -eq '1') { return }

# ----------------------------------------------------------------- main loop
Write-Log "Watchdog started. bridge='$($Config.BridgeExe)' steam='$($Config.SteamExe)'"

$hadGame        = $false
$noGameSince    = $null
$lastScrub      = Get-Date
$lastSteamSeen  = $false
$fullscreenPid  = 0
$fullscreenName = ''

# Clean slate at startup: no Steam, bridge owns the pad - unless a game is
# already running, in which case this is a restart mid-session and Steam stays.
$steamAtStart = [bool](Get-SteamProcess)
$gameAtStart  = $steamAtStart -and (Test-SteamGameRunning)
switch (Get-StartupAction -SteamRunning $steamAtStart -GameRunning $gameAtStart) {
    'AdoptGame' {
        Write-Log 'A Steam game is already running; leaving Steam alone (restart mid-session).' 'warn'
        $hadGame       = $true
        $lastSteamSeen = $true
    }
}

while ($true) {
    try {
        $steamRunning = [bool](Get-SteamProcess)
        $gameRunning  = $false

        if ($steamRunning) {
            $gameRunning = Test-SteamGameRunning
            if ($gameRunning) {
                if (-not $hadGame) { Write-Log 'Steam game detected; Steam Input is driving the pad.' }
                $hadGame = $true
                $noGameSince = $null
            } elseif ($null -eq $noGameSince) {
                $noGameSince = Get-Date
            }
        } else {
            if ($lastSteamSeen) {
                Write-Log 'Steam is gone - returning the controller to the bridge.'
                Start-Sleep -Seconds ([int]$Config.BridgeStartDelaySeconds)
            }
            $hadGame = $false
            $noGameSince = $null
        }

        $noGameSeconds = 0
        if ($null -ne $noGameSince) { $noGameSeconds = ((Get-Date) - $noGameSince).TotalSeconds }

        # Remember the last full screen thing that was not the shell, so that
        # when it disappears we know a game just ended rather than a window.
        $fg = Get-ForegroundInfo
        if ($fg.Pid -ne 0 -and $fg.Fullscreen -and ($NeverGameProcesses -notcontains $fg.Name)) {
            $fullscreenPid  = $fg.Pid
            $fullscreenName = $fg.Name
        }
        if ($fullscreenPid -ne 0 -and -not (Get-Process -Id $fullscreenPid -ErrorAction SilentlyContinue)) {
            $back = Get-XboxReturnDecision `
                -Enabled ([bool]$Config.ReturnToXboxAfterGame) `
                -HadFullscreenApp $true `
                -AppStillRunning $false `
                -ForegroundIsShell ($ShellProcesses -contains $fg.Name)
            if ($back -eq 'FocusXbox') { Show-XboxApp -Reason "$fullscreenName exited" }
            # Cleared either way: the app is gone, and if the user moved to
            # something else on purpose we must not keep waiting to pounce.
            $fullscreenPid  = 0
            $fullscreenName = ''
        }

        # The device probe is by far the most expensive call in this loop, and
        # it only changes the outcome when we are about to start the bridge.
        # Skip it entirely the rest of the time.
        $bridgeRunning     = [bool](Get-BridgeProcess)
        $controllerPresent = $true
        if (-not $steamRunning -and -not $bridgeRunning) { $controllerPresent = Test-ControllerPresent }

        $decision = Get-WatchdogDecision `
            -SteamRunning $steamRunning `
            -GameRunning $gameRunning `
            -BridgeRunning $bridgeRunning `
            -ControllerPresent $controllerPresent

        switch ($decision.Action) {
            'StopBridge'  { Stop-Bridge }
            'StartBridge' { Start-Bridge }
            default { Write-Log $decision.Reason 'debug' }
        }

        $lastSteamSeen = $steamRunning
    } catch {
        Write-Log "Loop error: $($_.Exception.Message)" 'error'
        Start-Sleep -Seconds 2
    }

    Start-Sleep -Milliseconds ([int]$Config.PollMilliseconds)
}
