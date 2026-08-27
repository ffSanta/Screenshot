#include "options.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    ss::Options opt;
    switch (ss::parseOptions(argc, argv, opt)) {
        case ss::ParseResult::ExitOk:  return 0;
        case ss::ParseResult::ExitErr: return 2;
        case ss::ParseResult::Run:     break;
    }
    std::puts("screenshot: viewfinder not implemented yet");
    return 0;
}
