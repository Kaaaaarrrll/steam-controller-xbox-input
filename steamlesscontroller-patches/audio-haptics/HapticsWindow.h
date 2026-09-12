#pragma once

#include <windows.h>

// Settings window for audio haptics.
//
// Deliberately plain Win32 rather than the WebView2 surface RemapWindow uses:
// this is five controls and a status line, and it should open instantly from
// the tray without spinning up a browser control.
//
// Every change is applied live. Gain, bass and latency reach the capture loop
// on the next report; enable and rate restart the stream. Nothing here asks the
// user to restart the application, because a settings window that needs one is
// a config file with extra steps.
class HapticsWindow {
public:
    // Opens the window, or brings it forward when it already exists.
    static void Open(HINSTANCE instance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void BuildControls(HWND hwnd);
    // Pushes one slider's value into the registry and asks the engine to reload.
    static void Commit(HWND hwnd);
    // Refreshes the value labels and the live status line.
    static void Refresh(HWND hwnd);
};
