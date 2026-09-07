#include "clipboard.hpp"

#include <X11/Xatom.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace ss::clipboard {
namespace {

using Clock = std::chrono::steady_clock;

// How long the parent waits for the holder to say it owns the clipboard. The
// wait exists to close the race where a very fast Ctrl+V beats ownership; it
// is bounded because a stuck holder must never hold up the saved path.
constexpr int kHandshakeMs = 2000;

// Above this, a transfer goes out through INCR. The server here would take a
// single 16 MB request, but keeping the threshold low means an ordinary
// full-screen capture exercises the INCR path instead of leaving it untested.
constexpr std::size_t kDirectMax = 256u * 1024u;

// A requestor that stops collecting an INCR transfer must not pin us forever.
constexpr auto kTransferIdle = std::chrono::seconds(10);

// A screenshot nobody ever pastes should not leave a process behind for the
// rest of the session.
constexpr auto kIdleLifetime = std::chrono::minutes(15);

cairo_status_t appendPng(void* closure, const unsigned char* data, unsigned int len) {
    auto* out = static_cast<std::vector<unsigned char>*>(closure);
    out->insert(out->end(), data, data + len);
    return CAIRO_STATUS_SUCCESS;
}

struct Atoms {
    Atom clipboard, targets, timestamp, incr;
    Atom imagePng, imageXPng, uriList, utf8, text;
    Atom probe;
};

Atoms internAtoms(Display* d) {
    Atoms a{};
    a.clipboard = XInternAtom(d, "CLIPBOARD",   False);
    a.targets   = XInternAtom(d, "TARGETS",     False);
    a.timestamp = XInternAtom(d, "TIMESTAMP",   False);
    a.incr      = XInternAtom(d, "INCR",        False);
    a.imagePng  = XInternAtom(d, "image/png",   False);
    a.imageXPng = XInternAtom(d, "image/x-png", False);
    a.uriList   = XInternAtom(d, "text/uri-list", False);
    a.utf8      = XInternAtom(d, "UTF8_STRING", False);
    a.text      = XInternAtom(d, "TEXT",        False);
    a.probe     = XInternAtom(d, "SCREENSHOT_TIME_PROBE", False);
    return a;
}

// What the holder serves. The PNG is not copied: the holder is a forked child,
// so it reads the parent's buffer through copy-on-write.
struct Payload {
    const std::vector<unsigned char>* png = nullptr;
    std::string uri;   // file:///home/... for text/uri-list
    std::string path;  // the bare path for the string flavours
};

// One INCR transfer in flight. Several can overlap - two apps can paste at
// once - so these are driven from the shared event loop rather than from a
// blocking inner loop.
struct Transfer {
    Window               requestor = 0;
    Atom                 property  = None;
    Atom                 type      = None;
    const unsigned char* data      = nullptr;
    std::size_t          size      = 0;
    std::size_t          sent      = 0;
    bool                 image     = false;
    Clock::time_point    last{};
};

// A requestor that dies mid-transfer turns our next request into a BadWindow.
// That is expected, not fatal.
int swallowError(Display*, XErrorEvent*) { return 0; }

// The X server going away is the one error we cannot continue past.
int fatalIoError(Display*) { _exit(0); return 0; }

Bool isPropertyNotify(Display*, XEvent* e, XPointer arg) {
    return e->type == PropertyNotify &&
           e->xproperty.window == *reinterpret_cast<Window*>(arg);
}

class Holder {
public:
    Holder(Display* d, const Payload& p) : d_(d), a_(internAtoms(d)), pay_(p) {}

    bool acquire();
    void serve();

private:
    Time  serverTime();
    void  onRequest(const XSelectionRequestEvent& req);
    void  reply(const XSelectionRequestEvent& req, Atom property);
    bool  deliver(const XSelectionRequestEvent& req, Atom prop, Atom type,
                  const unsigned char* data, std::size_t size, bool image);
    bool  pump(Transfer& t);
    void  dropTransfers(Window w);
    void  reapStalled();

