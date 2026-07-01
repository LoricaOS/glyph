# glyph — the LoricaOS GUI toolkit.
#
# Builds the four static libraries the desktop components link against, and
# bundles them with their headers into a versioned ARTIFACT (`make artifact`)
# that component repos (lumen, bastion, the apps, ...) fetch at build time —
# the same model LoricaOS uses to consume the kernel (a built library closure,
# not source). The libs are static, so they are a BUILD-time dependency only;
# nothing here is a runtime herald package.
#
#   make            build libglyph.a libcitadel.a libaudio.a libauth.a
#   make artifact   build + bundle dist/glyph-<VERSION>.tar.gz (include/ + lib/)
#
# Needs a musl cross-compiler. Point MUSL_CC at it (defaults to PATH musl-gcc).
MUSL_CC ?= musl-gcc
CFLAGS  ?= -O2 -fno-pie -no-pie -Wall
AR      ?= ar

# Header dirs (consumers get these flattened under include/ in the artifact).
INCLUDES = -Ilib/glyph -Ilib/citadel -Ilib/audio -Ilib/libauth

GLYPH_OBJS   = $(addprefix lib/glyph/,   draw.o theme.o window.o font.o \
                  lumen_client.o glyph_term.o apps.o icons.o image_load.o \
                  glyph_cursor.o layout.o)
CITADEL_OBJS = lib/citadel/taskbar.o
AUDIO_OBJS   = lib/audio/audio.o
AUTH_OBJS    = lib/libauth/auth.o

LIBS = libglyph.a libcitadel.a libaudio.a libauth.a

OBJS = $(GLYPH_OBJS) $(CITADEL_OBJS) $(AUDIO_OBJS) $(AUTH_OBJS)

all: $(LIBS)

# -MMD -MP emits a per-object .d file listing every header the object includes,
# so editing a header (e.g. adding a field to glyph_window in glyph.h)
# recompiles exactly the objects that need it — no more stale .o linked into a
# fresh archive. The .d files are pulled in below.
%.o: %.c
	$(MUSL_CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

libglyph.a:   $(GLYPH_OBJS);   $(AR) rcs $@ $^
libcitadel.a: $(CITADEL_OBJS); $(AR) rcs $@ $^
libaudio.a:   $(AUDIO_OBJS);   $(AR) rcs $@ $^
libauth.a:    $(AUTH_OBJS);    $(AR) rcs $@ $^

artifact: all
	sh tools/make-artifact.sh

clean:
	rm -f $(LIBS) lib/*/*.o lib/*/*.d
	rm -rf dist

-include $(OBJS:.o=.d)
