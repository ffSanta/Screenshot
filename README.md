# screenshot

A region screenshot tool for **Arch Linux + i3**, written in C++20 on raw Xlib.

Type `screenshot` in a terminal and a floating frame appears with a
**see-through middle**. Drag its border to resize, drag its toolbar to move it,
and when the region is framed the way you want, click **Save**. The image lands
in `~/Pictures/screenshot/` with a timestamped name, the path is printed to
stdout, and the image goes on the clipboard so `Ctrl+V` pastes the picture
itself.

```
┌─────────────────────────────┐
│▝▘           ▂            ▝▘│  ← drag the border to resize (8 grips)
│                             │
│    the desktop shows        │
│    straight through here    │
│    — this is exactly        │
│    what gets captured       │
│                             │
│▖▗           ▀            ▖▗│
└─────────────────────────────┘
┌─────────────────────────────┐
│ ⣿ 800 × 500  at 283, 134    │  ← drag here to move
│              [Cancel] [Save]│
└─────────────────────────────┘
```

## Features

- **See-through viewfinder** — you frame the real pixels, not a dimmed guess.
- **Eight resize grips** — four corners, four edge midpoints, with the cursor
  changing to match whichever one is under the pointer.
- **Live readout** of the exact size and origin that will be captured.
- **Works on i3 with no config changes** — the window is unmanaged, so the
  tiling layout never touches it.
- **Full keyboard control**, including pixel-precise nudging.
- **Prints the saved path** to stdout, so it composes with other commands.
- **Puts the PNG on the clipboard** — `Ctrl+V` pastes the image, with no
  `xclip`, `xsel` or `wl-clipboard` needed to copy it.
- No dependency on a compositor. One small process does stay behind to hold the
  clipboard, because an X11 selection is served by whoever owns it; it exits as
  soon as the image is pasted, or as soon as anything else copies.
  `--no-clipboard` skips it entirely.

## Tools and libraries

| Library | Why it is here |
|---|---|
| **libX11** (Xlib) | Creates the viewfinder window, runs the event loop, reads the screen with `XGetImage`, and owns the `CLIPBOARD` selection |
| **libXext** → **XShape** | Punches the hole through the middle of the window |
| **libXcursor** | Loads the resize cursors from the user's cursor theme |
| **cairo** + **cairo-xlib** | Draws the frame, grips, toolbar and readout onto the X window — *and* writes the final PNG, so libpng is never used directly |
| **C++20 stdlib** | `std::filesystem` for the output tree, `std::chrono` for the settle delay |
| **CMake** + **pkg-config** | Build |

On Arch these come from `libx11 libxext libxcursor cairo`, all of which are
already pulled in by a normal Xorg desktop.

Copying needs nothing else — the clipboard is served by this process directly.
*Pasting* is up to the receiving application, and several shell out to `xclip`;
the Claude Code CLI is one, so `pacman -S xclip` is worth having.

## How it works

Four parts of this are not obvious.

### 1. Escaping i3's tiling

i3 is a tiling window manager: a normal window gets snapped into the layout and
cannot be freely moved or resized, which is exactly what a viewfinder needs to
do. The window is therefore created **override-redirect**:

```cpp
XSetWindowAttributes a{};
a.override_redirect = True;
```

That flag tells X the window manager should not manage this window at all. i3
never sees it — no tiling, no i3 border, no `for_window` rule, **no i3 config
change required**. Its geometry is ours to set pixel by pixel.

The trade-off is that an unmanaged window is never given focus, so the keyboard
has to be taken explicitly with `XGrabKeyboard`, and moving and resizing are
implemented by hand rather than delegated to the WM.

### 2. The see-through middle is a hole, not transparency

The window's **bounding shape** is restricted to its four border strips plus the
toolbar:

```cpp
XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, parts, 5,
                        ShapeSet, Unsorted);
```

