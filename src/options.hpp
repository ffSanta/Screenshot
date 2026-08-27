#pragma once

#include "geometry.hpp"
#include <filesystem>
#include <optional>

namespace ss {

struct Options {
    std::optional<std::filesystem::path> dir;       // --dir, overrides the XDG default
    std::optional<Rect>                  geometry;  // --geometry WxH+X+Y
    bool                                 open = false;  // --open
};

enum class ParseResult {
    Run,      // carry on
    ExitOk,   // --help / --version, nothing to do
    ExitErr,  // bad arguments, message already on stderr
};

ParseResult parseOptions(int argc, char** argv, Options& out);

} // namespace ss
