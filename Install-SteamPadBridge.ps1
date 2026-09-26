<#
    Install-SteamPadBridge.ps1
    ---------------------------------------------------------------------------
    One-shot setup for: boot straight into the Xbox full screen experience with
    a Steam Controller (2026) working as a normal Xbox pad, and automatic
    hand-off to Steam Input whenever a Steam game is running.

    What it does
      1. Installs ViGEmBus (virtual Xbox 360 pad driver) if missing.
      2. Installs SteamlessController (raw Steam Controller HID -> virtual pad).
      3. Turns off SteamlessController's own autostart, so the scheduled task
         starts it elevated instead. Steam's autostart is left alone.
      4. Discovers your Steam and Xbox app paths, writes config.json.
      5. Registers a logon scheduled task that runs SteamPadWatchdog.ps1
         elevated, which arbitrates the controller between the bridge and Steam.

    Usage (right-click > Run with PowerShell as Administrator, or):
        powershell -ExecutionPolicy Bypass -File .\Install-SteamPadBridge.ps1

    Options:
        -SkipInstall   configure only; do not download/install anything
        -Uninstall     remove the scheduled task + config
#>

[CmdletBinding()]
param(
    [switch] $SkipInstall,
    [switch] $Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is ~10x faster without it

$TaskName  = 'SteamPad Bridge Watchdog'
$AppDir    = Join-Path $env:LOCALAPPDATA 'SteamPadBridge'
$ConfigPath= Join-Path $AppDir 'config.json'
$WatchdogSrc = Join-Path $PSScriptRoot 'SteamPadWatchdog.ps1'
$WatchdogDst = Join-Path $AppDir 'SteamPadWatchdog.ps1'
$RunKey    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function Say  { param($m) Write-Host "  $m" }
function Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Warn { param($m) Write-Host "  ! $m" -ForegroundColor Yellow }
function Good { param($m) Write-Host "  + $m" -ForegroundColor Green }
function Bad  { param($m) Write-Host "  x $m" -ForegroundColor Red }

# ------------------------------------------------------------ elevation check
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Warn 'Administrator rights are required. Relaunching elevated...'
    # -NoExit: this script ends by printing a state dump, and the elevated
    # window would otherwise close before anyone could read it.
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-NoExit','-File',"`"$PSCommandPath`"")
    if ($SkipInstall) { $argList += '-SkipInstall' }
    if ($Uninstall)   { $argList += '-Uninstall' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList
    return
}

# ------------------------------------------------------------------ uninstall
if ($Uninstall) {
    Step 'Removing the SteamPad bridge automation'
    try {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction Stop
        Good "Scheduled task '$TaskName' removed."
    } catch { Warn "No scheduled task to remove." }
    Get-Process -Name 'SteamlessController' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (Test-Path $AppDir) {
        Rename-Item $AppDir "$AppDir.removed-$(Get-Date -Format yyyyMMddHHmmss)" -ErrorAction SilentlyContinue
        Good 'Config and logs archived.'
    }
    Say 'If an older version of this installer turned Steam''s autostart off, re-enable it in Steam > Settings > Interface.'
    Say 'SteamlessController and ViGEmBus are still installed; remove them from Apps & features if you no longer want them.'
    return
}

Write-Host ''
Write-Host '  Steam Controller (2026)  ->  Xbox full screen experience' -ForegroundColor White
Write-Host '  ---------------------------------------------------------' -ForegroundColor DarkGray

# --------------------------------------------------------------- sanity checks
Step 'Checking the machine'
$os = Get-CimInstance Win32_OperatingSystem
Say "$($os.Caption) build $($os.BuildNumber)"

$xboxPkg = Get-AppxPackage -Name 'Microsoft.GamingApp' -ErrorAction SilentlyContinue
if ($xboxPkg) { Good "Xbox app $($xboxPkg.Version) found." }
else          { Warn 'Xbox app (Microsoft.GamingApp) not found - install it from the Microsoft Store.' }

$XboxAumid = ''
if ($xboxPkg) {
    try {
        $manifest = Get-AppxPackageManifest -Package $xboxPkg.PackageFullName
        $appId    = @($manifest.Package.Applications.Application)[0].Id
        $XboxAumid = '{0}!{1}' -f $xboxPkg.PackageFamilyName, $appId
        Good "Xbox app id: $XboxAumid"
    } catch { Warn "Could not read the Xbox app id: $($_.Exception.Message)" }
}

$pad = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like '*VID_28DE*' }
if ($pad) {
    Good "Valve device(s) attached:"
    $pad | ForEach-Object { Say "    $($_.FriendlyName)  [$($_.Status)]" }
} else {
    Warn 'No Valve (VID_28DE) device attached right now. Plug the wireless puck in before testing.'
}

