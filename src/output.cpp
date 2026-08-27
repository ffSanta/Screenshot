#include "output.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ss::output {
namespace fs = std::filesystem;
namespace {

fs::path homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    throw std::runtime_error("HOME is not set, cannot find the pictures folder");
}

// Expand the $HOME prefix that user-dirs.dirs writes its paths with.
fs::path expandHome(std::string v) {
    if (v.rfind("$HOME", 0) == 0) return homeDir() / fs::path(v.substr(6));
    if (v.rfind("~/", 0) == 0)    return homeDir() / fs::path(v.substr(2));
    return v;
}

// user-dirs.dirs holds lines like: XDG_PICTURES_DIR="$HOME/Pictures"
std::optional<fs::path> fromUserDirs() {
    std::ifstream in(homeDir() / ".config" / "user-dirs.dirs");
    if (!in) return std::nullopt;

    const std::string key = "XDG_PICTURES_DIR=";
    for (std::string line; std::getline(in, line); ) {
        const auto at = line.find(key);
        if (at == std::string::npos || line.find('#') < at) continue;
        std::string v = line.substr(at + key.size());
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        if (!v.empty()) return expandHome(v);
    }
    return std::nullopt;
}

std::string timestampedName() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "screenshot_%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

} // namespace

fs::path defaultDir() {
    if (const char* env = std::getenv("XDG_PICTURES_DIR"); env && *env)
        return expandHome(env) / "screenshot";
    if (auto p = fromUserDirs())
        return *p / "screenshot";
    return homeDir() / "Pictures" / "screenshot";
}

fs::path save(cairo_surface_t* surf, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir))
        throw std::runtime_error("cannot create " + dir.string() +
                                 (ec ? ": " + ec.message() : ""));

    const std::string stem = timestampedName();
    fs::path path = dir / (stem + ".png");
    // Two captures inside the same second would otherwise overwrite silently.
    for (int n = 1; fs::exists(path) && n < 1000; ++n)
        path = dir / (stem + "_" + std::to_string(n) + ".png");

    const cairo_status_t st = cairo_surface_write_to_png(surf, path.c_str());
    if (st != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error("cannot write " + path.string() + ": " +
                                 cairo_status_to_string(st));

    return path;
}

void openWith(const fs::path& path) {
    const pid_t first = fork();
    if (first < 0) return;              // nothing worth failing the save over

    if (first == 0) {
        // Intermediate child forks again and exits, so the grandchild is
        // orphaned onto init rather than lingering as our zombie.
        if (fork() == 0) {
            setsid();
            execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
            _exit(127);                 // exec failed; nothing else to do here
        }
        _exit(0);
    }

    int status = 0;
    waitpid(first, &status, 0);         // reaps immediately, grandchild lives on
}

} // namespace ss::output
