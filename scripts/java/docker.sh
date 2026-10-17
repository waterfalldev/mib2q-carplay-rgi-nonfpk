#!/bin/bash
# Host launcher for the Java build and tests: runs scripts/java/{build,test}.sh in
# eclipse-temurin:8-jdk-jammy, so no host JDK is needed. Inputs are environment variables
# (README.md, "Building"); the defaults are the java_stock/ and ../../Tools/jxe2jar layout.
#
#   bash scripts/java/docker.sh build
#   bash scripts/java/docker.sh test [all|route|transports|pdc|partial360|fuzz|maneuvers|contract|linkage]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS=${CARPLAY_TOOLS_DIR:-${JXE2JAR:-$ROOT/../../Tools/jxe2jar}}
mode=${1:-build}; shift || true
default_stock=0
if [ -z "${STOCK_JAR:-}" ]; then
    STOCK_JAR=$ROOT/java_stock/US_MU1316_lsd.jar
    default_stock=1
    # The checkout's own layout also carries the EU and CN firmware; an explicit STOCK_JAR
    # builds and tests only what the caller supplies.
    if [ -z "${STOCK_EU_JAR:-}" ] && [ -f "$ROOT/java_stock/EU_MU1329_lsd.jar" ]; then STOCK_EU_JAR=$ROOT/java_stock/EU_MU1329_lsd.jar; fi
    if [ -z "${STOCK_CN_JAR:-}" ] && [ -f "$ROOT/java_stock/CN_MU1003_lsd.jar" ]; then STOCK_CN_JAR=$ROOT/java_stock/CN_MU1003_lsd.jar; fi
fi
CARPLAY_DEPENDENCIES=${CARPLAY_DEPENDENCIES:-$ROOT/toolchain/java}
JAVA_OUTPUT=${JAVA_OUTPUT:-$ROOT/build}
CARPLAY_BUILD_ID=${CARPLAY_BUILD_ID:-$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)}
CARPLAY_BUILD_ID=$(printf '%s' "$CARPLAY_BUILD_ID" | tr -cd 'A-Za-z0-9._-')
# The build receipt records the checkout, which is not a git repository inside the container.
# Ask git before MSYS_NO_PATHCONV is set below: Git for Windows needs the converted path.
CARPLAY_COMMIT=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
CARPLAY_SOURCE_DIRTY=false
if [ -n "$(git -C "$ROOT" status --porcelain -- java_patch java_patch_cn java_resources 2>/dev/null)" ]; then CARPLAY_SOURCE_DIRTY=true; fi
if [ "$default_stock" = 1 ]; then
    # The car's class library is optional for building; use it when it is present.
    default_boot=$TOOLS/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar
    if [ -z "${STOCK_BOOT_JAR:-}" ] && [ -f "$default_boot" ]; then STOCK_BOOT_JAR=$default_boot; fi
fi
host_path() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) cygpath -am "$1" ;;
        *) (cd "$(dirname "$1")" && printf '%s/%s\n' "$PWD" "$(basename "$1")") ;;
    esac
}
mount_file() {
    local input=$1 target=$2 envname=$3
    [ -f "$input" ] || { echo "Missing $envname input: $input" >&2; exit 1; }
    local dest="$target/$(basename "$input")"
    args+=(--mount "type=bind,source=$(host_path "$input"),target=$dest,readonly" -e "$envname=$dest")
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$JAVA_OUTPUT"
[ -d "$CARPLAY_DEPENDENCIES" ] || { echo "Missing CARPLAY_DEPENDENCIES directory: $CARPLAY_DEPENDENCIES" >&2; exit 1; }
args=(run --rm --network none --env TZ=UTC --env LC_ALL=C
    --mount "type=bind,source=$(host_path "$ROOT"),target=/src,readonly"
    --mount "type=bind,source=$(host_path "$CARPLAY_DEPENDENCIES"),target=/deps,readonly"
    --mount "type=bind,source=$(host_path "$JAVA_OUTPUT"),target=/out"
    -e SOURCE_ROOT=/src -e CARPLAY_DEPENDENCIES=/deps -e JAVA_OUTPUT=/out -e "CARPLAY_BUILD_ID=$CARPLAY_BUILD_ID"
    -e "CARPLAY_COMMIT=$CARPLAY_COMMIT" -e "CARPLAY_SOURCE_DIRTY=$CARPLAY_SOURCE_DIRTY"
    -e "CARPLAY_OUTPUT_DISPLAY=$(host_path "$JAVA_OUTPUT")")
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1 ;;
    *) args+=(--user "$(id -u):$(id -g)") ;;
