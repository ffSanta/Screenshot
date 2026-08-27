#include "xsession.hpp"

#include <X11/extensions/shape.h>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace ss {

XSession::XSession() {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        const char* d = std::getenv("DISPLAY");
        std::string msg = "cannot open X display";
        if (d && *d) {
            msg += " '" + std::string(d) + "'";
        } else if (std::getenv("WAYLAND_DISPLAY")) {
            msg += ": this is a Wayland session and screenshot is X11-only";
        } else {
            msg += ": DISPLAY is not set";
        }
        throw std::runtime_error(msg);
    }

    screen_  = DefaultScreen(dpy_);
    root_    = RootWindow(dpy_, screen_);
    visual_  = DefaultVisual(dpy_, screen_);
    depth_   = DefaultDepth(dpy_, screen_);
    screenW_ = DisplayWidth(dpy_, screen_);
    screenH_ = DisplayHeight(dpy_, screen_);

    int event_base = 0, error_base = 0;
    hasShape_ = XShapeQueryExtension(dpy_, &event_base, &error_base) == True;
}

XSession::~XSession() {
    if (dpy_) XCloseDisplay(dpy_);
}

} // namespace ss
