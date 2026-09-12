#include "HapticsWindow.h"

#include <commctrl.h>

#include <cstdio>
#include <string>

#include "../steam/AudioHaptics.h"

namespace {

constexpr wchar_t kClassName[] = L"SteamlessHapticsWindow";
constexpr wchar_t kRegistryKey[] = L"Software\\SteamlessController";

// Control ids.
constexpr int kIdEnable   = 100;
constexpr int kIdGain     = 101;
constexpr int kIdBass     = 102;
constexpr int kIdLatency  = 103;
constexpr int kIdRate8000 = 104;
constexpr int kIdRate4000 = 105;
constexpr int kIdGainText = 200;
constexpr int kIdBassText = 201;
constexpr int kIdLatText  = 202;
constexpr int kIdStatus   = 203;
constexpr int kIdReset    = 300;
constexpr int kIdGate     = 301;
constexpr int kIdDelay    = 302;
constexpr int kIdDelayText = 303;
// Six consecutive ids for the EQ band sliders, and six more for their labels.
constexpr int kIdBand0    = 400;
constexpr int kIdBandVal0 = 410;
constexpr int kBandCount  = 6;
constexpr int kBandResetDefaults[kBandCount] = {400, 160, 0, 0, 0, 0};
const wchar_t* const kBandNames[kBandCount] = {L"30", L"60", L"120", L"240", L"480", L"960"};

constexpr UINT_PTR kStatusTimer = 1;

HWND g_window = nullptr;

void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD data = 0;
    DWORD cb   = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, name, RRF_RT_REG_DWORD, nullptr, &data, &cb)
        != ERROR_SUCCESS) {
        return fallback;
    }
    return data;
}

int S(int value);
HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int width, int id = -1) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, S(x), S(y), S(width), S(20),
                           parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

HWND MakeSlider(HWND parent, int id, int x, int y, int width, int minimum, int maximum,
                int value) {
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, S(x), S(y),
                                  S(width), S(30), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  nullptr, nullptr);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    return slider;
}

/** Applies a nicer font than the 1995 default to every child. */
BOOL CALLBACK ApplyFont(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

} // namespace

namespace {

// Every coordinate below is in 96-dpi units and scaled through this. The first
// version hardcoded pixels, which clipped every label on a scaled display -
// the control boxes stayed put while the font grew.
int g_dpi = 96;
int S(int value) { return MulDiv(value, g_dpi, 96); }

HFONT g_font = nullptr;

/** The shell font at the window's dpi, rather than the 1995 bitmap default. */
HFONT MakeFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                   static_cast<UINT>(g_dpi))) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace

