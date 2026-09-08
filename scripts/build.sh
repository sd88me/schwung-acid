#!/usr/bin/env bash
# =============================================================================
# Build the Acid module for Ableton Move (ARM64) and package it into an
# installable tarball under dist/.
#
#   dist/acid-module.tar.gz   (folder: acid/)
#
# Requires Docker. Cross-compiles the DSP with aarch64-linux-gnu-gcc.
# Before building, vendor the two Schwung headers into src/include/:
#   cp <schwung>/src/host/plugin_api_v1.h  src/include/
#   cp <schwung>/src/host/midi_fx_api_v1.h src/include/
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=acid-builder

# Sanity: headers must be present (kept out of git; see README).
if [[ ! -f src/include/plugin_api_v1.h || ! -f src/include/midi_fx_api_v1.h ]]; then
  echo "ERROR: missing vendored headers in src/include/" >&2
  echo "  copy plugin_api_v1.h and midi_fx_api_v1.h from a Schwung checkout's src/host/" >&2
  exit 1
fi

echo "== Building toolchain image =="
docker build -t "$IMG" scripts

echo "== Cross-compiling + packaging =="
docker run --rm -v "$PWD":/build -w /build "$IMG" bash -euxc '
  CROSS=aarch64-linux-gnu-
  rm -rf dist && mkdir -p dist/acid

  ${CROSS}gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Isrc/include \
      src/acid/dsp/acid.c \
      -o dist/acid/dsp.so -lm

  cp src/acid/module.json dist/acid/
  cp src/acid/help.json   dist/acid/

  file dist/acid/dsp.so

  ( cd dist && tar -czf acid-module.tar.gz acid )
  ls -la dist
'

echo "== Done =="
echo "dist/acid-module.tar.gz"
