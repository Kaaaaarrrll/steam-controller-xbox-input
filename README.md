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
- Steam keeps closing by itself / Steam quits on its own (see [Troubleshooting](#troubleshooting))
- "Driver required" or "Xbox controller driver missing" after uninstalling Apollo, Sunshine or
  DS4Windows — ViGEmBus was removed
- Steam Controller **"..." / Quick Access button does nothing** on Windows
- Steam Controller trackpad click turns into a **click and drag**
- Play audio through the Steam Controller trackpads / audio haptics instead of rumble
- `GAMEMODE: exclusive claim blocked - another process holds a write handle`
- `GAMEMODE: ViGEm virtual controller failed (stage=vigem_connect, err=0xE0000001, driverMissing=1)`

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
| Windows desktop, Xbox UI, Game Pass, anything outside a Steam game | **Bridge** → virtual Xbox 360 pad |
| A Steam game is running | **Steam Input** (trackpads, gyro, back paddles, per-game configs) |
| The Steam game exits — Steam is left open | **Bridge** again. Steam is not touched. |

**It never closes Steam.** Earlier versions did — 20 seconds after a game ended, or after two
minutes of Steam simply sitting in the background — to get the controller back. From the
outside that looks like Steam quitting at random, and it deleted Steam's autostart entry too.
Both are gone. See [Troubleshooting](#troubleshooting) if an older version did this to you.

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
3. Remove SteamlessController's own autostart entry, so the scheduled task starts it elevated
   instead. Steam's autostart is left alone.
4. Detect your Steam path and the Xbox app's launch id, and write
   `%LOCALAPPDATA%\SteamPadBridge\config.json`.
5. Register a logon scheduled task, **SteamPad Bridge Watchdog**, elevated, with a 15 s delay.
6. Start it and print a state dump so you can see what was found.

Uninstall with `-Uninstall`. Re-run with `-SkipInstall` to refresh config and the task only.

### Three settings the installer cannot set for you

**1. SteamlessController mode (required).** Open its tray icon and set:

- Mode: **"Off ONLY while in Steam game"** (`AutoSteamMode = 2`)
- Emulation type: **Xbox 360**
- **Turn off** its own "Start with Windows" — the scheduled task starts it *elevated*, which
  matters, because a non-elevated bridge loses trackpad output to elevated windows.

> Do **not** leave it on Manual and just tick "Enable Steamless Mode". That toggle is runtime
> state and is **never written to the registry**, so the bridge comes up holding nothing after
> every reboot and after every handoff back from Steam.
>
> Mode 2 rather than mode 1 ("Off while Steam is running") is what lets the controller come back
> after a Steam game **without closing Steam**. On mode 1 the bridge stays off for as long as
> Steam is open — which is why older versions of this watchdog closed Steam.

**2. Steam button mapping.** In SteamlessController, check the Steam button emits the Xbox
**Guide** button — that is what opens Game Bar and the Xbox home overlay.

**3. Steam autostart is yours to choose.** Older versions of this installer turned it off, and
the watchdog kept deleting Steam's Run key every five minutes. Neither happens any more. If an
old version switched it off for you: Steam → Settings → Interface → *"Run Steam when my PC
starts"*.

### Optional: boot straight into Xbox mode

Settings → Gaming → **Xbox mode** (called "Full screen experience" on older builds): set
**Choose home app = Xbox** and turn on **Enter Xbox mode on startup**.

The Xbox app already aggregates Steam, Epic, GOG and Battle.net libraries, so launching a Steam
game from the Xbox UI just starts Steam on demand — which is the handoff the watchdog is built
around: Steam Input takes the controller for the game, and the bridge takes it back when you
return.

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

27 checks covering the state machine, the startup path, the return-to-Xbox rules, process
counting, the probe cache and config parsing — including one that sweeps every combination of
inputs and asserts that none can produce an action that closes Steam.

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
| Steam button | Xbox **Guide** | fixed |
| "..." (Quick Access) button | toggles the Windows touch keyboard | fixed — **patched build only**; stock ignores this button |

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
| `ReturnToXboxAfterGame` | `true` | Return to the Xbox app when a full screen app exits to the desktop. |
| `BridgeStartDelaySeconds` | `3` | Wait after a Steam game exits before starting the bridge. Raise to 5–6 if handoffs are flaky. |
| `PollMilliseconds` | `500` | Loop speed. |

`GraceAfterGameSeconds`, `IdleBrowseSeconds`, `FocusXboxAfterSteam`, `CycleDeviceOnHandoff` and
`KeepSteamAutostartOff` belonged to the Steam-closing behaviour and are gone. Left in an old
`config.json` they are ignored.

Trackpad feel lives in the registry under `HKCU\Software\SteamlessController`, applies only to
the patched build described below, and is read **once at bridge startup**. The full list is in
[`steamlesscontroller-patches/README.md`](steamlesscontroller-patches/README.md#settings); the
ones worth knowing:

| Value | Default | What it does |
|---|---|---|
| `PadPressArea` | `1400` | Contact area at which the pointer freezes for a click. Lower it if clicks still drag. |
| `PadMomentumMs` | `300` | Coast time after a flick. `0` disables coasting. |
| `XboxScrollDivisor` | `6` | Divides scroll while the Xbox app is in front. `1` restores stock. |

---

## Optional: the patched SteamlessController build

[`steamlesscontroller-patches/steamlesscontroller-1.17.patch`](steamlesscontroller-patches/)
is one patch against [SteamlessController](https://github.com/ddeverill/SteamlessController)
1.17. **It is optional** — everything above works on the stock binary. Verified to apply and
build on a pristine 1.17 checkout. What it adds:

- **Audio haptics** — system audio streamed to the trackpad actuators, replacing rumble
  (see [below](#audio-haptics)).
- **The "..." (Quick Access) button toggles the Windows touch keyboard.** It is bit `0x10` of
  report byte 2, which stock never decodes — so on stock the button does nothing at all.
- **Trackpad clicks no longer turn into drags.** Pressing hard enough to click rolls the
  fingertip; the pointer now freezes while the pad is being pressed, triggered by contact area,
  which rises *before* the click registers.
- **Flick-to-coast** on both pads, from a least-squares velocity fit rather than a two-sample
  difference — which feels chaotic, because one unit of jitter over a 4 ms report gap is
  250 units/s of noise.
- **Whole-detent scrolling**, because the Xbox app's WinUI surfaces shiver on fractional wheel
  deltas; plus **scroll scaling and cursor warp** for the Xbox app, where one notch is a whole
  content row and the wheel goes to whatever is under the cursor.
- **One settings window** with Controls and Audio Haptics tabs, and a **clearer popup** when
  ViGEmBus goes missing.

How to apply and build, every setting, and the reasoning behind each change:
[`steamlesscontroller-patches/README.md`](steamlesscontroller-patches/README.md).

> **Based on SteamlessController 1.17.** Upstream is at 1.24 and rewrote many of the same
> files, so the patch does not apply there yet. And **a SteamlessController update silently
> overwrites the patched binary** — everything reverts to stock with no error.

---

## Audio haptics

The same application can also **stream system audio to the controller's trackpad actuators as
PCM and replace game rumble with it** - an EQ, a speaker-sync delay, and an option to gate the
haptics on the game's own rumble calls so the game supplies the timing and the audio supplies
the texture.

That work, the HID protocol it uses, and two hardware findings that are not documented
anywhere else - only one of five vendor interfaces drives the actuators, and a write costs
4000 us because it waits for the radio slot - are in
[`docs/audio-haptics.md`](docs/audio-haptics.md). The code is part of the patch above.

## Troubleshooting

| Symptom | Fix |
|---|---|
| **Steam keeps closing by itself** | An older version of this watchdog did that on purpose — 20 s after a game ended, or after 2 min of Steam idle in the background. Re-run the installer to update it. If this was the cause, `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log` has lines reading `Shutting Steam down (...)`. |
| **Steam no longer starts with Windows** | Older versions deleted Steam's Run key, and kept deleting it. Turn it back on in Steam → Settings → Interface; it sticks now. |
| **Popup: "Driver required" / "Xbox controller driver missing"** — and the Steam button, the "..." button and the paddles all stopped working | **ViGEmBus is gone** — most often removed by uninstalling an app that had installed it: Apollo, Sunshine, DS4Windows and others. SteamlessController's event log shows `vigem_connect ... driverMissing=1`. Reinstall **ViGEmBus 1.22.0** from [its releases](https://github.com/nefarius/ViGEmBus/releases/latest) (the final release; the project is retired). The bridge retries every 30 s and picks it up without a restart. Installed on its own, it no longer belongs to the other app. |
| **Trackpad click turns into a click-and-drag** | Patched build: the pointer freezes while the pad is pressed. Still dragging? Lower `PadPressArea` (try `1100`) and restart the bridge. Stock SteamlessController has no protection against this. |
| **The "..." button does nothing** | Stock SteamlessController never decodes it. The patched build maps it to the touch keyboard. |
| **On-screen keyboard appears but its keys can't be clicked with the trackpad** | That is `osk.exe`, which runs at High integrity, so Windows discards injected clicks aimed at it. Use the **touch** keyboard instead — the "..." button in the patched build, or the Touch Keyboard binding. |
| After a Steam game, the Xbox app gets no controller until Steam is closed | Check SteamlessController is on **"Off ONLY while in Steam game"** — mode 1 stays off for as long as Steam is open. If it already is, see [Known limitations](#known-limitations). |
| Xbox UI ignores the pad at boot | Usually the dongle enumerated after the bridge started. Raise the task delay, or unplug/replug. `-SelfTest` will show `ControllerPresent: False`. |
| Pad works, but the Xbox UI does nothing while a Steam game runs | Working as designed — Steam Input owns the pad during a Steam game. It comes back when the game exits. |
| Steam game launches but Steam Input does not see the pad | The bridge released it a fraction too late. Press the Steam button, or unplug and replug the dongle. |
| Trackpad-as-mouse stops working over some windows | Elevation. Make sure the scheduled task is starting the bridge, not its own "Start with Windows". |
| **Pad is dead after a reboot until you click the tray icon** | `AutoSteamMode` is back to `Manual` (0). The tray's "Enable Steamless Mode" tick is runtime-only and is never persisted. Set it to `2`. |
| Scrolling does nothing in the Xbox app | Almost certainly the cursor, not the scroll — see the cursor-warp note above. |
| Scrolling in the Xbox app flies down the page | One notch there is a whole content row. Raise `XboxScrollDivisor`. |
| Every patched feature stops at once — haptics, the "..." button, momentum, scroll fixes | A SteamlessController update overwrote the patched binary. Rebuild and copy back. |
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
- **Not yet verified on hardware: the controller returning to the bridge while Steam is still
  open.** The watchdog now steps aside only while a Steam *game* runs, and leaves the rest to
  SteamlessController's own "Off ONLY while in Steam game" mode. Whether SteamlessController can
  take the device while an idle Steam still holds it depends on Steam's own controller handling,
  and [issue #79](https://github.com/ddeverill/SteamlessController/issues/79) suggests it may
  not. The watchdog's side is covered by its tests; this part is not. If the Xbox app gets no
  controller after a Steam game until you close Steam, that is this — please open an issue and
  attach `%LOCALAPPDATA%\SteamlessController\events.log`.

## Credits and licence

This repository is MIT-licensed — see [LICENSE](LICENSE).

It installs and configures, but does not include, these projects:

- **[SteamlessController](https://github.com/ddeverill/SteamlessController)** by ddeverill (MIT)
  — reads the controller's raw HID reports. The patch in `steamlesscontroller-patches/` is a
  derivative work of that project and remains under its MIT licence.
- **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** by Nefarius — the virtual pad driver.

Not affiliated with Valve, Microsoft or AMD. "Steam" and "Xbox" are trademarks of their
respective owners.

## Contributing

Issues and pull requests welcome. Particularly useful:

- Testing on other Windows builds and Steam Controller firmware revisions
- Hardware reports on the one unverified case — the controller coming back to the bridge while
  Steam is still open (see [Known limitations](#known-limitations))
- Porting the patch to current SteamlessController (1.24), and upstreaming the parts that
  belong there — decoding the "..." button is a small, self-contained start
- Handoff edge cases the watchdog gets wrong — attach
  `%LOCALAPPDATA%\SteamPadBridge\logs\watchdog.log`

## Related searches

Steam Controller 2026 XInput · Steam Controller Xbox app · Steam Controller Game Pass ·
Steam Controller without Steam · Steam Controller lizard mode fix · Steam Controller ViGEmBus ·
Steam Controller virtual Xbox 360 controller · SteamlessController watchdog ·
Steam Controller Xbox full screen experience · Steam exclusive claim controller ·
Steam closes by itself · ViGEmBus removed after uninstalling Apollo · ViGEmBus uninstalled by
Sunshine · Steam Controller Quick Access button Windows · Steam Controller ellipsis button ·
Steam Controller touch keyboard · Steam Controller trackpad click drag · Steam Controller haptics
audio · Steam Controller play music through haptics · Steam Controller PCM haptics
