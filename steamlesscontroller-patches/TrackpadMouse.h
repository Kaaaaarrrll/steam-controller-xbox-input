#pragma once
#include <cstdint>
#include <cstddef>
#include "TrackpadConfig.h"

// Desktop input driven by ONE physical trackpad. One instance per pad, so the
// two pads can be configured independently — this used to be a single instance
// switched between pads by a bool, which structurally could not represent
// "both pads doing something".
//
// Only movement lives here. The pad's click is a binding like any other and is
// dispatched by ControllerManager alongside the back paddles, so it works the
// same whatever this pad's movement mode is.
class TrackpadMouse {
public:
    // Which physical pad to read. Set once when the slot is created.
    void SetPad(bool isLeftPad) { m_isLeftPad = isLeftPad; }
    void SetMode(TrackpadMode mode);
    void SetScrollDirection(ScrollDirection dir) { m_scrollDir = dir; }

    void Update(const uint8_t* buf, size_t n);
    void Reset();

private:
    void UpdatePointer(int dx, int dy);
    void UpdateScroll(int dx, int dy);
    void NoteMovementSent(long px, bool haveCursor, long cursorX, long cursorY);

    // Raw pad delta -> whichever desktop motion this pad's mode drives. Shared
    // so that a coasting frame and a dragged frame are literally the same code
    // path; anything that treated them differently would drift apart.
    void Emit(int dxRaw, int dyRaw);
    void Coast(float dtSec);

    // A wheel notch is not a universal unit, and that is a second, separate
    // problem from sub-detent shivering.
    //
    // An ordinary window scrolls about three lines per notch. The Xbox full
    // screen experience is a TV-style row UI where one notch advances a whole
    // content row - measured here at roughly 54% of the viewport. So the swipe
    // that reads perfectly in a normal window throws the Xbox UI ten rows down
    // the page, and a flick's coast piles more on top. Scroll output is scaled
    // down while that app is in front; everywhere else is left alone, because
    // everywhere else already feels right.
    float        ScrollScale();
    static float XboxScrollDivisor();

    // Windows delivers a wheel event to the window under the *cursor*, not the
    // focused one. In an ordinary app that is exactly right. In the Xbox full
    // screen experience it is a trap: the UI is driven by a gamepad, nobody is
    // looking at the pointer, and the other pad can easily have left it on the
    // nav rail, the title bar or a screen edge - all of which swallow the
    // wheel completely. Measured directly: six notches with the cursor at
    // (3439,0) scrolled nothing at all, while the same six at the centre moved
    // more than a full screen. So before the first notch of a gesture lands,
    // make sure the cursor is somewhere the wheel can actually be received.
    void         EnsureCursorOverContent();
    static bool  XboxWarpCursorEnabled();

    // How long coasting takes to decay to 1/e of its release speed, in seconds.
    // Read once from HKCU\Software\SteamlessController\PadMomentumMs so the
    // feel can be tuned without a rebuild; 0 disables coasting entirely.
    static float MomentumTauSec();

    // --- velocity estimation ------------------------------------------------
    //
    // Lift-off velocity is estimated by least-squares fitting the recent
    // *positions* of the finger, not by differencing the last two reports.
    // Differencing is the obvious approach and it does not work: a report gap
    // is a few milliseconds, so dividing an integer pad delta by it turns one
    // unit of capacitive jitter into hundreds of units per second, and any
    // smoothing fast enough to still feel responsive is dominated by that
    // noise. The result reads as chaotic, because it is describing the last
    // two reports rather than the swipe.
    //
    // This is the approach Android's VelocityTracker settled on: keep a
    // bounded history, discard anything older than a 100 ms horizon, and fit
    // a 2nd-degree polynomial across what remains. The linear coefficient at
    // the moment of release is the velocity. The quadratic term is what makes
    // an accelerating flick feel right — a straight-line fit would report the
    // average speed across the window, not the speed at the instant of lift.
    //
    // Sampling every touched report, including ones where the finger did not
    // move, is deliberate: it is what makes "drag, pause, lift" correctly
    // produce no fling at all, because the stationary tail is in the fit.
    struct Sample { double t; float x; float y; };

