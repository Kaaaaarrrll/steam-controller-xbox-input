#include "AudioHaptics.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t kValveVendor   = 0x28DE;
// Wired, Bluetooth and puck. Only the wired one carries 16-bit.
constexpr uint16_t kPidWired      = 0x1302;
constexpr uint16_t kPidBluetooth  = 0x1303;
constexpr uint16_t kPidPuck       = 0x1304;
// The actuators answer on the vendor-defined interface, not the mouse or
// keyboard collections the pad also exposes in lizard mode.
constexpr uint16_t kVendorUsagePage = 0xFF00;

constexpr uint8_t kReportPcmMode = 0x86;
constexpr uint8_t kReportStream  = 0x88;
constexpr uint8_t kPcmDisable    = 0x01;
constexpr uint8_t kPcmEnable     = 0x02;
// The two haptic actuator channels.
constexpr uint8_t kSideLeft      = 2;
constexpr uint8_t kSideRight     = 5;

// u-law carries 31 samples per channel in one 64-byte report; 16-bit carries 15.
constexpr size_t kMuLawSamplesPerReport = 31;
constexpr size_t kPcm16SamplesPerReport = 15;

// The firmware needs this long between a disable and the following enable.
constexpr DWORD kPcmSettleMs = 200;

// Retry interval when the audio device went away.
constexpr auto kRetryDelay = std::chrono::seconds(3);

/** Reads one DWORD tunable, clamped. */
DWORD ReadSetting(const wchar_t* name, DWORD fallback, DWORD maximum) {
    DWORD data = 0;
    DWORD cb   = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\SteamlessController", name,
                     RRF_RT_REG_DWORD, nullptr, &data, &cb) != ERROR_SUCCESS) {
        return fallback;
    }
    return data > maximum ? maximum : data;
}

/**
 * G.711 u-law encode. Textbook algorithm, kept here so the transport does not
 * depend on a codec library for thirteen lines of arithmetic.
 */
