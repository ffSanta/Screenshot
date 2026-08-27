#include "viewfinder.hpp"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <stdexcept>

namespace ss {

Rect defaultRect(const XSession& x) {
    Rect r;
    r.w = std::min(800, std::max(MIN_W, x.screenW() - 80));
    r.h = std::min(500, std::max(MIN_H, x.screenH() - 120));
    r.x = (x.screenW() - r.w) / 2;
    r.y = (x.screenH() - r.h) / 2;
    return r;
}

Viewfinder::Viewfinder(XSession& x, Rect initial) : x_(x) {
    sel_    = clampToScreen(initial, x.screenW(), x.screenH());
    layout_ = layoutFor(sel_, x.screenW(), x.screenH());

    XSetWindowAttributes a{};
    a.override_redirect = True;    // the line that keeps i3 from tiling us
    a.background_pixmap = None;    // we paint every pixel ourselves, no flash
    a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask
                 | PointerMotionMask | KeyPressMask | StructureNotifyMask;

    win_ = XCreateWindow(x_.dpy(), x_.root(),
                         layout_.win.x, layout_.win.y,
                         static_cast<unsigned>(layout_.win.w),
                         static_cast<unsigned>(layout_.win.h),
                         0, x_.depth(), InputOutput, x_.visual(),
                         CWOverrideRedirect | CWBackPixmap | CWEventMask, &a);
    if (!win_) throw std::runtime_error("failed to create the viewfinder window");

    // Not needed for correctness given override-redirect, but it makes the
    // window identifiable in xprop/xwininfo when something misbehaves.
    XClassHint cls{ const_cast<char*>("screenshot"), const_cast<char*>("Screenshot") };
    XSetClassHint(x_.dpy(), win_, &cls);
    XStoreName(x_.dpy(), win_, "screenshot");

    applyShape();
    XMapRaised(x_.dpy(), win_);

    // Override-redirect windows get no focus from the WM, so take the
    // keyboard directly or the key bindings would never fire.
    XGrabKeyboard(x_.dpy(), win_, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XSync(x_.dpy(), False);
}

Viewfinder::~Viewfinder() {
    teardown();
    if (win_) {
        XDestroyWindow(x_.dpy(), win_);
        win_ = 0;
    }
}

void Viewfinder::teardown() {
    if (!win_) return;
    XUngrabKeyboard(x_.dpy(), CurrentTime);
    XUnmapWindow(x_.dpy(), win_);
    XSync(x_.dpy(), False);
}

// Restrict the window to its border strips and toolbar, leaving the middle
// as a genuine hole rather than a transparent pixel. The desktop underneath
// shows through untouched, so what you see framed is exactly what gets
// captured - and unlike an ARGB visual this needs no compositor running.
void Viewfinder::applyShape() {
    if (!x_.hasShape()) return;  // no extension: window stays a solid box

    const Rect& f = layout_.frameLocal;
    const Rect& t = layout_.toolbarLocal;

    const XRectangle parts[5] = {
        // top / bottom / left / right strips of the frame
        { static_cast<short>(f.x),            static_cast<short>(f.y),
          static_cast<unsigned short>(f.w),   static_cast<unsigned short>(BORDER) },
        { static_cast<short>(f.x),            static_cast<short>(f.bottom() - BORDER),
          static_cast<unsigned short>(f.w),   static_cast<unsigned short>(BORDER) },
        { static_cast<short>(f.x),            static_cast<short>(f.y + BORDER),
          static_cast<unsigned short>(BORDER),
          static_cast<unsigned short>(f.h - 2 * BORDER) },
        { static_cast<short>(f.right() - BORDER), static_cast<short>(f.y + BORDER),
          static_cast<unsigned short>(BORDER),
          static_cast<unsigned short>(f.h - 2 * BORDER) },
        // the toolbar strip
        { static_cast<short>(t.x),            static_cast<short>(t.y),
          static_cast<unsigned short>(t.w),   static_cast<unsigned short>(t.h) },
    };

    XShapeCombineRectangles(x_.dpy(), win_, ShapeBounding, 0, 0,
                            const_cast<XRectangle*>(parts), 5, ShapeSet, Unsorted);
}

void Viewfinder::setGeometry(const Rect& sel) {
    sel_    = sel;
    layout_ = layoutFor(sel_, x_.screenW(), x_.screenH());
    XMoveResizeWindow(x_.dpy(), win_,
                      layout_.win.x, layout_.win.y,
                      static_cast<unsigned>(layout_.win.w),
                      static_cast<unsigned>(layout_.win.h));
    applyShape();  // the shape is in window coords, so it must follow every resize
}

std::optional<Rect> Viewfinder::run() {
    std::optional<Rect> result;

    for (bool running = true; running; ) {
        XEvent e;
        XNextEvent(x_.dpy(), &e);

        switch (e.type) {
        case KeyPress: {
            const KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) {
                running = false;
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                result = sel_;
                running = false;
            }
            break;
        }
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                result = sel_;
                running = false;
            }
            break;
        default:
            break;
        }
    }

    teardown();
    return result;
}

} // namespace ss
