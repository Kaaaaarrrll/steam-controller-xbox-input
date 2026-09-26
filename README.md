# Steam Controller → Xbox Controller on Windows (outside Steam)

Use the **2026 Steam Controller as a normal Xbox controller** on Windows — in the Xbox app,
Xbox mode (the full screen experience), Game Pass games, and any game that expects an Xbox
controller. The trackpads work as a mouse.

> [!IMPORTANT]
> **This does not work while Steam is running.**
>
> - **When Steam opens, this switches itself off** and Steam takes over the controller.
> - **To get the Xbox controller back, exit Steam completely.** Closing or minimising the Steam
>   window is not enough — Steam keeps running in the background. Use **Steam → Exit**, or
>   right-click the Steam icon next to the clock → **Exit Steam**.
> - **It never closes Steam for you.**
>
> Steam and this can't share the controller: with both running, every button press goes to
> both apps at once.

## How it works

| Steam is… | Your Steam Controller is… |
|---|---|
| **Not running** | an **Xbox controller**, with the trackpads as a mouse |
| **Running** — even minimised, even with no game open | Steam's. This is switched off. |

A typical evening: play in the Xbox app → launch a Steam game (Steam opens and takes the
controller) → finish → **exit Steam** → a few seconds later the Xbox controller is back.

## What it fixes

- Steam Controller not working outside Steam, in the Xbox app, or with Game Pass
- Steam Controller has no XInput mode / only works when Steam is running
- Steam Controller stuck in lizard mode (acting as a keyboard and mouse)
- Steam Controller not detected by a game that expects an Xbox controller

