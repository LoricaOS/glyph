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
                  glyph_cursor.o)
CITADEL_OBJS = lib/citadel/taskbar.o
AUDIO_OBJS   = lib/audio/audio.o
AUTH_OBJS    = lib/libauth/auth.o

LIBS = libglyph.a libcitadel.a libaudio.a libauth.a

all: $(LIBS)

%.o: %.c
	$(MUSL_CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

libglyph.a:   $(GLYPH_OBJS);   $(AR) rcs $@ $^
libcitadel.a: $(CITADEL_OBJS); $(AR) rcs $@ $^
libaudio.a:   $(AUDIO_OBJS);   $(AR) rcs $@ $^
libauth.a:    $(AUTH_OBJS);    $(AR) rcs $@ $^

artifact: all
	sh tools/make-artifact.sh

clean:
	rm -f $(LIBS) lib/*/*.o
	rm -rf dist
