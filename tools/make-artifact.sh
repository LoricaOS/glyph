#!/bin/sh
# make-artifact.sh — bundle the built toolkit into dist/glyph-<VERSION>.tar.gz.
#
# Layout consumers expect (fetch-glyph.sh unpacks with --strip-components=1):
#   include/   all toolkit headers, flattened (no basename collisions)
#   lib/       libglyph.a libcitadel.a libaudio.a libauth.a
#   VERSION
# A component then builds with `-Iinclude -Llib -lglyph -lcitadel -laudio -lauth`.
set -eu
cd "$(dirname "$0")/.."

VER="$(cat VERSION)"
STAGE="dist/glyph-${VER}"
rm -rf "$STAGE" "dist/glyph-${VER}.tar.gz"
mkdir -p "$STAGE/include" "$STAGE/lib"

cp libglyph.a libcitadel.a libaudio.a libauth.a "$STAGE/lib/"
cp lib/glyph/*.h lib/citadel/*.h lib/audio/*.h lib/libauth/*.h "$STAGE/include/"
cp VERSION "$STAGE/VERSION"

( cd dist && tar czf "glyph-${VER}.tar.gz" "glyph-${VER}" )
echo "[glyph] artifact -> dist/glyph-${VER}.tar.gz ($(wc -c < dist/glyph-${VER}.tar.gz) bytes)"