uint8_t LinearToMuLaw(int16_t sample) {
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    int value = sample;
    const int sign = (value >> 8) & 0x80;
    if (sign != 0) value = -value;
    if (value > kClip) value = kClip;
    value += kBias;
    int exponent = 7;
    for (int mask = 0x4000; (value & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
    const int mantissa = (value >> (exponent + 3)) & 0x0F;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

// Band centres. Chosen for what the actuators can reproduce rather than what a
// speaker can: above roughly 1 kHz a voice coil this size is barely felt, so
// spending bands up there would waste the interface.
const float kBandCentres[AudioHaptics::kBandCount] = {30.0f, 60.0f, 120.0f,
                                                      240.0f, 480.0f, 960.0f};

/**
 * One second-order bandpass section, constant peak gain.
 * A parallel bank of these is a graphic EQ: each band is filtered out on its
 * own and scaled, so setting a band to zero genuinely removes that range
 * instead of merely turning it down.
 */
struct Biquad {
    float b0{0}, b1{0}, b2{0}, a1{0}, a2{0};
    float x1{0}, x2{0}, y1{0}, y2{0};

    void SetBandpass(float centreHz, float q, float sampleRate) {
        if (centreHz >= sampleRate * 0.5f) { b0 = b1 = b2 = a1 = a2 = 0.0f; return; }
        const float w     = 6.2831853f * centreHz / sampleRate;
        const float alpha = std::sin(w) / (2.0f * q);
        const float cosw  = std::cos(w);
        const float a0    = 1.0f + alpha;
        b0 =  alpha / a0;
        b1 =  0.0f;
        b2 = -alpha / a0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    float Process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

/** One-pole coefficient for a cutoff in Hz at a sample rate. */
float OnePole(float cutoffHz, float sampleRate) {
    if (cutoffHz <= 0.0f || sampleRate <= 0.0f) return 1.0f;
    const float value = 1.0f - std::exp(-6.2831853f * cutoffHz / sampleRate);
    return std::clamp(value, 0.0f, 1.0f);
}

/** Soft-knee limiter, so a loud passage compresses instead of clipping. */
float SoftLimit(float value) {
    if (value > 1.0f)  return 1.0f;
    if (value < -1.0f) return -1.0f;
    return value - (value * value * value) / 3.0f;
}

} // namespace

AudioHaptics& AudioHaptics::Instance() {
    static AudioHaptics instance;
    return instance;
}

AudioHaptics::~AudioHaptics() {
    Stop();
}

bool AudioHaptics::Enabled() {
    return ReadSetting(L"HapticAudioMode", 0, 1) != 0;
}

float AudioHaptics::Gain() {
    return static_cast<float>(ReadSetting(L"HapticAudioGain", 854, 2000)) / 100.0f;
}

float AudioHaptics::BandGain(int band) {
    if (band < 0 || band >= kBandCount) return 0.0f;
    // Tuned by hand on real hardware, not derived: every band above 120 Hz
    // is off because nothing up there is felt as texture on these actuators,
    // and the bottom two sit at maximum because that is where the authority
    // is. A flat EQ feels like noise; this feels like impact.
    static const DWORD defaults[kBandCount] = {400, 160, 0, 0, 0, 0};
    wchar_t name[32];
    _snwprintf_s(name, _TRUNCATE, L"HapticEqBand%d", band);
    return static_cast<float>(ReadSetting(name, defaults[band], 400)) / 100.0f;
}

float AudioHaptics::DelayMs() {
    return static_cast<float>(ReadSetting(L"HapticAudioDelayMs", 68, 500));
}

bool AudioHaptics::RumbleGated() {
    return ReadSetting(L"HapticRumbleGate", 1, 1) != 0;
}

float AudioHaptics::RumbleReleaseMs() {
    const DWORD ms = ReadSetting(L"HapticRumbleRelease", 220, 3000);
    return ms < 20 ? 20.0f : static_cast<float>(ms);
}

void AudioHaptics::SetRumbleLevel(float level) {
    // Recorded unconditionally and consulted only when gating is on, so turning
    // the setting on takes effect on the next rumble rather than the next game.
    m_rumbleLevel.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
    m_rumbleAtMs.store(static_cast<long long>(GetTickCount64()), std::memory_order_relaxed);
}

uint32_t AudioHaptics::Rate() {
    // Default 4 kHz. A report carries 31 samples, so at 8 kHz one is due every
    // 3875 us - but a write to the puck takes 4000 us waiting for its radio
    // slot, so 8 kHz can never be sustained there and the shortfall is felt as
    // stutter. 4 kHz gives a 7750 us period, which the same write fills to 52%.
    // Wired is different and its 16-bit mode is chosen separately.
    return ReadSetting(L"HapticAudioRate", 4000, 8000) >= 6000 ? 8000u : 4000u;
}

size_t AudioHaptics::LatencyCap() {
    const size_t cap = ReadSetting(L"HapticAudioLatency", 64, 128);
    return cap < 4 ? 4 : cap;
}

void AudioHaptics::SendPcmMode(uint8_t op, uint8_t param) {
    for (HidDevice& hid : m_actuators) {
        if (!hid.IsOpen()) continue;
        for (uint8_t side : {kSideLeft, kSideRight}) {
            uint8_t report[64] = {};
            report[0] = kReportPcmMode;
            report[1] = op;
            report[2] = side;
            report[3] = param;
            hid.WriteOutputReport(report, sizeof(report));
        }
    }
}

/**
 * The USB interface number from a device path, or a large value when absent.
 * Paths look like ...&mi_02&col03..., and that number is fixed by the device
 * descriptor - unlike enumeration order, which is not sorted and differs
 * between boots.
 */
static int InterfaceNumber(const std::wstring& path) {
    const size_t at = path.find(L"&mi_");
    if (at == std::wstring::npos) return 0xFF;
    return static_cast<int>(wcstol(path.c_str() + at + 4, nullptr, 16));
}

bool AudioHaptics::OpenActuators() {
    CloseActuators();

    // Several vendor collections accept a stream report and only one is wired
    // to the actuators, so acceptance alone is not a test. Measured on this
    // hardware: four accept, and the one that moves the pad is the lowest
    // interface number. Writing to all four instead costs 16 ms per report
    // against a 3875 us period, because each write waits for its own radio
    // slot - which is what made the stream stutter.
    std::wstring bestPath;
    int bestInterface = 0x100;
    uint16_t bestPid = 0;

    for (uint16_t pid : {kPidWired, kPidBluetooth, kPidPuck}) {
        for (const std::wstring& path :
             HidDevice::Enumerate(kValveVendor, pid, kVendorUsagePage)) {
            HidDevice hid;
            if (!hid.Open(path)) continue;
            uint8_t probe[64] = {};
            probe[0] = kReportStream;
            const bool accepted = hid.WriteOutputReport(probe, sizeof(probe));
            hid.Close();
            if (!accepted) continue;
            const int number = InterfaceNumber(path);
            if (number < bestInterface) {
                bestInterface = number;
                bestPath = path;
                bestPid = pid;
            }
        }
    }

    if (bestPath.empty()) return false;

    HidDevice chosen;
    if (!chosen.Open(bestPath)) return false;
    if (bestPid == kPidWired) m_sixteenBit = true;
    if (bestPid == kPidBluetooth) m_rate = 4000;
    m_actuators.push_back(std::move(chosen));
    return true;
}

void AudioHaptics::CloseActuators() {
    for (HidDevice& hid : m_actuators) hid.Close();
    m_actuators.clear();
}

bool AudioHaptics::Start() {
    if (!Enabled()) return false;
    if (m_running.exchange(true)) return true;

    m_rate = Rate();
    m_gain.store(Gain());
    for (int b = 0; b < kBandCount; ++b) m_bands[b].store(BandGain(b));
    m_rumbleGated.store(RumbleGated());
    m_delayMs.store(DelayMs());
    m_latency.store(LatencyCap());
    if (!OpenActuators()) {
        printf("AudioHaptics: no controller actuator interface answered.\n");
        m_running.store(false);
        return false;
    }

    // Disable first: the firmware refuses an enable on a channel already in PCM
    // mode from a previous session that did not shut down cleanly.
    const uint8_t param = m_sixteenBit ? 0 : static_cast<uint8_t>(m_rate == 8000 ? 8 : 9);
    SendPcmMode(kPcmDisable, 0);
    Sleep(10);
    SendPcmMode(kPcmEnable, param);
    Sleep(kPcmSettleMs);

    m_interfaceCount.store(static_cast<int>(m_actuators.size()));
    printf("AudioHaptics: PCM enabled on %u interface(s), %s @ %u Hz.\n",
           static_cast<unsigned>(m_actuators.size()), m_sixteenBit ? "16-bit" : "u-law", m_rate);

    m_capture = std::thread(&AudioHaptics::CaptureLoop, this);
    m_sender  = std::thread(&AudioHaptics::SendLoop, this);
    return true;
}

void AudioHaptics::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_capture.joinable()) m_capture.join();
    if (m_sender.joinable())  m_sender.join();
    m_active.store(false, std::memory_order_relaxed);
    m_interfaceCount.store(0);
    // Hand the actuators back, or they stay in PCM mode and rumble stays dead.
    SendPcmMode(kPcmDisable, 0);
    Sleep(10);
    CloseActuators();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
}

void AudioHaptics::SendLoop() {
    // One report is exactly its sample count at the transport rate: 31 samples
    // at 8 kHz is 3875 us. Holding that clock is the whole job - the actuators
    // reproduce a waveform, so a late report is an audible gap, not a delay.
    const auto period = std::chrono::microseconds(
        m_sixteenBit ? 1875LL : static_cast<long long>(31'000'000ULL / m_rate));

    // Windows' default timer resolution is about 15.6 ms. sleep_until on a
    // 3.875 ms period therefore overshoots by whole timer ticks and the stream
    // arrives in clumps, which is felt as stutter rather than as latency. Ask
    // for 1 ms and spin the last stretch.
    timeBeginPeriod(1);

    // Prime before starting the clock. WASAPI hands over roughly 10 ms of audio
    // at a time, so reports are produced in bursts of two or three; starting on
    // the first one guarantees an underrun a few milliseconds later.
    const size_t prime = 4;
    while (m_running.load()) {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            queued = m_queue.size();
        }
        if (queued >= prime) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto next = std::chrono::steady_clock::now();
    while (m_running.load()) {
        Report report{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_queue.empty()) {
                report = m_queue.front();
                m_queue.erase(m_queue.begin());
                have = true;
            }
        }

        if (have) {
            for (HidDevice& hid : m_actuators) {
                if (hid.IsOpen()) hid.WriteOutputReport(report.data(), report.size());
            }
        }
        // The clock advances whether or not a report was ready. An underrun is
        // one missing packet, not a reason to restart the cadence - resetting it
        // on every gap is what turns a brief starve into permanent judder.
        next += period;

        const auto now = std::chrono::steady_clock::now();
        if (now > next + period * 4) {
            // Genuinely far behind: resynchronise rather than burst-send to
            // catch up, which would flood the firmware.
            next = now;
            continue;
        }
        // Sleep the coarse part, spin the last millisecond. The spin is bounded
        // by the period, so it costs well under 1% of a core at this rate.
        const auto coarse = next - std::chrono::milliseconds(1);
        if (std::chrono::steady_clock::now() < coarse) {
            std::this_thread::sleep_until(coarse);
        }
        while (std::chrono::steady_clock::now() < next) {
            YieldProcessor();
        }
    }

    timeEndPeriod(1);
}

void AudioHaptics::CaptureLoop() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(comInit);
    while (m_running.load()) {
        RunCaptureSession();
        if (!m_running.load()) break;
        m_active.store(false, std::memory_order_relaxed);
        std::this_thread::sleep_for(kRetryDelay);
    }
    if (ownsCom) CoUninitialize();
}

