#pragma once

#include "xsession.hpp"

#include <cairo.h>
#include <filesystem>
#include <vector>

namespace ss::clipboard {

// PNG-encodes the surface into memory, the same cairo encoder output::save
// writes to disk with. Throws std::runtime_error if cairo cannot encode.
std::vector<unsigned char> encodePng(cairo_surface_t* surf);

enum class CopyStatus {
    Ok,          // a holder owns CLIPBOARD and has confirmed it
    ForkFailed,  // pipe or fork failed, nothing was published
    NoDisplay,   // the holder could not open its own X connection
    Refused,     // ownership did not stick
    Timeout,     // the holder never reported back; it may still come up
};

// Publishes the PNG on the CLIPBOARD selection as image/png, and `path` as the
// text flavours. An X11 selection is served by its owner - the bytes live in
// this process, not in the server - so this forks a detached holder that stays
// alive to answer paste requests. Returns once the holder confirms ownership,
// or after a short bounded wait.
//
// `x` is used only to close the parent's X socket inside the holder.
CopyStatus copyPng(const XSession& x,
               const std::vector<unsigned char>& png,
               const std::filesystem::path& path);

// One line for stderr, for anything other than Ok.
const char* describe(CopyStatus s);

} // namespace ss::clipboard
