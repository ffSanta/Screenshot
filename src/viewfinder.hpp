#pragma once

#include "geometry.hpp"
#include "xsession.hpp"

#include <X11/Xlib.h>
#include <optional>

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
    void teardown();

    XSession& x_;
    Window    win_ = 0;
    Rect      sel_{};
    Layout    layout_{};
};

// Sensible starting frame: 800x500, shrunk to fit and centred on screen.
Rect defaultRect(const XSession& x);

} // namespace ss
