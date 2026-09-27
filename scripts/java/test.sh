#!/bin/bash
# Container-side tests of the shipping JAR. Java stays in the shared JDK 8 image.
set -euo pipefail
SOURCE_ROOT=${SOURCE_ROOT:-/src}
JAVA_OUTPUT=${JAVA_OUTPUT:-/out}
STOCK_JAR=${STOCK_JAR:-/inputs/stock.jar}
STOCK_RUNTIME_JAR=${STOCK_RUNTIME_JAR:-$STOCK_JAR}
BOOT_JAR=${BOOT_JAR:-$STOCK_JAR}
CARPLAY_DEPENDENCIES=${CARPLAY_DEPENDENCIES:-/deps}
ASM_JAR=${ASM_JAR:-/inputs/asm.jar}
ASM_TREE_JAR=${ASM_TREE_JAR:-/inputs/asm-tree.jar}
PATCH_JAR=${CARPLAY_HOOK_JAR:-$JAVA_OUTPUT/carplay_hook.jar}
GROUP=${1:-all}
case "$GROUP" in all|route|transports|pdc|linkage|maneuvers|contract) ;;
    *) echo "Unknown Java test group: $GROUP" >&2; exit 2;; esac
for input in "$PATCH_JAR" "$STOCK_JAR" "$STOCK_RUNTIME_JAR"; do
    test -f "$input" || { echo "Missing Java test input: $input" >&2; exit 1; }