void AudioHaptics::RunCaptureSession() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           endpoint   = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    WAVEFORMATEX*        format     = nullptr;

    const auto release = [&]() {
        if (capture)    capture->Release();
        if (client)     client->Release();
        if (endpoint)   endpoint->Release();
        if (enumerator) enumerator->Release();
        if (format)     CoTaskMemFree(format);
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))
        || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint))
        || FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(&client)))
        || FAILED(client->GetMixFormat(&format))) {
        release();
        return;
    }

    const bool extensible = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    const bool isFloat =
        format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
        || (extensible && format->cbSize >= 22
            && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat.Data1
                   == WAVE_FORMAT_IEEE_FLOAT);
    const bool isPcm16 = !isFloat && format->wBitsPerSample == 16;
    if (!isFloat && !isPcm16) {
        release();
        return;
    }

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  100'000, 0, format, nullptr))
        || FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                     reinterpret_cast<void**>(&capture)))
        || FAILED(client->Start())) {
        release();
        return;
    }

    const float    sampleRate = static_cast<float>(format->nSamplesPerSec);
    const uint32_t channels   = format->nChannels == 0 ? 1u : format->nChannels;
    // Decimation ratio to the transport rate. Anything above the transport's
    // Nyquist has to go before decimating, or it aliases down into the band we
    // can actually feel, which reads as grit rather than bass.
    const float    ratio      = sampleRate / static_cast<float>(m_rate);
    const float    antiAlias  = OnePole(static_cast<float>(m_rate) * 0.45f, sampleRate);
    // One bank per channel. Q of 1.1 gives bands that overlap slightly, so a
    // sweep across them is continuous rather than stepped.
    Biquad leftBank[kBandCount];
    Biquad rightBank[kBandCount];
    for (int b = 0; b < kBandCount; ++b) {
        leftBank[b].SetBandpass(kBandCentres[b], 1.1f, sampleRate);
        rightBank[b].SetBandpass(kBandCentres[b], 1.1f, sampleRate);
    }
    const float releaseMs = RumbleReleaseMs();

    // Delay line on the decimated stream. One second of headroom at 8 kHz is
    // ample for the 500 ms ceiling and costs 32 KB.
    const size_t delayCapacity = 8000;
    std::vector<float> delayLeft(delayCapacity, 0.0f);
    std::vector<float> delayRight(delayCapacity, 0.0f);
    size_t delayWrite = 0;
    // Read per sample below, not captured here, so a slider moves the feel now.
    const size_t   perReport  = m_sixteenBit ? kPcm16SamplesPerReport : kMuLawSamplesPerReport;

    float leftLp = 0.0f, rightLp = 0.0f;
    float accumulator = 0.0f;
    Report report{};
    report[0] = kReportStream;
    size_t filled = 0;

    m_active.store(true, std::memory_order_relaxed);

    while (m_running.load()) {
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        if (packet == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;

        for (UINT32 i = 0; i < frames; ++i) {
            float left = 0.0f, right = 0.0f;
            if (!silent) {
                const size_t base = static_cast<size_t>(i) * channels;
                if (isFloat) {
                    const float* samples = reinterpret_cast<const float*>(data);
                    left  = samples[base];
                    right = channels > 1 ? samples[base + 1] : samples[base];
                } else {
                    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                    left  = static_cast<float>(samples[base]) / 32768.0f;
                    right = static_cast<float>(channels > 1 ? samples[base + 1] : samples[base])
                            / 32768.0f;
                }
            }

            // Graphic EQ: filter each band out on its own and sum them back
            // with its own level. A band set to zero contributes nothing at all,
            // which a shelf could never do.
            float shapedLeft = 0.0f, shapedRight = 0.0f;
            for (int b = 0; b < kBandCount; ++b) {
                const float level = m_bands[b].load(std::memory_order_relaxed);
                if (level <= 0.0f) {
                    // Still run the filter: its state has to stay continuous or
                    // re-enabling the band clicks.
                    leftBank[b].Process(left);
                    rightBank[b].Process(right);
                    continue;
                }
                shapedLeft  += leftBank[b].Process(left) * level;
                shapedRight += rightBank[b].Process(right) * level;
            }
            left  = shapedLeft;
            right = shapedRight;

            leftLp  += antiAlias * (left - leftLp);
            rightLp += antiAlias * (right - rightLp);

            accumulator += 1.0f;
            if (accumulator < ratio) continue;
            accumulator -= ratio;

            // Delay the AUDIO, never the response.
            //
            // The delay line carries the raw band-shaped waveform and nothing
            // else. Gain and the rumble gate are applied AFTER it, against the
            // clock as it is now - so an impact opens the gate the instant the
            // game asks, while the sound it lets through is the one that lines
            // up with the speakers. Applying the gate before the line, as this
            // did at first, delayed the response along with the audio and made
            // every hit late by whatever the slider said.
            delayLeft[delayWrite]  = leftLp;
            delayRight[delayWrite] = rightLp;
            size_t offset = static_cast<size_t>(m_delayMs.load(std::memory_order_relaxed)
                                                * static_cast<float>(m_rate) / 1000.0f);
            if (offset >= delayCapacity) offset = delayCapacity - 1;
            const size_t readAt = (delayWrite + delayCapacity - offset) % delayCapacity;
            const float delayedLeft  = delayLeft[readAt];
            const float delayedRight = delayRight[readAt];
            delayWrite = (delayWrite + 1) % delayCapacity;

            float gain = m_gain.load(std::memory_order_relaxed);
            if (m_rumbleGated.load(std::memory_order_relaxed)) {
                // The game says WHEN, the audio says WHAT. Outside a rumble
                // request the pad is silent, so ambient music no longer buzzes
                // continuously; during one, the texture comes from the sound
                // rather than from a motor byte.
                const auto now = static_cast<long long>(GetTickCount64());
                const long long since = now - m_rumbleAtMs.load(std::memory_order_relaxed);
                const float level = m_rumbleLevel.load(std::memory_order_relaxed);
                float envelope = 0.0f;
                if (level > 0.0f && since >= 0) {
                    envelope = 1.0f - static_cast<float>(since) / releaseMs;
                    if (envelope < 0.0f) envelope = 0.0f;
                    envelope *= level;
                }
                gain *= envelope;
            }
            const float l = SoftLimit(delayedLeft * gain);
            const float r = SoftLimit(delayedRight * gain);
            const auto li = static_cast<int16_t>(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
            const auto ri = static_cast<int16_t>(std::clamp(r, -1.0f, 1.0f) * 32767.0f);

            if (m_sixteenBit) {
                report[2 + filled * 2]  = static_cast<uint8_t>(li & 0xFF);
                report[3 + filled * 2]  = static_cast<uint8_t>((li >> 8) & 0xFF);
                report[33 + filled * 2] = static_cast<uint8_t>(ri & 0xFF);
                report[34 + filled * 2] = static_cast<uint8_t>((ri >> 8) & 0xFF);
            } else {
                report[2 + filled]  = LinearToMuLaw(li);
                report[33 + filled] = LinearToMuLaw(ri);
            }
            ++filled;

            if (filled >= perReport) {
                report[1] = static_cast<uint8_t>(m_sixteenBit ? 30 : perReport);
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    // Drop the oldest rather than grow: late haptics are worse
                    // than missing ones, and an unbounded queue becomes delay.
                    if (m_queue.size() >= m_latency.load(std::memory_order_relaxed))
                        m_queue.erase(m_queue.begin());
                    m_queue.push_back(report);
                }
                report = Report{};
                report[0] = kReportStream;
                filled = 0;
            }
        }
        capture->ReleaseBuffer(frames);
    }

    m_active.store(false, std::memory_order_relaxed);
    client->Stop();
    release();
}

void AudioHaptics::Reload() {
    // Gain, bass and latency are live: the capture loop reads them per sample,
    // so a slider is felt before it is released.
    m_gain.store(Gain(), std::memory_order_relaxed);
    for (int b = 0; b < kBandCount; ++b)
        m_bands[b].store(BandGain(b), std::memory_order_relaxed);
    m_rumbleGated.store(RumbleGated(), std::memory_order_relaxed);
    m_delayMs.store(DelayMs(), std::memory_order_relaxed);
    m_latency.store(LatencyCap(), std::memory_order_relaxed);

    // The enable flag and the transport rate are structural - they decide
    // whether a stream exists at all and at what clock - so they restart it.
    const bool wanted = Enabled();
    const bool running = m_running.load();
    if (wanted && running && Rate() != m_rate) {
        Stop();
        Start();
        return;
    }
    if (wanted && !running) Start();
    if (!wanted && running) Stop();
}