The selection area is not part of the window at all. The desktop underneath is
untouched rather than tinted, so what you see framed is precisely what gets
captured — and unlike an ARGB visual with alpha, this needs **no compositor
running**. The shape is expressed in window coordinates, so it is reapplied on
every resize.

### 3. Keeping the frame out of its own screenshot

`XGetImage` reads what is composited on screen right now, so a naive grab
photographs the viewfinder along with everything else. The sequence is:

```cpp
XUnmapWindow(dpy, win);
XSync(dpy, False);
std::this_thread::sleep_for(std::chrono::milliseconds(120));  // let it land
XImage* img = XGetImage(dpy, root, r.x, r.y, r.w, r.h, AllPlanes, ZPixmap);
```

The delay matters: with a compositor such as **picom**, windows *fade* out, so
the frame is still partly on screen when `XSync` returns. 120 ms clears the
default fade.

Converting the `XImage` to cairo takes a fast path — a straight row copy — but
only after checking that the visual really is 32 bpp little-endian with
`0xff0000/0x00ff00/0x0000ff` masks, which is what makes an X pixel byte-identical
to `CAIRO_FORMAT_RGB24`. Any other visual falls back to per-channel mask
arithmetic. Both paths were verified to produce identical output.

### 4. The clipboard is a process, not a buffer

X11 has no clipboard *storage*. Copying makes a window the **owner** of the
`CLIPBOARD` selection, and every paste is a live round trip: the pasting app
sends a `SelectionRequest`, the owner answers with the bytes. Under a bare i3
session there is no clipboard manager to take the data over, so if `screenshot`
exited straight after `XSetSelectionOwner` the clipboard would be empty before
you could press `Ctrl+V`.

So the PNG is encoded into memory and a holder is double-forked away — `setsid`,
reparented to init, and stdin/stdout/stderr on `/dev/null`. That last part is
not cosmetic: a holder still holding the inherited stdout would keep

```fish
set shot (screenshot)
```

blocked until the holder died, because command substitution waits for every
writer of the pipe to close. The holder opens a **fresh** `XOpenDisplay` — an
Xlib connection must never be used across a `fork` — takes the selection on a
1x1 unmapped `InputOnly` window, and answers requests until the image has been
handed over once. A one-byte pipe back to the parent means the path is not
printed until ownership is confirmed, so a fast `Ctrl+V` cannot beat it.

Anything over 256 KiB goes out through the ICCCM **INCR** protocol, because a
single X request cannot carry an arbitrarily large property: the property is
first set to type `INCR` with a size hint, and the requestor then deletes it
once per chunk to ask for the next one, ending on a zero-length write. A
full-screen capture is comfortably past that threshold, so this is the path
that normally runs, not a rarely-exercised fallback.

Only the image ends the holder. `TARGETS` is a probe, not a paste — Claude Code
asks for it first to find out what is on offer — and the file path is served as
text alongside the image, which some apps will prefer; neither consumes the
screenshot.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Install

```sh
cmake --install build --prefix ~/.local
```

That puts the binary at `~/.local/bin/screenshot`. If that directory is not on
your `PATH`, add it — for fish:

```fish
fish_add_path ~/.local/bin
```

## Usage

```
screenshot [--dir <path>] [--geometry WxH+X+Y] [--no-clipboard] [--open]
           [--help] [--version]
```

| Flag | Effect |
|---|---|
| `--dir <path>` | Save here instead of `~/Pictures/screenshot` |
| `--geometry WxH+X+Y` | Starting frame; the `+X+Y` offset is optional and the frame is centred without it |
| `--no-clipboard` | Do not copy the image to the clipboard, and leave no holder process |
| `--open` | Open the saved image with `xdg-open` afterwards |
| `-h`, `--help` | Usage |
| `-V`, `--version` | Version |

### Mouse

| Action | Effect |
|---|---|
| Drag the border | Resize from that corner or edge |
| Drag the toolbar | Move the frame |
| Click **Save** | Capture and write the PNG |
| Click **Cancel** | Quit without writing anything |

Pressing a button and releasing somewhere else cancels the click, as usual.

