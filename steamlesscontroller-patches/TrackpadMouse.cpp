#include "TrackpadMouse.h"
#include "InputInjection.h"
#include "ForegroundWatcher.h"
#include "EventLog.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace {

// Seconds from the performance counter. The frequency is fixed for the life of
// the process, so it is only ever queried once.
double NowSec() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? static_cast<double>(f.QuadPart) : 1.0;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

}  // namespace

// Coasting is deliberately tunable at runtime rather than compiled in: "a bit
// of momentum" is a matter of feel, and the alternative is a rebuild per guess.
float TrackpadMouse::MomentumTauSec() {
    static const float tau = [] {
        DWORD ms   = 300;   // enough to glide, short of skating away
        DWORD data = 0;
        DWORD cb   = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\SteamlessController",
                         L"PadMomentumMs", RRF_RT_REG_DWORD, nullptr,
                         &data, &cb) == ERROR_SUCCESS) {
            ms = data;
        }
        if (ms > 2000) ms = 2000;   // past this it is not momentum, it is drift
        return static_cast<float>(ms) / 1000.0f;
    }();
    return tau;
}

// How much to divide scroll by in a row-snapping UI. Tunable, because "one
// row per flick" is a matter of taste and the alternative is a rebuild.
float TrackpadMouse::XboxScrollDivisor() {
    static const float divisor = [] {
        DWORD v    = 6;
        DWORD data = 0;
        DWORD cb   = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\SteamlessController",
                         L"XboxScrollDivisor", RRF_RT_REG_DWORD, nullptr,
                         &data, &cb) == ERROR_SUCCESS) {
            v = data;
        }
        if (v < 1)  v = 1;    // 1 restores stock behaviour
        if (v > 50) v = 50;
        return static_cast<float>(v);
    }();
    return divisor;
}

bool TrackpadMouse::XboxWarpCursorEnabled() {
    static const bool on = [] {
        DWORD data = 0;
        DWORD cb   = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\SteamlessController",
                         L"XboxScrollWarpCursor", RRF_RT_REG_DWORD, nullptr,
                         &data, &cb) == ERROR_SUCCESS) {
            return data != 0;
        }
        return true;   // on by default; without it scrolling silently does nothing
    }();
    return on;
}

void TrackpadMouse::EnsureCursorOverContent() {
    if (!XboxWarpCursorEnabled()) return;

    const HWND fg = GetForegroundWindow();
    if (!fg) return;
    RECT r{};
    if (!GetWindowRect(fg, &r)) return;
    POINT c{};
    if (!GetCursorPos(&c)) return;

    const LONG insetX = (r.right - r.left) / CONTENT_INSET_DIVISOR;
    const LONG insetY = (r.bottom - r.top) / CONTENT_INSET_DIVISOR;
    if (c.x >= r.left + insetX && c.x <= r.right  - insetX
     && c.y >= r.top  + insetY && c.y <= r.bottom - insetY) {
        return;   // already somewhere a wheel event will be received
    }

    const int cx = static_cast<int>((r.left + r.right) / 2);
    const int cy = static_cast<int>((r.top + r.bottom) / 2);
    if (SetCursorPos(cx, cy)) {
        EventLog::Write("SCROLL: cursor was at (%ld,%ld), off the content area "
                        "— moved to (%d,%d) so the wheel lands",
                        c.x, c.y, cx, cy);
    }
}

float TrackpadMouse::ScrollScale() {
    const double now = NowSec();
    if (now - m_fgCheckedAt >= FG_RECHECK_SEC) {
        m_fgCheckedAt = now;
        // Prefix, not the whole AUMID: the Xbox app runs as ...!Microsoft.Xbox.AppL
        // in the full screen experience and ...!Microsoft.Xbox.App elsewhere.
        const ForegroundIdentity id = ForegroundWatcher::Current();
        const bool snapping = id.aumid.rfind(L"Microsoft.GamingApp_", 0) == 0;
        if (snapping != m_fgRowSnapping) {
            m_fgRowSnapping = snapping;
            EventLog::Write("SCROLL: %s row-snapping scaling (1/%.0f)",
                            snapping ? "entered" : "left", XboxScrollDivisor());
        }
    }
    return m_fgRowSnapping ? (1.0f / XboxScrollDivisor()) : 1.0f;
}

void TrackpadMouse::SetMode(TrackpadMode mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    Reset();  // carried remainders and touch state mean nothing to the new mode
}

