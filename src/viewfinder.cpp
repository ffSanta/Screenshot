#include "viewfinder.hpp"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xcursor/Xcursor.h>
#include <cairo-xlib.h>
#include "theme.hpp"

#include <algorithm>
#include <cstdio>
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
                 | PointerMotionMask | KeyPressMask | StructureNotifyMask
                 | EnterWindowMask | LeaveWindowMask;

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

    surf_ = cairo_xlib_surface_create(x_.dpy(), win_, x_.visual(),
                                      layout_.win.w, layout_.win.h);
    if (cairo_surface_status(surf_) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error("failed to create the cairo drawing surface");

    XMapRaised(x_.dpy(), win_);

    // Override-redirect windows get no focus from the WM, so take the
    // keyboard directly or the key bindings would never fire.
    XGrabKeyboard(x_.dpy(), win_, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XSync(x_.dpy(), False);
}

Viewfinder::~Viewfinder() {
    teardown();
    if (surf_) {
        cairo_surface_destroy(surf_);
        surf_ = nullptr;
    }
    for (auto& [zone, cur] : cursors_)
        if (cur) XFreeCursor(x_.dpy(), cur);
    cursors_.clear();
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


namespace {

void fillRect(cairo_t* cr, const Rect& r) {
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

} // namespace

void Viewfinder::drawFrame(cairo_t* cr) {
    const Rect& f = layout_.frameLocal;

    theme::set(cr, theme::base);
    if (!x_.hasShape()) {
        fillRect(cr, f);            // no hole to preserve, so fill it flat
    } else {
        fillRect(cr, { f.x, f.y, f.w, BORDER });
        fillRect(cr, { f.x, f.bottom() - BORDER, f.w, BORDER });
        fillRect(cr, { f.x, f.y + BORDER, BORDER, f.h - 2 * BORDER });
        fillRect(cr, { f.right() - BORDER, f.y + BORDER, BORDER, f.h - 2 * BORDER });
    }

    // A hairline right on the hole edge so the capture boundary is exact.
    const Rect& h = layout_.holeLocal;
    theme::set(cr, theme::blue);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, h.x - 0.5, h.y - 0.5, h.w + 1.0, h.h + 1.0);
    cairo_stroke(cr);
}

// Blue marks showing where the eight resize grips are: an L at each corner
// and a short bar at each edge midpoint. The hovered one lights up.
void Viewfinder::drawHandles(cairo_t* cr) {
    const Rect& f = layout_.frameLocal;
    constexpr int kLeg = 22;   // corner arm length
    constexpr int kBar = 30;   // edge bar length

    auto mark = [&](Zone z, const Rect& a, const Rect& b) {
        theme::set(cr, hover_ == z ? theme::sky : theme::blue);
        fillRect(cr, a);
        if (b.w > 0 && b.h > 0) fillRect(cr, b);
    };

    const int l = f.x, t = f.y, r = f.right(), b = f.bottom();
    const int legX = std::min(kLeg, f.w / 3);
    const int legY = std::min(kLeg, f.h / 3);

    mark(Zone::NW, { l, t, legX, BORDER }, { l, t, BORDER, legY });
    mark(Zone::NE, { r - legX, t, legX, BORDER }, { r - BORDER, t, BORDER, legY });
    mark(Zone::SW, { l, b - BORDER, legX, BORDER }, { l, b - legY, BORDER, legY });
    mark(Zone::SE, { r - legX, b - BORDER, legX, BORDER },
                   { r - BORDER, b - legY, BORDER, legY });

    const int barW = std::min(kBar, std::max(0, f.w - 2 * legX - 8));
    const int barH = std::min(kBar, std::max(0, f.h - 2 * legY - 8));
    if (barW > 0) {
        mark(Zone::N, { l + (f.w - barW) / 2, t, barW, BORDER }, {});
        mark(Zone::S, { l + (f.w - barW) / 2, b - BORDER, barW, BORDER }, {});
    }
    if (barH > 0) {
        mark(Zone::W, { l, t + (f.h - barH) / 2, BORDER, barH }, {});
        mark(Zone::E, { r - BORDER, t + (f.h - barH) / 2, BORDER, barH }, {});
    }
}

void Viewfinder::drawButton(cairo_t* cr, const Rect& r, const char* label,
                            bool primary, bool hovered, bool pressed) {
    theme::Color bg = primary ? (hovered ? theme::teal : theme::green)
                              : (hovered ? theme::surface2 : theme::surface1);
    if (pressed) { bg.r *= 0.82; bg.g *= 0.82; bg.b *= 0.82; }

    theme::set(cr, bg);
    theme::roundedRect(cr, r.x, r.y, r.w, r.h, 4.0);
    cairo_fill(cr);

    theme::set(cr, primary ? theme::crust : theme::text);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);

    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_move_to(cr, r.x + (r.w - te.width) / 2.0 - te.x_bearing,
                      r.y + (r.h - te.height) / 2.0 - te.y_bearing);
    cairo_show_text(cr, label);
}

void Viewfinder::drawToolbar(cairo_t* cr) {
    const Rect& t = layout_.toolbarLocal;

    theme::set(cr, theme::mantle);
    fillRect(cr, t);

    // Separator on whichever side faces the frame.
    theme::set(cr, theme::surface1);
    fillRect(cr, { t.x, layout_.toolbarOnTop ? t.bottom() - 1 : t.y, t.w, 1 });

    // Grip dots hinting that the toolbar is what you drag to move the frame.
    theme::set(cr, hover_ == Zone::Move ? theme::subtext : theme::surface2);
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row)
            fillRect(cr, { PAD + col * 4, t.y + TOOLBAR_H / 2 - 5 + row * 4, 2, 2 });

    // Live readout: the exact pixel size and origin that will be captured.
    char size[64];
    std::snprintf(size, sizeof size, "%d \xc3\x97 %d", sel_.w, sel_.h);
    char at[64];
    std::snprintf(at, sizeof at, "at %d, %d", sel_.x, sel_.y);

    const double ty = t.y + TOOLBAR_H / 2.0 + 4.0;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.5);
    theme::set(cr, theme::text);
    cairo_move_to(cr, PAD + 16, ty);
    cairo_show_text(cr, size);

    cairo_text_extents_t te;
    cairo_text_extents(cr, size, &te);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.5);
    theme::set(cr, theme::subtext);
    const double atX = PAD + 16 + te.x_advance + 10;
    if (atX + 70 < layout_.cancelBtn.x) {   // hide it rather than collide
        cairo_move_to(cr, atX, ty);
        cairo_show_text(cr, at);
    }

    drawButton(cr, layout_.cancelBtn, "Cancel", false,
               hover_ == Zone::CancelBtn, pressed_ == Zone::CancelBtn);
    drawButton(cr, layout_.saveBtn, "Save", true,
               hover_ == Zone::SaveBtn, pressed_ == Zone::SaveBtn);
}

