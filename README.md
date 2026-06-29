# glyph

The GUI toolkit for [AspisOS](https://github.com/AspisOS/AspisOS) — a
capability-based, no-ambient-authority x86-64 operating system built on the
from-scratch [Aegis](https://github.com/AspisOS/Aegis) kernel.

glyph is a set of four C static libraries that the graphical components of
AspisOS link against: the **lumen** compositor, the display/login manager, and
the GUI applications. It provides software-framebuffer drawing primitives, a
runtime theme system, TTF and bitmap text, image decoding, the client side of
the lumen window protocol, a reusable terminal-emulator core, application-bundle
discovery, procedural icons, taskbar chrome, audio playback, and credential
helpers.

glyph is a **build-time dependency only**. It compiles to static archives
(`.a`) that are baked into each consuming binary; there is no shared object and
nothing in glyph is installed as a runtime package. Like the Aegis kernel, glyph
publishes a **versioned artifact** (`dist/glyph-<version>.tar.gz`, containing the
four archives plus their flattened headers) that consumer repositories fetch at
build time rather than rebuilding from source.

## Where it fits in AspisOS

AspisOS is decomposed into independent repositories:

- **`AspisOS/Aegis`** — the kernel. Published as a versioned `aegis.elf`
  artifact; the OS fetches it rather than rebuilding it.
- **`AspisOS/AspisOS`** — the OS: userland, rootfs, ISO assembly.
- **`AspisOS/glyph`** — this repository: the shared GUI toolkit.
- **`AspisOS/lumen`** and the other graphical component repos — the compositor,
  display manager and apps. Each fetches the glyph artifact and links the static
  libraries into its binary.

The toolkit and the components version independently. A component pins the
toolkit version it builds against and fetches exactly that release, so a glyph
change never silently alters a component until that component bumps its pin.

## The four libraries

### libglyph — core toolkit

The bulk of the toolkit. Sources in `lib/glyph/`.

- **Drawing** (`draw.h`) — software-framebuffer primitives over a `surface_t`
  (`uint32_t *buf; int w, h, pitch`). Pixels are `0x00RRGGBB` (XRGB; the top
  byte is ignored by the framebuffer). Primitives include filled/outline
  rectangles, vertical gradients, lines, circles, rounded rectangles, bitmap and
  TTF text, plain/scaled/keyed/alpha blits, box blur and alpha-blended rectangle
  fills. The keyed/blur/blend variants back the compositor's frosted-glass and
  window open/close animation effects.
- **Theme** (`glyph_theme.h`, `theme.c`) — the single source of truth for the
  Aegis design language. Semantic `THEME_*` color tokens (surfaces, borders,
  text tiers, accent, status), a 4px spacing scale, corner-radius and type
  scales. Surface/text/border colors and the accent are **runtime** values held
  in `g_glyph_theme`, loaded from `/etc/aegis/theme.conf` then
  `$HOME/.config/aegis/theme.conf`, so light/dark switching and the accent
  picker are live, persisted settings rather than recompiles. Eight accent
  presets (blue, indigo, purple, pink, red, orange, green, teal) and additional
  persisted preferences (animations, 12/24h clock, natural scroll, timezone
  offset, pointer speed, wallpaper, night light, NTP-allowed). Two fixed
  `THEME_KEY_*` values (`0x000A0A14` frost key, `0x00080810` chrome key) are a
  protocol contract with the compositor and must never change. `draw.h` keeps
  legacy `C_*` names as aliases onto these tokens.
- **Fonts** (`font.h`, `font.c`) — TTF rendering via vendored stb_truetype.
  `font_init()` loads Inter (UI) and JetBrains Mono (terminal) from
  `/usr/share/fonts/` into the globals `g_font_ui` / `g_font_mono` and also
  triggers the theme load. Glyphs are baked into per-size ASCII (32–126) alpha
  atlases on demand (`font_bake`) and alpha-blended over surface content. If the
  TTF files are absent, callers fall back to the bitmap `draw_char`/`draw_text`
  path (a built-in fixed bitmap font, `terminus20.h`).
- **Window + geometry** (`glyph.h`, `window.c`) — an immediate-mode
  `glyph_window_t` with a backing `surface_t`, chrome rendering (titlebar,
  border, traffic-light close button, focused/admin/frosted variants),
  rectangle-based dirty tracking, and compositor-integration callbacks
  (`on_key`, `on_render`, mouse/scroll/wheel handlers). Includes a small
  rectangle-geometry helper API.
- **Lumen client** (`lumen_client.h`, `lumen_proto.h`, `lumen_client.c`) — the
  client side of the compositor's AF_UNIX window protocol. Connect to
  `/run/lumen.sock`, create windows/panels, receive a shared-memory pixel buffer
  over `SCM_RIGHTS`, render into a private back buffer and present, and poll/wait
  for input, focus, resize and drag-and-drop events. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- **Terminal core** (`glyph_term.h`, `glyph_term.c`) — a window-system-agnostic
  terminal emulator: cell grid, ANSI/CSI/OSC escape parser, scrollback ring,
  selection, scrollbar, key→PTY translation, and a PTY-open-and-spawn helper.
  Renders into a caller-supplied surface; the caller owns presentation. Shared
  by lumen's in-process dropdown terminal and the standalone terminal app.
- **App bundles** (`apps.h`, `apps.c`) — scans `/apps/<id>/` system application
  bundles, parsing each `app.ini` (`name=`, `exec=`) and skipping bundles whose
  ELF is missing. Per-user app directories are deliberately not scanned, so a
  user-writable path can never become a capability-policy subject.
- **Procedural icons** (`icons.h`, `icons.c`) — one icon source for the dock and
  launcher. Uses a bundle's own `/apps/<id>/icon.png` if present, otherwise draws
  built-in procedural artwork with `draw.c` primitives, with a hashed
  letter-tile fallback for unknown ids.
- **Image loading** (`image_load.h`, `image_load.c`) — decodes PNG/BMP/JPEG via
  vendored stb_image into `0x00RRGGBB` buffers (alpha flattened), with a 64-MPix
  hard cap.

### libcitadel — desktop-shell chrome

`lib/citadel/`. Window-management and shell helpers built on libglyph: the
desktop top bar (`taskbar.h`) — the Aegis menu, a volume slider and a clock,
with frosted-glass blur caching and hit-testing helpers. `theme.h` here is a
thin set of layout constants that derive their colors from the libglyph theme
tokens.

### libaudio — audio playback

`lib/audio/`. `audio.h` decodes WAV (16-bit PCM, any rate, mono/stereo) and MP3
(via vendored minimp3) and streams to a 48 kHz / 16-bit / stereo PCM sink such as
`/dev/audio`, linearly resampling and expanding mono to stereo. Provides
synchronous playback, a duration estimator, and an async fork-and-play helper
that returns a child pid.

### libauth — credential helpers

`lib/libauth/`. `auth.h` provides `/etc/passwd` and `/etc/shadow` lookups,
`crypt()`-based password verification, a combined `auth_check`, identity setting
(`setuid`/`setgid`), and `auth_elevate_session` — which marks the session
authenticated and binds the verified `(uid, gid)` so a later setuid may only
assume that identity. Used by `/bin/login` (text) and the graphical
login/unlock path (`bastion`). Shadow access requires `CAP_KIND_AUTH`.

## Repository layout

```
Makefile                build the four .a libraries; `make artifact` bundles a release
VERSION                 toolkit version string (one line)
tools/
  make-artifact.sh      bundle libs + flattened headers into dist/glyph-<ver>.tar.gz
  fetch-glyph.sh        consumer-side fetch+unpack helper (canonical copy)
lib/
  glyph/                libglyph: draw, theme, font, window, lumen client,
                        terminal core, apps, icons, image load (+ vendored
                        stb_truetype.h, stb_image.h, terminus20.h bitmap font)
  citadel/              libcitadel: taskbar + shell layout constants
  audio/                libaudio: WAV/MP3 playback (+ vendored minimp3.h)
  libauth/              libauth: passwd/shadow/crypt auth helpers
```

## Building

glyph builds with a **musl cross-compiler** targeting AspisOS userspace. Point
`MUSL_CC` at it (defaults to `musl-gcc` on `PATH`).

```sh
make                       # build libglyph.a libcitadel.a libaudio.a libauth.a
make MUSL_CC=/path/to/musl-gcc
make artifact              # build + bundle dist/glyph-<VERSION>.tar.gz
make clean
```

Default flags are `-O2 -fno-pie -no-pie -Wall`. The archives land at the repo
root; objects under `lib/*/`. All four are static.

`make artifact` produces `dist/glyph-<VERSION>.tar.gz` containing:

```
glyph-<VERSION>/
  include/   all toolkit headers, flattened (no basename collisions)
  lib/       libglyph.a libcitadel.a libaudio.a libauth.a
  VERSION
```

## Consuming the toolkit

A component repo fetches a pinned toolkit release with `tools/fetch-glyph.sh`
(copy it into the component, or vendor it). Usage:

```sh
fetch-glyph.sh <version> <dest-dir>
```

It resolves a local cache (`vendor/glyph-<version>.tar.gz`) first, otherwise
downloads `https://github.com/AspisOS/glyph/releases/download/v<version>/glyph-<version>.tar.gz`,
then unpacks `include/` and `lib/` into `<dest-dir>`. Place the tarball at
`vendor/glyph-<version>.tar.gz` to build offline.

Then compile and link against it. Because libcitadel, libaudio and libauth all
depend on symbols in libglyph, **list `-lglyph` last** (static link order
matters — a library must precede the ones it depends on):

```sh
# fetch the toolkit this component is pinned to
sh tools/fetch-glyph.sh 1.0.0 build/glyph

# build a component against it
musl-gcc -O2 -fno-pie -no-pie \
    -Ibuild/glyph/include \
    -c mywidget.c -o mywidget.o

musl-gcc -no-pie -o mycomponent mywidget.o \
    -Lbuild/glyph/lib \
    -lcitadel -laudio -lauth -lglyph
```

A component that only needs the core toolkit can link `-lglyph` alone; pull in
`-lcitadel`, `-laudio` or `-lauth` as needed (each still ahead of `-lglyph`).

## Artifact / versioning model

glyph is consumed as a built artifact, not as source, for the same reasons the
OS consumes the kernel that way:

- **Reproducibility** — every component that pins `glyph 1.0.0` links bit-for-bit
  the same toolkit, regardless of when or where it builds.
- **Decoupling** — components don't carry a musl toolchain configuration for
  glyph or recompile it on every build; they fetch one tarball.
- **Independent cadence** — the toolkit releases on its own schedule; a component
  adopts a new toolkit only when it bumps its pin.

`VERSION` is the single source of the version string; `make artifact` stamps it
into the tarball name and ships a `VERSION` file inside, and `fetch-glyph.sh`
resolves releases by that string.

## Vendored third-party code

glyph vendors three single-file libraries. All are public-domain (or
public-domain-equivalent / MIT dual-licensed by their authors); they are
included verbatim:

- **stb_truetype.h** (`lib/glyph/`) — TrueType rasterizer by Sean Barrett. Public
  domain. Note its own warning: it does no bounds-checking and must not be used
  on untrusted font files; glyph only loads the fixed system fonts under
  `/usr/share/fonts/`.
- **stb_image.h** (`lib/glyph/`) — image decoder by Sean Barrett. Public domain.
- **minimp3.h** (`lib/audio/`) — MP3 decoder by Lion (lieff). Released to the
  public domain under CC0.

`terminus20.h` is a built-in bitmap font used as the no-TTF fallback.
