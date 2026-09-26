# SteamlessController additions: audio haptics, the "..." button, trackpad click without drag

One patch, **`steamlesscontroller-1.24.patch`**, against
[SteamlessController](https://github.com/ddeverill/SteamlessController) by ddeverill at
**version 1.24** (commit `26c5b4a`), the current release at the time of writing.

Verified on a pristine checkout of that commit: it applies with `git apply`, the result is
identical to the fork's `additions-1.24` branch, and it builds with no errors or warnings under
upstream's `/W4 /WX`, producing `SteamlessController.exe` and `AudioHapticsProbe.exe`. Upstream's
own logic checks (`BindingCodecProbe`, `TrackpadDirectionProbe`) still pass.

**It only adds.** Three new files and small hooks into upstream's code. The one upstream line it
rewrites is the wording of the ViGEmBus-missing popup. Everything else upstream does in 1.24,
including tap-to-click, press detection, the settings window and signed releases, is untouched.

## Use any of it, anywhere

Anyone may use any of this code in any of their own projects: copy one function, one file, or
the whole patch. There is no need to fork this repository or the fork, or branch from either, and
no need to ask. It is MIT licensed, so keep the licence notice with a substantial copy.
ddeverill's original code stays under his MIT licence.

## What it adds

| | |
|---|---|
| **Audio haptics** | Streams system audio to the trackpad actuators as PCM (HID reports `0x86` / `0x88`) and can replace game rumble with it: six-band EQ, speaker-sync delay, and a gate that lets audio through only while the game asks for rumble. Off by default. Set from a new **Audio Haptics** tab in the Customize Controls window; changes apply live. Write-up: [`docs/audio-haptics.md`](../docs/audio-haptics.md). |
| **The "..." (Quick Access) button works** | It is bit `0x10` of report byte 2, which upstream never decodes, so the button does nothing there. Here it toggles the Windows touch keyboard: press to show, press again to hide. |
| **Trackpad click without drag** | Pressing a pad hard enough to click rolls the fingertip, so the cursor moved between button-down and button-up and Windows saw a drag. The pointer now holds still while the pad is pressed, and for 120 ms after. It uses upstream's own press detection (contact area, `kPadPressArea`), so nothing new to tune. |
| **A clearer "driver missing" popup** | Says what ViGEmBus is for, and that uninstalling a streaming or controller app (Apollo, Sunshine and similar) often removes it. |
| **`AudioHapticsProbe.exe`** | Finds the one interface that actually drives the actuators, and measures the transport. |

| File | Change |
|---|---|
| `src/steam/AudioHaptics.{h,cpp}` | new: capture, DSP, transport, rumble gate |
| `src/probe/AudioHapticsProbe.cpp` | new: standalone check of the capture path |
| `src/steam/SteamController.{h,cpp}` | starts and stops the engine with the rumble thread; drops motor rumble while audio streams; names bit `0x10` |
| `src/app/RemapWindow.cpp` | the Audio Haptics tab, as one self-contained piece of the page, plus the C++ that reads and writes its settings |
| `src/app/ControllerManager.cpp` | the "..." button toggles the touch keyboard |
| `src/app/TrackpadInput.{h,cpp}` | the press hold |
| `src/app/TrayApp.cpp` | popup wording |
| `CMakeLists.txt` | builds the new files and the probe |

## Apply and build

**Easier:** the same change is on the `additions-1.24` branch of the fork (its default branch),
[github.com/Kaaaaarrrll/SteamlessController](https://github.com/Kaaaaarrrll/SteamlessController).
Clone it and skip straight to the two `cmake` lines. To apply the patch yourself:

```
git clone https://github.com/ddeverill/SteamlessController
cd SteamlessController
git checkout 26c5b4a
git apply path\to\steamlesscontroller-1.24.patch
cmake --preset release
cmake --build build/release --config Release
```

Needs Visual Studio 2026 with the C++ desktop workload.

**Clone to a short path.** A deep one runs into MSBuild's 260-character path limit and fails
with `FileTracker : error FTK1011: could not create the new file tracking log file`. That is the
path, not the patch.

Copy `build\release\Release\SteamlessController.exe` over the installed one, keeping the
original as `SteamlessController.exe.stock`.

> **A SteamlessController update silently overwrites the patched program**, and everything above
> reverts to standard with no error. Rebuild and copy back.

## Settings

Audio haptics are set from the **Audio Haptics** tab and apply live. They are stored in
`HKCU\Software\SteamlessController` as `HapticAudioMode`, `HapticAudioGain`,
`HapticAudioDelayMs`, `HapticAudioLatency`, `HapticAudioRate`, `HapticRumbleGate` and
`HapticEqBand0` to `HapticEqBand5`. The other additions have no settings.

## Older version

The first version of these changes was against 1.17, before upstream reworked the trackpads. It
is still on the fork's `patched-1.17` branch and in this repository's history. It also had
flick-to-coast, Xbox-app scroll scaling and cursor warp, and a rearranged settings window. Those
are not carried forward: they would mean rewriting upstream code rather than adding to it.
