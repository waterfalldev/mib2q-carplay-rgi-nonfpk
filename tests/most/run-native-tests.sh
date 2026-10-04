#!/bin/bash
# maneuver_render host tests, run with the host gcc inside the QNX toolchain image
# (the same image that builds the ARM binary).
#
#   run-native-tests.sh <renderer tree>
#
# <renderer tree> is the source tree (maneuver_render/, common/, toolchain/).
# Each test #includes the real renderer sources and fakes only the QNX screen / EGL / GL
# driver calls; the SDP's own EGL/GLES2/KHR headers are used, with hostshim/ standing in
# for the QNX libc headers screen.h pulls in.
set -e

[ "$#" -eq 1 ] || { echo "usage: run-native-tests.sh <renderer tree>"; exit 2; }
IMG=${QNX_TOOLCHAIN_IMAGE:-qnx65-armv7-toolchain:latest}
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$1" && pwd)"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # Docker Desktop wants Windows paths; stop MSYS rewriting the container paths.
        export MSYS_NO_PATHCONV=1
        TESTS_DIR="$(cygpath -m "$TESTS_DIR")"
        SRC_DIR="$(cygpath -m "$SRC_DIR")"
        ;;
esac

docker image inspect "$IMG" >/dev/null 2>&1 || { echo "ERROR: docker image '$IMG' not found."; exit 1; }

docker run --rm --platform=linux/amd64 -v "$SRC_DIR":/src:ro -v "$TESTS_DIR":/tests:ro "$IMG" bash -c '
  set -e
  export PATH=/usr/sbin:/usr/bin:/sbin:/bin     # host gcc/as, not the image'"'"'s QNX cross tools
  SDP=/opt/qnx650/target/qnx6/usr/include
  mkdir -p /tmp/inc /tmp/bin
  cp -r "$SDP/EGL" "$SDP/GLES2" "$SDP/KHR" /src/toolchain/qnx65-abi/include/screen /tmp/inc/
  # No ASan: GCC 10'"'"'s libasan spins in DEADLYSIGNAL at random under the high mmap ASLR
  # entropy of current kernels, and Docker'"'"'s seccomp profile refuses setarch -R.
  CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-result -D__QNXNTO__ -fsanitize=undefined \
          -fno-sanitize-recover=all -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
          -I/src/maneuver_render -I/src/common -I/tmp/inc -I/tests/hostshim"
  failed=0
  for test in most_output_platform_test most_output_render_test most_mask_cache_test map_layer_nv12_test; do
    [ -f "/tests/$test.c" ] || { echo "Missing suite $test"; exit 1; }
    echo "--- $test ---"
    extra=""
    [ "$test" = most_mask_cache_test ] && extra="/src/maneuver_render/maneuver.c /src/maneuver_render/route_path.c"
    gcc $CFLAGS "/tests/$test.c" $extra -lm -lpthread -o "/tmp/bin/$test"
    if ! timeout 60 "/tmp/bin/$test" 2>"/tmp/bin/$test.stderr"; then
      failed=1
      tail -20 "/tmp/bin/$test.stderr"
    fi
  done
  exit $failed
'
