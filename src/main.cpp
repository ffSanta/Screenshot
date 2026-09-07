#include "capture.hpp"
#include "clipboard.hpp"
#include "options.hpp"
#include "output.hpp"
#include "viewfinder.hpp"
#include "xsession.hpp"

#include <cstdio>
#include <filesystem>
#include <exception>
#include <vector>

int main(int argc, char** argv) {
    ss::Options opt;
    switch (ss::parseOptions(argc, argv, opt)) {
        case ss::ParseResult::ExitOk:  return 0;
        case ss::ParseResult::ExitErr: return 2;
        case ss::ParseResult::Run:     break;
    }

    try {
        ss::XSession x;

        ss::Rect start = opt.geometry ? *opt.geometry : ss::defaultRect(x);
        if (opt.geometry && start.x < 0) {  // "WxH" with no offset: centre it
            start.x = (x.screenW() - start.w) / 2;
            start.y = (x.screenH() - start.h) / 2;
        }

        ss::Viewfinder vf(x, start);
        auto sel = vf.run();
        if (!sel) return 1;

        cairo_surface_t* img = ss::captureRegion(x, *sel);
        std::filesystem::path path;
        std::vector<unsigned char> png;
        try {
            path = ss::output::save(img, opt.dir ? *opt.dir : ss::output::defaultDir());
            // Encoded while the surface is still alive. Wrapped separately so a
            // clipboard problem never costs the file that is already on disk.
            if (opt.clipboard) {
                try {
                    png = ss::clipboard::encodePng(img);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "screenshot: clipboard: %s\n", e.what());
                }
            }
        } catch (...) {
            cairo_surface_destroy(img);
            throw;
        }
        cairo_surface_destroy(img);

        // Before the path is printed, so anything reading stdout can assume the
        // clipboard is already live. Bounded, so it cannot stall the print.
        if (!png.empty()) {
            const auto st = ss::clipboard::copyPng(x, png, path);
            if (st != ss::clipboard::CopyStatus::Ok)
                std::fprintf(stderr, "screenshot: clipboard: %s\n",
                             ss::clipboard::describe(st));
        }

        std::printf("%s\n", path.c_str());
        std::fflush(stdout);            // the path is useful even if xdg-open stalls

        if (opt.open) ss::output::openWith(path);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "screenshot: %s\n", e.what());
        return 2;
    }
}
