# Audio haptics for the Steam Controller (2026)

**Stream system audio to the trackpad actuators as PCM, and replace game rumble with it.**

The Steam Controller has voice-coil actuators under each trackpad, and firmware that will
accept raw audio and drive them with it. That is how the "music through a Steam Controller"
demos work. This turns the same mechanism into a live system-audio haptic engine inside
[SteamlessController](https://github.com/ddeverill/SteamlessController), with an EQ, a delay
line, and an option to gate it on the game's own rumble calls.

Searchable: Steam Controller audio haptics · PCM haptics · haptic audio streaming ·
Steam Controller rumble replacement · HID report 0x86 0x88 · actuator PCM mode.

---

## Why this lives inside SteamlessController

The device claim is exclusive. Anything that streams to the actuators needs the HID handle,
and SteamlessController already holds it in order to publish the virtual Xbox pad. A separate
process would have to fight it for the device — the same wall Steam hits. Inside, there is
nothing to contend with.

## The protocol

Established by [LiveHaptics](https://github.com/FamBoy32-dev/steam-controller-live-haptics)
(MIT) and [SteamHapticsPlayer](https://github.com/Pixel1011/SteamHapticsPlayer). This is an
independent implementation of the same documented protocol.

```
0x86  [0x86, op, side, param]    op 1 = disable, 2 = enable
                                 side 2 and 5 are the two actuators
                                 param 0 = 16-bit, 8 = 8 kHz u-law, 9 = 4 kHz
                                 200 ms settle between disable and enable

0x88  64-byte report             [1] = sample count
                                 u-law : left at 2 + i, right at 33 + i, 31 each
                                 16-bit: 15 frames, low byte first

rate  one report every 31,000,000 / rate microseconds
      4 kHz -> 7750 us     8 kHz -> 3875 us     wired 16-bit -> 1875 us
```

Device: VID `0x28DE`, PID `0x1302` wired / `0x1303` Bluetooth / `0x1304` puck, on the
**vendor-defined interface, usage page `0xFF00`**.

## Two findings that are not in any README

Both were measured on real hardware with the probe included here, and both are the difference
between this working and not.

### 1. Only one interface is wired to the actuators, and it is not the first one enumerated

The puck exposes **five** vendor interfaces. **Four accept a stream report.** Exactly one
moves the pad. Accepting a write proves nothing.

They are indistinguishable by HID capability — identical usage, usage page, and report
lengths:

```
[0] usage=0001 page=FF00 out=64 in=54 feat=64   mi_05
[1] usage=0001 page=FF00 out=64 in=54 feat=64   mi_02   <- the only one that works
[2] usage=0001 page=FF00 out=64 in=54 feat=64   mi_03
[3] usage=0001 page=FF00 out=64 in=54 feat=64   mi_04
```

The discriminator is the **USB interface number** in the device path, and the working one is
the **lowest**. Note the enumeration is not sorted — `mi_05` comes back first — so picking
"the first that answers" selects a dud that accepts every write and drives nothing.

### 2. A write costs 4000 us, so 8 kHz cannot be sustained on the puck

Each write waits for the 2.4 GHz radio slot. Measured, dead uniform:

| | 4 interfaces | 1 interface |
|---|---:|---:|
| average write | 16001 us | **4000 us** |
| share of a 3875 us period | 413% | 103% |
| reports delivered in 4 s | 251 / 1032 | **1000 / 1032** |

Writing to every interface that answers costs four serialised radio round-trips and delivers a
quarter of the stream — which is felt as heavy stutter. And even one interface exceeds an
8 kHz period, so **4 kHz is the default**: a 7750 us period that the same write fills to 52%.

Wired (`0x1302`) has no radio slot and uses 16-bit mode instead.

## Design notes

**Rumble is gated at the wire, not at the call sites.** `SendRumbleOutput` is the one function
that puts a motor command on the wire, so the suppression lives there. There were two callers —
the rumble thread and `SetRumble`'s own immediate write — and gating only the first left game
rumble fully audible. A stop (`0, 0`) still goes through, because silencing the motors on the
way into PCM mode must never be the thing that is blocked.

**The delay line carries the waveform, never the response.** Loopback taps the stream ahead of
the endpoint buffer and the DAC, so without a delay the pad leads the speakers. But gain and
the rumble gate are applied *after* the line, against the clock as it is now — so an impact
opens the gate the instant the game asks, while the sound it lets through is the time-shifted
one. Applying the gate before the line delays the response too, and makes every hit late.

**The EQ is a parallel filter bank, not a shelf.** Six second-order bandpass sections at 30,
60, 120, 240, 480 and 960 Hz, each summed at its own level, so a band at zero contributes
nothing. Q is 1.1 so bands overlap and a sweep is continuous. A muted band still runs its
filter, or re-enabling it clicks.

**Settings apply live.** Gain, EQ, delay and buffer are atomics the capture loop reads per
sample; the enable flag and the transport rate restart the stream. Nothing asks the user to
restart anything.

## Defaults, and why they look extreme

```
Strength   1250%      EQ  30 Hz 400%   60 Hz 400%   120 Hz 160%   240+ off
Delay        76 ms    Buffer  64       Rate  4 kHz  Gate  on
```

Tuned by hand on real hardware, not derived.

**Strength is high because loopback capture is post-volume.** Ordinary playback measures
around **0.035 peak** (about -29 dBFS), so a sane-looking gain of 200% produces roughly 6%
drive — below the level an actuator is felt at.

**Everything above 120 Hz is off** because nothing up there reads as texture on these
actuators. A flat EQ feels like noise.

**The gate is on** because it is the better experience in a game: the game supplies the
timing, the audio supplies the texture. It also means the pad is silent until something
happens, which can look broken — the settings window says so explicitly in that state.

## Files

| | |
|---|---|
| `AudioHaptics.{h,cpp}` | capture, DSP, transport, rumble gate |
| `HapticsWindow.{h,cpp}` | the settings window (plain Win32, DPI-aware, live) |
| `AudioHapticsProbe.cpp` | identifies the working interface and measures the transport |
| `integration.patch` | the changes to `SteamController.cpp`, `TrayApp`, `CMakeLists.txt` |

## Building

Drop the sources into a SteamlessController checkout, apply `integration.patch`, and build:

```
cmake --preset release
cmake --build build/release --config Release
```

`SteamController` needs `ole32 winmm` linked; `SteamlessController` needs `comctl32`.

## The probe

The engine runs inside an elevated, windowless process, so its failures are invisible. The
probe does the same work where the output can be read:

```
AudioHapticsProbe.exe             play a tone down each interface in turn
AudioHapticsProbe.exe --caps-only dump HID capabilities and paths
AudioHapticsProbe.exe 8000        compare transport rates
```

Untick the haptics box first so the bridge releases the device.

Every real bug in this feature was found by that probe and none by reasoning — including two
where the reasoning was confident and wrong. If something here misbehaves, measure before
theorising.

## Licence

These files are derivative works of
[SteamlessController](https://github.com/ddeverill/SteamlessController) and remain under its
MIT licence. Protocol credit to LiveHaptics (MIT) and SteamHapticsPlayer.