    Display*              d_;
    Atoms                 a_;
    const Payload&        pay_;
    Window                win_    = 0;
    Time                  owned_  = CurrentTime;
    std::size_t           chunk_  = kDirectMax;
    bool                  done_   = false;   // the image has been handed over
    bool                  running_ = true;
    Clock::time_point     lastAct_ = Clock::now();
    std::vector<Transfer> xfers_;
};

// A zero-length append on our own window comes back as a PropertyNotify
// carrying the server's current time. ICCCM forbids taking a selection with
// CurrentTime: the owner then has nothing truthful to answer TIMESTAMP with.
Time Holder::serverTime() {
    XChangeProperty(d_, win_, a_.probe, XA_STRING, 8, PropModeAppend, nullptr, 0);
    XEvent e;
    XIfEvent(d_, &e, isPropertyNotify, reinterpret_cast<XPointer>(&win_));
    return e.xproperty.time;
}

bool Holder::acquire() {
    XSetWindowAttributes at{};
    at.override_redirect = True;
    at.event_mask        = PropertyChangeMask;   // needed for the time probe
    // InputOnly and never mapped: a selection owner only needs a window id.
    win_ = XCreateWindow(d_, DefaultRootWindow(d_), -10, -10, 1, 1, 0,
                         0, InputOnly, CopyFromParent,
                         CWOverrideRedirect | CWEventMask, &at);
    if (!win_) return false;

    owned_ = serverTime();
    XSetSelectionOwner(d_, a_.clipboard, win_, owned_);
    // Round-trips, so it both flushes and tells us whether it stuck.
    if (XGetSelectionOwner(d_, a_.clipboard) != win_) return false;

    // Both calls report the limit in 4-byte units; XExtendedMaxRequestSize is
    // the BIG-REQUESTS value and is 0 when the extension is missing. 64 bytes
    // covers the ChangeProperty header and the 4-byte padding.
    long units = XExtendedMaxRequestSize(d_);
    if (units <= 0) units = XMaxRequestSize(d_);
    std::size_t limit = static_cast<std::size_t>(units) * 4;
    limit = limit > 64 ? limit - 64 : 4096;
    chunk_ = std::min(limit, kDirectMax);

    // Escape hatch for exercising INCR with a small image.
    if (const char* v = std::getenv("SCREENSHOT_CLIPBOARD_CHUNK")) {
        const long n = std::strtol(v, nullptr, 10);
        if (n >= 256) chunk_ = std::min(chunk_, static_cast<std::size_t>(n));
    }
    return true;
}

void Holder::reply(const XSelectionRequestEvent& req, Atom property) {
    XSelectionEvent n{};
    n.type      = SelectionNotify;
    n.display   = d_;
    n.requestor = req.requestor;
    n.selection = req.selection;
    n.target    = req.target;
    n.property  = property;   // None means refused
    n.time      = req.time;   // echo the request's time, never CurrentTime
    XSendEvent(d_, req.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&n));
    XFlush(d_);
}

// Returns true when the whole payload has reached the requestor.
bool Holder::deliver(const XSelectionRequestEvent& req, Atom prop, Atom type,
                     const unsigned char* data, std::size_t size, bool image) {
    if (size <= chunk_) {
        XChangeProperty(d_, req.requestor, prop, type, 8, PropModeReplace,
                        data, static_cast<int>(size));
        reply(req, prop);
        XSync(d_, False);   // the bytes are on their window before we can exit
        return true;
    }

    // INCR. The event mask has to be in place before the reply, or the first
    // "send me the next chunk" PropertyNotify can arrive before we are looking
    // for it. Masks are per-client, so the requestor's own is untouched.
    XSelectInput(d_, req.requestor, PropertyChangeMask | StructureNotifyMask);
    const long lower = static_cast<long>(size);   // a lower bound, not a promise
    XChangeProperty(d_, req.requestor, prop, a_.incr, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&lower), 1);
    reply(req, prop);

    xfers_.push_back(Transfer{req.requestor, prop, type, data, size, 0, image,
                              Clock::now()});
    return false;
}

