# Steam Controller → Xbox Controller (XInput) on Windows

**Make the 2026 Steam Controller work as a normal Xbox gamepad in Windows, outside Steam** —
in the Xbox app, Xbox full screen experience, Game Pass titles, emulators, and any game that
only speaks XInput. Then hand the controller back to Steam Input automatically whenever a
Steam game is running.

If you searched for any of these, you are in the right place:

- Steam Controller not working outside Steam
- Steam Controller doesn't work with Xbox Game Pass / the Xbox app
- Steam Controller has no XInput mode
- Steam Controller only works when Steam is running
- Steam Controller stuck in lizard mode (keyboard and mouse)
- Steam Controller not detected by game
- `GAMEMODE: exclusive claim blocked - another process holds a write handle`

---

## The problem

The 2026 Steam Controller **has no XInput mode at all** — not even a button-combo fallback
like 8BitDo pads have. Left alone it sits in **"lizard mode"**, emulating a keyboard and
mouse, and only becomes a gamepad when Steam itself is running and translating it.

Windows, the Xbox app and Game Pass titles speak **XInput** and `Windows.Gaming.Input`.
Neither understands lizard mode. So out of the box the pad cannot even move the selection in
the Xbox UI.

Two 2026 changes help but do not solve it:

- **SDL 3 added a native driver** (merged 14 May 2026), so SDL-based games see the pad
  without Steam. The Xbox app is not an SDL app, and many Game Pass titles are XInput-only.
- **Adding a non-Steam game to Steam** works for Win32 games, but Microsoft Store / Game Pass
  titles are packaged apps that Steam Input cannot attach to. This is the wall people hit
  with Forza Horizon 6 and friends.

So the bridge has to work at driver level: read the controller's raw HID reports and publish
a **virtual Xbox 360 pad** through ViGEmBus. Windows cannot tell it from a real Xbox pad.

## The catch this project exists to solve

**Steam and the bridge cannot both run.** Steam takes an exclusive claim on the controller
the moment it starts, which blocks the bridge and can leak memory in the Steam process. Even
launching Steam with `-nocontroller` does not release the device — tested directly:

```
GAMEMODE: exclusive claim blocked - another process holds a write handle (Steam running)
```

So exactly one of them may own the pad at any moment. `SteamPadWatchdog.ps1` arbitrates that
automatically, at the process level:

| Situation | Who owns the controller |
|---|---|
| Windows desktop, Xbox UI, browsing, launching anything | **Bridge** → virtual Xbox 360 pad |
| A Steam game is running | **Steam Input** (trackpads, gyro, back paddles, per-game configs) |
| Steam game exits, you leave Steam in the background | 20 s later Steam quits, bridge takes over |
| You open Big Picture deliberately | Steam keeps it, until idle in the background for 2 min |

Net effect: you stop thinking about it. The Xbox UI always responds to the pad, and Steam
games still get the full Steam Input experience.

---

## Requirements

- Windows 10 or 11, 64-bit
- A 2026 Steam Controller
- PowerShell 5.1 (ships with Windows)
- Administrator rights for the install (it registers a scheduled task and may install a driver)

## Install

1. Download or clone this repository.
2. Right-click **`Install-SteamPadBridge.ps1`** → **Run with PowerShell** (it relaunches itself
   elevated), or from an admin prompt:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-SteamPadBridge.ps1
