# glyph architecture

This document describes the internals of the glyph toolkit: the rendering model,
the lumen window protocol, the text pipeline, the app-bundle scanner, the theme
system, and how the libraries layer on one another. It is grounded in the code
under `lib/`; where a behavior is a deliberate contract (color keys, link order)
it is called out as such.

## Layering

```
            libcitadel        libaudio        libauth
          (taskbar/shell)   (WAV/MP3 sink)  (passwd/shadow)
                 │
                 ▼
              libglyph
   draw ─ theme ─ font ─ window ─ lumen_client ─ glyph_term ─ apps ─ icons ─ image_load
                 │
                 ▼
        surface_t (software framebuffer) + AspisOS/musl syscalls
```

libglyph is self-contained. libcitadel builds on libglyph (its taskbar draws
with `draw.h` primitives and reads `THEME_*` tokens), which is why it must be
linked ahead of `-lglyph`. libaudio and libauth are independent of the GUI stack
but ship in the same artifact for convenience; libaudio depends only on libc and
its vendored decoder, libauth only on libc and `crypt()`.

## Rendering model: software framebuffer surfaces

All drawing targets a `surface_t`:

```c
typedef struct { uint32_t *buf; int w, h, pitch; } surface_t;  /* pitch in pixels */
```

There is no GPU path. Every primitive in `draw.c` writes `0x00RRGGBB` words
directly into `buf`, indexing rows by `pitch` (so a surface can be a window into
a larger buffer). The top byte is ignored by the framebuffer; where alpha
matters it is carried explicitly (the `draw_blit_alpha_scaled` path treats the
source as `0xAARRGGBB`).

The primitive set in `draw.h` falls into a few groups:

- **Fills and outlines** — `draw_px`, `draw_fill_rect`, `draw_rect`,
  `draw_rounded_rect`, `draw_line`, `draw_circle[_filled]`, `draw_gradient_v`.
- **Blits** — plain (`draw_blit`), nearest-neighbor scaled (`draw_blit_scaled`),
  color-keyed (`draw_blit_keyed`), per-pixel-alpha scaled
  (`draw_blit_alpha_scaled`), and a global-alpha + keyed + strided scale
  (`draw_blit_scaled_alpha`) used for window open/close scale-and-fade.
- **Compositing effects** — `draw_box_blur`, `draw_blend_rect`,
  `draw_blend_rounded_rect`. These implement frosted-glass surfaces: the
  compositor blurs the backdrop under a frosted window and tints it, keyed on
  the regions a client left at the frost key color.
- **Text** — bitmap (`draw_char`, `draw_text`, `draw_text_t`, `draw_text_center`)
  and TTF-aware (`draw_text_ui` plus the `glyph_text_width`/`_height` measurement
  helpers that fall back to the `FONT_W`×`FONT_H` bitmap cell when no TTF is
  loaded).

### Windows and chrome

`glyph_window_t` (in `glyph.h`) pairs a `surface_t` with on-screen geometry and
a set of flags. `window.c` renders the chrome — a rounded titlebar/border, the
window title, and a traffic-light close button — then calls the client's
`on_render` callback to fill the client area. Several flags change the chrome:

- `frosted` — the titlebar and client background are cleared to the chrome key
  color so the compositor's blur+tint shows through; the border is dropped.
- `chromeless` — there is no titlebar/border; the surface *is* the client area.
- `admin` — the titlebar is painted a solid danger-red, overriding even the
  frost key, so an elevated shell's window is unmistakable.
- `focused_window` — selects the focused vs. unfocused titlebar gradient and
  traffic-light brightness.

Damage is tracked as a single coalesced rectangle (`dirty_rect` + `has_dirty`,
managed by `glyph_window_mark_dirty_rect` / `_mark_all_dirty` /
`_get_dirty_rect`). The compositor reads it to repaint only what changed.

