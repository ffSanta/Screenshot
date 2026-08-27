#pragma once

// Pure geometry for the viewfinder: the selection rectangle, the window
// layout derived from it, and which part of the frame the pointer is over.
// Header-only and free of any X11 dependency so it stays easy to reason about.

#include <algorithm>

namespace ss {

inline constexpr int BORDER    = 6;   // frame thickness around the selection
inline constexpr int HANDLE    = 16;  // corner grab zone, measured along each edge
inline constexpr int TOOLBAR_H = 34;  // height of the Save/Cancel strip
inline constexpr int MIN_W     = 20;  // smallest selection we allow
inline constexpr int MIN_H     = 20;

inline constexpr int BTN_W   = 78;
inline constexpr int BTN_H   = 22;
inline constexpr int BTN_GAP = 8;
inline constexpr int PAD     = 10;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    constexpr int right()  const { return x + w; }
    constexpr int bottom() const { return y + h; }

    constexpr bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

enum class Zone {
    None, Move, N, S, E, W, NE, NW, SE, SW, SaveBtn, CancelBtn
};

constexpr bool isResize(Zone z) {
    return z == Zone::N || z == Zone::S || z == Zone::E || z == Zone::W
        || z == Zone::NE || z == Zone::NW || z == Zone::SE || z == Zone::SW;
}

// Everything the window needs to position and paint itself for a given
// selection. Root-coordinate rects describe where things sit on screen;
// *Local rects are relative to the window's own origin, which is what both
// cairo and XShape want.
struct Layout {
    Rect sel;                    // the region that will actually be captured
    Rect win;                    // whole window, root coords
    bool toolbarOnTop = false;   // flipped up when there is no room below
    Rect frameLocal;             // border box (selection + BORDER on all sides)
    Rect holeLocal;              // the see-through part, == sel in local coords
    Rect toolbarLocal;
    Rect saveBtn;
    Rect cancelBtn;
};

// The window wraps the selection in a border and hangs a toolbar off it. The
// toolbar normally sits below; when the selection is close enough to the
// bottom of the screen that the toolbar would be pushed off, it flips above.
inline Layout layoutFor(const Rect& sel, int screenW, int screenH) {
    Layout L;
    L.sel = sel;

    const int frameW = sel.w + 2 * BORDER;
    const int frameH = sel.h + 2 * BORDER;

    L.win.w = frameW;
    L.win.h = frameH + TOOLBAR_H;
    L.win.x = sel.x - BORDER;

    const int below = sel.y - BORDER;              // window top if toolbar below
    const int above = sel.y - BORDER - TOOLBAR_H;  // window top if toolbar above

    // Prefer below, flip up only if that runs off the bottom *and* there is
    // room up top. Otherwise accept the clip rather than hiding the buttons.
    L.toolbarOnTop = (below + L.win.h > screenH) && (above >= 0);
    L.win.y = L.toolbarOnTop ? above : below;

    L.frameLocal   = { 0, L.toolbarOnTop ? TOOLBAR_H : 0, frameW, frameH };
    L.holeLocal    = { BORDER, L.frameLocal.y + BORDER, sel.w, sel.h };
    L.toolbarLocal = { 0, L.toolbarOnTop ? 0 : frameH, frameW, TOOLBAR_H };

    // Buttons hug the right edge of the toolbar: [ Cancel ][ Save ]
    const int by = L.toolbarLocal.y + (TOOLBAR_H - BTN_H) / 2;
    L.saveBtn   = { frameW - PAD - BTN_W, by, BTN_W, BTN_H };
    L.cancelBtn = { L.saveBtn.x - BTN_GAP - BTN_W, by, BTN_W, BTN_H };

    (void)screenW;
    return L;
}

// Point is in window-local coordinates. Buttons win over the toolbar, the
// toolbar's empty space moves the window, and the border resizes it with
// corners taking priority over edges.
inline Zone hitTest(const Layout& L, int lx, int ly) {
    if (L.saveBtn.contains(lx, ly))   return Zone::SaveBtn;
    if (L.cancelBtn.contains(lx, ly)) return Zone::CancelBtn;
    if (L.toolbarLocal.contains(lx, ly)) return Zone::Move;

    if (!L.frameLocal.contains(lx, ly)) return Zone::None;
    if (L.holeLocal.contains(lx, ly))   return Zone::None;  // shaped away anyway

    const int fx = lx - L.frameLocal.x;
    const int fy = ly - L.frameLocal.y;

    // Corner reach is capped so it never swallows a whole short edge.
    const int reachX = std::min(HANDLE, L.frameLocal.w / 3);
    const int reachY = std::min(HANDLE, L.frameLocal.h / 3);

    const bool L_ = fx < reachX;
    const bool R_ = fx >= L.frameLocal.w - reachX;
    const bool T_ = fy < reachY;
    const bool B_ = fy >= L.frameLocal.h - reachY;

    if (T_ && L_) return Zone::NW;
    if (T_ && R_) return Zone::NE;
    if (B_ && L_) return Zone::SW;
    if (B_ && R_) return Zone::SE;

    if (fy < BORDER)                      return Zone::N;
    if (fy >= L.frameLocal.h - BORDER)    return Zone::S;
    if (fx < BORDER)                      return Zone::W;
    if (fx >= L.frameLocal.w - BORDER)    return Zone::E;
    return Zone::Move;
}

// Produce the new selection for a drag of (dx, dy) from the rect the drag
// started on. Keeps the result on screen and no smaller than MIN_W x MIN_H.
inline Rect applyDrag(Zone z, const Rect& anchor, int dx, int dy,
                      int screenW, int screenH) {
    if (z == Zone::Move) {
        Rect n = anchor;
        n.x = std::clamp(anchor.x + dx, 0, std::max(0, screenW - anchor.w));
        n.y = std::clamp(anchor.y + dy, 0, std::max(0, screenH - anchor.h));
        return n;
    }
    if (!isResize(z)) return anchor;

    const bool movesL = (z == Zone::W || z == Zone::NW || z == Zone::SW);
    const bool movesR = (z == Zone::E || z == Zone::NE || z == Zone::SE);
    const bool movesT = (z == Zone::N || z == Zone::NW || z == Zone::NE);
    const bool movesB = (z == Zone::S || z == Zone::SW || z == Zone::SE);

    int l = anchor.x, t = anchor.y, r = anchor.right(), b = anchor.bottom();
    if (movesL) l += dx;
    if (movesR) r += dx;
    if (movesT) t += dy;
    if (movesB) b += dy;

    l = std::clamp(l, 0, screenW);
    r = std::clamp(r, 0, screenW);
    t = std::clamp(t, 0, screenH);
    b = std::clamp(b, 0, screenH);

    // Honour the minimum by pushing back whichever edge the user is dragging.
    if (r - l < MIN_W) { if (movesL) l = r - MIN_W; else r = l + MIN_W; }
    if (b - t < MIN_H) { if (movesT) t = b - MIN_H; else b = t + MIN_H; }

    if (l < 0) { r -= l; l = 0; }
    if (t < 0) { b -= t; t = 0; }

    return { l, t, r - l, b - t };
}

// Nudge or grow the selection with the keyboard. Resizing pulls on the
// bottom-right corner so the origin stays put.
inline Rect applyKey(bool resize, int dx, int dy, const Rect& cur,
                     int screenW, int screenH) {
    if (!resize) return applyDrag(Zone::Move, cur, dx, dy, screenW, screenH);
    Rect n = cur;
    n.w = std::clamp(cur.w + dx, MIN_W, screenW - cur.x);
    n.h = std::clamp(cur.h + dy, MIN_H, screenH - cur.y);
    return n;
}

// X cursor font names, looked up through Xcursor so themes are respected.
inline const char* cursorName(Zone z) {
    switch (z) {
        case Zone::NW: return "top_left_corner";
        case Zone::NE: return "top_right_corner";
        case Zone::SW: return "bottom_left_corner";
        case Zone::SE: return "bottom_right_corner";
        case Zone::N:  return "top_side";
        case Zone::S:  return "bottom_side";
        case Zone::W:  return "left_side";
        case Zone::E:  return "right_side";
        case Zone::Move: return "fleur";
        default: return "left_ptr";
    }
}

// Clamp an arbitrary requested rect (e.g. from --geometry) onto the screen.
inline Rect clampToScreen(Rect r, int screenW, int screenH) {
    r.w = std::clamp(r.w, MIN_W, screenW);
    r.h = std::clamp(r.h, MIN_H, screenH);
    r.x = std::clamp(r.x, 0, screenW - r.w);
    r.y = std::clamp(r.y, 0, screenH - r.h);
    return r;
}

} // namespace ss