void HapticsWindow::BuildControls(HWND hwnd) {
    g_dpi = static_cast<int>(GetDpiForWindow(hwnd));
    if (g_dpi <= 0) g_dpi = 96;
    g_font = MakeFont();

    // Labels get the full width and their own row. Putting a value label beside
    // a title label is what produced the overlapping text: at 150% the title
    // ran straight through the value.
    const int margin = 18;
    const int width  = 430;
    int y = 16;

    CreateWindowExW(0, L"BUTTON", L"Replace rumble with audio haptics",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(margin), S(y), S(width), S(24), hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEnable)), nullptr, nullptr);
    SendMessageW(GetDlgItem(hwnd, kIdEnable), BM_SETCHECK,
                 ReadSetting(L"HapticAudioMode", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;

    MakeLabel(hwnd, L"System audio is streamed to the trackpad actuators.", margin, y, width);
    y += 20;
    MakeLabel(hwnd, L"Normal rumble is disabled while this is on.", margin, y, width);
    y += 28;

    MakeLabel(hwnd, L"Strength", margin, y, 200);
    MakeLabel(hwnd, L"", margin + 260, y, 170, kIdGainText);
    y += 22;
    MakeSlider(hwnd, kIdGain, margin, y, width, 25, 2000,
               static_cast<int>(ReadSetting(L"HapticAudioGain", 854)));
    y += 40;

    // A real EQ: one vertical fader per band, zero at the bottom, exactly like
    // a music app. A band at zero is removed from the signal entirely.
    MakeLabel(hwnd, L"Equaliser (Hz)", margin, y, 240);
    y += 24;
    const int columnWidth = width / kBandCount;
    const int faderHeight = 130;
    const DWORD bandDefaults[kBandCount] = {400, 160, 0, 0, 0, 0};
    for (int b = 0; b < kBandCount; ++b) {
        wchar_t key[32];
        _snwprintf_s(key, _TRUNCATE, L"HapticEqBand%d", b);
        const int x = margin + columnWidth * b + columnWidth / 2 - 14;
        HWND fader = CreateWindowExW(
            0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_NOTICKS | TBS_DOWNISLEFT, S(x), S(y), S(28),
            S(faderHeight), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBand0 + b)), nullptr, nullptr);
        SendMessageW(fader, TBM_SETRANGE, TRUE, MAKELPARAM(0, 400));
        SendMessageW(fader, TBM_SETPOS, TRUE,
                     static_cast<int>(ReadSetting(key, bandDefaults[b])));
        MakeLabel(hwnd, kBandNames[b], x - 6, y + faderHeight + 2, 44);
        MakeLabel(hwnd, L"", x - 6, y + faderHeight + 22, 44, kIdBandVal0 + b);
    }
    y += faderHeight + 48;

    // Loopback taps the stream ahead of the endpoint buffer and the DAC, so
    // without this the pad leads the speakers. This is a sync control, not a
    // latency control - the Buffer slider below is the latency one.
    MakeLabel(hwnd, L"Delay (sync to speakers)", margin, y, 240);
    MakeLabel(hwnd, L"", margin + 260, y, 170, kIdDelayText);
    y += 22;
    MakeSlider(hwnd, kIdDelay, margin, y, width, 0, 500,
               static_cast<int>(ReadSetting(L"HapticAudioDelayMs", 68)));
    y += 40;

    MakeLabel(hwnd, L"Buffer (higher is smoother)", margin, y, 240);
    MakeLabel(hwnd, L"", margin + 260, y, 170, kIdLatText);
    y += 22;
    MakeSlider(hwnd, kIdLatency, margin, y, width, 4, 64,
               static_cast<int>(ReadSetting(L"HapticAudioLatency", 64)));
    y += 42;

    const DWORD rate = ReadSetting(L"HapticAudioRate", 4000);
    MakeLabel(hwnd, L"Transport rate", margin, y + 4, 140);
    CreateWindowExW(0, L"BUTTON", L"4 kHz (wireless)",
                    WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, S(margin + 150), S(y),
                    S(150), S(24), hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRate4000)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"8 kHz", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                    S(margin + 310), S(y), S(120), S(24), hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRate8000)), nullptr, nullptr);
    SendMessageW(GetDlgItem(hwnd, rate >= 6000 ? kIdRate8000 : kIdRate4000), BM_SETCHECK,
                 BST_CHECKED, 0);
    y += 34;

    CreateWindowExW(0, L"BUTTON", L"Only while the game asks for rumble",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(margin), S(y), S(width), S(24),
                    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdGate)), nullptr,
                    nullptr);
    SendMessageW(GetDlgItem(hwnd, kIdGate), BM_SETCHECK,
                 ReadSetting(L"HapticRumbleGate", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    MakeLabel(hwnd, L"The game supplies the timing, the audio supplies the texture.", margin, y,
              width);
    y += 30;

    CreateWindowExW(0, L"BUTTON", L"Reset to defaults", WS_CHILD | WS_VISIBLE, S(margin), S(y),
                    S(190), S(30), hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReset)), nullptr, nullptr);
    y += 40;

    // Two rows: the status sentence is long and must never be clipped, because
    // it is the only thing that says whether the actuators accepted the stream.
    MakeLabel(hwnd, L"", margin, y, width, kIdStatus);
    y += 40;

    EnumChildWindows(hwnd, ApplyFont, reinterpret_cast<LPARAM>(g_font));

    // Size the window to the content rather than to a guess.
    RECT wanted{0, 0, S(width + margin * 2), S(y)};
    AdjustWindowRectExForDpi(&wanted, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             FALSE, 0, static_cast<UINT>(g_dpi));
    SetWindowPos(hwnd, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    Refresh(hwnd);
}