void Holder::onRequest(const XSelectionRequestEvent& req) {
    // property == None is the pre-ICCCM form; the convention is to use the
    // target as the property.
    const Atom prop = req.property != None ? req.property : req.target;

    if (req.selection != a_.clipboard) { reply(req, None); return; }

    if (req.target == a_.targets) {
        // Image first, so apps that honour the order paste the picture rather
        // than the path. MULTIPLE is deliberately absent: no image-paste path
        // in xclip, GTK, Qt or Chromium uses it.
        const Atom list[] = { a_.imagePng, a_.imageXPng, a_.targets, a_.timestamp,
                              a_.uriList, a_.utf8, XA_STRING, a_.text };
        XChangeProperty(d_, req.requestor, prop, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(list),
                        static_cast<int>(sizeof list / sizeof list[0]));
        reply(req, prop);
        return;
    }
    if (req.target == a_.timestamp) {
        // Format 32 means an array of C long here, not uint32_t. Time is
        // unsigned long, so this is the right width.
        XChangeProperty(d_, req.requestor, prop, XA_INTEGER, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&owned_), 1);
        reply(req, prop);
        return;
    }
    if (req.target == a_.imagePng || req.target == a_.imageXPng) {
        if (deliver(req, prop, req.target, pay_.png->data(), pay_.png->size(), true))
            done_ = true;
        return;
    }
    if (req.target == a_.uriList) {
        deliver(req, prop, req.target,
                reinterpret_cast<const unsigned char*>(pay_.uri.data()),
                pay_.uri.size(), false);
        return;
    }
    if (req.target == a_.utf8 || req.target == XA_STRING || req.target == a_.text) {
        deliver(req, prop, req.target == a_.text ? XA_STRING : req.target,
                reinterpret_cast<const unsigned char*>(pay_.path.data()),
                pay_.path.size(), false);
        return;
    }

    reply(req, None);   // a refusal still needs a reply, or the app hangs
}

// One chunk per call. Returns true when the transfer is over and can be
// dropped.
bool Holder::pump(Transfer& t) {
    const std::size_t n = std::min(chunk_, t.size - t.sent);
    XChangeProperty(d_, t.requestor, t.property, t.type, 8, PropModeReplace,
                    t.data + t.sent, static_cast<int>(n));
    t.sent += n;
    t.last  = Clock::now();

    if (n != 0) return false;      // more to come

    // A zero-length write is what ends an INCR transfer. After the sync the
    // requestor holds every chunk, so we are free to exit.
    XSync(d_, False);
    XSelectInput(d_, t.requestor, NoEventMask);
    if (t.image) done_ = true;
    return true;
}

void Holder::dropTransfers(Window w) {
    xfers_.erase(std::remove_if(xfers_.begin(), xfers_.end(),
                                [w](const Transfer& t) { return t.requestor == w; }),
                 xfers_.end());
}

void Holder::reapStalled() {
    const auto now = Clock::now();
    xfers_.erase(std::remove_if(xfers_.begin(), xfers_.end(),
                                [&](const Transfer& t) {
                                    return now - t.last > kTransferIdle;
                                }),
                 xfers_.end());
}

void Holder::serve() {
    const int fd = ConnectionNumber(d_);

    while (running_ && !done_) {
        while (running_ && !done_ && XPending(d_)) {
            XEvent e;
            XNextEvent(d_, &e);
            lastAct_ = Clock::now();

            switch (e.type) {
            case SelectionRequest:
                onRequest(e.xselectionrequest);
                break;
            case SelectionClear:
                // Someone else took the clipboard - very likely the next
                // screenshot. Our data is stale now.
                if (e.xselectionclear.selection == a_.clipboard) running_ = false;
                break;
            case PropertyNotify:
                if (e.xproperty.window != win_ && e.xproperty.state == PropertyDelete) {
                    // The requestor deleting the property is its request for
                    // the next chunk.
                    for (auto it = xfers_.begin(); it != xfers_.end(); ++it) {
                        if (it->requestor == e.xproperty.window &&
                            it->property  == e.xproperty.atom) {
                            if (pump(*it)) xfers_.erase(it);
                            break;
                        }
                    }
                }
                break;
            case DestroyNotify:
                dropTransfers(e.xdestroywindow.window);
                break;
            default:
                break;
            }
        }
        if (!running_ || done_) break;

        reapStalled();
        if (Clock::now() - lastAct_ > kIdleLifetime) break;

        XFlush(d_);
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        timeval tv{1, 0};
        if (select(fd + 1, &r, nullptr, nullptr, &tv) < 0 && errno != EINTR) break;
    }

    XCloseDisplay(d_);   // destroys the window, which releases the selection
}