# ------------------------------------------------------------------- installs
function Test-ViGEmBus {
    if (Get-CimInstance Win32_SystemDriver -Filter "Name='ViGEmBus'" -ErrorAction SilentlyContinue) { return $true }
    return (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\ViGEmBus')
}

function Get-InstalledApp {
    param([string] $NameLike)
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    Get-ItemProperty $roots -ErrorAction SilentlyContinue |
        Where-Object { $_.PSObject.Properties['DisplayName'] -and $_.DisplayName -like $NameLike }
}

function Find-BridgeExe {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\SteamlessController\SteamlessController.exe'),
        (Join-Path $env:ProgramFiles 'SteamlessController\SteamlessController.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'SteamlessController\SteamlessController.exe')
    )
    foreach ($c in $candidates) { if ($c -and (Test-Path -LiteralPath $c)) { return $c } }

    foreach ($app in (Get-InstalledApp -NameLike '*Steamless*')) {
        if ($app.PSObject.Properties['InstallLocation'] -and $app.InstallLocation) {
            $p = Join-Path $app.InstallLocation 'SteamlessController.exe'
            if (Test-Path -LiteralPath $p) { return $p }
        }
    }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if (-not $root -or -not (Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Filter 'SteamlessController.exe' -Recurse -Depth 3 -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return ''
}

function Get-GitHubAsset {
    param([string] $Repo, [string] $AssetPattern)
    Say "Querying $Repo ..."
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" `
                             -Headers @{ 'User-Agent' = 'SteamPadBridge-Setup' } -TimeoutSec 60
    $asset = $rel.assets | Where-Object { $_.name -like $AssetPattern } | Select-Object -First 1
    if (-not $asset) { throw "No asset matching '$AssetPattern' in $Repo $($rel.tag_name)" }
    Say "$($rel.tag_name) -> $($asset.name)"
    $dst = Join-Path $env:TEMP $asset.name
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $dst -UseBasicParsing -TimeoutSec 300
    return $dst
}

function Invoke-Installer {
    # $Arguments = '' means: run it interactively and let the user click through.
    param([string] $Path, [string] $Arguments = '', [int] $TimeoutSeconds = 600)
    Say "Running $(Split-Path -Leaf $Path) $(if ($Arguments) { $Arguments } else { '(interactive)' })"
    if ($Arguments) { $p = Start-Process -FilePath $Path -ArgumentList $Arguments -PassThru }
    else            { $p = Start-Process -FilePath $Path -PassThru }
    # Never block forever. The wrong silent-flag dialect makes an installer show
    # its GUI instead, and an unattended run would sit on that dialog for good.
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        Warn "$(Split-Path -Leaf $Path) is still running after $TimeoutSeconds s - moving on."
        return $null
    }
    Start-Sleep -Seconds 2
    return $p.ExitCode
}

if (-not $SkipInstall) {
    Step 'SteamlessController (the bridge)'
    $BridgeExe = Find-BridgeExe
    if ($BridgeExe) {
        Good "Already installed: $BridgeExe"
    } else {
        try {
            $setup = Get-GitHubAsset -Repo 'ddeverill/SteamlessController' -AssetPattern '*Setup*.exe'
            # Try both silent dialects (NSIS first, then Inno), then hand it
            # to the user. A wrong dialect just shows the GUI, which the
            # timeout in Invoke-Installer keeps from wedging an unattended run.
            foreach ($flags in @('/S', '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '')) {
                if ($flags -eq '') { Warn 'Silent install did not land; click through the installer.' }
                Invoke-Installer -Path $setup -Arguments $flags | Out-Null
                $BridgeExe = Find-BridgeExe
                if ($BridgeExe) { break }
            }
        } catch {
            Bad "Download/install failed: $($_.Exception.Message)"
            Say 'Grab it manually from https://github.com/ddeverill/SteamlessController/releases and re-run with -SkipInstall.'
        }
        if ($BridgeExe) { Good "Installed: $BridgeExe" }
    }

    Step 'ViGEmBus (virtual Xbox 360 pad driver)'
    if (Test-ViGEmBus) {
        Good 'ViGEmBus already present.'
    } else {
        $done = $false
        if (Get-Command winget -ErrorAction SilentlyContinue) {
            foreach ($id in @('Nefarius.ViGEmBus','ViGEm.ViGEmBus')) {
                Say "winget install $id"
                & winget install --id $id --exact --silent --accept-source-agreements --accept-package-agreements 2>&1 | Out-Null
                if (Test-ViGEmBus) { $done = $true; break }
            }
        }
        if (-not $done) {
            try {
                $vigem = Get-GitHubAsset -Repo 'nefarius/ViGEmBus' -AssetPattern '*.exe'
                foreach ($flags in @('/quiet /norestart', '')) {
                    Invoke-Installer -Path $vigem -Arguments $flags | Out-Null
                    $done = Test-ViGEmBus
                    if ($done) { break }
                }
            } catch { Bad "ViGEmBus install failed: $($_.Exception.Message)" }
        }
        if ($done) { Good 'ViGEmBus installed.' }
        else { Warn 'ViGEmBus is not installed. SteamlessController usually offers to install it on first run - do that, then re-run this script with -SkipInstall.' }
    }
} else {
    Step 'Skipping installs (-SkipInstall)'
    $BridgeExe = Find-BridgeExe
}

if (-not $BridgeExe) { $BridgeExe = Find-BridgeExe }
if (-not $BridgeExe) { Bad 'SteamlessController.exe could not be located - the watchdog will not be able to start it.' }

# ------------------------------------------------------------------- autostart
Step 'Autostart (the scheduled task starts the bridge, elevated)'
# Only SteamlessController's own entry. Steam's used to be removed here too, and
# a running watchdog kept scrubbing it - so Steam silently stopped starting with
# Windows. Whether Steam starts at logon is the user's decision, not ours.
$runKeyObj = Get-Item $RunKey -ErrorAction SilentlyContinue
foreach ($name in @('SteamlessController','Steamless Controller')) {
    if ($runKeyObj -and $null -ne $runKeyObj.GetValue($name, $null)) {
        Remove-ItemProperty -Path $RunKey -Name $name -ErrorAction SilentlyContinue
        Good "Removed '$name' from HKCU Run (the scheduled task starts it elevated instead)."
    }
}
Say "Untick 'Start with Windows' inside SteamlessController, so it starts elevated from the task instead."
Say "Set its mode to 'Off ONLY while in Steam game' - the manual toggle is runtime-only and never persisted."

# --------------------------------------------------------------------- config
Step 'Writing configuration'
$SteamExe = ''
try { $SteamExe = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamExe -ErrorAction Stop).SteamExe } catch { }
if ($SteamExe) { $SteamExe = $SteamExe -replace '/', '\' }
if (-not $SteamExe -or -not (Test-Path -LiteralPath $SteamExe)) {
    foreach ($guess in @("${env:ProgramFiles(x86)}\Steam\steam.exe", "$env:ProgramFiles\Steam\steam.exe")) {
        if (Test-Path -LiteralPath $guess) { $SteamExe = $guess; break }
    }
}
if ($SteamExe) { Good "Steam: $SteamExe" } else { Warn 'Steam not found - the hand-off will still work once Steam is installed.' }

$null = New-Item -ItemType Directory -Path $AppDir -Force
$config = [ordered]@{
    BridgeExe               = $BridgeExe
    SteamExe                = $SteamExe
    XboxAumid               = $XboxAumid
    ControllerVid           = 'VID_28DE'
    PollMilliseconds        = 500
    BridgeStartDelaySeconds = 3
    ReturnToXboxAfterGame   = $true
    LogLevel                = 'info'
}
# UTF-8 *without* BOM - PowerShell 5.1's ConvertFrom-Json trips over a BOM,
# which would silently fall back to empty defaults.
$json = $config | ConvertTo-Json
[System.IO.File]::WriteAllText($ConfigPath, $json, (New-Object System.Text.UTF8Encoding($false)))
Good "config.json -> $ConfigPath"

if (Test-Path -LiteralPath $WatchdogSrc) {
    Copy-Item -LiteralPath $WatchdogSrc -Destination $WatchdogDst -Force
    Good "watchdog -> $WatchdogDst"
} elseif (-not (Test-Path -LiteralPath $WatchdogDst)) {
    Bad "SteamPadWatchdog.ps1 not found next to this script. Copy it to $AppDir manually."
}

# ------------------------------------------------------------- scheduled task
Step 'Registering the logon task'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
          -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$WatchdogDst`" -ConfigPath `"$ConfigPath`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
try { $trigger.Delay = 'PT15S' } catch { Warn 'Could not set the 15s startup delay (harmless).' }
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
             -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) `
             -MultipleInstances IgnoreNew -StartWhenAvailable
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
                       -Settings $settings -Description 'Arbitrates the Steam Controller between SteamlessController and Steam Input.' -Force | Out-Null
Good "Task '$TaskName' registered (logon, elevated, 15s delay)."

Step 'Starting it now'
Get-Process -Name 'SteamlessController' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName $TaskName
Start-Sleep -Seconds 8
if (Test-Path -LiteralPath $WatchdogDst) {
    & $WatchdogDst -ConfigPath $ConfigPath -SelfTest
} else {
    Bad "Watchdog script missing at $WatchdogDst - copy it there and re-run."
}

Write-Host ''
Write-Host '  Remaining manual steps (30 seconds, cannot be scripted):' -ForegroundColor White
Write-Host '   1. Settings > Gaming > Xbox mode: set "Choose home app" = Xbox, turn ON "Enter Xbox mode on startup".'
Write-Host '   2. Open SteamlessController from the tray: set mode = "Off while Steam is running",'
Write-Host '      emulation = Xbox 360, and turn OFF its own "Start with Windows".'
Write-Host '      (Manual mode + the "Enable Steamless Mode" tick is NOT persisted - the bridge would come'
Write-Host '       up holding nothing after every reboot and every handoff back from Steam.)'
Write-Host '   3. Steam > Settings > Interface: untick "Run Steam when my PC starts".'
Write-Host '   4. Reboot and check the Xbox UI responds to the left stick and A button.'
Write-Host ''
Write-Host "  Log: $AppDir\logs\watchdog.log" -ForegroundColor DarkGray
Write-Host "  Re-check state any time:  powershell -ExecutionPolicy Bypass -File `"$WatchdogDst`" -SelfTest" -ForegroundColor DarkGray
Write-Host ''