void HapticsWindow::Commit(HWND hwnd) {
    WriteSetting(L"HapticAudioMode",
                 SendMessageW(GetDlgItem(hwnd, kIdEnable), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1
                                                                                             : 0);
    WriteSetting(L"HapticAudioGain",
                 static_cast<DWORD>(SendMessageW(GetDlgItem(hwnd, kIdGain), TBM_GETPOS, 0, 0)));
    for (int b = 0; b < kBandCount; ++b) {
        wchar_t key[32];
        _snwprintf_s(key, _TRUNCATE, L"HapticEqBand%d", b);
        WriteSetting(key, static_cast<DWORD>(
                              SendMessageW(GetDlgItem(hwnd, kIdBand0 + b), TBM_GETPOS, 0, 0)));
    }
    WriteSetting(L"HapticAudioDelayMs",
                 static_cast<DWORD>(SendMessageW(GetDlgItem(hwnd, kIdDelay), TBM_GETPOS, 0, 0)));
    WriteSetting(L"HapticRumbleGate",
                 SendMessageW(GetDlgItem(hwnd, kIdGate), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1
                                                                                           : 0);
    WriteSetting(L"HapticAudioLatency",
                 static_cast<DWORD>(SendMessageW(GetDlgItem(hwnd, kIdLatency), TBM_GETPOS, 0, 0)));
    WriteSetting(L"HapticAudioRate",
                 SendMessageW(GetDlgItem(hwnd, kIdRate4000), BM_GETCHECK, 0, 0) == BST_CHECKED
                     ? 4000
                     : 8000);
    // The engine re-reads and applies without a restart, which is the whole
    // point of this window existing rather than a list of registry keys.
    AudioHaptics::Instance().Reload();
    Refresh(hwnd);
}

void HapticsWindow::Refresh(HWND hwnd) {
    wchar_t text[128];

    const LRESULT gain = SendMessageW(GetDlgItem(hwnd, kIdGain), TBM_GETPOS, 0, 0);
    _snwprintf_s(text, _TRUNCATE, L"%ld%%", static_cast<long>(gain));
    SetWindowTextW(GetDlgItem(hwnd, kIdGainText), text);

    for (int b = 0; b < kBandCount; ++b) {
        const LRESULT level = SendMessageW(GetDlgItem(hwnd, kIdBand0 + b), TBM_GETPOS, 0, 0);
        if (level == 0) wcscpy_s(text, L"off");
        else _snwprintf_s(text, _TRUNCATE, L"%ld%%", static_cast<long>(level));
        SetWindowTextW(GetDlgItem(hwnd, kIdBandVal0 + b), text);
    }

    const LRESULT delay = SendMessageW(GetDlgItem(hwnd, kIdDelay), TBM_GETPOS, 0, 0);
    if (delay == 0) wcscpy_s(text, L"none");
    else _snwprintf_s(text, _TRUNCATE, L"%ld ms", static_cast<long>(delay));
    SetWindowTextW(GetDlgItem(hwnd, kIdDelayText), text);

    const LRESULT latency = SendMessageW(GetDlgItem(hwnd, kIdLatency), TBM_GETPOS, 0, 0);
    _snwprintf_s(text, _TRUNCATE, L"%ld packets", static_cast<long>(latency));
    SetWindowTextW(GetDlgItem(hwnd, kIdLatText), text);

    // The status line is the thing a registry key could never give: whether the
    // actuators actually accepted the stream. A silent failure here used to be
    // indistinguishable from "the setting is too low to feel".
    const bool enabled = SendMessageW(GetDlgItem(hwnd, kIdEnable), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int interfaces = AudioHaptics::Instance().InterfaceCount();
    if (!enabled) {
        wcscpy_s(text, L"Off - the controller uses normal rumble.");
    } else if (AudioHaptics::Instance().Active() && interfaces > 0) {
        const bool gated =
            SendMessageW(GetDlgItem(hwnd, kIdGate), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (gated) {
            _snwprintf_s(text, _TRUNCATE,
                         L"Streaming to %d interface(s) - silent until a game asks for rumble.",
                         interfaces);
        } else {
            _snwprintf_s(text, _TRUNCATE, L"Streaming to %d actuator interface(s).", interfaces);
        }
    } else if (interfaces > 0) {
        _snwprintf_s(text, _TRUNCATE, L"PCM enabled on %d interface(s), waiting for audio.",
                     interfaces);
    } else {
        wcscpy_s(text,
                 L"No actuator interface answered - is the controller connected and not held by Steam?");
    }
    SetWindowTextW(GetDlgItem(hwnd, kIdStatus), text);
}

LRESULT CALLBACK HapticsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        BuildControls(hwnd);
        // The status line reflects hardware state, which changes without any
        // input from this window, so it polls rather than waiting for a click.
        SetTimer(hwnd, kStatusTimer, 700, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == kStatusTimer) Refresh(hwnd);
        return 0;

    case WM_VSCROLL:
    case WM_HSCROLL:
        // Live while dragging: the feel is the feedback, so waiting for the
        // mouse to be released would make tuning guesswork.
        Commit(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdEnable:
        case kIdGate:
        case kIdRate8000:
        case kIdRate4000:
            Commit(hwnd);
            return 0;
        case kIdReset:
            SendMessageW(GetDlgItem(hwnd, kIdGain), TBM_SETPOS, TRUE, 854);
            for (int b = 0; b < kBandCount; ++b)
                SendMessageW(GetDlgItem(hwnd, kIdBand0 + b), TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(kBandResetDefaults[b]));
            SendMessageW(GetDlgItem(hwnd, kIdGate), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(GetDlgItem(hwnd, kIdLatency), TBM_SETPOS, TRUE, 64);
            SendMessageW(GetDlgItem(hwnd, kIdDelay), TBM_SETPOS, TRUE, 68);
            // 4 kHz: the puck's radio slot cannot sustain 8 kHz.
            SendMessageW(GetDlgItem(hwnd, kIdRate4000), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(GetDlgItem(hwnd, kIdRate8000), BM_SETCHECK, BST_UNCHECKED, 0);
            Commit(hwnd);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kStatusTimer);
        g_window = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void HapticsWindow::Open(HINSTANCE instance) {
    if (g_window != nullptr) {
        ShowWindow(g_window, SW_RESTORE);
        SetForegroundWindow(g_window);
        return;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = instance;
        // This project builds without UNICODE defined, so IDC_ARROW is the ANSI
        // macro. Name the resource id directly rather than casting a lie.
        wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Audio Haptics",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                               CW_USEDEFAULT, 416, 400, nullptr, nullptr, instance, nullptr);
    if (g_window == nullptr) return;
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
}