With the optional [patched build](#optional-the-patched-steamlesscontroller) it also:

- makes the **"..." (Quick Access) button** open the Windows touch keyboard
- stops **trackpad clicks turning into drags**
- plays your **audio through the trackpad haptics** instead of plain rumble

---

## Install

**You need:** Windows 10 or 11 (64-bit), a 2026 Steam Controller, and administrator rights for
the install.

1. Download or clone this repository.
2. Right-click **`Install-SteamPadBridge.ps1`** → **Run with PowerShell**. It asks for
   administrator rights itself. Or, from an admin prompt:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install-SteamPadBridge.ps1
   ```

3. Do the one-time settings below.

The installer sets up [SteamlessController](https://github.com/ddeverill/SteamlessController)
(reads the controller) and [ViGEmBus](https://github.com/nefarius/ViGEmBus) (the virtual Xbox
controller driver) if they are missing, and registers a small background task —
**SteamPad Bridge Watchdog** — that switches the bridge off when Steam starts and back on when
Steam exits. It does not touch Steam's own settings.

Uninstall with `-Uninstall`. Re-run with `-SkipInstall` to refresh the configuration only.

### One-time settings

Open SteamlessController from its icon next to the clock:

1. **Mode: "Off while Steam is running."** Not Manual — the manual on/off switch is not saved,
   so the controller would be dead after every restart.
2. **Emulation type: Xbox 360.**
3. **Turn off its own "Start with Windows".** The background task starts it with administrator
   rights instead, which it needs for the trackpads to work over every window.
4. **Check the Steam button is set to the Xbox Guide button** — that's what opens the Xbox
   overlay.

**Steam starting with Windows is up to you** — but if it does, the controller is Steam's until
you exit Steam. If you mostly use the Xbox app, turn it off in Steam → Settings → Interface →
*"Run Steam when my computer starts"*.

**Optional — boot straight into Xbox mode:** Windows Settings → Gaming → **Xbox mode** (called
"Full screen experience" on older builds): set **Choose home app = Xbox** and turn on **Enter
Xbox mode on startup**.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| **The controller doesn't work in the Xbox app / Xbox mode** | Steam is almost certainly running. Look for its icon next to the clock and **exit Steam** (right-click → Exit Steam). The controller comes back a few seconds later. |
| **Every button press does something in Steam *and* the Xbox app** | Steam and the bridge are both running. Set SteamlessController's mode to **"Off while Steam is running"** and exit Steam. |
| **Steam keeps closing by itself** | An older version of this watchdog did that on purpose — 20 s after a game ended, or after 2 min of Steam idle in the background. Re-run the installer to update it. If this was the cause, `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log` has lines reading `Shutting Steam down (...)`. |
| **Steam no longer starts with Windows** | Older versions deleted Steam's autostart entry, and kept deleting it. Turn it back on in Steam → Settings → Interface; it sticks now. |
| **Popup: "Driver required" or "Xbox controller driver missing"** — and the Steam button, the "..." button and the paddles all stopped working | **ViGEmBus is gone** — most often removed by uninstalling an app that had installed it: Apollo, Sunshine, DS4Windows and others. SteamlessController's event log shows `vigem_connect ... driverMissing=1`. Reinstall **ViGEmBus 1.22.0** from [its releases page](https://github.com/nefarius/ViGEmBus/releases/latest) (the final release; the project is retired). The bridge retries every 30 seconds and picks it up without a restart. Installed on its own, it no longer belongs to the other app. |
| **Controller is dead after a restart until you click the tray icon** | SteamlessController is on Manual. Set it to **"Off while Steam is running"** — the manual switch is never saved. |
| **The Xbox app ignores the controller right after boot** | Either Steam started with Windows (see the first row), or the dongle appeared after the bridge started — unplug and replug it. `-SelfTest` (below) shows `ControllerPresent: False` in that case. |
| **A trackpad click turns into a click-and-drag** | Use the patched build, which freezes the pointer while the pad is pressed. Still dragging? Lower `PadPressArea` (try `1100`) and restart the bridge. |
| **The "..." button does nothing** | Standard SteamlessController never reads that button. The patched build makes it open the touch keyboard. |
| **The on-screen keyboard appears but its keys can't be clicked with the trackpad** | That is `osk.exe`, which runs with higher privileges, so Windows discards clicks aimed at it. Use the **touch** keyboard instead — the "..." button in the patched build, or the Touch Keyboard binding. |
| **The trackpad mouse stops working over some windows** | The bridge isn't running with administrator rights. Make sure the background task starts it, not its own "Start with Windows". |
| **A Steam game starts but Steam doesn't see the controller** | The bridge let go a moment too late. Press the Steam button, or unplug and replug the dongle. |
| **Scrolling does nothing in the Xbox app** | Almost certainly the cursor, not the scroll — see *cursor warp* under the patched build. |
| **Scrolling in the Xbox app flies down the page** | One notch there is a whole row of content. Raise `XboxScrollDivisor`. |
| **Every patched feature stops at once** — haptics, the "..." button, momentum, scroll fixes | A SteamlessController update replaced the patched program. Rebuild and copy it back. |
| **Everything is dead** | The controller falls back to lizard mode, where the right trackpad is a mouse — you can always click your way out. |
| **A controller firmware update breaks the bridge** | It has happened once (SteamlessController #40). Update SteamlessController before assuming this setup is at fault. |

---

# Reference

Everything below is detail. You don't need it to use the bridge.

## Why a bridge is needed

The 2026 Steam Controller **has no XInput mode at all** — not even a button-combo fallback like
8BitDo pads have. Left alone it sits in **"lizard mode"**, pretending to be a keyboard and mouse,
and only becomes a gamepad when Steam is running and translating it.

Windows, the Xbox app and Game Pass titles speak **XInput** and `Windows.Gaming.Input`. Neither
understands lizard mode, so out of the box the pad can't even move the selection in the Xbox UI.

Two 2026 changes help but don't solve it:

- **SDL 3 added a native driver** (merged 14 May 2026), so SDL-based games see the pad without
  Steam. The Xbox app is not an SDL app, and many Game Pass titles are XInput-only.
- **Adding a non-Steam game to Steam** works for Win32 games, but Microsoft Store / Game Pass
  titles are packaged apps that Steam Input can't attach to. This is the wall people hit with
  Forza Horizon 6 and friends.

So the bridge works at driver level: it reads the controller's raw HID reports and publishes a
**virtual Xbox 360 controller** through ViGEmBus. Windows can't tell it from a real one.

## Why it can't run alongside Steam

Steam takes an exclusive claim on the controller the moment it starts, which blocks the bridge.
Launching Steam with `-nocontroller` does not release it — tested directly:

```
GAMEMODE: exclusive claim blocked - another process holds a write handle (Steam running)
```

Where the bridge falls back to shared access instead, both apps receive every press. Tested on
hardware: with Steam open and the bridge active, pressing the Steam button brought Steam to the
front, and both apps acted on the same input.

So `SteamPadWatchdog.ps1` stops the bridge whenever Steam is running — game or no game — and
starts it again once Steam exits. It never closes Steam. Earlier versions did, to get the
controller back automatically: 20 seconds after a game ended, or after two minutes of Steam
sitting in the background. From the outside that looked like Steam quitting at random, and they
also deleted Steam's autostart entry. Both are gone.

## Check it's working

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\SteamPadBridge\SteamPadWatchdog.ps1" -SelfTest
```

Shows whether the controller is attached, whether ViGEmBus is loaded and an Xbox controller is
visible, whether the bridge and Steam are running, and where the log is. The log lives at
`%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log` and records every handover.

The watchdog's decisions have their own tests, which need no controller, no Steam and no Xbox
app:

```powershell
powershell -ExecutionPolicy Bypass -File .\Test-WatchdogDecision.ps1
```

23 checks — including two that sweep every possible combination of inputs and prove that the
watchdog can never close Steam, and can never leave the bridge running alongside Steam.

## Controller layout

SteamlessController keeps **no config file** — every setting is a DWORD under
`HKCU\Software\SteamlessController`, read **once at startup**. Changing one does nothing until
the bridge restarts (quit it from the tray; the watchdog brings it back). Audio haptics are the
exception: they are set from their own tab and apply live.

| Control | Behaviour | Registry value |
|---|---|---|
| L4 | X | `BackBtnL4 = 2` |
| L5 | B | `BackBtnL5 = 1` |
| R4 | Y | `BackBtnR4 = 3` |
| R5 | A | `BackBtnR5 = 0` |
| Right trackpad | mouse | `RightPadMode = 1` |
| Right trackpad click | left mouse button | `RightPadClick = 12` |
| Left trackpad | scroll wheel | `LeftPadMode = 2` |
| Left trackpad click | right mouse button | `LeftPadClick = 13` |
| Scroll direction | natural | `LeftPadScrollDir = 0` (`1` reverses) |
| Steam button | Xbox **Guide** | fixed |
| "..." (Quick Access) button | toggles the Windows touch keyboard | fixed — **patched build only**; standard SteamlessController ignores this button |

Value meanings, decoded from the MIT source because none of it is documented:

- **buttons / pad clicks** — `A=0 B=1 X=2 Y=3 LB=4 RB=5 LT=6 RT=7 DPadUp=8 DPadDown=9
  DPadLeft=10 DPadRight=11 LeftMouseButton=12 RightMouseButton=13 None=14 Menu=15 View=16
  L3=17 R3=18 TouchKeyboard=19`
- **pad mode** — `None=0 MousePointer=1 ScrollWheel=2 DS4Touchpad=3`
- **scroll direction** — `Natural=0 Reversed=1`
- **emulated platform** — `Xbox=0 PlayStation=1`
- **auto mode** — `Manual=0 OffWhileSteam=1 OffOnlyInGame=2 OffUnlessProfile=3` — use `1`

This layout applies **only while the bridge owns the controller** — that is, while Steam is not
running. While Steam runs, Steam Input's own configuration applies.

### Matching the layout inside Steam games

Steam Input is **not optional** for this controller — a game's controller properties list it as
"Enabled, always required", where every other pad type says "per-game override".

**The paddles already match and need nothing.** Steam names face buttons by diamond position
(`Button_Pad_Down`=A, `Button_Pad_Right`=B, `Button_Pad_Left`=X, `Button_Pad_Up`=Y) and its
default back-grip bindings are L4=`Button_Pad_Left`, L5=`Button_Pad_Right`, R4=`Button_Pad_Up`,
R5=`Button_Pad_Down` — exactly L4=X, L5=B, R4=Y, R5=A.

**Only the trackpads differ**, because game layouts default the right pad to `Right_Stick` for
aiming. That is a per-game decision, worth changing only where a pointer genuinely beats a stick.

Two things that cost real time if you don't know them:

- **Quick Settings can't do it.** Its trackpad dropdown offers only stick options. The control
  that works is **Edit Layout → Trackpads → Behavior** (`As Mouse`, `Scroll Wheel`,
  `Mouse Region`, …). Set the pad's `Click` from the **MOUSE** tab of the binding picker.
- **There is no global default.** Layouts are per game. Saving a personal template makes repeat
  visits quick.

Edited layouts live in **Steam Cloud**, not in
`userdata\<id>\241100\remote\controller_config\<appid>\`, so an empty folder there neither means
an edit was lost nor confirms one.

## Tuning

The watchdog reads `%LOCALAPPDATA%\SteamPadBridge\config.json` at startup — edit it, then
restart the task.

| Key | Default | What it does |
|---|---|---|
| `ReturnToXboxAfterGame` | `true` | Bring the Xbox app back when a full screen game exits to the desktop. |
| `BridgeStartDelaySeconds` | `3` | Wait after Steam exits before starting the bridge. Raise to 5–6 if handovers are flaky. |
| `PollMilliseconds` | `500` | How often it checks. |

`GraceAfterGameSeconds`, `IdleBrowseSeconds`, `FocusXboxAfterSteam`, `CycleDeviceOnHandoff` and
`KeepSteamAutostartOff` belonged to the old Steam-closing behaviour and are gone. Left in an old
`config.json`, they are ignored.

Trackpad feel is set in the registry under `HKCU\Software\SteamlessController`, applies only to
the patched build, and is read **once at bridge startup**. The full list is in
[`steamlesscontroller-patches/README.md`](steamlesscontroller-patches/README.md#settings); the
ones worth knowing:

| Value | Default | What it does |
|---|---|---|
| `PadPressArea` | `1400` | Contact area at which the pointer freezes for a click. Lower it if clicks still drag. |
| `PadMomentumMs` | `300` | Coast time after a flick. `0` turns coasting off. |
| `XboxScrollDivisor` | `6` | Divides scrolling while the Xbox app is in front. `1` restores standard behaviour. |

## Optional: the patched SteamlessController

Everything above works with standard SteamlessController. The patched build adds the following,
**none of which are in upstream SteamlessController as of 1.24** (checked against its source):

- **Audio haptics** — your system audio played through the trackpad actuators, optionally
  replacing game rumble, with an EQ, a delay to sync with your speakers, and a gate that lets it
  through only when the game asks for rumble. See [below](#audio-haptics).
- **The "..." (Quick Access) button toggles the Windows touch keyboard.** It's bit `0x10` of
  report byte 2, which standard SteamlessController never reads — so there, the button does
  nothing at all.
- **Trackpad clicks no longer turn into drags.** Pressing hard enough to click rolls the
  fingertip; the pointer now freezes while the pad is pressed, triggered by contact area, which
  rises *before* the click registers.
- **Flick-to-coast** on both pads, from a least-squares velocity fit rather than a two-sample
  difference — which feels chaotic, because one unit of jitter over a 4 ms report gap is
  250 units/s of noise.
- **Scroll scaling and cursor warp for the Xbox app**, where one wheel notch is a whole row of
  content and the wheel goes to whatever is under the cursor rather than the focused window.
- **One settings window** with Controls and Audio Haptics tabs, and a **clearer popup** when
  ViGEmBus goes missing.

It also sends only **whole wheel notches** when scrolling, because the Xbox app's WinUI surfaces
shiver in place on fractional ones.

Two ways to get it — they're the same change:

- **The fork** — [github.com/Kaaaaarrrll/SteamlessController](https://github.com/Kaaaaarrrll/SteamlessController),
  branch `patched-1.17`: SteamlessController 1.17 with the changes already applied. Clone and
  build.
- **The patch** — [`steamlesscontroller-patches/steamlesscontroller-1.17.patch`](steamlesscontroller-patches/),
  to apply to SteamlessController 1.17 yourself. Verified to apply and build on a clean checkout.

Build instructions, every setting, and the reasoning behind each change:
[`steamlesscontroller-patches/README.md`](steamlesscontroller-patches/README.md).

> **Based on SteamlessController 1.17.** Upstream is now at 1.24 and has improvements this build
> lacks — tap-to-click, press detection that catches clicks the firmware misses, and signed
> releases among them. And **a SteamlessController update silently replaces the patched
> program** — everything reverts to standard with no error.

## Audio haptics

The patched build can **stream your system audio to the controller's trackpad actuators as PCM
and use it in place of game rumble** — an EQ, a speaker-sync delay, and an option to gate it on
the game's own rumble calls, so the game supplies the timing and the audio supplies the texture.

That work, the HID protocol it uses, and two hardware findings not documented anywhere else —
only one of five vendor interfaces drives the actuators, and a write costs 4000 µs because it
waits for the radio slot — are in [`docs/audio-haptics.md`](docs/audio-haptics.md).

## Known limitations

- **Not compatible with Steam running** — see the top of this page. By design: the two can't
  share the controller.
- In the Xbox UI and Game Pass titles the controller is a plain Xbox 360 controller: **no gyro,
  no trackpad-as-joystick, no pressure sensitivity, no back-paddle profiles.** XInput has no
  concept of those. You get them back inside Steam games, where Steam Input takes over.
- Rumble works (or audio haptics, with the patched build). The second trackpad is a mouse, not a
  second stick.
- ViGEmBus is end-of-life — still functional, and still what DS4Windows and everything else in
  this space uses. It is why GlosSI, the old way of doing this, was archived in November 2025.
- This depends on a third-party tool. It is MIT-licensed and actively developed; if it ever stops
  being maintained, the SDL driver source documents the protocol well enough to rebuild the
  bridge.
- The patched build is based on SteamlessController 1.17; upstream is at 1.24.

## Credits and licence

This repository is MIT-licensed — see [LICENSE](LICENSE).

It installs and configures, but does not include, these projects:

- **[SteamlessController](https://github.com/ddeverill/SteamlessController)** by ddeverill (MIT)
  — reads the controller's raw HID reports. The patch in `steamlesscontroller-patches/` and the
  [fork](https://github.com/Kaaaaarrrll/SteamlessController) are derivative works of that
  project and remain under its MIT licence.
- **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** by Nefarius — the virtual controller
  driver.

Not affiliated with Valve, Microsoft or AMD. "Steam" and "Xbox" are trademarks of their
respective owners.

## Contributing

Issues and pull requests welcome. Particularly useful:

- Testing on other Windows builds and Steam Controller firmware revisions
- Porting the patch to current SteamlessController (1.24), and upstreaming the parts that belong
  there — decoding the "..." button is a small, self-contained start
- Handover edge cases the watchdog gets wrong — attach
  `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log`

## Related searches

Steam Controller 2026 XInput · Steam Controller Xbox app · Steam Controller Game Pass ·
Steam Controller without Steam · Steam Controller lizard mode fix · Steam Controller ViGEmBus ·
Steam Controller virtual Xbox 360 controller · SteamlessController watchdog ·
Steam Controller Xbox full screen experience · Steam exclusive claim controller ·
Steam Controller stops working when Steam is open · Steam closes by itself ·
ViGEmBus removed after uninstalling Apollo · ViGEmBus uninstalled by Sunshine ·
`GAMEMODE: ViGEm virtual controller failed (stage=vigem_connect, err=0xE0000001, driverMissing=1)` ·
Steam Controller Quick Access button Windows · Steam Controller ellipsis button ·
Steam Controller touch keyboard · Steam Controller trackpad click drag ·
Steam Controller haptics audio · Steam Controller play music through haptics ·
Steam Controller PCM haptics