```

The installer will:

1. Install **[SteamlessController](https://github.com/ddeverill/SteamlessController)** (MIT) if
   not already present — this is the component that reads the pad's raw HID reports.
2. Install **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** if missing (winget first,
   GitHub release as fallback) — the virtual Xbox 360 pad driver.
3. Remove Steam's and SteamlessController's own autostart entries; the watchdog owns startup.
4. Detect your Steam path and the Xbox app's launch id, and write
   `%LOCALAPPDATA%\SteamPadBridge\config.json`.
5. Register a logon scheduled task, **SteamPad Bridge Watchdog**, elevated, with a 15 s delay.
6. Start it and print a state dump so you can see what was found.

Uninstall with `-Uninstall`. Re-run with `-SkipInstall` to refresh config and the task only.

### Three settings the installer cannot set for you

**1. SteamlessController mode (required).** Open its tray icon and set:

- Mode: **"Off while Steam is running"**
- Emulation type: **Xbox 360**
- **Turn off** its own "Start with Windows" — the scheduled task starts it *elevated*, which
  matters, because a non-elevated bridge loses trackpad output to elevated windows.

> Do **not** leave it on Manual and just tick "Enable Steamless Mode". That toggle is runtime
> state and is **never written to the registry**, so the bridge comes up holding nothing after
> every reboot and after every handoff back from Steam. `AutoSteamMode = 1`
> ("Off while Steam is running") claims the pad automatically whenever Steam is absent.

**2. Steam button mapping.** In SteamlessController, check the Steam button emits the Xbox
**Guide** button — that is what opens Game Bar and the Xbox home overlay.

**3. Steam autostart.** Steam → Settings → Interface → untick *"Run Steam when my PC starts"*.
Steam re-adds its own Run key when it exits, so the watchdog scrubs it every 5 minutes too,
but turning the setting off is cleaner.

### Optional: boot straight into Xbox mode

Settings → Gaming → **Xbox mode** (called "Full screen experience" on older builds): set
**Choose home app = Xbox** and turn on **Enter Xbox mode on startup**.

Xbox mode should boot first and **Steam should not autostart**. The reverse would mean Steam
owns the pad from boot, and every trip to the Xbox app would leave you with a dead controller.
The Xbox app already aggregates Steam, Epic, GOG and Battle.net libraries, so launching a Steam
game from the Xbox UI just starts Steam on demand — which is the handoff the watchdog is built
around.

## Verify it works

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\SteamPadBridge\SteamPadWatchdog.ps1" -SelfTest
```

Prints whether the pad is attached, whether ViGEmBus is loaded and an XInput pad is visible,
whether the bridge is running, whether Steam or a Steam game is running, and where the log is.
The log lives at `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log` and records every handoff.

The state machine has its own test suite, which needs no controller, no Steam and no Xbox app:

```powershell
powershell -ExecutionPolicy Bypass -File .\Test-WatchdogDecision.ps1
```

32 checks covering the state machine, the startup path, the return-to-Xbox rules, process
counting, the probe cache and config parsing.

---

## Controller layout

SteamlessController keeps **no config file** — every setting is a DWORD under
`HKCU\Software\SteamlessController`, read **once at process startup**. Changing one does
nothing until the bridge restarts (quit it from the tray; the watchdog brings it back).

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

Value meanings, decoded from the MIT source because none of it is documented:

- **buttons / pad clicks** — `A=0 B=1 X=2 Y=3 LB=4 RB=5 LT=6 RT=7 DPadUp=8 DPadDown=9
  DPadLeft=10 DPadRight=11 LeftMouseButton=12 RightMouseButton=13 None=14 Menu=15 View=16
  L3=17 R3=18 TouchKeyboard=19`
- **pad mode** — `None=0 MousePointer=1 ScrollWheel=2 DS4Touchpad=3`
- **scroll direction** — `Natural=0 Reversed=1`
- **emulated platform** — `Xbox=0 PlayStation=1`
- **auto mode** — `Manual=0 OffWhileSteam=1 OffOnlyInGame=2 OffUnlessProfile=3`

This layout applies **only while the bridge owns the pad** — the Xbox UI, Game Pass titles and
every non-Steam game. Inside a Steam game, Steam Input owns the pad and its own configuration
applies.

### Matching the layout inside Steam games

Steam Input is **not optional** for this controller — a game's controller properties list it as
"Enabled, always required", where every other pad type says "per-game override". That is also
why the bridge can never reach into a Steam game.

**The paddles already match and need nothing.** Steam names face buttons by diamond position
(`Button_Pad_Down`=A, `Button_Pad_Right`=B, `Button_Pad_Left`=X, `Button_Pad_Up`=Y) and its
default back-grip bindings are L4=`Button_Pad_Left`, L5=`Button_Pad_Right`, R4=`Button_Pad_Up`,
R5=`Button_Pad_Down` — exactly L4=X, L5=B, R4=Y, R5=A.

**Only the trackpads differ**, because game layouts default the right pad to `Right_Stick` for
aiming. That is a per-game decision, and worth changing only where a pointer genuinely beats a
stick.

Two things that cost real time if you do not know them:

- **Quick Settings cannot do it.** Its trackpad dropdown offers only stick options. The control
  that works is **Edit Layout → Trackpads → Behavior** (`As Mouse`, `Scroll Wheel`,
  `Mouse Region`, …). Set the pad's `Click` from the **MOUSE** tab of the binding picker.