### Keyboard

| Key | Effect |
|---|---|
| `Esc` | Cancel |
| `Enter` or `Ctrl+S` | Save |
| Arrows | Move 1 px |
| `Ctrl` + Arrows | Move 10 px |
| `Shift` + Arrows | Resize 1 px |
| `Ctrl+Shift` + Arrows | Resize 10 px |

Holding an arrow accelerates. A tap is exactly the base step, so pixel
precision is intact. The moment X starts auto-repeating, the key has already
been held for the server's repeat delay, so the step jumps straight to 16x and
then doubles per event to a 128 px cap - full speed within about 120 ms of the
first repeat.

The run of repeats is recognised from the server's own auto-repeat delay
(`XkbGetAutoRepeatRate`), not a fixed guess, so acceleration works whatever
`xset r rate` is set to. If the initial pause before repeating still feels
long, that pause is the X setting itself and applies to every application:
`xset r rate 250 30` shortens it to 250 ms.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Saved; the path is on stdout |
| `1` | Cancelled by the user; nothing written, nothing printed |
| `2` | Error — bad arguments, no X display, or the file could not be written |

Because the path goes to stdout and cancelling prints nothing, it composes:

```fish
set shot (screenshot); and echo "saved $shot"
```

## Where files go

The output directory is resolved in this order:

1. `$XDG_PICTURES_DIR`, if set
2. `XDG_PICTURES_DIR` from `~/.config/user-dirs.dirs`
3. `~/Pictures`

with a `screenshot/` subfolder appended, created on first run. Files are named
`screenshot_YYYY-MM-DD_HH-MM-SS.png`; a numeric suffix is added if two captures
land in the same second.

## i3 integration

Nothing is required for the tool to work. To bind it to the `Print` key, add
this to `~/.config/i3/config` and reload with `$mod+Shift+R`:

```
bindsym Print exec --no-startup-id screenshot
```

## Troubleshooting

**"cannot open X display … this is a Wayland session"** — this tool is X11-only.
i3 is an X11 window manager, so under i3 you are on X11; if you are on Sway or
another Wayland compositor, use `grim` and `slurp` instead.

**The frame appears in the saved image** — the settle delay is not clearing your
compositor's fade animation. Raise `kSettleDelay` in `src/capture.cpp`, or
shorten the fade in your picom config.

**The window is a solid box with no hole** — the X server is missing the Shape
extension. The tool detects this and falls back to a filled frame; it still
captures correctly, you just cannot see through it while framing.

**`Ctrl+V` pastes nothing** — the pasting application decides how it reads the
clipboard, and several shell out to `xclip`, which is not installed by default:
`pacman -S xclip`. Check the holder is alive with
`ps --ppid 1 -o pid,comm | grep sshot-clip`, and that `screenshot` printed no
`clipboard:` warning. Remember the image is served once — after a successful
paste the holder exits, so a second paste has nothing to fetch.

**A `screenshot` process is still running** — that is the clipboard holder, by
design; see part 4 above. It exits when the image is pasted, when anything else
copies, when the X server ends, or after fifteen idle minutes. `--no-clipboard`
never starts one.

**Colours look wrong in the PNG** — your display is not 24-bit TrueColor, so the
generic conversion path is in use. It handles arbitrary channel masks, so please
report the `depth`/`bits_per_pixel`/mask values if something is still off.

## Layout

```
src/
├── main.cpp        entry point: parse, frame, capture, save
├── options.*       command-line parsing
├── geometry.hpp    Rect, Zone, layout and hit-testing (no X11, header-only)
├── xsession.*      the X connection and screen facts
├── theme.hpp       colours and rounded-rect helper
├── viewfinder.*    the window: shaping, drawing, drag and key handling
├── capture.*       XGetImage and the XImage → cairo conversion
├── output.*        directory resolution, timestamped naming, PNG writing
└── clipboard.*     in-memory PNG encoding and the CLIPBOARD selection owner
```
