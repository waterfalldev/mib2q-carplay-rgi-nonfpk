#!/bin/bash
# Runs inside the container (run.sh): /src = source tree (ro), /h = tests/renderer (ro).
MODE=$1
export PATH=/usr/sbin:/usr/bin:/sbin:/bin       # host gcc/g++, not the image's QNX cross tools
set -e
SDP=/opt/qnx650/target/qnx6/usr/include
B=/tmp/b; mkdir -p $B /tmp/inc
cp -r "$SDP/EGL" "$SDP/GLES2" "$SDP/KHR" /tmp/inc/
INC="-I/src/maneuver_render -I/src/common -I/tmp/inc -I/h/shim -I/h"
# No ASan (see tests/most/run-native-tests.sh); UBSan aborts on the first finding.
SAN="-fsanitize=undefined -fno-sanitize-recover=all"
CF="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-result -DPLATFORM_QNX -D__QNXNTO__ $SAN $INC"
XF="-std=c++11 -fno-exceptions -fno-rtti -O1 -g -Wall -Wextra -DPLATFORM_QNX -D__QNXNTO__ $SAN $INC"

build_common() {
    local j=0
    for s in render route_path server; do
        gcc $CF -c /src/maneuver_render/$s.c -o $B/$s.o 2>$B/$s.log & 
    done
    for s in scene geometry layout lane_panel; do
        g++ $XF -c /src/maneuver_render/scene/$s.cpp -o $B/sc_$s.o 2>$B/sc_$s.log &
    done
    gcc $CF -c /h/gl_fake.c -o $B/gl_fake.o 2>$B/gl_fake.log &
    gcc $CF -c /h/fake_platform.c -o $B/fake_platform.o 2>$B/fake_platform.log &
    gcc $CF -c /src/maneuver_render/maneuver.c -o $B/maneuver.o 2>$B/maneuver.log &
    wait
    for o in render route_path server sc_scene sc_geometry sc_layout sc_lane_panel gl_fake fake_platform maneuver; do
        [ -f $B/$o.o ] || { echo "BUILD FAILED: $o" >&2; head -40 $B/${o}.log $B/sc_${o#sc_}.log 2>/dev/null >&2; exit 1; }
    done
}
SCENE_OBJS="$B/sc_scene.o $B/sc_geometry.o $B/sc_layout.o $B/sc_lane_panel.o"
LINK="g++ $SAN"

if [ "$MODE" = fingerprint ] || [ "$MODE" = digest ]; then
    build_common
    gcc $CF -c /h/fingerprint.c -o $B/fingerprint.o 2>$B/fp.log || { head -60 $B/fp.log >&2; echo "BUILD FAILED: fingerprint" >&2; exit 1; }
    [ -s $B/fp.log ] && head -30 $B/fp.log >&2
    # maneuver.o is linked, not included: fingerprint.c only uses the public engine API.
    $LINK $B/fingerprint.o $B/maneuver.o $B/render.o $B/route_path.o $B/server.o $B/gl_fake.o \
        $B/fake_platform.o $SCENE_OBJS -lm -lpthread -o $B/fingerprint
    cd $B
    FP_OUT=$B/fp.txt timeout 600 ./fingerprint 2>$B/fp.err || { tail -30 $B/fp.err >&2; echo "fingerprint run FAILED" >&2; exit 1; }
    grep "^fingerprint:" $B/fp.err >&2 || true
    if [ "$MODE" = digest ]; then
        # One "<md5> <unit>" line per unit: small enough to commit, and names what changed.
        mkdir -p $B/u
        awk -v d=$B/u '/^UNIT /{if (f) close(f); n++; f=sprintf("%s/%04d", d, n)} n{print > f}' $B/fp.txt
        for f in $B/u/*; do echo "$(md5sum < $f | cut -c1-32) $(head -1 $f | cut -c6-)"; done
        exit 0
    fi
    cat $B/fp.txt
    exit 0
fi

fail=0
build_common
# --- lane_guidance_test: header-only, no renderer objects
gcc $CF -c /src/tests/lane_guidance_test.c -o $B/lane_guidance_test.o 2>$B/t1.log && \
    gcc $SAN $B/lane_guidance_test.o -o $B/lane_guidance_test 2>>$B/t1.log || { head -30 $B/t1.log; echo "lane_guidance_test: BUILD FAILED"; fail=1; }
# --- renderer_peer_loss_test: includes server.c
gcc $CF /src/tests/renderer_peer_loss_test.c -o $B/renderer_peer_loss_test 2>$B/t3.log || { head -30 $B/t3.log; echo "renderer_peer_loss_test: BUILD FAILED"; fail=1; }
# --- maneuver_parity_test: includes main.c + maneuver.c; renderer, scene and GL/platform fakes linked
gcc $CF -c /src/tests/maneuver_parity_test.c -o $B/maneuver_parity_test.o 2>$B/t2.log || { head -30 $B/t2.log; echo "maneuver_parity_test: BUILD FAILED"; fail=1; }
if [ -f $B/maneuver_parity_test.o ]; then
    $LINK $B/maneuver_parity_test.o $B/render.o $B/route_path.o $B/server.o $B/gl_fake.o $B/fake_platform.o \
        $SCENE_OBJS -lm -lpthread -o $B/maneuver_parity_test 2>$B/t2l.log || { head -30 $B/t2l.log; echo "maneuver_parity_test: LINK FAILED"; fail=1; }
fi
cd $B
for t in lane_guidance_test maneuver_parity_test renderer_peer_loss_test; do
    [ -x $B/$t ] || continue
    if timeout 120 $B/$t >$B/$t.out 2>$B/$t.err; then
        echo "$t: $(tail -1 $B/$t.out)"
    else
        echo "$t: FAIL (exit $?)"; tail -15 $B/$t.out; tail -15 $B/$t.err; fail=1
    fi
done
[ $fail = 0 ] && echo "ALL PASS" || echo "FAILED"
exit $fail
