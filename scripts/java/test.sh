#!/bin/bash
# Container-side tests of the shipping JARs. Java stays in the shared JDK 8 image;
# scripts/java/docker.sh is the host launcher.
#
#   all         route transports pdc partial360 contract linkage
#   route       route guidance, cluster KDK, lanes, VC text (scripts/test_route_info.sh)
#   transports  local transports, AltScreen, lifecycle, then pdc (scripts/test_java_transports.sh)
#   pdc         parking resource policy and its stock regression controls (scripts/test_pdc.sh)
#   partial360  camera/display contracts (scripts/test_partial360_preflight.sh)
#   fuzz        seeded route/context fuzz (scripts/test_route_context_fuzz.sh); not part of all
#   contract    Java half of the native RGI contract (needs RGD_CONTRACT_FRAMES)
#   linkage     stock linkage audit of every variant (scripts/audit_java_variants.sh)
#
# STOCK_JAR is the main variant; STOCK_EU_JAR and STOCK_CN_JAR add the EU firmware (same
# carplay_hook.jar) and the CN firmware (carplay_hook_cn.jar) where the tests have per-variant runs.
set -euo pipefail
SOURCE_ROOT=${SOURCE_ROOT:-/src}
JAVA_OUTPUT=${JAVA_OUTPUT:-/out}
: "${STOCK_JAR:?}"
STOCK_RUNTIME_JAR=${STOCK_RUNTIME_JAR:-$STOCK_JAR}
BOOT_JAR=${BOOT_JAR:-}
CARPLAY_DEPENDENCIES=${CARPLAY_DEPENDENCIES:-/deps}
ASM_JAR=${ASM_JAR:-/inputs/asm.jar}
ASM_TREE_JAR=${ASM_TREE_JAR:-/inputs/asm-tree.jar}
PATCH_JAR=${CARPLAY_HOOK_JAR:-$JAVA_OUTPUT/carplay_hook.jar}
PATCH_CN_JAR=${CARPLAY_HOOK_CN_JAR:-$JAVA_OUTPUT/carplay_hook_cn.jar}
GROUP=${1:-all}
want_route=0 want_transports=0 want_pdc=0 want_partial360=0 want_fuzz=0 want_contract=0 want_linkage=0
case "$GROUP" in
    all) want_route=1 want_transports=1 want_pdc=1 want_partial360=1 want_contract=1 want_linkage=1 ;;
    route) want_route=1 want_contract=1 ;;
    transports) want_transports=1 want_pdc=1 ;;
    pdc) want_pdc=1 ;;
    partial360) want_partial360=1 ;;
    fuzz) want_fuzz=1 ;;
    contract) want_contract=1 ;;
    linkage) want_linkage=1 ;;
    *) echo "Unknown Java test group: $GROUP" >&2; exit 2 ;;
esac
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
if [ $((want_transports + want_pdc + want_partial360 + want_linkage)) -gt 0 ]; then
    stage_jar "$ASM_JAR" asm; ASM_JAR=$STAGED_JAR
    stage_jar "$ASM_TREE_JAR" asm-tree; ASM_TREE_JAR=$STAGED_JAR
fi
if [ -n "$BOOT_JAR" ]; then stage_jar "$BOOT_JAR" boot; BOOT_JAR=$STAGED_JAR; fi
# Variants: the main stock JAR, plus the EU and CN firmware when supplied. The EU firmware uses
# the same carplay_hook.jar and is skipped when it is the same file as the main stock JAR.
vnames=(main) vpatch=("$PATCH_JAR") vstock=("$STOCK_JAR")
if [ -n "${STOCK_EU_JAR:-}" ] && ! cmp -s "$STOCK_EU_JAR" "$STOCK_JAR"; then
    stage_jar "$STOCK_EU_JAR" stock-eu
    vnames+=(eu); vpatch+=("$PATCH_JAR"); vstock+=("$STAGED_JAR")
fi
if [ -n "${STOCK_CN_JAR:-}" ]; then
    test -f "$PATCH_CN_JAR" || { echo "Missing CN patch JAR: $PATCH_CN_JAR (build with STOCK_CN_JAR set)" >&2; exit 1; }
    stage_jar "$PATCH_CN_JAR" patch-cn; cn_patch=$STAGED_JAR
    stage_jar "$STOCK_CN_JAR" stock-cn
    vnames+=(cn); vpatch+=("$cn_patch"); vstock+=("$STAGED_JAR")
