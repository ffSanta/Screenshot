#pragma once

#include <cairo.h>
#include <filesystem>
#include <optional>

namespace ss::output {

// Where screenshots go when --dir is not given: the XDG pictures directory
// with a "screenshot" subfolder, falling back to ~/Pictures/screenshot.
std::filesystem::path defaultDir();

// Writes the surface as a PNG into `dir`, creating the directory if needed,
// under a timestamped name. Returns the path written.
// Throws std::runtime_error if the directory or the file cannot be written.
std::filesystem::path save(cairo_surface_t* surf,
                           const std::filesystem::path& dir);

// Hands the file to xdg-open and returns immediately. Double-forks so the
// viewer is reparented to init and outlives us, and so we never leave a
// zombie behind for a process we are not going to wait on.
void openWith(const std::filesystem::path& path);

} // namespace ss::output