esac
mount_file "$STOCK_JAR" /inputs/stock STOCK_JAR
if [ -n "${STOCK_BOOT_JAR:-}" ]; then mount_file "$STOCK_BOOT_JAR" /inputs/boot BOOT_JAR; fi
if [ -n "${STOCK_EU_JAR:-}" ]; then mount_file "$STOCK_EU_JAR" /inputs/stock-eu STOCK_EU_JAR; fi
if [ -n "${STOCK_CN_JAR:-}" ]; then
    mount_file "$STOCK_CN_JAR" /inputs/stock-cn STOCK_CN_JAR
    if [ "$mode" = build ] || { [ "$mode" = test ] && [ "${JAVA_SKIP_BUILD:-0}" != 1 ]; }; then
        # The CN variant is java_patch/ plus the CN stock delta. The container has no patch
        # tool, so apply it here (-F0: a hunk that no longer fits fails instead of misplacing).
        command -v patch >/dev/null 2>&1 || { echo 'The CN variant needs the patch tool on the host.' >&2; exit 1; }
        mkdir "$work/cn-src"
        cp -R "$ROOT/java_patch/." "$work/cn-src/"
        for p in "$ROOT"/java_patch_cn/*.patch; do
            patch -p1 -F0 -s -d "$work/cn-src" < "$p" ||
                { echo "ERROR: $p no longer applies to java_patch/ - update it (java_patch_cn/README.md)" >&2; exit 1; }
        done
        args+=(--mount "type=bind,source=$(host_path "$work/cn-src"),target=/cn-src,readonly" -e CN_SOURCE_DIR=/cn-src)
    fi
fi
if [ "$mode" = test ]; then
    if [ -n "${STOCK_RUNTIME_JAR:-}" ]; then mount_file "$STOCK_RUNTIME_JAR" /inputs/runtime STOCK_RUNTIME_JAR; fi
    case "${1:-all}" in
        all|transports|pdc|partial360|linkage)
            default_asm=$CARPLAY_DEPENDENCIES/asm-9.7.jar
            default_asm_tree=$CARPLAY_DEPENDENCIES/asm-tree-9.7.jar
            [ -f "$default_asm" ] || default_asm=$TOOLS/tools/uninline/lib/asm-9.7.jar
            [ -f "$default_asm_tree" ] || default_asm_tree=$TOOLS/tools/uninline/lib/asm-tree-9.7.jar
            mount_file "${ASM_JAR:-$default_asm}" /inputs/asm ASM_JAR
            mount_file "${ASM_TREE_JAR:-$default_asm_tree}" /inputs/asm-tree ASM_TREE_JAR ;;
    esac
    if [ -n "${RGD_CONTRACT_FRAMES:-}" ]; then args+=(--mount "type=bind,source=$(host_path "$RGD_CONTRACT_FRAMES"),target=/frames,readonly" -e RGD_CONTRACT_FRAMES=/frames); fi
    if [ -n "${VC_UNICODE_TEST_DIR:-}" ]; then args+=(--mount "type=bind,source=$(host_path "$VC_UNICODE_TEST_DIR"),target=/unicode-tests,readonly" -e VC_UNICODE_TEST_DIR=/unicode-tests); fi
    if [ -n "${CARPLAY_HOOK_JAR:-}" ]; then mount_file "$CARPLAY_HOOK_JAR" /inputs/patch CARPLAY_HOOK_JAR; fi
    if [ -n "${CARPLAY_HOOK_CN_JAR:-}" ]; then mount_file "$CARPLAY_HOOK_CN_JAR" /inputs/patch-cn CARPLAY_HOOK_CN_JAR; fi
    if [ -n "${JAVA_TEST_JOBS:-}" ]; then args+=(-e "JAVA_TEST_JOBS=$JAVA_TEST_JOBS"); fi
    if [ -n "${CARPLAY_FUZZ_ITERATIONS:-}" ]; then args+=(-e "CARPLAY_FUZZ_ITERATIONS=$CARPLAY_FUZZ_ITERATIONS"); fi
    if [ -n "${CARPLAY_FUZZ_SEEDS:-}" ]; then args+=(-e "CARPLAY_FUZZ_SEEDS=$CARPLAY_FUZZ_SEEDS"); fi
fi
image=${CARPLAY_JAVA_IMAGE:-eclipse-temurin:8-jdk-jammy}
docker image inspect "$image" >/dev/null 2>&1 || docker pull "$image"
image=$(docker image inspect --format '{{.Id}}' "$image")
printf '%s\n' "$image" > "$JAVA_OUTPUT/java-image-id.txt"
case "$mode" in
    build) docker "${args[@]}" "$image" bash /src/scripts/java/build.sh ;;
    build-tests) docker "${args[@]}" "$image" bash /src/scripts/test_java_build.sh --inside ;;
    test) docker "${args[@]}" -e "JAVA_SKIP_BUILD=${JAVA_SKIP_BUILD:-0}" "$image" bash -ec '
        [ "$JAVA_SKIP_BUILD" = 1 ] || bash /src/scripts/java/build.sh
        exec bash /src/scripts/java/test.sh "$@"
        ' -- "$@" ;;
    javap) exec docker "${args[@]}" "$image" bash -c 'exec javap -classpath "$STOCK_JAR" -c -p "$@"' -- "$@" ;;
    *) echo "Unknown Java action: $mode" >&2; exit 2 ;;
esac
