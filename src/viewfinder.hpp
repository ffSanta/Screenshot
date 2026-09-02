#pragma once

#include "geometry.hpp"
#include "xsession.hpp"

#include <X11/Xlib.h>
#include <cairo.h>
#include <optional>
#include <unordered_map>

namespace ss {

// The floating frame the user positions over the screen.
//
// The window is created override-redirect, which means the window manager
// never sees it. On a tiling WM like i3 that is the whole trick: a managed
// window would be snapped into the layout and could not be freely moved or
// resized, whereas this one is ours to place pixel by pixel, with no i3
// config changes required.
class Viewfinder {
public:
    Viewfinder(XSession& x, Rect initial);
    ~Viewfinder();

    Viewfinder(const Viewfinder&)            = delete;
    Viewfinder& operator=(const Viewfinder&) = delete;

    // Runs the event loop until the user saves or cancels. Returns the region
    // to capture, or nullopt if cancelled. On return the window is unmapped
    // and the keyboard released, so the caller can grab the screen cleanly.
    std::optional<Rect> run();

private:
    void setGeometry(const Rect& sel);
    void applyShape();
    void draw();
    void drawFrame(cairo_t* cr);
    void drawHandles(cairo_t* cr);
    void drawToolbar(cairo_t* cr);
    void drawButton(cairo_t* cr, const Rect& r, const char* label, bool primary,
                    bool hovered, bool pressed);
    void teardown();

    void onButtonPress(const XButtonEvent& e);
    void onMotion(const XMotionEvent& e);
    void onButtonRelease(const XButtonEvent& e);
    void onKeyPress(const XKeyEvent& e);
    void setHover(Zone z);
    Cursor cursorFor(Zone z);

    XSession& x_;
    Window    win_ = 0;
    cairo_surface_t* surf_ = nullptr;
    Rect      sel_{};
    Layout    layout_{};
    Zone      hover_   = Zone::Outside;   // zone under the pointer, for highlights
    Zone      pressed_ = Zone::Outside;   // toolbar button being held down

    // Drag in progress. The anchor is the selection as it was when the drag
    // started, so every motion is applied to that rather than accumulated -
    // which keeps the frame from creeping if an event is ever dropped.
    Zone      dragZone_ = Zone::Outside;
    Rect      anchor_{};
    int       anchorRootX_ = 0;
    int       anchorRootY_ = 0;

    // Held-arrow acceleration. X gives us no key-release event here, so a
    // run of repeats is recognised by direction, mode and arrival time rather
    // than by tracking the key being down.
    int       keyDx_     = 0;
    int       keyDy_     = 0;
    bool      keyResize_ = false;
    Time      keyTime_   = 0;
    int       keyRepeats_ = 0;
    unsigned long keyGapMs_ = KEY_REPEAT_GAP_FALLBACK_MS;  // from X, see ctor

    std::unordered_map<int, Cursor> cursors_;
    Zone      cursorZone_ = Zone::CancelBtn;  // deliberately != initial hover

    std::optional<Rect> result_;
    bool      running_ = true;
};

// Sensible starting frame: 800x500, shrunk to fit and centred on screen.
Rect defaultRect(const XSession& x);

} // namespace ss
