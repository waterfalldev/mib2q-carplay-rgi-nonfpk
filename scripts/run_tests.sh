#!/bin/sh
# Host-side C tests for the RGI hook framework and maneuver renderer.
# Java/renderer suites: test_route_info.sh, test_java_transports.sh,
# test_maneuver_native.sh.  Safe to run anywhere (no HU access).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
if [ "$(uname)" = Darwin ]; then DEAD_STRIP=-Wl,-dead_strip; DL_LIB=; RT_LIB=; else DEAD_STRIP=-Wl,--gc-sections; DL_LIB=-ldl; RT_LIB=-lrt; fi

printf '%-32s ' maneuver_surface_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -ffunction-sections -fdata-sections -Imaneuver_render/hostcheck -Icommon \
    tests/maneuver_surface_test.c "$DEAD_STRIP" -lpthread -lm $DL_LIB -o "$OUT/maneuver_surface"
"$OUT/maneuver_surface"

printf '%-32s ' gl_program_cache_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -Imaneuver_render/hostcheck -Icommon tests/gl_program_cache_test.c $DL_LIB -o "$OUT/gl_program_cache"
"$OUT/gl_program_cache"

printf '%-32s ' frame_pacer_test
cc -D_GNU_SOURCE -std=c99 -O1 -Wall -Wextra -Werror -pedantic -Wno-unused-function \
    -Imaneuver_render tests/frame_pacer_test.c -o "$OUT/frame_pacer"
"$OUT/frame_pacer"

printf '%-32s ' frame_profile_test
cc -D_GNU_SOURCE -std=c99 -O1 -Wall -Wextra -Werror -pedantic -Wno-unused-function \
    -Imaneuver_render tests/frame_profile_test.c -o "$OUT/frame_profile"
"$OUT/frame_profile"

printf '%-32s ' coverart_safety_test
cc -D_GNU_SOURCE -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ihook \
    tests/coverart_safety_test.c hook/coverart/jpeg_safety.c \
    $DL_LIB -o "$OUT/coverart_safety"
"$OUT/coverart_safety"

printf '%-32s ' coverart_stream_test
cc -D_GNU_SOURCE -std=c99 -O2 -Wall -Wextra -Werror -Ihook \
    tests/coverart_stream_test.c hook/coverart/coverart_stream.c \
    hook/framework/iap2_protocol.c $DL_LIB -o "$OUT/coverart_stream"
"$OUT/coverart_stream"

printf '%-32s ' coverart_pipeline_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
    -Ihook "-DCOVERART_DIR=\"$OUT/artwork\"" tests/coverart_pipeline_test.c \
    hook/coverart/jpeg_safety.c hook/coverart/coverart_stream.c \
    hook/framework/iap2_protocol.c -lpthread -lz -lm $DL_LIB -o "$OUT/coverart_pipeline"
"$OUT/coverart_pipeline"

printf '%-32s ' rgd_tlv_test
cc -D_GNU_SOURCE -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-function \
    -DENABLE_LOGGING=0 -Ihook tests/rgd_tlv_test.c hook/routeguidance/rgd_tlv.c \
    $DL_LIB -o "$OUT/rgd_tlv"
"$OUT/rgd_tlv"

printf '%-32s ' rgd_start_burst_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
    -Wno-unused-variable -Wno-unused-but-set-variable -DENABLE_LOGGING=0 -Ihook -Ihook/framework -Itests \
    tests/rgd_start_burst_test.c hook/routeguidance/rgd_tlv.c hook/framework/iap2_protocol.c \
    -lpthread $DL_LIB -o "$OUT/rgd_start_burst"
"$OUT/rgd_start_burst"

printf '%-32s ' altscreen_hook_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -DENABLE_LOGGING=0 \
    -DALT_SHOW_UI_DELAY_MS=10 -DALT_NIGHT_TOGGLE_MS=10 -DALT_HOLD_CONNECT_MS=400 "-DCVR_SHM_NAME=\"/cr_cluster_video_test$$\"" \
    -Ihook -Icommon tests/altscreen_hook_test.c hook/altscreen/altscreen_hook.c hook/framework/cflite.c \
    -rdynamic -lpthread $DL_LIB $RT_LIB -o "$OUT/altscreen_hook"