`blit_src` and `presented` support **proxy windows**: a chromeless in-compositor
window can point `blit_src` at a client's shared memory buffer so the compositor
composites straight from the client's pixels with no extra copy, and starts
unpresented (`presented = 0`) so a freshly opened window never flashes its
uninitialized buffer — it flips to presented on the first damage. The
frosted-glass `blur_cache` fields memoize the blurred backdrop under a frosted
window so a frame that changes only the window's own content reuses the blur
instead of recomputing the box blur over the whole footprint.

## The lumen client/window protocol

External processes (apps, the display manager) talk to the **lumen** compositor
over an AF_UNIX `SOCK_STREAM` socket at `/run/lumen.sock`. The wire format is
defined in `lumen_proto.h` and shared verbatim by client and server; the
client-side convenience API is `lumen_client.h` (`lumen_client.c`).

### Connection and handshake

`lumen_connect()` opens the socket; `lumen_connect_retry()` wraps it to retry on
`ECONNREFUSED` (50 × 100 ms = 5 s) while the compositor's `accept()` loop is
still coming up. After connecting, the client and server exchange a raw
`lumen_hello_t` / `lumen_hello_reply_t` pair (magic `LMEN`, version 1) — these
are sent unframed, before any framed message.

Because `SOCK_STREAM` has no message boundaries, all subsequent traffic is
length-framed: a `lumen_msg_hdr_t { op, len }` precedes each payload, and
`lumen_read_full()` accumulates across short reads to reassemble exactly `n`
bytes. This is the framing primitive every external protocol client uses.

### Window creation and the shared pixel buffer

A client sends `LUMEN_OP_CREATE_WINDOW` (`lumen_create_window_t`: 16-bit
width/height, flags, a 64-byte title) or `LUMEN_OP_CREATE_PANEL`
(`lumen_create_panel_t`, a chromeless bottom-centered panel with no focus). The
`LUMEN_WIN_FLAG_FULLSCREEN` flag requests a chromeless framebuffer-sized window
at the origin, in which case the requested width/height are ignored and the
real dimensions come back in the reply.

The server replies with a framed `lumen_window_created_t` (status, assigned
`window_id`, actual width/height, screen `x,y`) **and** passes a `memfd` for the
window's pixel buffer as an `SCM_RIGHTS` ancillary message. The client
`mmap`s that fd `MAP_SHARED` (the compositor reads from it) and allocates a
private malloc'd back buffer of the same size. This is the double-buffer:

```
client draws ──▶ win->backbuf (private)   ── lumen_window_present() ─▶  win->shared (memfd, MAP_SHARED) ──▶ compositor blit
```

`lumen_window_present()` is a single `memcpy` from `backbuf` to `shared`. The
resulting `lumen_window_t` carries both buffers, the assigned id, the dimensions
(`stride == w` in v1), and the placement `x,y`.

`lumen_window_create` / `_create_ex` / `lumen_panel_create` build the window;
`lumen_window_destroy` unmaps the shared buffer, frees the back buffer and closes
the fds.

### Events

`lumen_poll_event` (non-blocking) and `lumen_wait_event` (with a timeout) read
framed server→client events into a `lumen_event_t`, a tagged union over the event
opcodes:

- `LUMEN_EV_KEY` — keycode + modifiers + pressed. Keyboard bytes come from the
  kernel keyboard ISR; modifiers are reserved (0) in v1.
- `LUMEN_EV_MOUSE` — client-area-relative `x,y`, a button bitmask
  (bit0 left, bit1 right, bit2 middle), an `evtype` (`MOVE`/`DOWN`/`UP`/`WHEEL`),
  and a signed wheel `scroll` delta for the `WHEEL` type.
- `LUMEN_EV_CLOSE_REQUEST`, `LUMEN_EV_FOCUS` (focused flag),
  `LUMEN_EV_RESIZED` (defined but never sent in v1).
- Drag-and-drop: `LUMEN_EV_DRAG_OVER`, `LUMEN_EV_DRAG_LEAVE`, `LUMEN_EV_DROP`.

### Drag-and-drop