void TrackpadMouse::Reset() {
    m_touching   = false;
    m_prevX      = 0;
    m_prevY      = 0;
    m_remX       = 0.0f;
    m_remY       = 0.0f;
    m_scrollRemX = 0.0f;
    m_scrollRemY = 0.0f;
    m_haveRef    = false;
    m_travelSent = 0;
    m_velX       = 0.0f;
    m_velY       = 0.0f;
    m_coasting   = false;
    m_coastRemX  = 0.0f;
    m_coastRemY  = 0.0f;
    m_haveTime   = false;
    ClearSamples();
}

void TrackpadMouse::PushSample(double t, float x, float y) {
    m_hist[m_histHead] = Sample{ t, x, y };
    m_histHead = (m_histHead + 1) % HISTORY_SIZE;
    if (m_histCount < HISTORY_SIZE) ++m_histCount;
}

// Least-squares fit of position against time over the horizon, velocity being
// the first derivative at the newest sample. Time is measured relative to that
// newest sample so the fitted linear coefficient is the velocity at lift-off
// directly, with no evaluation step and no loss of conditioning from large
// absolute timestamps.
bool TrackpadMouse::EstimateVelocity(float& vx, float& vy) const {
    if (m_histCount < 2) return false;

    const int    newest = (m_histHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    const double tNew   = m_hist[newest].t;

    // Sums for the normal equations of y = b0 + b1*t + b2*t^2.
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    double gx0 = 0.0, gx1 = 0.0, gx2 = 0.0;
    double gy0 = 0.0, gy1 = 0.0, gy2 = 0.0;
    int    used = 0;

    for (int i = 0; i < m_histCount; ++i) {
        const int    idx = (newest - i + HISTORY_SIZE) % HISTORY_SIZE;
        const double t   = m_hist[idx].t - tNew;          // <= 0
        if (-t > static_cast<double>(HORIZON_SEC)) break;  // older than the horizon

        const double x = static_cast<double>(m_hist[idx].x);
        const double y = static_cast<double>(m_hist[idx].y);
        const double t2 = t * t;

        s0 += 1.0; s1 += t; s2 += t2; s3 += t2 * t; s4 += t2 * t2;
        gx0 += x; gx1 += t * x; gx2 += t2 * x;
        gy0 += y; gy1 += t * y; gy2 += t2 * y;
        ++used;
    }

    if (used < 2) return false;

    // Straight line through the window. Also the fallback when the quadratic
    // system is degenerate, which happens when the samples span too little
    // time to support three coefficients.
    const double linDet = s0 * s2 - s1 * s1;
    if (std::fabs(linDet) < 1e-12) return false;

    if (used >= 3) {
        const double det =
              s0 * (s2 * s4 - s3 * s3)
            - s1 * (s1 * s4 - s3 * s2)
            + s2 * (s1 * s3 - s2 * s2);

        if (std::fabs(det) > 1e-18) {
            const double detX =
                  s0 * (gx1 * s4 - s3 * gx2)
                - gx0 * (s1 * s4 - s3 * s2)
                + s2 * (s1 * gx2 - gx1 * s2);
            const double detY =
                  s0 * (gy1 * s4 - s3 * gy2)
                - gy0 * (s1 * s4 - s3 * s2)
                + s2 * (s1 * gy2 - gy1 * s2);

            vx = static_cast<float>(detX / det);
            vy = static_cast<float>(detY / det);
            if (std::isfinite(vx) && std::isfinite(vy)) return true;
        }
    }

    vx = static_cast<float>((s0 * gx1 - s1 * gx0) / linDet);
    vy = static_cast<float>((s0 * gy1 - s1 * gy0) / linDet);
    return std::isfinite(vx) && std::isfinite(vy);
}

// Tracks whether accepted movement is reaching the cursor. Re-arms whenever the
// cursor does move, so only a genuinely pinned cursor accumulates travel.
void TrackpadMouse::NoteMovementSent(long px, bool haveCursor,
                                     long cursorX, long cursorY) {
    if (!haveCursor) {
        // No cursor position to compare against — GetCursorPos itself fails
        // off the input desktop, and InputInjection reports that separately.
        m_haveRef    = false;
        m_travelSent = 0;
        return;
    }

    if (!m_haveRef || cursorX != m_refCursorX || cursorY != m_refCursorY) {
        m_refCursorX = cursorX;
        m_refCursorY = cursorY;
        m_haveRef    = true;
        m_travelSent = 0;
    }

    m_travelSent += px;
    if (m_travelSent >= STUCK_TRAVEL_PX) {
        POINT at{ m_refCursorX, m_refCursorY };
        InputInjection::LogCursorNotMoving(m_travelSent, at);
        m_travelSent = 0;  // re-arm; the log call rate-limits itself
    }
}

void TrackpadMouse::UpdatePointer(int dx, int dy) {
    // Carry sub-pixel remainders between frames — per-frame deltas scaled by
    // sensitivity are often below one pixel, and truncating them each frame
    // would discard slow movement entirely.
    const float fx = static_cast<float>(dx) * SENSITIVITY + m_remX;
    const float fy = static_cast<float>(dy) * SENSITIVITY + m_remY;
    const LONG  ix = static_cast<LONG>(fx);
    const LONG  iy = static_cast<LONG>(fy);
    m_remX = fx - static_cast<float>(ix);
    m_remY = fy - static_cast<float>(iy);
    if (ix == 0 && iy == 0) return;

    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx      = ix;
    input.mi.dy      = iy;
    // Read the cursor before sending: SendInput queues the event rather than
    // applying it, so a position read straight after would still be the old one.
    POINT      before{};
    const bool haveBefore = GetCursorPos(&before) != FALSE;
    if (InputInjection::Send(input, "trackpad-move")) {
        NoteMovementSent(std::labs(ix) + std::labs(iy),
                         haveBefore, before.x, before.y);
    }
}

// Wheel events are quantised to WHEEL_DELTA notches, which is much coarser
// than a pad frame's movement — so the same remainder-carry the pointer path
// uses is what makes slow scrolling work at all here.
//
// Takes raw pad deltas (Y growing upward). Natural scrolling means the
// content follows the finger, which is the opposite sense to the wheel's own
// convention that positive is "away from the user" — hence the inversion
// here rather than at the call site.
void TrackpadMouse::UpdateScroll(int dx, int dy) {
    if (m_scrollDir == ScrollDirection::Natural) { dx = -dx; dy = -dy; }

    const float scale = ScrollScale();
    m_scrollRemY += static_cast<float>(dy) * SCROLL_SENSITIVITY * scale;
    m_scrollRemX += static_cast<float>(dx) * SCROLL_SENSITIVITY * scale;

    // Emit whole detents only, and carry the rest.
    //
    // Sub-detent wheel events are legal, and classic Win32 scroll bars simply
    // accumulate them — which is why this went unnoticed. WinUI/UWP surfaces
    // do not: each wheel message retargets a smooth-scroll animation, so a
    // continuous stream of fractional deltas retargets it to somewhere it has
    // effectively already reached, over and over. The view shivers in place
    // and never travels. The Xbox full screen experience is exactly that kind
    // of surface, and it is the one place this pad has to work. A physical
    // wheel only ever sends whole notches; so does this now.
    const float detent   = static_cast<float>(WHEEL_DELTA);
    const int   notchesY = static_cast<int>(m_scrollRemY / detent);
    const int   notchesX = static_cast<int>(m_scrollRemX / detent);
    m_scrollRemY -= static_cast<float>(notchesY) * detent;
    m_scrollRemX -= static_cast<float>(notchesX) * detent;

    // Only in a row-snapping UI, only once per gesture, and only when the
    // cursor is somewhere useless — an ordinary app scrolls what you point at,
    // and that is the behaviour people expect.
    if (m_fgRowSnapping && !m_warpedThisGesture && (notchesY != 0 || notchesX != 0)) {
        EnsureCursorOverContent();
        m_warpedThisGesture = true;
    }

    if (notchesY != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(notchesY * WHEEL_DELTA);
        InputInjection::Send(input, "trackpad-scroll");
    }
    if (notchesX != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(notchesX * WHEEL_DELTA);
        InputInjection::Send(input, "trackpad-hscroll");
    }
}

void TrackpadMouse::Emit(int dxRaw, int dyRaw) {
    if (m_mode == TrackpadMode::MousePointer) {
        // Pad Y grows upward, screen Y grows downward.
        UpdatePointer(dxRaw, -dyRaw);
    } else {
        // Raw deltas — UpdateScroll owns the direction convention.
        UpdateScroll(dxRaw, dyRaw);
    }
}

// One frame of glide after the finger has gone. Decay is exponential in real
// time rather than per report, so the feel does not change with the controller's
// report rate or a hitching frame.
void TrackpadMouse::Coast(float dtSec) {
    const float tau = MomentumTauSec();
    if (dtSec <= 0.0f || tau <= 0.0f) { m_coasting = false; return; }

    const float decay = std::exp(-dtSec / tau);
    m_velX *= decay;
    m_velY *= decay;

    if (std::sqrt(m_velX * m_velX + m_velY * m_velY) < MIN_COAST_SPEED) {
        m_coasting = false;
        m_velX = 0.0f;
        m_velY = 0.0f;
        return;
    }

    // Same sub-unit carry as everywhere else: at the tail of a glide the
    // per-frame distance is well under one raw unit, and truncating it would
    // end the coast early and abruptly.
    const float fx = m_velX * dtSec + m_coastRemX;
    const float fy = m_velY * dtSec + m_coastRemY;
    const int   ix = static_cast<int>(fx);
    const int   iy = static_cast<int>(fy);
    m_coastRemX = fx - static_cast<float>(ix);
    m_coastRemY = fy - static_cast<float>(iy);

    if (ix != 0 || iy != 0) Emit(ix, iy);
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    // Pad clicks are handled by ControllerManager, alongside every other
    // binding — this is movement only. Only the two desktop-driving modes
    // produce anything here; None and DS4Touchpad are both "not the desktop's".
    if (n < 30
        || (m_mode != TrackpadMode::MousePointer && m_mode != TrackpadMode::ScrollWheel))
        return;

    // Reports keep arriving after the finger lifts, and that is what drives the
    // coast — so timing is updated on every report, not only touched ones.
    const double now = NowSec();
    float dt = 0.0f;
    if (m_haveTime) {
        const double elapsed = now - m_lastTime;
        if (elapsed > 0.0 && elapsed <= static_cast<double>(MAX_FRAME_SEC))
            dt = static_cast<float>(elapsed);
    }
    m_lastTime = now;
    m_haveTime = true;

    const uint8_t b2 = buf[4];
    const uint8_t b3 = buf[5];

    const bool touching = m_isLeftPad
        ? (b3 & SteamController::BTN_TP_LT) != 0
        : (b2 & SteamController::BTN_TP_RT) != 0;

    int16_t x = 0, y = 0;
    if (m_isLeftPad) {
        memcpy(&x, buf + 18, 2);
        memcpy(&y, buf + 20, 2);
    } else {
        memcpy(&x, buf + 24, 2);
        memcpy(&y, buf + 26, 2);
    }

    if (touching) {
        if (!m_touching) {
            // A finger landing is a grab: whatever was gliding stops dead,
            // which is what every touch surface does and what makes a flick
            // catchable. The old swipe's history goes with it.
            m_coasting  = false;
            m_velX      = 0.0f;
            m_velY      = 0.0f;
            m_coastRemX = 0.0f;
            m_coastRemY = 0.0f;
            m_warpedThisGesture = false;
            ClearSamples();
        }

        // Every touched report is sampled, moved or not — a stationary tail is
        // exactly what tells the fit that "drag, pause, lift" is not a fling.
        PushSample(now, static_cast<float>(x), static_cast<float>(y));

        if (m_touching) {
            const int dxRaw = static_cast<int>(x - m_prevX);
            const int dyRaw = static_cast<int>(y - m_prevY);
            if (dxRaw != 0 || dyRaw != 0) Emit(dxRaw, dyRaw);
        }
    } else if (m_touching) {
        // Just released: ask the fit how fast the finger was actually going.
        m_coasting  = false;
        m_coastRemX = 0.0f;
        m_coastRemY = 0.0f;

        float vx = 0.0f, vy = 0.0f;
        if (MomentumTauSec() > 0.0f && EstimateVelocity(vx, vy)) {
            float speed = std::sqrt(vx * vx + vy * vy);
            if (speed > MAX_FLING_SPEED) {
                const float k = MAX_FLING_SPEED / speed;
                vx *= k; vy *= k; speed = MAX_FLING_SPEED;
            }
            if (speed >= MIN_FLICK_SPEED) {
                m_velX     = vx;
                m_velY     = vy;
                m_coasting = true;
            }
        }
        ClearSamples();
    } else if (m_coasting) {
        Coast(dt);
    }

    if (touching) { m_prevX = x; m_prevY = y; }
    m_touching = touching;
}