- **There is no global default.** Layouts are per game. Saving a personal template makes repeat
  visits quick.

Edited layouts live in **Steam Cloud**, not in
`userdata\<id>\241100\remote\controller_config\<appid>\`, so an empty folder there neither
means an edit was lost nor confirms one.

---

## Tuning

Edit `%LOCALAPPDATA%\SteamPadBridge\config.json`, then restart the task.

| Key | Default | What it does |
|---|---|---|
| `GraceAfterGameSeconds` | `20` | Delay after a game exits *and you have left Steam in the background* before Steam is shut down. While Big Picture is focused, Steam is never touched. |
| `IdleBrowseSeconds` | `120` | Steam open, no game, not focused → quit. Set very high if you like leaving Steam open. |
| `FocusXboxAfterSteam` | `true` | Relaunch/focus the Xbox app once Steam is gone. |
| `ReturnToXboxAfterGame` | `true` | Also return to the Xbox app when a full screen app exits to the desktop. |
| `CycleDeviceOnHandoff` | `false` | Last resort — power-cycles the dongle at handoff to force re-enumeration. |
| `BridgeStartDelaySeconds` | `3` | Wait after Steam exits before grabbing the controller. Raise to 5–6 if handoffs are flaky. |
| `PollMilliseconds` | `500` | Loop speed. |

Trackpad feel lives in the registry under `HKCU\Software\SteamlessController`, and applies only
to the patched build described below:

| Value | Default | What it does |
|---|---|---|
| `PadMomentumMs` | `300` | Coast time after a flick. `0` disables coasting. |
| `XboxScrollDivisor` | `6` | Divides scroll while the Xbox app is in front. `1` restores stock. |
| `XboxScrollWarpCursor` | `1` | Move the cursor onto content before scrolling in the Xbox app. |

All three are read **once at bridge startup**.

---

## Optional: the patched SteamlessController build

`steamlesscontroller-patches/` holds reference copies of three changes to
[SteamlessController](https://github.com/ddeverill/SteamlessController), none of which are
reachable from its settings. **They are optional** — everything above works on the stock binary.

- **Flick-to-coast on both pads.** Stock, pad movement stops dead the instant the finger lifts.
  The patch estimates lift-off velocity and decays it exponentially after release.

  The velocity estimate is the part that matters, and the obvious implementation does not work.
  Differencing the last two reports and smoothing feels chaotic, because a report gap is ~4 ms:
  one unit of capacitive jitter becomes 250 units/s of noise, and any smoothing quick enough to
  stay responsive is dominated by it. So this follows what Android's `VelocityTracker` settled
  on — keep a bounded history of finger *positions*, discard anything older than a 100 ms
  horizon, and least-squares fit a 2nd-degree polynomial across the rest. The linear coefficient
  at release is the velocity; the quadratic term is what makes an accelerating flick feel right.
  Every touched report is sampled, moved or not, which is what makes "drag, pause, lift"
  correctly produce no fling. `velocity_test.cpp` checks the fit against synthetic swipes.

- **Whole-detent scrolling.** Stock, scroll is emitted as sub-`WHEEL_DELTA` deltas at report
  rate. Classic Win32 scroll bars accumulate those happily, but WinUI/UWP surfaces retarget a
  smooth-scroll animation on every wheel message, so a stream of fractional deltas leaves the
  view shivering in place without travelling. That is exactly what the Xbox full screen
  experience does. A real wheel only sends whole notches; so does the patch.

- **Xbox app scroll scaling and cursor warp.** One notch in the Xbox UI advances a whole content
  row (~54% of the viewport), roughly 8× an ordinary window — hence `XboxScrollDivisor`. And
  Windows delivers wheel events to the window under the **cursor**, not the focused one; the
  Xbox UI is gamepad-driven, so the other pad can easily park the pointer on the nav rail or a
  screen edge, which swallows the wheel entirely. Measured: six notches at (3439,0) scrolled
  nothing, while six at screen centre moved more than a full screen. The patch recentres the
  cursor once per gesture, only in that app, only when the cursor is somewhere useless.

Build from the SteamlessController source with the VS 2026 C++ toolset
(`cmake --preset release`, then `cmake --build build/release --config Release`) and copy the
result over the installed binary, keeping the original as `SteamlessController.exe.stock`.

> **A SteamlessController update silently overwrites the patch** and both behaviours revert to
> stock. Nothing errors. Rebuild and copy back, or restore `.stock` deliberately.

---

## Audio haptics

The same application can also **stream system audio to the controller's trackpad actuators as
PCM and replace game rumble with it** - an EQ, a speaker-sync delay, and an option to gate the
haptics on the game's own rumble calls so the game supplies the timing and the audio supplies
the texture.

That work, the HID protocol it uses, and two hardware findings that are not documented
anywhere else - only one of five vendor interfaces drives the actuators, and a write costs
4000 us because it waits for the radio slot - are in
[`steamlesscontroller-patches/audio-haptics/`](steamlesscontroller-patches/audio-haptics/).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Xbox UI ignores the pad at boot | Usually the dongle enumerated after the bridge started. Raise the task delay, or unplug/replug. `-SelfTest` will show `ControllerPresent: False`. |
| Pad works, but the Xbox UI does nothing while Steam is open | Working as designed — Steam owns the pad. Quit Steam or wait out the idle timer. |
| Steam game launches but Steam Input does not see the pad | The bridge released it a fraction too late. Press the Steam button, or set `CycleDeviceOnHandoff: true`. |
| Trackpad-as-mouse stops working over some windows | Elevation. Make sure the scheduled task is starting the bridge, not its own "Start with Windows". |
| **Pad is dead after a reboot until you click the tray icon** | `AutoSteamMode` is back to `Manual` (0). The tray's "Enable Steamless Mode" tick is runtime-only and is never persisted. Set it to `1`. |
| Scrolling does nothing in the Xbox app | Almost certainly the cursor, not the scroll — see the cursor-warp note above. |
| Scrolling in the Xbox app flies down the page | One notch there is a whole content row. Raise `XboxScrollDivisor`. |
| Momentum / whole-detent scroll / Xbox scroll fixes all stop at once | A SteamlessController update overwrote the patched binary. Rebuild and copy back. |
| Everything is dead | The pad falls back to lizard mode: the right trackpad is a mouse, so you can always click your way out. Nothing here can strand you without input. |
| A firmware update breaks the bridge | It has happened once (SteamlessController #40). Update SteamlessController before assuming this setup is at fault. |

## Known limitations

- In the Xbox UI and Game Pass titles the pad is a plain Xbox 360 controller: **no gyro, no
  trackpad-as-joystick, no pressure sensitivity, no back-paddle profiles.** XInput has no
  concept of those. You get them back inside Steam games, where Steam Input takes over.
- Rumble works. The second trackpad is a mouse, not a second stick.
- ViGEmBus is end-of-life — still functional, and still what DS4Windows and everything else in
  this space uses. It is why GlosSI, the old way of doing this, was archived in Nov 2025.
- This depends on a third-party tool. It is MIT-licensed and actively developed; if it ever
  stops being maintained, the SDL driver source documents the protocol well enough to rebuild
  the bridge.
- SteamlessController v1.17's own "auto mode" sounds like it replaces this watchdog. It does
  not: [issue #79](https://github.com/ddeverill/SteamlessController/issues/79) is that auto mode
  can never take the controller while Steam holds it, which is precisely the case the watchdog
  exists for. Arbitrating at the process level is still the only thing that works.

## Credits and licence

This repository is MIT-licensed — see [LICENSE](LICENSE).

It installs and configures, but does not include, these projects:

- **[SteamlessController](https://github.com/ddeverill/SteamlessController)** by ddeverill (MIT)
  — reads the controller's raw HID reports. The files in `steamlesscontroller-patches/` are
  derivative works of that project and remain under its MIT licence.
- **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** by Nefarius — the virtual pad driver.

Not affiliated with Valve, Microsoft or AMD. "Steam" and "Xbox" are trademarks of their
respective owners.

## Contributing

Issues and pull requests welcome. Particularly useful:

- Testing on other Windows builds and Steam Controller firmware revisions
- Upstreaming the trackpad patches into SteamlessController itself
- Handoff edge cases the watchdog gets wrong — attach
  `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log`

## Related searches

Steam Controller 2026 XInput · Steam Controller Xbox app · Steam Controller Game Pass ·
Steam Controller without Steam · Steam Controller lizard mode fix · Steam Controller ViGEmBus ·
Steam Controller virtual Xbox 360 controller · SteamlessController watchdog ·
Steam Controller Xbox full screen experience · Steam exclusive claim controller
