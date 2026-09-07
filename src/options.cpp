#include "options.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace ss {
namespace {

constexpr const char* kVersion = "screenshot 1.0.0";

void usage(std::FILE* to) {
    std::fprintf(to,
        "Usage: screenshot [OPTIONS]\n"
        "\n"
        "Opens a resizable viewfinder frame with a see-through middle. Position\n"
        "it over what you want, then click Save.\n"
        "\n"
        "Options:\n"
        "  --dir <path>         Save here instead of ~/Pictures/screenshot\n"
        "  --geometry WxH+X+Y   Starting frame, e.g. 640x480+100+100 (offset optional)\n"
        "  --no-clipboard       Do not copy the image to the clipboard\n"
        "  --open               Open the saved image afterwards with xdg-open\n"
        "  -h, --help           Show this help\n"
        "  -V, --version        Show version\n"
        "\n"
        "Mouse:\n"
        "  Drag the border to resize, drag the toolbar to move.\n"
        "\n"
        "Keyboard:\n"
        "  Esc                  Cancel\n"
        "  Enter, Ctrl+S        Save\n"
        "  Arrows               Move 1px       Ctrl+Arrows        Move 10px\n"
        "  Shift+Arrows         Resize 1px     Ctrl+Shift+Arrows  Resize 10px\n"
        "  Holding an arrow accelerates, up to 128px per step.\n"
        "\n"
        "Prints the saved path to stdout. Exit 0 on save, 1 on cancel.\n"
        "\n"
        "The image also goes on the clipboard. An X11 selection is served by\n"
        "the process that owns it, so a small holder stays behind until the\n"
        "image is pasted once, or until something else takes the clipboard.\n");
}

// Accepts "WxH" or "WxH+X+Y". Rejects trailing junk so typos are not silently
// half-applied.
bool parseGeometry(const char* s, Rect& out) {
    int w = 0, h = 0, x = 0, y = 0, n = 0;
    if (std::sscanf(s, "%dx%d+%d+%d%n", &w, &h, &x, &y, &n) == 4 && s[n] == '\0') {
        out = { x, y, w, h };
        return w > 0 && h > 0;
    }
    n = 0;
    if (std::sscanf(s, "%dx%d%n", &w, &h, &n) == 2 && s[n] == '\0') {
        out = { -1, -1, w, h };  // -1 means "centre it"
        return w > 0 && h > 0;
    }
    return false;
}

} // namespace

ParseResult parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        auto needsValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "screenshot: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            usage(stdout);
            return ParseResult::ExitOk;
        }
        if (!std::strcmp(a, "-V") || !std::strcmp(a, "--version")) {
            std::puts(kVersion);
            return ParseResult::ExitOk;
        }
        if (!std::strcmp(a, "--open")) {
            out.open = true;
        } else if (!std::strcmp(a, "--no-clipboard")) {
            out.clipboard = false;
        } else if (!std::strcmp(a, "--dir")) {
            const char* v = needsValue("--dir");
            if (!v) return ParseResult::ExitErr;
            out.dir = std::filesystem::path(v);
        } else if (!std::strcmp(a, "--geometry")) {
            const char* v = needsValue("--geometry");
            if (!v) return ParseResult::ExitErr;
            Rect r{};
            if (!parseGeometry(v, r)) {
                std::fprintf(stderr,
                    "screenshot: bad --geometry '%s', expected WxH or WxH+X+Y\n", v);
                return ParseResult::ExitErr;
            }
            out.geometry = r;
        } else {
            std::fprintf(stderr, "screenshot: unknown option '%s'\n", a);
            usage(stderr);
            return ParseResult::ExitErr;
        }
    }
    return ParseResult::Run;
}

} // namespace ss
