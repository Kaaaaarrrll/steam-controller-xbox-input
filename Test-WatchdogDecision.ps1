<#  Test-WatchdogDecision.ps1
    Exercises the watchdog's state machine without touching Windows.
    Run:  pwsh -File .\Test-WatchdogDecision.ps1        (works on Windows or Linux)
#>
$env:STEAMPAD_TEST_MODE = '1'
$ErrorActionPreference = 'Stop'
# Lets the harness run on any OS (the decision logic is platform-neutral).
if (-not $env:LOCALAPPDATA) {
    $env:LOCALAPPDATA = Join-Path ([System.IO.Path]::GetTempPath()) 'steampad-test'
    $null = New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force
}
. (Join-Path $PSScriptRoot 'SteamPadWatchdog.ps1') -ConfigPath (Join-Path $PSScriptRoot 'no-such-config.json')

$cases = @(
    @{ n='Boot: nothing running, pad attached -> start bridge'
       a=@{SteamRunning=$false;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$false;NoGameSeconds=0}
       e='StartBridge' }

    @{ n='Boot: pad not attached yet -> wait, do not spawn bridge'
       a=@{SteamRunning=$false;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$false;HadGame=$false;NoGameSeconds=0}
       e='None' }

    @{ n='Steady state in Xbox UI: bridge up, no Steam -> nothing to do'
       a=@{SteamRunning=$false;GameRunning=$false;BridgeRunning=$true;SteamForeground=$false;ControllerPresent=$true;HadGame=$false;NoGameSeconds=0}
       e='None' }

    @{ n='Steam just launched while bridge holds the pad -> release it'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$true;SteamForeground=$true;ControllerPresent=$true;HadGame=$false;NoGameSeconds=1}
       e='StopBridge' }

    @{ n='Steam game running -> hands off, let Steam Input drive'
       a=@{SteamRunning=$true;GameRunning=$true;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$true;NoGameSeconds=0}
       e='None' }

    @{ n='Game just exited, inside grace window -> wait (user may launch another)'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$true;ControllerPresent=$true;HadGame=$true;NoGameSeconds=5}
       e='None' }

    @{ n='Game exited but Big Picture is focused -> leave Steam alone, you are picking the next game'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$true;ControllerPresent=$true;HadGame=$true;NoGameSeconds=25}
       e='None' }

    @{ n='Game exited, Steam left in the background, grace elapsed -> quit Steam so the pad comes back'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$true;NoGameSeconds=25}
       e='StopSteam' }

    @{ n='Game exited, Steam backgrounded but still inside grace -> wait it out'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$true;NoGameSeconds=5}
       e='None' }

    @{ n='Browsing Big Picture, no game yet, Steam focused -> leave Steam alone'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$true;ControllerPresent=$true;HadGame=$false;NoGameSeconds=600}
       e='None' }

    @{ n='Steam opened but abandoned in the background -> quit it after the idle timeout'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$false;NoGameSeconds=130}
       e='StopSteam' }

    @{ n='Steam in background but under the idle timeout -> leave it'
       a=@{SteamRunning=$true;GameRunning=$false;BridgeRunning=$false;SteamForeground=$false;ControllerPresent=$true;HadGame=$false;NoGameSeconds=60}
       e='None' }

    @{ n='Bridge respawned during a Steam game -> kill it (this is the pad-stealing case)'
       a=@{SteamRunning=$true;GameRunning=$true;BridgeRunning=$true;SteamForeground=$false;ControllerPresent=$true;HadGame=$true;NoGameSeconds=0}
       e='StopBridge' }

    @{ n='Steam closed, bridge already back -> nothing to do'
       a=@{SteamRunning=$false;GameRunning=$false;BridgeRunning=$true;SteamForeground=$false;ControllerPresent=$true;HadGame=$false;NoGameSeconds=0}
       e='None' }
)

$pass = 0; $fail = 0
foreach ($c in $cases) {
    $splat = $c.a
    $d = Get-WatchdogDecision @splat
    if ($d.Action -eq $c.e) {
        $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green
    } else {
        $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}' ({3})" -f $c.n, $c.e, $d.Action, $d.Reason) -ForegroundColor Red
    }
}
# ------------------------------------------------------- return-to-Xbox tests
# Firing this at the wrong moment is worse than not firing at all - it would
# yank the user out of whatever they switched to.
$xboxChecks = @(
    @{ n='Full screen game exits to the desktop -> go home to Xbox'
       got=(Get-XboxReturnDecision -Enabled $true -HadFullscreenApp $true -AppStillRunning $false -ForegroundIsShell $true)
       e='FocusXbox' }
    @{ n='Game still running -> leave it alone'
       got=(Get-XboxReturnDecision -Enabled $true -HadFullscreenApp $true -AppStillRunning $true -ForegroundIsShell $false)
       e='None' }
    @{ n='Game exited but the user switched to another app -> do not pounce'
       got=(Get-XboxReturnDecision -Enabled $true -HadFullscreenApp $true -AppStillRunning $false -ForegroundIsShell $false)
       e='None' }
    @{ n='Nothing full screen was ever seen -> nothing to return from'
       got=(Get-XboxReturnDecision -Enabled $true -HadFullscreenApp $false -AppStillRunning $false -ForegroundIsShell $true)
       e='None' }
    @{ n='Turned off in config -> never fires'
       got=(Get-XboxReturnDecision -Enabled $false -HadFullscreenApp $true -AppStillRunning $false -ForegroundIsShell $true)
       e='None' }
)
foreach ($c in $xboxChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}

