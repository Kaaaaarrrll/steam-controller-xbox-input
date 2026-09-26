# SteamlessController patch: audio haptics, the "..." button, trackpad click without drag

One patch, **`steamlesscontroller-1.17.patch`**, against
[SteamlessController](https://github.com/ddeverill/SteamlessController) by ddeverill at
**version 1.17** (commit `6a513b0`).

Verified on a pristine checkout of that commit: it applies with `git apply` and builds with no
errors, producing `SteamlessController.exe` and `AudioHapticsProbe.exe`.

> **Based on 1.17. Upstream has since moved on** — 1.24 at the time of writing, with signed
> releases — and rewrote many of the same files, so this patch does **not** apply to current
> upstream. Porting it is planned. If you want a signed, current build and do not need these
> additions, use upstream's releases.

These changes are derivative works of SteamlessController and remain under **its** MIT licence.

## What it adds

| | |
|---|---|
| **Audio haptics** | Streams system audio to the trackpad actuators as PCM (HID reports `0x86` / `0x88`) and can replace game rumble with it — six-band EQ, speaker-sync delay, and a gate that lets audio through only while the game asks for rumble. Off by default. Write-up: [`docs/audio-haptics.md`](../docs/audio-haptics.md). |
| **The "..." (Quick Access) button works** | It is bit `0x10` of report byte 2, which stock SteamlessController never decodes, so the button does nothing. Here it toggles the Windows touch keyboard — press to show, press again to hide. |
| **Trackpad click without drag** | Pressing a pad hard enough to click rolls the fingertip, so the cursor moved between button-down and button-up and Windows saw a drag. The pointer now freezes while the pad is being *pressed* — triggered by contact area, which rises before the click registers — and for a short grace after. |
| **Flick-to-coast** | The pointer glides on after a flick. Velocity comes from a least-squares fit over the last 100 ms of positions, not from the last two reports (see below). |
| **Whole-detent scrolling** | WinUI/UWP surfaces such as the Xbox app shiver in place on fractional wheel deltas. A real wheel sends whole notches; so does this. |
| **Xbox app scroll scaling and cursor warp** | One notch in the Xbox app is a whole content row, and the wheel goes to whatever is under the cursor, not the focused window. |
| **One settings window, two tabs** | Controls and Audio Haptics together, in two columns. Both tabs fit without scrolling on a 4K display at 200% scaling. |
| **A clearer "driver missing" popup** | Says what ViGEmBus is for, and that uninstalling a streaming or controller app — Apollo, Sunshine and similar — often removes it. |
| **Undecoded-bit logging** | Any report bit the decoder does not name is written to the event log as `UNKNOWNBIT:`. That is how the "..." button was found. |
| **`AudioHapticsProbe.exe`** | Finds the one interface that actually drives the actuators, and measures the transport. |

## Apply and build

```
git clone https://github.com/ddeverill/SteamlessController
cd SteamlessController
git checkout 6a513b0
git apply path\to\steamlesscontroller-1.17.patch
cmake --preset release
cmake --build build/release --config Release
```

Needs Visual Studio 2026 with the C++ desktop workload.

**Clone to a short path.** A deep one runs into MSBuild's 260-character path limit and fails
with `FileTracker : error FTK1011: could not create the new file tracking log file`. That is the
path, not the patch.

Copy `build\release\Release\SteamlessController.exe` over the installed one, keeping the
original as `SteamlessController.exe.stock`.

> **A SteamlessController update silently overwrites the patched binary**, and everything above
> reverts to stock with no error. Rebuild and copy back.

## Settings

The audio haptics are set from the **Audio Haptics** tab and apply live. The trackpad values
below live in `HKCU\Software\SteamlessController` and are read once when the program starts.

| Value | Default | What it does |
|---|---|---|
| `PadPressArea` | `1400` | Contact area at which the pointer freezes for a press. Lower it if clicks still drag. |
| `PadPressReleaseArea` | `900` | Area it must fall back below before the pointer moves again. |
| `PadPressGraceMs` | `120` | How long the freeze holds after the press ends. |
| `PadMomentumMs` | `300` | Coast time after a flick. `0` disables coasting. |
| `XboxScrollDivisor` | `6` | Divides scrolling while the Xbox app is in front. `1` restores stock. |
| `XboxScrollWarpCursor` | `1` | Move the cursor onto content before scrolling in the Xbox app. |
| `DevMode` | `0` | `1` enables right-click and developer tools in the settings window. |

## The velocity fit, and its test

Differencing the last two reports looks right and feels chaotic: reports are ~4 ms apart, so one
unit of capacitive jitter becomes 250 units/s of noise, and any smoothing fast enough to stay
responsive is dominated by it. So this follows what Android's `VelocityTracker` settled on —
keep a bounded history of finger *positions*, drop anything older than 100 ms, and fit a
second-degree polynomial across the rest. The linear coefficient at release is the velocity.
Every touched report is sampled, moved or not, which is what makes "drag, pause, lift"
correctly produce no fling.

`velocity_test.cpp` checks the fit against synthetic swipes — constant, accelerating,
drag-then-pause, jittery, and stale samples outside the horizon. From a VS developer prompt:

```
cl /nologo /EHsc /W4 /WX /O2 velocity_test.cpp
.\velocity_test.exe
```

9 checks, all passing.
