#!/bin/sh
# Native ASan/UBSan/LSan checks. No hardware, API key or installation required.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
test_build=${1:-${XDG_CACHE_HOME:-${HOME:-/var/tmp}/.cache}/svxlink-sanitize-native}
test_python=${CHAT_TEST_PYTHON:-python3}
flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:abort_on_error=1:quarantine_size_mb=16}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
cmake -S src -B "$test_build" -DUSE_QT=OFF -DBUILD_CHAT_TESTS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
  -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
  -DCMAKE_MODULE_LINKER_FLAGS="$flags" -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$test_build" --parallel "${JOBS:-6}"
"$test_python" tests/test_upstream_merge.py --build "$test_build" --sanitizers
"$test_python" tests/virtual_radio.py --build "$test_build"
"$test_python" tests/chat_native.py --build "$test_build"
"$test_python" tests/frn_routing_virtual.py --build "$test_build"
"$test_python" tests/reflector_virtual.py --build "$test_build"
"$test_python" tests/test_svxstats_radios.py --sanitizers
g++ -std=c++11 -g $flags -pthread -I src/svxlink/svxlink \
  src/svxlink/svxlink/AiorsLedClient.cpp tests/aiors_led_client.cpp -o "$test_build/led-test"
"$test_build/led-test"
echo 'PASS native sanitizer checks (address, undefined behavior, leaks)'