done
TESTS="$SOURCE_ROOT/tests"
CORE="$SOURCE_ROOT/java_patch/com/luka/carplay/core"
STUBS="$TESTS/stubs"
REPORTS="$JAVA_OUTPUT/tests"
mkdir -p "$REPORTS"
# Keep compiler/JVM temporary files off the host bind mount, and run with an
# empty current directory so VC text data can only come from the shipping JAR.
WORK=$(mktemp -d)
trap 'wait; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/cwd"
# JVMs repeatedly read the large firmware archive. Copy each unique input once
# to Linux storage rather than doing those reads through a Windows bind mount.
# Keep basenames: the stock linkage audit distinguishes -final.jar inventories.
staged_sources=() staged_targets=()
stage_jar() {
    local input=$1 label=$2 i
    for ((i=0; i<${#staged_sources[@]}; i++)); do
        if [ "$input" = "${staged_sources[$i]}" ] || [ "$input" -ef "${staged_sources[$i]}" ]; then
            STAGED_JAR=${staged_targets[$i]}
            return
        fi
    done
    mkdir -p "$WORK/jars/$label"
    STAGED_JAR="$WORK/jars/$label/${input##*/}"
    cp "$input" "$STAGED_JAR"
    staged_sources+=("$input"); staged_targets+=("$STAGED_JAR")
}
stage_jar "$STOCK_JAR" stock; STOCK_JAR=$STAGED_JAR
stage_jar "$STOCK_RUNTIME_JAR" runtime; STOCK_RUNTIME_JAR=$STAGED_JAR
stage_jar "$PATCH_JAR" patch; PATCH_JAR=$STAGED_JAR
stage_jar "$CARPLAY_DEPENDENCIES/org.osgi.framework-1.10.0.jar" framework; OSGI_FRAMEWORK=$STAGED_JAR
stage_jar "$CARPLAY_DEPENDENCIES/org.osgi.util.tracker-1.5.4.jar" tracker; OSGI_TRACKER=$STAGED_JAR
case "$GROUP" in
    all|transports|pdc|linkage)
        stage_jar "$ASM_JAR" asm; ASM_JAR=$STAGED_JAR
        stage_jar "$ASM_TREE_JAR" asm-tree; ASM_TREE_JAR=$STAGED_JAR ;;
esac
if [ "$GROUP" = all ] || [ "$GROUP" = linkage ]; then
    stage_jar "$BOOT_JAR" boot; BOOT_JAR=$STAGED_JAR
fi
OSGI="$OSGI_FRAMEWORK:$OSGI_TRACKER"
ASM="$ASM_JAR:$ASM_TREE_JAR"
HOST_CP="$PATCH_JAR:$STOCK_JAR:$OSGI"
RUNTIME_CP="$PATCH_JAR:$STOCK_RUNTIME_JAR:$OSGI"
JOBS=${JAVA_TEST_JOBS:-$(getconf _NPROCESSORS_ONLN)}
case "$JOBS" in ''|*[!0-9]*|0) echo 'JAVA_TEST_JOBS must be a positive integer' >&2; exit 2;; esac
names=() defects=() logs=() statuses=()
active=0
compile() {
    local name=$1 cp=$2
    shift 2
    COMPILED_DIR="$WORK/$name"
    mkdir -p "$COMPILED_DIR"
    local args=(-nowarn -encoding UTF-8 -d "$COMPILED_DIR")
    if [ -n "$cp" ]; then args+=(-cp "$cp"); fi
    if ! javac "${args[@]}" "$@" > "$REPORTS/$GROUP-$name-compile.log" 2>&1; then
        cat "$REPORTS/$GROUP-$name-compile.log" >&2
        exit 1
    fi
}
queue() {
    local name=$1 cp=$2 main=$3 verify=$4 defect=$5
    shift 5
    local index=${#names[@]}
    names+=("$name"); defects+=("$defect")
    logs+=("$REPORTS/$GROUP-$index.log"); statuses+=("$WORK/$index.status")
    local args=()
    if [ "$verify" = no ]; then args+=(-Xverify:none); fi
    (
        cd "$WORK/cwd"
        set +e
        java "${args[@]}" -cp "$cp" "$main" "$@" > "${logs[$index]}" 2>&1
        printf '%s\n' "$?" > "${statuses[$index]}"
    ) &
    active=$((active + 1))
    if [ "$active" -ge "$JOBS" ]; then wait -n; active=$((active - 1)); fi
}

route_sources=(VCTextScrollTest RouteInfoPresentationTest RendererMapperDirectionTest
    RouteGuidanceDeltaTest DistanceBargraphChainTest KomoGraphicsStateTest
    ManeuverParityTest RendererViewportTest ClusterKdkSyncTest ClusterKdkBapChainTest
    ClusterKdkRendererLifecycleTest LaneGuidanceTransportTest LaneGuidanceLifecycleTest
    RgiDeliveryRecoveryTest CurrentPositionDeliveryTest RouteInfoTimeoutTest
    CurrentPositionStockChainTest ManeuverChainAudit RgdTeardownTest NativeGuidanceGateTest
    NativeGuidanceStateTest MostArrowsTest MostViewHandshakeTest ClusterOwnershipTest ClusterCoreTest)
transport_sources=(TouchpadControllerTest CarplayBusTransportTest RendererServerTransportTest GatedCombiServiceInitStateTest)
sources=()
if [ "$GROUP" = all ] || [ "$GROUP" = route ]; then
    for suite in "${route_sources[@]}"; do sources+=("$TESTS/$suite.java"); done
fi
if [ "$GROUP" = all ] || [ "$GROUP" = transports ]; then
    for suite in "${transport_sources[@]}"; do sources+=("$TESTS/$suite.java"); done
fi
if [ ${#sources[@]} -gt 0 ]; then
    compile classes "$HOST_CP" "${sources[@]}"
    classes=$COMPILED_DIR
fi
if [ "$GROUP" = all ] || [ "$GROUP" = route ]; then
    unicode_args=()
    if [ -n "${VC_UNICODE_TEST_DIR:-}" ]; then unicode_args+=("$VC_UNICODE_TEST_DIR"); fi
    queue VCTextScrollTest "$classes:$PATCH_JAR" com.luka.carplay.rgd.VCTextScrollTest yes '' "${unicode_args[@]}"
    for suite in RouteInfoPresentationTest RendererMapperDirectionTest RouteGuidanceDeltaTest; do
        queue "$suite" "$classes:$HOST_CP" "$suite" yes ''
    done
    for suite in DistanceBargraphChainTest ManeuverParityTest RendererViewportTest ClusterKdkSyncTest ClusterKdkBapChainTest ClusterKdkRendererLifecycleTest; do
        queue "$suite" "$classes:$HOST_CP" "$suite" no ''
    done
    queue KomoGraphicsStateTest "$classes:$RUNTIME_CP" KomoGraphicsStateTest no ''
    queue LaneGuidanceTransportTest "$classes:$HOST_CP" com.luka.carplay.rgd.LaneGuidanceTransportTest no '' "$JAVA_OUTPUT/lane-guidance-wire.bin"
    for suite in LaneGuidanceLifecycleTest RgiDeliveryRecoveryTest CurrentPositionDeliveryTest RouteInfoTimeoutTest; do
        queue "$suite" "$classes:$HOST_CP" "com.luka.carplay.rgd.$suite" no ''
    done
    queue CurrentPositionStockChainTest "$classes:$RUNTIME_CP" com.luka.carplay.rgd.CurrentPositionStockChainTest no ''
    for main in com.luka.carplay.core.RgdTeardownTest com.luka.carplay.core.NativeGuidanceGateTest NativeGuidanceStateTest MostArrowsTest MostViewHandshakeTest ClusterOwnershipTest com.luka.carplay.core.ClusterCoreTest; do
        queue "${main##*.}" "$classes:$HOST_CP" "$main" no ''
    done
fi
if [ "$GROUP" = all ] || [ "$GROUP" = transports ]; then
    for main in TouchpadControllerTest com.luka.carplay.bus.CarplayBusTransportTest com.luka.carplay.rgd.RendererServerTransportTest; do
        queue "${main##*.}" "$classes:$PATCH_JAR" "$main" yes ''
    done
    queue GatedCombiServiceInitStateTest "$classes:$PATCH_JAR:$STOCK_JAR" com.luka.carplay.rgd.GatedCombiServiceInitStateTest yes ''
    compile lifecycle '' "$CORE/CarPlayApp.java" "$CORE/Module.java" \
        "$TESTS/CarPlayAppLifecycleTest.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/bus/CarplayBus.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/pdc/PdcSmallStageGuard.java" \
        "$STUBS/app-lifecycle/de/audi/app/terminalmode/IContext.java" \
        "$STUBS/app-lifecycle/de/audi/atip/base/IFrameworkAccess.java"
    for scenario in publication during-start replug failure bounce; do
        queue "CarPlayAppLifecycleTest $scenario" "$COMPILED_DIR" com.luka.carplay.core.CarPlayAppLifecycleTest yes '' "$scenario"
    done
fi
# The original transport entry point includes the parking regression checks.
if [ "$GROUP" = all ] || [ "$GROUP" = transports ] || [ "$GROUP" = pdc ]; then
    compile pdc-stubs "$STOCK_RUNTIME_JAR:$OSGI" \
        "$STUBS/pdc/com/luka/carplay/core/CarPlayApp.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java" \
        "$STUBS/pdc/de/audi/atip/hmi/view/Screen.java" \
        "$STUBS/pdc/de/esolutions/hmi/widgets/audi/base/AbstractScreenWidget.java"
    pdc_stubs=$COMPILED_DIR
    pdc_base="$pdc_stubs:$PATCH_JAR:$STOCK_RUNTIME_JAR:$OSGI:$ASM"
    compile pdc-tests "$pdc_base" "$TESTS/PdcResourcePolicyTest.java" \
        "$TESTS/PdcExternalEventsTest.java" "$TESTS/OpsAudioDrawerTest.java" "$TESTS/OpsStatusLineTest.java"
    pdc_tests=$COMPILED_DIR
    for suite in PdcResourcePolicyTest PdcExternalEventsTest OpsAudioDrawerTest OpsStatusLineTest; do
        queue "$suite" "$pdc_tests:$pdc_base" "$suite" yes ''
    done
    compile pdc-lifecycle "$pdc_base" "$CORE/CarPlayApp.java" "$CORE/Module.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/bus/CarplayBus.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java"
    pdc_lifecycle_cp="$COMPILED_DIR:$pdc_tests:$pdc_base"
    compile pdc-lifecycle-test "$pdc_lifecycle_cp" "$TESTS/CarPlayPdcLifecycleTest.java"
    queue CarPlayPdcLifecycleTest "$COMPILED_DIR:$pdc_lifecycle_cp" CarPlayPdcLifecycleTest yes ''
    stock_first="$pdc_tests:$pdc_stubs:$STOCK_RUNTIME_JAR:$PATCH_JAR:$OSGI:$ASM"
    queue 'PdcResourcePolicyTest on stock' "$stock_first" PdcResourcePolicyTest yes 'pure OPS 108 toggled Main Wizard'
    queue 'OpsAudioDrawerTest on stock' "$stock_first" OpsAudioDrawerTest yes 'APS drawer still selected over CarPlay + side OPS'
    queue 'OpsStatusLineTest on stock' "$stock_first" OpsStatusLineTest yes 'MMI status line 62 remained over CarPlay'
fi
if [ "$GROUP" = all ] || [ "$GROUP" = route ] || [ "$GROUP" = contract ]; then
    if [ -n "${RGD_CONTRACT_FRAMES:-}" ]; then
        frames=()
        for frame in old-slot.txt new-no-angle.txt new-known-angle.txt new-generation.txt; do
            test -f "$RGD_CONTRACT_FRAMES/$frame" || { echo "Missing native contract frame: $frame" >&2; exit 1; }
            frames+=("$RGD_CONTRACT_FRAMES/$frame")
        done
        compile rgd-contract "$HOST_CP" "$TESTS/ManeuverChainAudit.java" "$TESTS/RgdNativeContractProbe.java"
        queue RgdNativeContractProbe "$COMPILED_DIR:$HOST_CP" RgdNativeContractProbe no '' "${frames[@]}"
    elif [ "$GROUP" = contract ]; then
        echo 'RGD_CONTRACT_FRAMES is required for the contract group' >&2; exit 1
    else
        echo 'SKIPPED RgdNativeContractProbe: no native frames supplied (the package build supplies them)'
    fi
fi
if [ "$GROUP" = maneuvers ]; then
    compile maneuvers "$HOST_CP" "$TESTS/ManeuverChainAudit.java" "$TESTS/RampDescriptorAudit.java" "$TESTS/ManeuverIconSelectionAudit.java"
    # These exporters report row counts rather than PASS and are not test suites.
    for item in ManeuverChainAudit:java_mapping.csv RampDescriptorAudit:ramp_mapping.csv ManeuverIconSelectionAudit:selection_examples.csv; do
        java -Xverify:none -cp "$COMPILED_DIR:$HOST_CP" "${item%%:*}" "$JAVA_OUTPUT/${item#*:}"
    done
fi

wait
report="$JAVA_OUTPUT/host-tests.tsv"
: > "$report"
failed=0
for ((i=0; i<${#names[@]}; i++)); do
    status=$(cat "${statuses[$i]}")
    summary=$(awk 'NF {last=$0} END {print last}' "${logs[$i]}" | tr -d '\r')
    if [ -n "${defects[$i]}" ]; then
        if [ "$status" -eq 0 ] || ! grep -Fq "${defects[$i]}" "${logs[$i]}"; then
            echo "FAIL ${names[$i]}: did not reproduce stock defect '${defects[$i]}'" >&2
            cat "${logs[$i]}" >&2; failed=1; continue
        fi
        summary="PASS (unpatched stock fails: ${defects[$i]})"
    elif [ "$status" -ne 0 ] || [[ "$summary" != *PASS* ]]; then
        echo "FAIL ${names[$i]} (exit $status):" >&2
        cat "${logs[$i]}" >&2; failed=1; continue
    fi
    printf '%s\t%s\n' "${names[$i]}" "$summary" >> "$report"
    echo "PASS ${names[$i]}"
done
if [ "$failed" -ne 0 ]; then exit 1; fi

if [ "$GROUP" = all ] || [ "$GROUP" = linkage ]; then
    compile audit "$ASM" "$TESTS/JavaStockLinkageAudit.java"
    libraries=("$PATCH_JAR" "$STOCK_JAR")
    if [ "$BOOT_JAR" != "$STOCK_JAR" ]; then libraries+=("$BOOT_JAR"); fi
    libraries+=("$OSGI_FRAMEWORK" "$OSGI_TRACKER")
    if ! java -Xmx2g -cp "$COMPILED_DIR:$ASM" JavaStockLinkageAudit "${libraries[@]}" > "$JAVA_OUTPUT/stock-linkage.txt" 2>&1; then
        cat "$JAVA_OUTPUT/stock-linkage.txt" >&2
        exit 1
    fi
    cat "$JAVA_OUTPUT/stock-linkage.txt"
    tail -n 1 "$JAVA_OUTPUT/stock-linkage.txt" | grep -Eq '^JavaStockLinkageAudit: .*errors=0$' || {
        echo 'Stock linkage audit did not report zero errors' >&2; exit 1;
    }
fi