fi
OSGI="$OSGI_FRAMEWORK:$OSGI_TRACKER"
ASM="$ASM_JAR:$ASM_TREE_JAR"
HOST_CP="$PATCH_JAR:$STOCK_JAR:$OSGI"
RUNTIME_CP="$PATCH_JAR:$STOCK_RUNTIME_JAR:$OSGI"
# Each suite launches a JVM that loads the stock HMI library. Keep the default
# bounded on hosts that expose many CPUs but give Docker only a small memory VM.
JOBS=${JAVA_TEST_JOBS:-2}
case "$JOBS" in ''|*[!0-9]*|0) echo 'JAVA_TEST_JOBS must be a positive integer' >&2; exit 2;; esac
names=() defects=() logs=() statuses=()
active=0
jvm_options=()
compile() {
    local name=$1 cp=$2
    shift 2
    # Every suite may use the shared helpers.
    case " $* " in *"/TestSupport.java "*) ;; *) set -- "$TESTS/TestSupport.java" "$@" ;; esac
    COMPILED_DIR="$WORK/$name"
    mkdir -p "$COMPILED_DIR"
    local args=(-nowarn -encoding UTF-8 -d "$COMPILED_DIR")
    if [ -n "$cp" ]; then args+=(-cp "$cp"); fi
    if ! javac "${args[@]}" "$@" > "$REPORTS/$GROUP-$name-compile.log" 2>&1; then
        cat "$REPORTS/$GROUP-$name-compile.log" >&2
        exit 1
    fi
}
# queue <label> <classpath> <main class> <verify: yes|no> <expected stock defect or ''> [arguments]
queue() {
    local name=$1 cp=$2 main=$3 verify=$4 defect=$5
    shift 5
    local index=${#names[@]}
    names+=("$name"); defects+=("$defect")
    logs+=("$REPORTS/$GROUP-$index.log"); statuses+=("$WORK/$index.status")
    local args=()
    if [ "$verify" = no ]; then args+=(-Xverify:none); fi
    if [ ${#jvm_options[@]} -gt 0 ]; then args+=("${jvm_options[@]}"); fi
    (
        cd "$WORK/cwd"
        set +e
        java "${args[@]}" -cp "$cp" "$main" "$@" > "${logs[$index]}" 2>&1
        printf '%s\n' "$?" > "${statuses[$index]}"
    ) &
    active=$((active + 1))
    if [ "$active" -ge "$JOBS" ]; then wait -n; active=$((active - 1)); fi
}
sources_of() {   # sources_of <test class...> -> test source paths in $sources
    local main
    sources=()
    for main in "$@"; do sources+=("$TESTS/${main##*.}.java"); done
}

if [ "$want_route" = 1 ]; then
    # Declare each ordinary suite once, for both compilation and execution.
    route_verified=(RouteInfoPresentationTest RendererMapperDirectionTest RouteGuidanceDeltaTest)
    route_stock=(DistanceBargraphChainTest ManeuverParityTest RendererViewportTest
        ClusterKdkSyncTest ClusterKdkBapChainTest ClusterKdkRendererLifecycleTest
        com.luka.carplay.rgd.LaneGuidanceLifecycleTest com.luka.carplay.rgd.RgiDeliveryRecoveryTest
        com.luka.carplay.rgd.CurrentPositionDeliveryTest com.luka.carplay.rgd.RouteInfoTimeoutTest
        com.luka.carplay.core.RgdTeardownTest com.luka.carplay.core.NativeGuidanceGateTest
        NativeGuidanceStateTest MostArrowsTest MostViewHandshakeTest MostMapViewTest
        ClusterOwnershipTest com.luka.carplay.core.ClusterCoreTest)
    route_runtime=(KomoGraphicsStateTest com.luka.carplay.rgd.CurrentPositionStockChainTest)
    route_variant=(com.luka.carplay.rgd.BapShutdownFailureTest com.luka.carplay.rgd.ManeuverFieldArrivalTest)
    sources_of VCTextScrollTest LaneGuidanceTransportTest ManeuverChainAudit \
        "${route_verified[@]}" "${route_stock[@]}" "${route_runtime[@]}" "${route_variant[@]}"
    compile route "$HOST_CP" "${sources[@]}"
    classes=$COMPILED_DIR
    unicode_args=()
    if [ -n "${VC_UNICODE_TEST_DIR:-}" ]; then unicode_args+=("$VC_UNICODE_TEST_DIR"); fi
    queue VCTextScrollTest "$classes:$PATCH_JAR" com.luka.carplay.rgd.VCTextScrollTest yes '' ${unicode_args[@]+"${unicode_args[@]}"}
    for main in "${route_verified[@]}"; do
        queue "${main##*.}" "$classes:$HOST_CP" "$main" yes ''
    done
    for main in "${route_stock[@]}"; do
        queue "${main##*.}" "$classes:$HOST_CP" "$main" no ''
    done
    for main in "${route_runtime[@]}"; do
        queue "${main##*.}" "$classes:$RUNTIME_CP" "$main" no ''
    done
    queue LaneGuidanceTransportTest "$classes:$HOST_CP" com.luka.carplay.rgd.LaneGuidanceTransportTest no '' "$JAVA_OUTPUT/lane-guidance-wire.bin"
    for ((v=0; v<${#vnames[@]}; v++)); do
        for main in "${route_variant[@]}"; do
            queue "${main##*.} (${vnames[$v]})" "$classes:${vpatch[$v]}:${vstock[$v]}" "$main" yes ''
        done
    done
fi
if [ "$want_transports" = 1 ]; then
    transport_plain=(TouchpadControllerTest com.luka.carplay.bus.CarplayBusTransportTest
        com.luka.carplay.rgd.RendererServerTransportTest)
    transport_stock=(com.luka.carplay.core.VehicleDataEncodingTest com.luka.carplay.core.UpmixSettingTest)
    sources_of "${transport_plain[@]}" com.luka.carplay.rgd.RendererRerouteTransportTest
    compile transport-plain "$PATCH_JAR" "${sources[@]}"
    plain=$COMPILED_DIR
    sources_of "${transport_stock[@]}"
    compile transport-stock "$PATCH_JAR:$STOCK_JAR" "${sources[@]}"
    stock_tests=$COMPILED_DIR
    for main in "${transport_plain[@]}"; do
        queue "${main##*.}" "$plain:$PATCH_JAR" "$main" yes ''
    done
    for main in "${transport_stock[@]}"; do
        queue "${main##*.}" "$stock_tests:$PATCH_JAR:$STOCK_JAR" "$main" yes ''
    done
    for ((v=0; v<${#vnames[@]}; v++)); do
        queue "RendererRerouteTransportTest (${vnames[$v]})" "$plain:${vpatch[$v]}:${vstock[$v]}" \
            com.luka.carplay.rgd.RendererRerouteTransportTest yes ''
    done
    # Stock Navigation may remain INITIALIZING while CarPlay already owns the independent
    # cluster video plane. Takeover forces NAVSD ready and restores the latest stock request.
    sources_of com.luka.carplay.rgd.GatedCombiServiceInitStateTest com.luka.carplay.core.AltScreenStartupTest \
        com.luka.carplay.core.AltScreenContextRaceTest
    compile nav-init "$PATCH_JAR:$STOCK_JAR" "${sources[@]}"
    nav=$COMPILED_DIR
    queue GatedCombiServiceInitStateTest "$nav:$PATCH_JAR:$STOCK_JAR" com.luka.carplay.rgd.GatedCombiServiceInitStateTest yes ''
    for scenario in cold warm stop bounce-loss; do
        queue "AltScreenStartupTest $scenario" "$nav:$PATCH_JAR:$STOCK_JAR" com.luka.carplay.core.AltScreenStartupTest yes '' "$scenario"
    done
    for ((v=0; v<${#vnames[@]}; v++)); do
        for scenario in stop change peer readiness reconnect direct target-error rate-error stock-error bounce-error drift; do
            queue "AltScreenContextRaceTest $scenario (${vnames[$v]})" "$nav:${vpatch[$v]}:${vstock[$v]}" \
                com.luka.carplay.core.AltScreenContextRaceTest yes '' "$scenario"
        done
    done
    # Isolate the native-HMI callback, while running the actual control-server source.
    compile altctl "$PATCH_JAR" "$CORE/AltscreenControlServer.java" \
        "$STUBS/com/luka/carplay/core/AltScreenModule.java" "$TESTS/AltscreenControlTransportTest.java"
    queue AltscreenControlTransportTest "$COMPILED_DIR:$PATCH_JAR" com.luka.carplay.core.AltscreenControlTransportTest yes ''
    # The actual lifecycle worker against controllable external modules.
    compile lifecycle '' "$CORE/CarPlayApp.java" "$CORE/Module.java" "$CORE/Features.java" \
        "$TESTS/CarPlayAppLifecycleTest.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/core/UpmixSetting.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/bus/CarplayBus.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/pdc/PdcSmallStageGuard.java" \
        "$STUBS/app-lifecycle/de/audi/app/terminalmode/IContext.java" \
        "$STUBS/app-lifecycle/de/audi/atip/base/IFrameworkAccess.java"
    for scenario in publication during-start replug failure bounce; do
        queue "CarPlayAppLifecycleTest $scenario" "$COMPILED_DIR" com.luka.carplay.core.CarPlayAppLifecycleTest yes '' "$scenario"
    done
fi
if [ "$want_pdc" = 1 ] || [ "$want_partial360" = 1 ]; then
    compile pdc-stubs "$STOCK_RUNTIME_JAR:$OSGI" \
        "$STUBS/pdc/com/luka/carplay/core/CarPlayApp.java" \
        "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java" \
        "$STUBS/pdc/de/audi/atip/hmi/view/Screen.java" \
        "$STUBS/pdc/de/esolutions/hmi/widgets/audi/base/AbstractScreenWidget.java"
    pdc_stubs=$COMPILED_DIR
    pdc_base="$pdc_stubs:$PATCH_JAR:$STOCK_RUNTIME_JAR:$OSGI:$ASM"
    pdc_suites=(PdcResourcePolicyTest PdcExternalEventsTest OpsAudioDrawerTest OpsStatusLineTest)
    sources_of "${pdc_suites[@]}"
    if [ "$want_partial360" = 1 ]; then sources+=("$TESTS/Partial360PreflightTest.java"); fi
    compile pdc-tests "$pdc_base" "${sources[@]}"
    pdc_tests=$COMPILED_DIR
    stock_first="$pdc_tests:$pdc_stubs:$STOCK_RUNTIME_JAR:$PATCH_JAR:$OSGI:$ASM"
    if [ "$want_pdc" = 1 ]; then
        for suite in "${pdc_suites[@]}"; do
            queue "$suite" "$pdc_tests:$pdc_base" "$suite" yes ''
        done
        # The isolated PDC tests fake CarPlayApp.active. Also run the real lifecycle worker with
        # only external modules/transport substituted, so disconnect does not depend on a later
        # HMI callback to release the presentation policy.
        compile pdc-lifecycle "$pdc_base" "$CORE/CarPlayApp.java" "$CORE/Module.java" \
            "$STUBS/app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java" \
            "$STUBS/app-lifecycle/com/luka/carplay/bus/CarplayBus.java" \
            "$STUBS/app-lifecycle/com/luka/carplay/framework/Log.java"
        pdc_lifecycle_cp="$COMPILED_DIR:$pdc_tests:$pdc_base"
        compile pdc-lifecycle-test "$pdc_lifecycle_cp" "$TESTS/CarPlayPdcLifecycleTest.java"
        queue CarPlayPdcLifecycleTest "$COMPILED_DIR:$pdc_lifecycle_cp" CarPlayPdcLifecycleTest yes ''
        # Negative controls: the unpatched stock classes must reproduce each reported defect.
        queue 'PdcResourcePolicyTest on stock' "$stock_first" PdcResourcePolicyTest yes 'pure OPS 108 toggled Main Wizard'
        queue 'OpsAudioDrawerTest on stock' "$stock_first" OpsAudioDrawerTest yes 'APS drawer still selected over CarPlay + side OPS'
        queue 'OpsStatusLineTest on stock' "$stock_first" OpsStatusLineTest yes 'MMI status line 62 remained over CarPlay'
    fi
    if [ "$want_partial360" = 1 ]; then
        stocks=("$STOCK_RUNTIME_JAR")
        if [ "$STOCK_JAR" != "$STOCK_RUNTIME_JAR" ]; then stocks+=("$STOCK_JAR"); fi
        for stock in "${stocks[@]}"; do
            queue "Partial360PreflightTest ($(basename "$stock"))" "$pdc_tests:$pdc_stubs:$PATCH_JAR:$stock:$OSGI:$ASM" Partial360PreflightTest yes ''
        done
        if [ "$want_pdc" != 1 ]; then
            queue PdcResourcePolicyTest "$pdc_tests:$pdc_base" PdcResourcePolicyTest yes ''
            queue 'PdcResourcePolicyTest on stock' "$stock_first" PdcResourcePolicyTest yes 'pure OPS 108 toggled Main Wizard'
        fi
    fi
fi
if [ "$want_fuzz" = 1 ]; then
    iterations=${CARPLAY_FUZZ_ITERATIONS:-10000}
    seeds=${CARPLAY_FUZZ_SEEDS:-"32 47426351 0x8b338d86 0x4ccb85a6"}
    case "$iterations" in ''|*[!0-9]*) echo "Invalid CARPLAY_FUZZ_ITERATIONS" >&2; exit 2 ;; esac
    [ "$iterations" -ge 100 ] || { echo "Use at least 100 events for coverage" >&2; exit 2; }
    sources_of com.luka.carplay.core.AltScreenContextFuzzTest RouteGenerationFuzzTest RouteGuidanceDeltaTest
    compile fuzz "$HOST_CP" "${sources[@]}"
    fuzz=$COMPILED_DIR
    jvm_options=(-Xmx512m)
    for ((v=0; v<${#vnames[@]}; v++)); do
        for seed in $seeds; do
            case "$seed" in ''|*[!0-9a-fA-FxX-]*) echo "Invalid seed: $seed" >&2; exit 2 ;; esac
            for suite in com.luka.carplay.core.AltScreenContextFuzzTest RouteGenerationFuzzTest; do
                queue "${suite##*.} ${vnames[$v]} seed=$seed events=$iterations" "$fuzz:${vpatch[$v]}:${vstock[$v]}" "$suite" yes '' "$seed" "$iterations"
            done
        done
    done
    jvm_options=()
fi
if [ "$want_contract" = 1 ]; then
    if [ -n "${RGD_CONTRACT_FRAMES:-}" ]; then
        frames=()
        for frame in old-slot.txt new-no-angle.txt new-known-angle.txt new-generation.txt; do
            test -f "$RGD_CONTRACT_FRAMES/$frame" || { echo "Missing native contract frame: $frame" >&2; exit 1; }
            frames+=("$RGD_CONTRACT_FRAMES/$frame")
        done
        compile rgd-contract "$HOST_CP" "$TESTS/TestSupport.java" "$TESTS/ManeuverChainAudit.java" "$TESTS/RgdNativeContractProbe.java"
        queue RgdNativeContractProbe "$COMPILED_DIR:$HOST_CP" RgdNativeContractProbe no '' "${frames[@]}"
    elif [ "$GROUP" = contract ]; then
        echo 'RGD_CONTRACT_FRAMES is required for the contract group' >&2; exit 1
    else
        echo 'SKIPPED RgdNativeContractProbe: no native frames supplied (set RGD_CONTRACT_FRAMES, or run check_java.sh)'
    fi
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
    elif [ "$status" -ne 0 ]; then
        echo "FAIL ${names[$i]} (exit $status):" >&2
        cat "${logs[$i]}" >&2; failed=1; continue
    fi
    printf '%s\t%s\n' "${names[$i]}" "$summary" >> "$report"
    echo "PASS ${names[$i]}"
done
if [ "$failed" -ne 0 ]; then exit 1; fi

if [ "$want_linkage" = 1 ]; then
    compile audit "$ASM" "$TESTS/JavaStockLinkageAudit.java"
    audit=$COMPILED_DIR
    for ((v=0; v<${#vnames[@]}; v++)); do
        stock=${vstock[$v]}
        libraries=("${vpatch[$v]}" "$stock")
        if [ -n "$BOOT_JAR" ] && [ "$BOOT_JAR" != "$stock" ]; then libraries+=("$BOOT_JAR"); fi
        libraries+=("$OSGI_FRAMEWORK" "$OSGI_TRACKER")
        # Unlike the build, the linkage audit needs the car's class library.
        if ! jar tf "$stock" | grep -qx 'java/lang/Object.class' && [ -z "$BOOT_JAR" ]; then
            echo 'Stock linkage needs the car Java library: set STOCK_BOOT_JAR (the car JCL).' >&2; exit 1
        fi
        out="$JAVA_OUTPUT/stock-linkage.txt"
        [ "${vnames[$v]}" = main ] || out="$JAVA_OUTPUT/stock-linkage-${vnames[$v]}.txt"
        echo "Linkage audit (${vnames[$v]}): $(basename "${vpatch[$v]}") vs $(basename "$stock")"
        if ! java -Xmx2g -cp "$audit:$ASM" JavaStockLinkageAudit "${libraries[@]}" > "$out" 2>&1; then
            cat "$out" >&2
            exit 1
        fi
        cat "$out"
        tail -n 1 "$out" | grep -Eq '^JavaStockLinkageAudit: .*errors=0$' || {
            echo 'Stock linkage audit did not report zero errors' >&2; exit 1;
        }
    done
fi