Drag-and-drop is compositor-brokered. A source window, while still holding the
left button whose press began in its content area, calls `lumen_drag_start`
(`LUMEN_OP_DRAG_START`, `lumen_drag_start_t`: op, a 64-byte ghost label, a
256-byte absolute path). The compositor draws the ghost at the cursor, sends
`DRAG_OVER`/`DRAG_LEAVE` to the proxy window under the pointer, and on release
delivers a `LUMEN_EV_DROP` carrying the path to the target — which executes the
operation itself (`LUMEN_DND_MOVE` or `LUMEN_DND_COPY`), since the payload is a
path and the receiver decides how to act on it.

### Other client requests

`LUMEN_OP_SET_TITLE`, `LUMEN_OP_DESTROY_WINDOW`, `LUMEN_OP_SET_ADMIN`
(`lumen_window_set_admin` — a terminal reports its shell's admin-session state so
the compositor tints that window's titlebar red), and `LUMEN_OP_INVOKE`
(`lumen_invoke` — asks the server to launch a built-in or `/apps` bundle by
name).

## Font and text pipeline

`font.c` renders TrueType via vendored **stb_truetype**. `font_init()` (called
once at startup, and which also triggers `glyph_theme_load()`):

1. Loads Inter from `/usr/share/fonts/Inter-Regular.ttf` into `g_font_ui` and
   JetBrains Mono from `JetBrainsMono-Regular.ttf` into `g_font_mono`.
2. Pre-bakes the UI font at 11/12/13/14/16/20 px and the mono font at
   14/16/18/20 px.

`font_bake(f, size_px)` rasterizes ASCII 32–126 into a per-size alpha atlas (a
no-op if that size is already baked); any size can be baked on demand.
`font_draw_text` / `font_draw_char` alpha-blend the atlas glyphs over existing
surface content with a transparent background, and `font_text_width` /
`font_height` report metrics for a baked size.

If the TTF files are missing, `g_font_ui`/`g_font_mono` stay `NULL` and callers
fall back to the fixed bitmap font (`terminus20.h`) via the `draw_char` /
`draw_text` family. The `draw.h` measurement helpers (`glyph_text_width`,
`glyph_text_height`, `glyph_char_width`) and `draw_text_ui` make this transparent:
they use the TTF font when available and the `FONT_W`×`FONT_H` bitmap cell
otherwise.

### Terminal core

`glyph_term.c` is a self-contained terminal emulator, deliberately decoupled from
any window system so the in-process lumen dropdown terminal and the standalone
terminal app share one implementation. `glyph_term_t` owns:

- a **cell grid** ring buffer (`rows + GLYPH_TERM_SCROLLBACK` total rows of
  `glyph_term_cell_t`, each a char + fg/bg + attrs), with bold/underline/reverse
  SGR attributes;
- an **ANSI/CSI/OSC parser** (`esc_state`, parameter stack, `?`-private flag,
  cursor visibility via DEC `?25`, an alternate-screen notion, and an OSC string
  buffer);
- **scrollback** and a user scroll offset, plus a draggable **scrollbar**;
- **selection** state with copy-out.

`glyph_term_feed` pushes PTY bytes through the parser; `glyph_term_render` paints
the grid, cursor, selection and scrollbar into a caller surface at
`(pad_x, pad_y)` after clearing a caller-specified rectangle to the terminal
background. The caller owns dirty-marking and presentation. Mouse/scroll helpers
return 1 when the visual state changed so the caller knows to repaint.

`glyph_term_translate_key` converts keys to PTY bytes, re-expanding lumen's
single-byte synthetic arrow codes (`0xF1..0xF4`) back into VT escape sequences
for proxy windows. A private OSC `]777;admin;<n>` lets the shell signal an
admin-session change, surfaced via `glyph_term_admin_take_change` so the terminal
app can tell the compositor. `glyph_pty_open_and_spawn` opens a PTY pair and
spawns `/bin/stsh` with stdio on the slave, returning the non-blocking master fd.

## App-bundle scanner

`apps.c` discovers system applications under `/apps/<id>/`. Each bundle has an
`app.ini` manifest (`name=`, `exec=`) and an ELF named by `exec=` (default the
bundle id). `glyph_apps_scan` parses up to `GLYPH_APPS_MAX` (64) bundles, skips
any whose ELF is absent (checked with `access`), and returns them sorted by
display name; `glyph_apps_find` looks one up by id.

The security boundary is explicit: only `/apps` is scanned, never per-user app
directories. `/apps` is root-controlled like `/bin`, so the kernel's cap-policy
trusted-path anchor accepts bundles there and they may carry `caps.d` policies. A
user-writable path must never become a capability-policy subject, so it is never
treated as a bundle source. The same scanner feeds lumen's `INVOKE` handler
(id → exec path), the Applications launcher grid, and the dock.

`icons.c` renders the matching icon: a bundle's own `/apps/<id>/icon.png`
(decoded and scaled via `image_load.c`) if present, otherwise built-in
procedural artwork drawn with `draw.c` primitives, with a hashed letter-tile
fallback for unknown ids. All geometry scales from the requested box edge so an
app looks identical at the dock's 48 px and the launcher's larger size.

## Theme system

`glyph_theme.h` defines the Aegis design language as **semantic** tokens
(`THEME_SURFACE`, `THEME_ACCENT`, `THEME_TEXT_DIM`, …) plus spacing (`SP_*`, a
4px grid), corner-radius (`R_*`) and type-size (`TYPE_*`) scales. Every widget,
compositor surface and app bundle is expected to derive its appearance from these
tokens rather than inline hex.

The surface/text/border tokens and the accent are **runtime expressions**: the
`THEME_*` macros read fields of the global `glyph_theme_t g_glyph_theme`, so they
resolve at draw time. (Because they are expressions, not constants, they must not
be used in static initializers or `case` labels — store a role index and resolve
later.) `theme.c` owns this state:

- `apply_palette(light)` sets every surface/text/border field from a built-in
  dark or light palette, so toggling the theme is a one-call live switch.
- Eight accent presets — blue, indigo, purple, pink, red, orange, green, teal —
  each with base/hover/active shades, selectable by index
  (`glyph_theme_set_accent`).
- Persisted preferences alongside the accent: animations, 12/24h clock, natural
  scroll, timezone offset, pointer speed, wallpaper preset, night light, and
  whether NTP sync is allowed. Setters change the live value; `glyph_theme_save`
  writes them back.
- `glyph_theme_load` reads `/etc/aegis/theme.conf` (system default) then
  `$HOME/.config/aegis/theme.conf` (per-user override); the config is a simple
  line-anchored `key=value` format. `glyph_theme_reload_prefs` lets a running
  compositor pick up Settings changes without a restart.

Two values are explicitly **not** routed through the theme: `THEME_KEY_FROST`
(`0x000A0A14`) and `THEME_KEY_SHADOW` (`0x00080810`). These are the color keys
the compositor matches to identify frosted-window client regions and chrome
transparency; they are a protocol contract between glyph and lumen, not palette
choices, and their numeric values must never change. The status colors and the
macOS-canonical traffic-light colors are likewise fixed constants.

`draw.h` re-exports the old `C_*` palette names as aliases onto the `THEME_*`
tokens so existing call sites compile unchanged while there remains a single
definition of each value; new code uses `THEME_*` directly.

## Audio pipeline

`audio.c` decodes a file and streams PCM to a fixed-format sink: 48 kHz, 16-bit,
stereo (the HDA driver's format, e.g. `/dev/audio`). WAV (16-bit PCM, any rate,
mono/stereo) is parsed directly; MP3 goes through vendored **minimp3**. A
streaming linear resampler converts any source rate to 48 kHz and expands mono to
stereo on the fly, so playback is at correct pitch. The caller owns the sink fd
and closes it afterward — closing `/dev/audio` is what flushes the DMA and
triggers playback. `audio_play_file_async` forks a child that streams under the
sink's backpressure (so it lives for the track's duration) and returns its pid,
keeping the caller's UI responsive; `audio_duration_ms` estimates length (exact
from a WAV header, approximate from MP3 size and first-frame bitrate).
