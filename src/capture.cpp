#include "capture.hpp"

#include <X11/Xutil.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace ss {
namespace {

// XGetImage reads what is composited on screen right now, so the viewfinder
// has to be gone before we grab or it photographs itself. The caller unmaps
// it; this is how long we wait for that to actually land. An XSync alone is
// not enough under a compositor - picom fades windows out, so the frame is
// still partly on screen when the sync returns.
constexpr auto kSettleDelay = std::chrono::milliseconds(120);

// Number of low zero bits in a mask, i.e. how far to shift a channel down.
int maskShift(unsigned long mask) {
    int shift = 0;
    while (mask && !(mask & 1)) { mask >>= 1; ++shift; }
    return shift;
}

// Largest value a channel can hold, used to rescale odd depths up to 8 bits.
unsigned long maskMax(unsigned long mask) {
    return mask >> maskShift(mask);
}

} // namespace

cairo_surface_t* surfaceFromXImage(XImage* img, bool forceGenericPath) {
    const int w = img->width;
    const int h = img->height;

    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error("could not allocate the image surface");

    cairo_surface_flush(surf);
    unsigned char* dst = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);

    // On a normal 24-bit TrueColor display an XImage pixel is already laid
    // out exactly like CAIRO_FORMAT_RGB24, so whole rows can be copied. The
    // masks are checked rather than assumed, because that equivalence is a
    // property of this visual, not of X.
    const bool sameLayout = !forceGenericPath
                         && img->bits_per_pixel == 32
                         && img->byte_order == LSBFirst
                         && img->red_mask   == 0x00ff0000
                         && img->green_mask == 0x0000ff00
                         && img->blue_mask  == 0x000000ff;

    if (sameLayout) {
        const int bytes = w * 4;
        for (int y = 0; y < h; ++y)
            std::memcpy(dst + y * stride, img->data + y * img->bytes_per_line, bytes);
    } else {
        // Fallback for any other visual: pull each channel out by its mask.
        const int   rs = maskShift(img->red_mask);
        const int   gs = maskShift(img->green_mask);
        const int   bs = maskShift(img->blue_mask);
        const unsigned long rm = maskMax(img->red_mask);
        const unsigned long gm = maskMax(img->green_mask);
        const unsigned long bm = maskMax(img->blue_mask);

        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(dst + y * stride);
            for (int px = 0; px < w; ++px) {
                const unsigned long v = XGetPixel(img, px, y);
                const unsigned long rr = rm ? ((v & img->red_mask)   >> rs) * 255 / rm : 0;
                const unsigned long gg = gm ? ((v & img->green_mask) >> gs) * 255 / gm : 0;
                const unsigned long bb = bm ? ((v & img->blue_mask)  >> bs) * 255 / bm : 0;
                row[px] = static_cast<uint32_t>((rr << 16) | (gg << 8) | bb);
            }
        }
    }

    cairo_surface_mark_dirty(surf);
    return surf;
}

cairo_surface_t* captureRegion(const XSession& x, const Rect& r) {
    std::this_thread::sleep_for(kSettleDelay);

    XImage* img = XGetImage(x.dpy(), x.root(), r.x, r.y,
                            static_cast<unsigned>(r.w),
                            static_cast<unsigned>(r.h),
                            AllPlanes, ZPixmap);
    if (!img) throw std::runtime_error("XGetImage failed to read the screen");

    cairo_surface_t* surf = nullptr;
    try {
        surf = surfaceFromXImage(img);
    } catch (...) {
        XDestroyImage(img);
        throw;
    }
    XDestroyImage(img);   // the pixels are ours now, nothing aliases X memory
    return surf;
}

} // namespace ss
