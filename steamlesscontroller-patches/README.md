# SteamlessController patches

Reference copies of three optional changes to
[SteamlessController](https://github.com/ddeverill/SteamlessController) by ddeverill.

**These files are derivative works of SteamlessController and remain under its MIT licence,
not this repository's.** They are included so the changes can be read without the build tree
to hand.

Everything in the parent repository works on the stock SteamlessController binary. These only
improve trackpad feel:

- `TrackpadMouse.cpp` / `TrackpadMouse.h` - flick-to-coast, whole-detent scrolling, and the
  Xbox-app scroll scaling and cursor warp.
- `velocity_test.cpp` - checks the velocity fit against synthetic swipes: constant velocity,
  accelerating, drag-then-pause, jittery, and stale samples outside the horizon.

See the parent README, section "Optional: the patched SteamlessController build", for what each
change does and why the obvious implementation of the velocity estimate does not work.

Build with the VS 2026 C++ toolset:

    cmake --preset release
    cmake --build build/release --config Release

Keep the original binary as `SteamlessController.exe.stock`. A SteamlessController update will
silently overwrite the patched build and revert both behaviours to stock.