void Viewfinder::draw() {
    if (!surf_) return;
    cairo_t* cr = cairo_create(surf_);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    drawFrame(cr);
    drawHandles(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    drawToolbar(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf_);
    XFlush(x_.dpy());
}

void Viewfinder::setGeometry(const Rect& sel) {
    sel_    = sel;
    layout_ = layoutFor(sel_, x_.screenW(), x_.screenH());
    XMoveResizeWindow(x_.dpy(), win_,
                      layout_.win.x, layout_.win.y,
                      static_cast<unsigned>(layout_.win.w),
                      static_cast<unsigned>(layout_.win.h));
    applyShape();  // the shape is in window coords, so it must follow every resize
    cairo_xlib_surface_set_size(surf_, layout_.win.w, layout_.win.h);
    draw();
}


Cursor Viewfinder::cursorFor(Zone z) {
    const int key = static_cast<int>(z);
    if (auto it = cursors_.find(key); it != cursors_.end()) return it->second;
    // Xcursor honours the user's cursor theme; a null result just means the
    // theme lacks that shape, in which case we inherit the default pointer.
    Cursor c = XcursorLibraryLoadCursor(x_.dpy(), cursorName(z));
    cursors_.emplace(key, c);
    return c;
}

void Viewfinder::setHover(Zone z) {
    if (z == hover_) return;
    hover_ = z;
    if (cursorZone_ != z) {
        cursorZone_ = z;
        XDefineCursor(x_.dpy(), win_, cursorFor(z));
    }
    draw();
}

void Viewfinder::onButtonPress(const XButtonEvent& e) {
    if (e.button != Button1) return;

    const Zone z = hitTest(layout_, e.x, e.y);
    if (z == Zone::SaveBtn || z == Zone::CancelBtn) {
        pressed_ = z;
        draw();
        return;
    }
    if (z != Zone::Move && !isResize(z)) return;

    dragZone_    = z;
    anchor_      = sel_;
    anchorRootX_ = e.x_root;
    anchorRootY_ = e.y_root;

    // Grab so the drag keeps tracking once the pointer leaves the thin border
    // - without this a fast drag would simply stop following the mouse.
    XGrabPointer(x_.dpy(), win_, True,
                 ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, cursorFor(z), CurrentTime);
}

void Viewfinder::onMotion(const XMotionEvent& e) {
    if (dragZone_ != Zone::Outside) {
        const Rect next = applyDrag(dragZone_, anchor_,
                                    e.x_root - anchorRootX_,
                                    e.y_root - anchorRootY_,
                                    x_.screenW(), x_.screenH());
        if (next.x != sel_.x || next.y != sel_.y ||
            next.w != sel_.w || next.h != sel_.h) {
            setGeometry(next);
        }
        return;
    }
    setHover(hitTest(layout_, e.x, e.y));
}

void Viewfinder::onButtonRelease(const XButtonEvent& e) {
    if (e.button != Button1) return;

    if (dragZone_ != Zone::Outside) {
        dragZone_ = Zone::Outside;
        XUngrabPointer(x_.dpy(), CurrentTime);
        setHover(hitTest(layout_, e.x, e.y));
        return;
    }

    if (pressed_ != Zone::Outside) {
        // Only fire if the release lands on the same button, so dragging off
        // a button cancels the click the way every other toolbar behaves.
        const Zone z = hitTest(layout_, e.x, e.y);
        const Zone was = pressed_;
        pressed_ = Zone::Outside;
        if (z == was) {
            if (was == Zone::SaveBtn) result_ = sel_;
            running_ = false;
        } else {
            draw();
        }
    }
}


void Viewfinder::onKeyPress(const XKeyEvent& e) {
    const bool ctrl  = (e.state & ControlMask) != 0;
    const bool shift = (e.state & ShiftMask) != 0;

    // Unshifted lookup throughout: the keys we bind (arrows, Escape, Return,
    // S) have no shifted variant, and Shift is read from the modifier mask
    // instead, where it selects resize rather than move.
    const KeySym ks = XLookupKeysym(const_cast<XKeyEvent*>(&e), 0);

    if (ks == XK_Escape) { running_ = false; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        result_ = sel_;
        running_ = false;
        return;
    }
    if (ctrl && ks == XK_s) {
        result_ = sel_;
        running_ = false;
        return;
    }

    int dx = 0, dy = 0;
    switch (ks) {
        case XK_Left:  dx = -1; break;
        case XK_Right: dx =  1; break;
        case XK_Up:    dy = -1; break;
        case XK_Down:  dy =  1; break;
        default:
            keyRepeats_ = 0;   // any other key breaks the run
            return;
    }

    // A held arrow ramps up; changing direction or mode, or pausing longer
    // than one auto-repeat interval, drops back to single pixels.
    const bool continues = dx == keyDx_ && dy == keyDy_ && shift == keyResize_
                        && e.time >= keyTime_
                        && e.time - keyTime_ <= KEY_REPEAT_GAP_MS;
    keyRepeats_ = continues ? keyRepeats_ + 1 : 0;
    keyDx_      = dx;
    keyDy_      = dy;
    keyResize_  = shift;
    keyTime_    = e.time;

    const int step = accelStep(ctrl ? 10 : 1, keyRepeats_);
    const Rect next = applyKey(shift, dx * step, dy * step, sel_,
                               x_.screenW(), x_.screenH());
    if (next.x != sel_.x || next.y != sel_.y ||
        next.w != sel_.w || next.h != sel_.h) {
        setGeometry(next);
    }
}

std::optional<Rect> Viewfinder::run() {
    XDefineCursor(x_.dpy(), win_, cursorFor(Zone::Outside));

    while (running_) {
        XEvent e;
        XNextEvent(x_.dpy(), &e);

        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0) draw();
            break;

        case MotionNotify: {
            // Collapse the queued motion events and act on the newest only,
            // otherwise a fast drag falls behind the pointer redrawing stale
            // positions.
            XEvent m = e;
            while (XCheckTypedWindowEvent(x_.dpy(), win_, MotionNotify, &m)) {}
            onMotion(m.xmotion);
            break;
        }

        case ButtonPress:
            onButtonPress(e.xbutton);
            break;

        case ButtonRelease:
            onButtonRelease(e.xbutton);
            break;

        case LeaveNotify:
            if (dragZone_ == Zone::Outside) setHover(Zone::Outside);
            break;

        case KeyPress:
            onKeyPress(e.xkey);
            break;

        default:
            break;
        }
    }

    teardown();
    return result_;
}

} // namespace ss
