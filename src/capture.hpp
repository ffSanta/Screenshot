#pragma once

#include "geometry.hpp"
#include "xsession.hpp"

#include <X11/Xlib.h>
#include <cairo.h>

namespace ss {

// Converts a grabbed XImage into a cairo image surface, copying the pixels so
// nothing aliases X-owned memory. `forceGenericPath` skips the fast row-copy
// and always goes through the mask arithmetic; it exists so the two paths can
// be tested against each other on hardware that only has one of them.
// The caller owns the result.
cairo_surface_t* surfaceFromXImage(XImage* img, bool forceGenericPath = false);

// Grabs a region of the root window into a fresh cairo image surface.
// The caller owns the result and must cairo_surface_destroy() it.
// Throws std::runtime_error if the grab fails.
cairo_surface_t* captureRegion(const XSession& x, const Rect& r);

} // namespace ss
