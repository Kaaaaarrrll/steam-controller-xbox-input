#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "../hid/HidDevice.h"

// Streams system audio to the Steam Controller's haptic actuators as PCM, so
// the actuators reproduce the sound rather than buzzing in time with it.
//
// Why not rumble: XInput carries two motor bytes and a game sends them on
// discrete events. Driving those bytes from an audio envelope is still only an
// intensity, and on voice-coil actuators it reads as a weak hum. The firmware
// has a real path - a PCM mode on the two actuator channels - and that is what
// the "music through a Steam Controller" demos use.
//
// Protocol credit: the report layout and timing below were established by
// LiveHaptics (FamBoy32-dev/steam-controller-live-haptics, MIT) and
// SteamHapticsPlayer (Pixel1011). This is an independent implementation of the
// same documented protocol.
//
//   0x86  [0x86, op, side, param]   op 1 = disable, 2 = enable
//                                   side 2 and 5 are the two actuators
//                                   param 0 = 16-bit, 8 = 8 kHz u-law, 9 = 4 kHz
//   0x88  64-byte report: [1] = sample count,
//                         left channel at 2 + i, right at 33 + i (u-law)
//   rate  one report every 31,000,000 / rate microseconds (3875 us at 8 kHz)
//
// While this is streaming, normal rumble is suppressed: both drive the same
// actuators, and a motor command on top of a waveform produces neither.
//
// Scope: runs only while this process owns the controller. Inside a Steam game
// Steam Input holds the device, which is the same exclusivity that shapes the
// rest of this application.
class AudioHaptics {
public:
    static AudioHaptics& Instance();

    // Opens its own handles to the vendor interfaces, enables PCM mode and
    // starts capture. False when disabled by settings or no controller answered.
    bool Start();
    void Stop();

    // True while PCM is streaming. Rumble is suppressed for exactly this long.
    bool Active() const { return m_active.load(std::memory_order_relaxed); }

    // Re-reads every tunable from the registry and applies it live. Gain, bass
    // and latency take effect on the next report; a change to the enable flag or
    // the transport rate restarts the stream. The settings window calls this, so
    // nothing there ever asks the user to restart the application.
    void Reload();

    // How many actuator interfaces accepted the stream probe. Zero while
    // stopped, and the number the settings window reports.
    int InterfaceCount() const { return m_interfaceCount.load(std::memory_order_relaxed); }

    // --- settings, read once at startup, HKCU\Software\SteamlessController ---

    // HapticAudioMode: 0 = off (default), 1 = stream audio to the actuators.
    static bool Enabled();
    // HapticAudioGain: loudness in percent. Default 200.
    static float Gain();
    // HapticEqBandN: per-band level in percent, 0 removes the band entirely.
    // Six bands centred at 30, 60, 120, 240, 480 and 960 Hz - the range the
    // actuators can actually reproduce, not the range a speaker can.
    static constexpr int kBandCount = 6;
    static float BandGain(int band);

    // HapticRumbleGate: 0 = always stream (default), 1 = only while the game is
    // asking for rumble. On, audio supplies the texture and the game supplies
    // the timing, so ambient music does not buzz the pad all the time.
    static bool RumbleGated();
    // HapticRumbleRelease: milliseconds the gate takes to fall. Default 220.
    static float RumbleReleaseMs();

    // HapticAudioDelayMs: hold the haptics back to line up with the speakers.
    // Loopback taps the stream before the endpoint buffer and the DAC, so the
    // pad is genuinely ahead of what you hear. Default 0, up to 500.
    static float DelayMs();

    // Called from the rumble path with the requested motor level, 0..1. Ignored
    // unless gating is on.
    void SetRumbleLevel(float level);
    // HapticAudioRate: 8000 or 4000. Default 8000.
    static uint32_t Rate();
    // HapticAudioLatency: queued reports before the oldest is dropped. Default 16.
    static size_t LatencyCap();

private:
    AudioHaptics() = default;
    ~AudioHaptics();
    AudioHaptics(const AudioHaptics&)            = delete;
    AudioHaptics& operator=(const AudioHaptics&) = delete;

    // One 64-byte 0x88 report.
    using Report = std::array<uint8_t, 64>;

    bool OpenActuators();
    void CloseActuators();
    // op 1 disables, op 2 enables; sent to both actuator channels.
    void SendPcmMode(uint8_t op, uint8_t param);

    void CaptureLoop();   // WASAPI loopback -> DSP -> decimate -> u-law -> queue
    void SendLoop();      // drains the queue onto the wire at the sample clock
    void RunCaptureSession();

    std::vector<HidDevice> m_actuators;

    std::thread       m_capture;
    std::thread       m_sender;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    // The wired controller (pid 0x1302) carries 16-bit instead of u-law.
    bool              m_sixteenBit{false};
    uint32_t          m_rate{8000};

    // Read live by the capture loop so the settings window takes effect at once.
    std::atomic<float>  m_gain{2.0f};
    std::atomic<float>  m_delayMs{0.0f};
    std::atomic<float>  m_bands[kBandCount]{};
    std::atomic<bool>   m_rumbleGated{false};
    // Latest rumble request and when it arrived, so the gate can fall smoothly
    // rather than cutting the waveform off mid-cycle.
    std::atomic<float>  m_rumbleLevel{0.0f};
    std::atomic<long long> m_rumbleAtMs{0};
    std::atomic<size_t> m_latency{16};
    std::atomic<int>    m_interfaceCount{0};

    std::mutex          m_queueMutex;
    std::vector<Report> m_queue;
};