"$OUT/altscreen_hook"

printf '%-32s ' airplay_seams_test
cc -std=gnu99 -O1 -Wall -Wextra -Werror -fPIC -shared tests/fake_airplay_teardown.c \
    -o "$OUT/libfake_airplay.so"
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -DENABLE_LOGGING=0 -Ihook -Itests \
    tests/airplay_seams_test.c hook/framework/airplay_seams.c "-L$OUT" -lfake_airplay "-Wl,-rpath,$OUT" \
    -lpthread $DL_LIB -o "$OUT/airplay_seams"
"$OUT/airplay_seams"

printf '%-32s ' cluster_video_ring_test
cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror -DCVR_DATA_BYTES='(1u << 16)' \
    "-DCVR_SHM_NAME=\"/cr_cluster_video_ring$$\"" -Icommon -Itests tests/cluster_video_ring_test.c \
    -lpthread $RT_LIB -o "$OUT/cluster_video_ring"
"$OUT/cluster_video_ring"

printf '%-32s ' cluster_video_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror "-DCVR_SHM_NAME=\"/cr_cluster_video_reader$$\"" \
    -Icommon -Itests tests/cluster_video_test.c -lpthread $RT_LIB -o "$OUT/cluster_video"
"$OUT/cluster_video"

printf '%-32s ' cluster_decoder_test
cc -D_GNU_SOURCE -std=gnu99 -O1 -Wall -Wextra -Werror -Itests tests/cluster_decoder_test.c \
    -rdynamic -lpthread $DL_LIB $RT_LIB -o "$OUT/cluster_decoder"
"$OUT/cluster_decoder"

printf '%-32s ' inject_generation_test
cc -D_GNU_SOURCE -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -DENABLE_LOGGING=0 -Ihook -Ihook/framework tests/inject_generation_test.c $DL_LIB -o "$OUT/inject_generation"
"$OUT/inject_generation"

printf '%-32s ' state_trace_test
cc -D_GNU_SOURCE -std=c99 -O2 -Wall -Wextra -Werror -Ihook \
    -DENABLE_LOGGING=0 -DENABLE_STATE_TRACE=1 \
    tests/state_trace_test.c hook/framework/state_trace.c \
    hook/framework/iap2_protocol.c $DL_LIB -o "$OUT/state_trace"
"$OUT/state_trace" && echo OK

printf '%-32s ' signal_guard_test
cc -D_GNU_SOURCE -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ihook \
    tests/signal_guard_test.c hook/framework/signal_guard.c \
    $DL_LIB -o "$OUT/signal_guard"
"$OUT/signal_guard"

printf '%-32s ' bus_transport_test
cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror \
    -Wno-unused-variable -Wno-unused-but-set-variable -Ihook \
    tests/bus_transport_test.c hook/framework/signal_guard.c \
    -lpthread $DL_LIB -o "$OUT/bus_transport"
"$OUT/bus_transport"

printf '%-32s ' state_trace_logger_test
cc -D_GNU_SOURCE -std=c99 -O2 -Wall -Wextra -Werror -Ihook \
    -DENABLE_LOGGING=0 -DENABLE_STATE_TRACE=1 \
    tests/state_trace_logger_test.c hook/framework/logging.c \
    -lpthread $DL_LIB -o "$OUT/state_trace_logger"
"$OUT/state_trace_logger" && echo OK

printf '%-32s ' local_protocols
python3 scripts/check_local_protocols.py

printf '%-32s ' supervisor_lifecycle_test
sh scripts/test_supervisor_lifecycle.sh

printf '%-32s ' install_dio_test
sh scripts/test_install_dio.sh

printf '%-32s ' logging_mib_test
sh scripts/test_logging_mib.sh

printf '%-32s ' install_listing_test
sh scripts/test_install_listing.sh

printf '%-32s ' install_payload_test
sh scripts/test_install_payload.sh
