<#
    SteamPadWatchdog.ps1
    ---------------------------------------------------------------------------
    Keeps a Steam Controller (2026) usable as an Xbox pad inside the Windows 11
    Xbox full screen experience, and gets out of Steam's way whenever Steam is
    running.

    Why this exists:
      * The 2026 Steam Controller has no XInput mode. The Xbox app only speaks
        XInput / Windows.Gaming.Input, so a translation layer is required.
      * SteamlessController provides that layer (raw HID -> virtual Xbox 360 pad
        via ViGEmBus), but it needs an exclusive handle on the controller, which
        Steam takes the moment Steam is running.
      * So: exactly one of {SteamlessController, Steam} may run at a time.
        This watchdog enforces that, and flips between them automatically.

    State machine (polled twice a second):
        Steam not running  ->  bridge running
        Steam running      ->  bridge stopped - game or no game

    Steam merely being open is enough to stop the bridge. Tested on hardware:
    with Steam open and the bridge also active, every press reached both apps.

    It never closes Steam - the user does that to get the pad back. It used to
    close Steam itself once it had sat in the background a while, and from the
    outside that looked like Steam quitting at random.

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
        [bool] $BridgeRunning,
        [bool] $ControllerPresent
    )

    # Steam running means the bridge is off - game or no game. They cannot share
    # the controller: tested with Steam open and the bridge also active, every
    # press reached both, Steam took focus, and both apps acted on the same input.
    #
    # Getting the pad back is done by the user closing Steam, never by us. An
    # earlier version shut Steam down itself once it had sat in the background a
    # while; from the outside that looked like Steam quitting at random.
    if ($SteamRunning) {
        if ($BridgeRunning) {
            return [pscustomobject]@{ Action = 'StopBridge'; Reason = 'Steam is running and owns the controller' }
        }
        return [pscustomobject]@{ Action = 'None'; Reason = 'Steam is running - close it to get the Xbox controller back' }
    }

    if (-not $BridgeRunning) {
        if (-not $ControllerPresent) {
            return [pscustomobject]@{ Action = 'None'; Reason = 'waiting for the controller to appear' }
        }
        return [pscustomobject]@{ Action = 'StartBridge'; Reason = 'no Steam - the bridge should own the pad' }
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

# Loaded by the test harness: define everything, then stop before the loop.
if ($env:STEAMPAD_TEST_MODE -eq '1') { return }

# ----------------------------------------------------------------- main loop
Write-Log "Watchdog started. bridge='$($Config.BridgeExe)' steam='$($Config.SteamExe)'"

$lastSteamSeen  = $false
$fullscreenPid  = 0
$fullscreenName = ''

while ($true) {
    try {
        $steamRunning = [bool](Get-SteamProcess)

        # Steam just closed: give it a moment to let go of the device before the
        # bridge tries to claim it.
        if (-not $steamRunning -and $lastSteamSeen) {
            Write-Log 'Steam is gone - returning the controller to the bridge.'
            Start-Sleep -Seconds ([int]$Config.BridgeStartDelaySeconds)
        }

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
