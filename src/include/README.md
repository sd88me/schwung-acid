# Vendored Schwung API headers

`plugin_api_v1.h` and `midi_fx_api_v1.h` are verbatim copies of the host ABI
headers from [charlesvestal/schwung](https://github.com/charlesvestal/schwung),
`src/host/`. The DSP (`src/acid/dsp/acid.c`) compiles against them.

They are committed here so the module builds from a clean clone with no Schwung
checkout. They change rarely; re-sync only when a Schwung release note says the
plugin ABI moved:

```bash
S=<path to a schwung checkout>
cp "$S/src/host/plugin_api_v1.h"  src/include/
cp "$S/src/host/midi_fx_api_v1.h" src/include/
```

Or without a checkout:

```bash
REF=main   # or a tag, e.g. v1.3.3
for h in plugin_api_v1.h midi_fx_api_v1.h; do
  curl -fsSL "https://raw.githubusercontent.com/charlesvestal/schwung/$REF/src/host/$h" \
    -o "src/include/$h"
done
```

**Synced with:** Schwung `src/host/` as of the v1.3.x line (Sept 2026).
Both headers still declare `*_API_VERSION 1`.
