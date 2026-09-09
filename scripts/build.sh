#!/usr/bin/env bash
# =============================================================================
# Build the Acid module for Ableton Move (ARM64) and package it into an
# installable tarball under dist/.
#
#   dist/acid-module.tar.gz   (folder: acid/)
#
# Requires Docker. Cross-compiles the DSP with aarch64-linux-gnu-gcc.
# The two Schwung ABI headers are vendored in src/include/ (committed) — no
# Schwung checkout needed. See src/include/README.md to re-sync them.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=acid-builder

# Sanity: vendored headers must be present (see src/include/README.md).
if [[ ! -f src/include/plugin_api_v1.h || ! -f src/include/midi_fx_api_v1.h ]]; then
  echo "ERROR: missing headers in src/include/ — they are committed; restore them" >&2
  echo "  with 'git checkout -- src/include/' or re-sync per src/include/README.md" >&2
  exit 1
fi

echo "== Building toolchain image =="
docker build -t "$IMG" scripts

echo "== Cross-compiling + packaging =="
# Run as the host user so dist/ isn't left root-owned.
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/build -w /build "$IMG" bash -euxc '
  CROSS=aarch64-linux-gnu-
  rm -rf dist && mkdir -p dist/acid

  ${CROSS}gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Isrc/include \
      src/acid/dsp/acid.c \
      -o dist/acid/dsp.so -lm

  cp src/acid/module.json dist/acid/
  cp src/acid/help.json   dist/acid/
  chmod 0755 dist/acid/dsp.so

  file dist/acid/dsp.so

  ( cd dist && tar -czf acid-module.tar.gz acid )
  ls -la dist
'

echo "== Done =="
echo "dist/acid-module.tar.gz"
