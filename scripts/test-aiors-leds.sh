#!/bin/sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_build=$(mktemp -d /tmp/svxlink-led-tests.XXXXXX)
trap 'rm -rf -- "$test_build"' EXIT HUP INT TERM
client="$repo/src/svxlink/svxlink"
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread -I"$client" \
  "$repo/tests/aiors_led_client.cpp" "$client/AiorsLedClient.cpp" -o "$test_build/client"
"$test_build/client"
if command -v tclsh >/dev/null 2>&1; then
  tclsh "$repo/tests/aiors_led_hooks.tcl" "$repo/events.d/local/aiors.tcl"
elif pkg-config --exists tcl; then
  g++ $(pkg-config --cflags tcl) "$repo/tests/aiors_tcl_runner.cpp" \
    $(pkg-config --libs tcl) -o "$test_build/tcl-runner"
  "$test_build/tcl-runner" "$repo/tests/aiors_led_hooks.tcl" "$repo/events.d/local/aiors.tcl"
else
  echo 'Tcl hook test skipped: install tclsh or the native Tcl development package.'
fi
if [ "$#" -ge 1 ]; then
  bsp=$(CDPATH='' cd -- "$1" && pwd)
  for source in cfg cli gpio i2c uart usb cm108 radio_type radio_sa818 radio_sa858 radio_ext util log routing; do
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DGPIO_FORCE_STUB \
      -DCMD_DIR='"/tmp/svxlink-aiors-bridge-test"' -I"$bsp/include" -I"$bsp/src" \
      -c "$bsp/src/$source.c" -o "$test_build/$source.o"
  done
  g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread -DGPIO_FORCE_STUB \
    -I"$bsp/include" -I"$bsp/src" -I"$client" "$repo/tests/aiors_bsp_bridge.cpp" \
    "$client/AiorsLedClient.cpp" "$test_build"/*.o -lm -o "$test_build/bridge"
  "$test_build/bridge"
fi
