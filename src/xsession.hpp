#pragma once

#include <X11/Xlib.h>

namespace ss {

// Owns the X connection and caches the handful of screen facts everything
// else needs. Throws std::runtime_error if X is unusable, which is the case
// worth reporting clearly: this tool is X11-only and does not run on Wayland.
//
// Named XSession rather than XContext because Xlib already defines a global
// XContext typedef used by XSaveContext/XFindContext.
class XSession {
public:
    XSession();
    ~XSession();

    XSession(const XSession&)            = delete;
    XSession& operator=(const XSession&) = delete;

    Display* dpy()      const { return dpy_; }
    Window   root()     const { return root_; }
    Visual*  visual()   const { return visual_; }
    int      depth()    const { return depth_; }
    int      screen()   const { return screen_; }
    int      screenW()  const { return screenW_; }
    int      screenH()  const { return screenH_; }
    bool     hasShape() const { return hasShape_; }

private:
    Display* dpy_      = nullptr;
    Window   root_     = 0;
    Visual*  visual_   = nullptr;
    int      depth_    = 0;
    int      screen_   = 0;
    int      screenW_  = 0;
    int      screenH_  = 0;
    bool     hasShape_ = false;
};

} // namespace ss
