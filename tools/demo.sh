#!/usr/bin/env bash
# Demo firmware: a made-up family, events and weather instead of Google (for screenshots).
#   tools/demo.sh build             build build-demo/homeplanner.bin (your build/ and sdkconfig stay as they are)
#   tools/demo.sh install [host]    build it and install it on the panel over Wi-Fi
# Then: tools/screenshot.sh, and to go back: idf.py build && tools/ota.sh
# The panel's settings and Google sign-in aren't touched; the demo saves nothing to flash.
set -euo pipefail
cd "$(dirname "$0")/.."
command -v idf.py >/dev/null || { echo "run '. ~/esp/esp-idf/export.sh' first" >&2; exit 1; }
case "${1:-}" in
  build|install) ;;
  *) echo "usage: tools/demo.sh build | install [panel address]" >&2; exit 1 ;;
esac
idf.py -B build-demo -D SDKCONFIG="$PWD/build-demo/sdkconfig" -D HP_DEMO=1 build
grep -q "CONFIG_HP_DEMO=y" build-demo/sdkconfig || { echo "demo: CONFIG_HP_DEMO isn't on in build-demo/sdkconfig" >&2; exit 1; }
if [ "$1" = install ]; then tools/ota.sh "${2:-homeplanner.local}" build-demo/homeplanner.bin; fi