# --------------------------------------------------------------- startup tests
# The scheduled task restarts this script after a crash. A restart that lands
# mid-game must not force-quit Steam out from under the running game.
$startupChecks = @(
    @{ n='Startup with no Steam -> nothing to clean up'
       got=(Get-StartupAction -SteamRunning $false -GameRunning $false); e='None' }
    @{ n='Startup with leftover Steam, no game -> shut it down'
       got=(Get-StartupAction -SteamRunning $true  -GameRunning $false); e='StopSteam' }
    @{ n='Restart lands mid-game -> adopt it, never kill Steam under a game'
       got=(Get-StartupAction -SteamRunning $true  -GameRunning $true);  e='AdoptGame' }
)
foreach ($c in $startupChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}

# ------------------------------------------------------------ process counting
# Regression: Get-Process returns a bare object for a single match, and under
# Set-StrictMode -Version Latest that object has no .Count. The old code read
# $p.Count directly, so Stop-Bridge threw on every poll while Steam was running
# and the bridge was never handed over - silently, into the log, forever.
$scalarCountThrows = $false
try { $null = ([pscustomobject]@{ Id = 1 }).Count } catch { $scalarCountThrows = $true }

$countChecks = @(
    @{ n='A single process object still counts as 1'; got=(Measure-Items ([pscustomobject]@{Id=1})); e=1 }
    @{ n='Several process objects count correctly';   got=(Measure-Items @(1,2,3));                  e=3 }
    @{ n='Nothing at all counts as 0';                got=(Measure-Items $null);                     e=0 }
    @{ n='StrictMode really does reject scalar .Count (guards the regression)'
       got=$scalarCountThrows; e=$true }
)
foreach ($c in $countChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}

# ------------------------------------------------------------- probe throttling
# Get-PnpDevice -PresentOnly costs ~400 ms on a real machine. At a 500 ms poll
# that is most of a core, forever, so the probe has to be cached between polls.
$script:LastPnpProbe = [datetime]::MinValue
$null       = Test-ControllerPresent
$firstProbe = $script:LastPnpProbe
$null       = Test-ControllerPresent          # straight away again: must be cached
$cachedAt   = $script:LastPnpProbe
$null       = Test-ControllerPresent -Force   # -Force must go back to the hardware
$forcedAt   = $script:LastPnpProbe

$probeChecks = @(
    @{ n='Controller probe is served from cache between polls'; got=($cachedAt -eq $firstProbe); e=$true }
    @{ n='-Force bypasses the controller probe cache';          got=($forcedAt -gt $firstProbe); e=$true }
)
foreach ($c in $probeChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}

# ---------------------------------------------------------------- config tests
# The silent-failure path: a config that does not parse leaves BridgeExe empty
# and the watchdog can never start the bridge. Includes a BOM, because that is
# exactly how Set-Content -Encoding UTF8 writes files on PowerShell 5.1.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) 'steampad-config-test.json'
$body = '{"BridgeExe":"C:\\Program Files\\SteamlessController\\SteamlessController.exe","GraceAfterGameSeconds":45,"FocusXboxAfterSteam":false}'
[System.IO.File]::WriteAllText($tmp, $body, (New-Object System.Text.UTF8Encoding($true)))   # $true = with BOM

. (Join-Path $PSScriptRoot 'SteamPadWatchdog.ps1') -ConfigPath $tmp

$configChecks = @(
    @{ n='BOM-prefixed config still parses';       got=$Config.BridgeExe;             e='C:\Program Files\SteamlessController\SteamlessController.exe' }
    @{ n='Override wins over default';             got=[int]$Config.GraceAfterGameSeconds; e=45 }
    @{ n='Boolean override survives round-trip';   got=[bool]$Config.FocusXboxAfterSteam;  e=$false }
    @{ n='Unspecified key falls back to default';  got=[int]$Config.IdleBrowseSeconds;     e=120 }
)
foreach ($c in $configChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}
Remove-Item $tmp -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("  {0} passed, {1} failed" -f $pass, $fail) -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
if ($fail) { exit 1 }