[[noreturn]] void holderMain(const Payload& pay, int statusFd) {
    // The parent may have given up on the handshake and closed its end.
    std::signal(SIGPIPE, SIG_IGN);
    XSetErrorHandler(swallowError);
    XSetIOErrorHandler(fatalIoError);
    prctl(PR_SET_NAME, "sshot-clip", 0, 0, 0);

    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        (void)!write(statusFd, "X", 1);
        _exit(1);
    }

    Holder h(d, pay);
    if (!h.acquire()) {
        (void)!write(statusFd, "E", 1);
        _exit(1);
    }

    (void)!write(statusFd, "K", 1);
    close(statusFd);   // EOF here is how the parent notices a dead holder

    h.serve();
    _exit(0);          // never exit(): the parent's stdio buffer is not ours
}

} // namespace

std::vector<unsigned char> encodePng(cairo_surface_t* surf) {
    std::vector<unsigned char> out;
    const cairo_status_t st = cairo_surface_write_to_png_stream(surf, appendPng, &out);
    if (st != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error(std::string("cannot encode the PNG: ") +
                                 cairo_status_to_string(st));
    return out;
}

CopyStatus copyPng(const XSession& x,
               const std::vector<unsigned char>& png,
               const std::filesystem::path& path) {
    Payload pay;
    pay.png  = &png;
    pay.path = path.string();
    pay.uri  = "file://" + pay.path + "\r\n";

    int fds[2];
    if (pipe(fds) != 0) return CopyStatus::ForkFailed;

    const pid_t first = fork();
    if (first < 0) { close(fds[0]); close(fds[1]); return CopyStatus::ForkFailed; }

    if (first == 0) {
        close(fds[0]);
        // Intermediate child forks again and exits, so the holder is orphaned
        // onto init rather than lingering as our zombie.
        if (fork() == 0) {
            // Close the parent's X socket - not XCloseDisplay, which would
            // write on a connection that is not ours. Left open, it would keep
            // the parent's client alive in the server for the holder's life.
            close(ConnectionNumber(x.dpy()));
            setsid();
            // Without this, `set p (screenshot)` hangs: command substitution
            // waits for every writer of the pipe to close, and the holder
            // inherited stdout.
            if (const int null = open("/dev/null", O_RDWR); null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                if (null > STDERR_FILENO) close(null);
            }
            holderMain(pay, fds[1]);
        }
        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    int status = 0;
    waitpid(first, &status, 0);   // reaps at once; the holder lives on

    pollfd p{fds[0], POLLIN, 0};
    CopyStatus out = CopyStatus::Timeout;
    if (poll(&p, 1, kHandshakeMs) > 0) {
        char c = 0;
        const ssize_t n = read(fds[0], &c, 1);
        if      (n == 1 && c == 'K') out = CopyStatus::Ok;
        else if (n == 1 && c == 'X') out = CopyStatus::NoDisplay;
        else                         out = CopyStatus::Refused;   // 'E', or EOF
    }
    close(fds[0]);
    return out;
}

const char* describe(CopyStatus s) {
    switch (s) {
        case CopyStatus::Ok:         return "ok";
        case CopyStatus::ForkFailed: return "could not start the clipboard holder";
        case CopyStatus::NoDisplay:  return "the clipboard holder could not open the display";
        case CopyStatus::Refused:    return "another client would not give up the clipboard";
        case CopyStatus::Timeout:    return "the clipboard holder did not confirm in time";
    }
    return "unknown error";
}

} // namespace ss::clipboard