    void PushSample(double t, float x, float y);
    void ClearSamples() { m_histCount = 0; m_histHead = 0; }
    // Velocity in raw pad units per second. False when there is not enough
    // recent history to say anything honest.
    bool EstimateVelocity(float& vx, float& vy) const;

    static constexpr int   HISTORY_SIZE = 32;     // 100 ms at the pad's report rate, with room
    static constexpr float HORIZON_SEC  = 0.100f;

    Sample   m_hist[HISTORY_SIZE]{};
    int      m_histCount = 0;
    int      m_histHead  = 0;   // next write index, buffer is circular

    bool            m_isLeftPad = false;
    TrackpadMode    m_mode      = TrackpadMode::None;
    ScrollDirection m_scrollDir = ScrollDirection::Natural;

    bool     m_touching  = false;
    int16_t  m_prevX     = 0;
    int16_t  m_prevY     = 0;
    float    m_remX      = 0.0f;  // sub-pixel movement carry
    float    m_remY      = 0.0f;
    float    m_scrollRemX = 0.0f;  // sub-detent scroll carry
    float    m_scrollRemY = 0.0f;

    // Flick-to-coast. Movement used to stop dead the instant the finger left
    // the pad, which reads as the pad snatching the cursor back rather than
    // letting go of it.
    float    m_velX      = 0.0f;   // raw pad units/sec, while coasting
    float    m_velY      = 0.0f;
    bool     m_coasting  = false;
    float    m_coastRemX = 0.0f;  // sub-unit carry for synthesised raw deltas
    float    m_coastRemY = 0.0f;
    double   m_lastTime  = 0.0;   // seconds, from the performance counter
    bool     m_haveTime  = false;

    // Which app is in front, refreshed at human speed rather than report speed
    // — resolving it walks child windows to see past ApplicationFrameHost, and
    // that is far too much to do 250 times a second.
    double   m_fgCheckedAt   = 0.0;
    bool     m_fgRowSnapping = false;
    static constexpr double FG_RECHECK_SEC = 0.25;

    // Once per gesture, not once per notch: repeatedly recentring mid-scroll
    // would be the pad wrestling the pointer away from the user.
    bool     m_warpedThisGesture = false;
    // Fraction of the window treated as chrome rather than content, at each
    // edge. The Xbox app's nav rail and header are both comfortably inside it.
    static constexpr int CONTENT_INSET_DIVISOR = 5;   // outer fifth

    // Movement Windows accepted, checked against the cursor actually moving.
    // A refused SendInput reports itself; motion that is accepted and then
    // discarded — by a cursor clip a game left behind, or a low-level hook
    // filtering injected input — looks identical to the user and otherwise
    // leaves no trace at all. Records where the cursor was and how much travel
    // has been sent since; when the travel adds up and the position has not
    // changed, that is worth a log line.
    long     m_refCursorX  = 0;
    long     m_refCursorY  = 0;
    bool     m_haveRef     = false;
    long     m_travelSent  = 0;

    // Enough sent movement that a still cursor cannot be a coincidence.
    static constexpr long  STUCK_TRAVEL_PX = 120;

    static constexpr float SENSITIVITY = 0.01125f;
    // Chosen so a full swipe across the pad is a few notches rather than a
    // page-length fling: the pad's axes span roughly +/-32000, and one wheel
    // detent is WHEEL_DELTA (120).
    static constexpr float SCROLL_SENSITIVITY = 0.02f;

    // A deliberate drag across half the pad in a second is about 32000 units/s,
    // and must NOT fling — you were placing the cursor, not throwing it. Set
    // above that so only a genuine flick coasts.
    static constexpr float MIN_FLICK_SPEED = 40000.0f;   // raw units/sec
    // Ceiling, so a jitter spike at lift-off cannot launch the cursor across
    // the desk. Roughly 4500 px/s once sensitivity is applied.
    static constexpr float MAX_FLING_SPEED = 400000.0f;  // raw units/sec
    // Coasting ends here instead of crawling asymptotically towards zero.
    static constexpr float MIN_COAST_SPEED = 3000.0f;    // raw units/sec
    // A gap longer than this is a stall, not a frame: the controller stopped
    // reporting, the app was descheduled, the machine slept. Treating it as
    // elapsed time would fire one enormous synthetic jump.
    static constexpr float MAX_FRAME_SEC = 0.1f;
};
