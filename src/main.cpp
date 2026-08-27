#include "capture.hpp"
#include "options.hpp"
#include "viewfinder.hpp"
#include "xsession.hpp"

#include <cstdio>
#include <exception>

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
        std::printf("captured %dx%d at %d,%d\n",
                    cairo_image_surface_get_width(img),
                    cairo_image_surface_get_height(img),
                    sel->x, sel->y);
        cairo_surface_destroy(img);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "screenshot: %s\n", e.what());
        return 2;
    }
}
