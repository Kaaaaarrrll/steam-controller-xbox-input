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
       a=@{SteamRunning=$false;BridgeRunning=$false;ControllerPresent=$true}
       e='StartBridge' }

    @{ n='Boot: pad not attached yet -> wait, do not spawn bridge'
       a=@{SteamRunning=$false;BridgeRunning=$false;ControllerPresent=$false}
       e='None' }

    @{ n='Steady state in Xbox UI: bridge up, no Steam -> nothing to do'
       a=@{SteamRunning=$false;BridgeRunning=$true;ControllerPresent=$true}
       e='None' }

    @{ n='Steam opens while the bridge holds the pad -> stop the bridge'
       a=@{SteamRunning=$true;BridgeRunning=$true;ControllerPresent=$true}
       e='StopBridge' }

    @{ n='Steam open, bridge off -> stay off until the user closes Steam'
       a=@{SteamRunning=$true;BridgeRunning=$false;ControllerPresent=$true}
       e='None' }

    @{ n='Steam closed, bridge already back -> nothing to do'
       a=@{SteamRunning=$false;BridgeRunning=$true;ControllerPresent=$true}
       e='None' }
)

# Two properties matter more than any single case, so they are checked across the
# whole input space rather than by example.
#
# 1. Nothing here may ever close Steam. It did for nine days before anyone
#    connected the two, because the symptom - Steam vanishing - looks nothing like
#    a controller tool.
# 2. The bridge may never be running while Steam is. Tested on hardware: with both
#    active, every press reaches both apps at once.
$closeSteamEscapes = @()
$sharedWithSteam   = @()
foreach ($steam in @($true, $false)) {
  foreach ($bridge in @($true, $false)) {
    foreach ($pad in @($true, $false)) {
      $d = Get-WatchdogDecision -SteamRunning $steam -BridgeRunning $bridge -ControllerPresent $pad
      if ($d.Action -notin @('None', 'StartBridge', 'StopBridge')) {
          $closeSteamEscapes += "$steam/$bridge/$pad -> $($d.Action)"
      }
      $bridgeAfter = ($bridge -and $d.Action -ne 'StopBridge') -or $d.Action -eq 'StartBridge'
      if ($steam -and $bridgeAfter) { $sharedWithSteam += "steam=$steam bridge=$bridge pad=$pad -> $($d.Action)" }
    }
  }
}

$pass = 0; $fail = 0
if ($closeSteamEscapes.Count -eq 0) {
    $pass++; Write-Host "  PASS  No input combination can close Steam (8 combinations swept)" -ForegroundColor Green
} else {
    $fail++
    Write-Host ("  FAIL  Something can still close Steam:`n        {0}" -f ($closeSteamEscapes -join "`n        ")) -ForegroundColor Red
}
if ($sharedWithSteam.Count -eq 0) {
    $pass++; Write-Host "  PASS  The bridge is never left running alongside Steam (8 combinations swept)" -ForegroundColor Green
} else {
    $fail++
    Write-Host ("  FAIL  The bridge stays up while Steam runs:`n        {0}" -f ($sharedWithSteam -join "`n        ")) -ForegroundColor Red
}
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
$body = '{"BridgeExe":"C:\\Program Files\\SteamlessController\\SteamlessController.exe","PollMilliseconds":45,"ReturnToXboxAfterGame":false}'
[System.IO.File]::WriteAllText($tmp, $body, (New-Object System.Text.UTF8Encoding($true)))   # $true = with BOM

. (Join-Path $PSScriptRoot 'SteamPadWatchdog.ps1') -ConfigPath $tmp

$configChecks = @(
    @{ n='BOM-prefixed config still parses';       got=$Config.BridgeExe;             e='C:\Program Files\SteamlessController\SteamlessController.exe' }
    @{ n='Override wins over default';             got=[int]$Config.PollMilliseconds;        e=45 }
    @{ n='Boolean override survives round-trip';   got=[bool]$Config.ReturnToXboxAfterGame;  e=$false }
    @{ n='Unspecified key falls back to default';  got=[int]$Config.BridgeStartDelaySeconds; e=3 }
)
foreach ($c in $configChecks) {
    if ($c.got -eq $c.e) { $pass++; Write-Host ("  PASS  {0}" -f $c.n) -ForegroundColor Green }
    else { $fail++; Write-Host ("  FAIL  {0}`n        expected '{1}' got '{2}'" -f $c.n, $c.e, $c.got) -ForegroundColor Red }
}
Remove-Item $tmp -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("  {0} passed, {1} failed" -f $pass, $fail) -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
if ($fail) { exit 1 }
