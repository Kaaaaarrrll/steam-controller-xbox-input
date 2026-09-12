// Standalone check of the least-squares velocity estimator used by
// TrackpadMouse. The fit below is character-for-character the one in
// TrackpadMouse::EstimateVelocity; this harness only feeds it synthetic
// swipes and checks the answers, because the failure that prompted the
// rewrite was a plausible-looking estimator that nobody had ever fed a
// known input.
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr int   HISTORY_SIZE = 32;
static constexpr float HORIZON_SEC  = 0.100f;

struct Sample { double t; float x; float y; };

static bool EstimateVelocity(const std::vector<Sample>& hist, float& vx, float& vy) {
    const int count = static_cast<int>(hist.size());
    if (count < 2) return false;

    const int    newest = count - 1;
    const double tNew   = hist[newest].t;

    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    double gx0 = 0.0, gx1 = 0.0, gx2 = 0.0;
    double gy0 = 0.0, gy1 = 0.0, gy2 = 0.0;
    int    used = 0;

    for (int i = 0; i < count; ++i) {
        const int    idx = newest - i;
        const double t   = hist[idx].t - tNew;
        if (-t > static_cast<double>(HORIZON_SEC)) break;

        const double x  = static_cast<double>(hist[idx].x);
        const double y  = static_cast<double>(hist[idx].y);
        const double t2 = t * t;

        s0 += 1.0; s1 += t; s2 += t2; s3 += t2 * t; s4 += t2 * t2;
        gx0 += x; gx1 += t * x; gx2 += t2 * x;
        gy0 += y; gy1 += t * y; gy2 += t2 * y;
        ++used;
    }

    if (used < 2) return false;

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

static int g_pass = 0, g_fail = 0;

static void Check(const char* name, float got, float want, float tolerance) {
    const bool ok = std::fabs(got - want) <= tolerance;
    if (ok) { ++g_pass; std::printf("  PASS  %-58s got %10.0f\n", name, got); }
    else    { ++g_fail; std::printf("  FAIL  %-58s got %10.0f want %10.0f (+/-%.0f)\n",
                                    name, got, want, tolerance); }
}

// A 250 Hz report stream, which is what the dongle actually delivers.
static constexpr double DT = 1.0 / 250.0;

int main() {
    // 1. Constant velocity. The estimator must return exactly the speed used
    //    to generate the samples.
    {
        std::vector<Sample> h;
        const double v = 100000.0;
        for (int i = 0; i < 25; ++i) {
            const double t = i * DT;
            h.push_back({ t, static_cast<float>(v * t), 0.0f });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        Check("constant 100k units/s recovers 100k", vx, 100000.0f, 2000.0f);
    }

    // 2. Accelerating swipe. This is the case a straight-line fit gets wrong:
    //    it would report the average speed over the window (~half), where the
    //    quadratic reports the speed at the instant of lift.
    {
        std::vector<Sample> h;
        const double a = 2000000.0;   // units/s^2
        for (int i = 0; i < 25; ++i) {
            const double t = i * DT;
            h.push_back({ t, static_cast<float>(0.5 * a * t * t), 0.0f });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        // v = a*t at the newest sample, t = 24*DT = 0.096 s -> 192000
        Check("accelerating swipe recovers LIFT-OFF speed, not average",
              vx, 192000.0f, 4000.0f);
    }

    // 3. Drag, then hold still, then lift. Must not fling: the stationary tail
    //    is inside the horizon and drags the fit to zero.
    {
        std::vector<Sample> h;
        double x = 0.0;
        for (int i = 0; i < 25; ++i) {           // moving
            const double t = i * DT;
            x = 100000.0 * t;
            h.push_back({ t, static_cast<float>(x), 0.0f });
        }
        for (int i = 25; i < 50; ++i) {          // parked for 100 ms
            h.push_back({ i * DT, static_cast<float>(x), 0.0f });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        Check("drag then pause then lift does NOT fling", vx, 0.0f, 5000.0f);
    }

    // 4. Capacitive jitter on top of a constant swipe. This is the one the old
    //    differencing estimator failed: one unit of jitter over a 4 ms report
    //    gap is 250 units/s of noise, and it dominated the answer.
    {
        std::vector<Sample> h;
        const double v = 100000.0;
        const int jitter[] = { 0, 3, -2, 1, -3, 2, 0, -1, 3, -2 };
        for (int i = 0; i < 25; ++i) {
            const double t = i * DT;
            h.push_back({ t, static_cast<float>(v * t + jitter[i % 10]), 0.0f });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        Check("jittery samples still recover ~100k", vx, 100000.0f, 8000.0f);
    }

    // 5. Only samples beyond the horizon survive in history. Anything older
    //    than 100 ms must be ignored, so an old fast swipe cannot fling a
    //    later slow one.
    {
        std::vector<Sample> h;
        for (int i = 0; i < 25; ++i) {           // ancient fast swipe
            h.push_back({ i * DT, static_cast<float>(500000.0 * (i * DT)), 0.0f });
        }
        const double base = h.back().t;
        const double x0   = h.back().x;
        for (int i = 1; i <= 25; ++i) {          // recent slow crawl
            const double t = base + i * DT;
            h.push_back({ t, static_cast<float>(x0 + 10000.0 * (i * DT)), 0.0f });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        Check("stale fast swipe outside the horizon is ignored", vx, 10000.0f, 4000.0f);
    }

    // 6. Two samples only - linear fallback rather than nonsense or a crash.
    {
        std::vector<Sample> h{ { 0.0, 0.0f, 0.0f }, { DT, 400.0f, 0.0f } };
        float vx = 0, vy = 0;
        const bool ok = EstimateVelocity(h, vx, vy);
        if (!ok) { ++g_fail; std::printf("  FAIL  two-sample fallback returned false\n"); }
        else     { Check("two samples fall back to a linear fit", vx, 100000.0f, 1.0f); }
    }

    // 7. A single sample cannot produce a velocity at all.
    {
        std::vector<Sample> h{ { 0.0, 0.0f, 0.0f } };
        float vx = 0, vy = 0;
        if (EstimateVelocity(h, vx, vy)) { ++g_fail; std::printf("  FAIL  one sample produced a velocity\n"); }
        else { ++g_pass; std::printf("  PASS  %-58s\n", "one sample produces no velocity"); }
    }

    // 8. Both axes at once, so x and y are not accidentally sharing sums.
    {
        std::vector<Sample> h;
        for (int i = 0; i < 25; ++i) {
            const double t = i * DT;
            h.push_back({ t, static_cast<float>(60000.0 * t), static_cast<float>(-80000.0 * t) });
        }
        float vx = 0, vy = 0;
        EstimateVelocity(h, vx, vy);
        Check("x axis independent", vx,  60000.0f, 2000.0f);
        Check("y axis independent", vy, -80000.0f, 2000.0f);
    }

    std::printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
