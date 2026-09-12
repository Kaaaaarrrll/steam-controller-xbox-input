// Identifies which controller interface actually drives the haptic actuators,
// and prints what distinguishes it.
//
// Accepting a write is not proof of being wired to anything: four of the five
// vendor collections accept a stream report, and only one moves the pad. The
// engine used to take the first that answered, which is a guess - and on this
// hardware the wrong one. This dumps each interface's HID capabilities so the
// right one can be selected by a stable property rather than by ordinal.
//
// Build: cmake --build build/release --config Release --target AudioHapticsProbe
// Run:   build\release\Release\AudioHapticsProbe.exe [rate] [--caps-only]

#include <windows.h>

#include <hidsdi.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../hid/HidDevice.h"

namespace {

constexpr uint16_t kVendor = 0x28DE;
constexpr uint16_t kPids[] = {0x1302, 0x1303, 0x1304};
constexpr uint16_t kVendorUsagePage = 0xFF00;
constexpr uint8_t  kPcmMode = 0x86;
constexpr uint8_t  kStream  = 0x88;

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

void SetPcm(HidDevice& device, bool enable, int rate) {
    for (uint8_t side : {uint8_t{2}, uint8_t{5}}) {
        uint8_t report[64] = {};
        report[0] = kPcmMode;
        report[1] = enable ? 0x02 : 0x01;
        report[2] = side;
        report[3] = enable ? static_cast<uint8_t>(rate == 8000 ? 8 : 9) : 0;
        device.WriteOutputReport(report, sizeof(report));
    }
}

/** Reads the HID capabilities of one interface path, for a discriminator. */
void PrintCaps(const std::wstring& path, int index) {
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("    [%d] could not open for caps\n", index);
        return;
    }
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (HidD_GetPreparsedData(handle, &preparsed)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
            std::printf("    [%d] usage=%04X page=%04X  out=%u in=%u feat=%u\n", index,
                        caps.Usage, caps.UsagePage, caps.OutputReportByteLength,
                        caps.InputReportByteLength, caps.FeatureReportByteLength);
        }
        HidD_FreePreparsedData(preparsed);
    }
    // The interface number is in the path as &mi_NN, and is stable across boots
    // in a way an enumeration index is not.
    const size_t at = path.find(L"&mi_");
    if (at != std::wstring::npos) {
        std::wprintf(L"         path mi=%.5s  col=%.10s\n", path.c_str() + at + 4,
                     path.find(L"&col") != std::wstring::npos
                         ? path.c_str() + path.find(L"&col") + 4
                         : L"-");
    }
    CloseHandle(handle);
}

} // namespace

int main(int argc, char** argv) {
    int rate = 4000;
    bool capsOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--caps-only") == 0) capsOnly = true;
        else rate = std::atoi(argv[i]);
    }

    std::printf("\nActuator interface identification  (rate %d Hz)\n", rate);
    std::printf("----------------------------------------------\n");

    std::vector<HidDevice> answered;
    std::vector<std::wstring> answeredPaths;
    int found = 0;

    for (uint16_t pid : kPids) {
        const std::vector<std::wstring> paths =
            HidDevice::Enumerate(kVendor, pid, kVendorUsagePage);
        if (!paths.empty()) {
            std::printf("  pid %04X : %u vendor interface(s)\n", pid,
                        static_cast<unsigned>(paths.size()));
        }
        found += static_cast<int>(paths.size());
        for (const std::wstring& path : paths) {
            HidDevice device;
            if (!device.Open(path)) continue;
            uint8_t probe[64] = {};
            probe[0] = kStream;
            if (!device.WriteOutputReport(probe, sizeof(probe))) {
                device.Close();
                continue;
            }
            answered.push_back(std::move(device));
            answeredPaths.push_back(path);
        }
    }

    std::printf("\n  found %d, accepted %u\n\n", found,
                static_cast<unsigned>(answered.size()));
    std::printf("  capabilities of each ACCEPTED interface:\n");
    for (size_t i = 0; i < answeredPaths.size(); ++i) {
        PrintCaps(answeredPaths[i], static_cast<int>(i));
    }

    if (answered.empty()) {
        std::printf("\n  Nothing accepted. Untick the haptics box so the bridge releases\n");
        std::printf("  the device, then run this again.\n\n");
        return 1;
    }
    if (capsOnly) {
        for (HidDevice& device : answered) device.Close();
        std::printf("\n");
        return 0;
    }

    std::printf("\n  Playing a 2.5 s tone down EACH interface in turn.\n");
    std::printf("  Note which index you FEEL.\n\n");

    for (size_t index = 0; index < answered.size(); ++index) {
        std::printf("  >>> interface %u of %u ... ", static_cast<unsigned>(index),
                    static_cast<unsigned>(answered.size()));
        std::fflush(stdout);

        HidDevice& device = answered[index];
        SetPcm(device, false, rate);
        Sleep(10);
        SetPcm(device, true, rate);
        Sleep(200);

        timeBeginPeriod(1);
        const auto period = std::chrono::microseconds(31000000 / rate);
        auto next = std::chrono::steady_clock::now();
        const auto until = next + std::chrono::milliseconds(2500);
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979 * 60.0 / rate;
        int sent = 0;

        while (std::chrono::steady_clock::now() < until) {
            uint8_t report[64] = {};
            report[0] = kStream;
            report[1] = 31;
            for (int i = 0; i < 31; ++i) {
                const auto value = static_cast<int16_t>(std::sin(phase) * 26000.0);
                phase += step;
                report[2 + i]  = LinearToMuLaw(value);
                report[33 + i] = LinearToMuLaw(value);
            }
            device.WriteOutputReport(report, sizeof(report));
            ++sent;

            next += period;
            const auto now = std::chrono::steady_clock::now();
            if (now > next + period * 4) { next = now; continue; }
            const auto coarse = next - std::chrono::milliseconds(1);
            if (std::chrono::steady_clock::now() < coarse) std::this_thread::sleep_until(coarse);
            while (std::chrono::steady_clock::now() < next) YieldProcessor();
        }
        timeEndPeriod(1);

        SetPcm(device, false, rate);
        std::printf("%d reports\n", sent);
        Sleep(800);
    }

    for (HidDevice& device : answered) device.Close();
    std::printf("\n  Which index did you feel? That is the one the engine should use.\n\n");
    return 0;
}
