#!/bin/bash
# Runs inside the Java 8 Docker image; scripts/java/docker.sh is the host launcher.
#   carplay_hook.jar     java_patch/ against STOCK_JAR
#   carplay_hook_cn.jar  CN_SOURCE_DIR (java_patch/ with java_patch_cn/ applied) against
#                        STOCK_CN_JAR; only when a CN stock JAR was supplied
set -euo pipefail
export LC_ALL=C TZ=UTC
: "${SOURCE_ROOT:?}" "${STOCK_JAR:?}" "${CARPLAY_DEPENDENCIES:?}" "${JAVA_OUTPUT:?}"
BUILD_ID=${CARPLAY_BUILD_ID:-unknown}
[[ "$BUILD_ID" =~ ^[A-Za-z0-9._-]+$ ]] || { echo 'Invalid build ID' >&2; exit 1; }
[[ $(javac -version 2>&1) == javac\ 1.8.* ]] || { echo 'Java 8 is required' >&2; exit 1; }
if [ -n "${STOCK_CN_JAR:-}" ]; then
    : "${CN_SOURCE_DIR:?The CN variant needs its patched sources (scripts/java/docker.sh prepares them)}"
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tools" "$JAVA_OUTPUT"
javac -d "$work/tools" "$SOURCE_ROOT/scripts/java/PackageJar.java"
framework=$CARPLAY_DEPENDENCIES/org.osgi.framework-1.10.0.jar
tracker=$CARPLAY_DEPENDENCIES/org.osgi.util.tracker-1.5.4.jar
resources="$SOURCE_ROOT/java_resources"
[ -f "$resources/com/luka/carplay/rgd/vc-text.bin" ] || { echo 'Missing vc-text.bin' >&2; exit 1; }

# build_variant <name> <source dir> <stock jar> <boot jar or empty> <output name> <build id>
build_variant() {
    local name=$1 source=$2 stock=$3 boot_input=$4 output=$5 id=$6
    local dir="$work/$name"
    mkdir -p "$dir/classes" "$dir/generated/com/luka/carplay/core"
    # Keep javac's small-file traffic on Linux storage rather than the host bind mount.
    cp -R "$source" "$dir/src"
    cp "$stock" "$dir/stock.jar"
    local boot="$dir/stock.jar"
    if [ -n "$boot_input" ] && [ "$boot_input" != "$stock" ]; then
        cp "$boot_input" "$dir/boot.jar"; boot="$dir/boot.jar"
    fi
    jar tf "$boot" > "$dir/boot-entries"
    local bootargs=(-bootclasspath "$boot")
    if ! grep -qx 'java/lang/Object.class' "$dir/boot-entries"; then
        if [ -n "$boot_input" ]; then
            echo 'Stock Java library missing. Supply STOCK_BOOT_JAR (the car JCL), or a stock JAR containing it.' >&2; exit 1
        fi
        # As the original build: JDK 8's classes. The car library only adds a stricter API check.
        echo "$name boot classes: JDK 8 (no car Java library supplied; STOCK_BOOT_JAR enables that check)"
        bootargs=()
    fi
    local app=com/luka/carplay/core/CarPlayApp.java
    grep -q '@BUILD_ID@' "$dir/src/$app" || { echo 'Missing BUILD_ID token' >&2; exit 1; }
    sed "s/@BUILD_ID@/$id/g" "$dir/src/$app" > "$dir/generated/$app"
    find "$dir/src" -type f -name '*.java' ! -path "*/$app" -print | LC_ALL=C sort > "$dir/source-paths"
    printf '%s\n' "$dir/generated/$app" >> "$dir/source-paths"
    while IFS= read -r path; do printf '"%s"\n' "$path"; done < "$dir/source-paths" > "$dir/sources.txt"
    echo "Compiling $(wc -l < "$dir/sources.txt") Java files for $output (build $id)..."
    javac -encoding UTF-8 -source 1.4 -target 1.4 "${bootargs[@]}" \
        -cp "$dir/stock.jar:$framework:$tracker" -sourcepath "$dir/generated:$dir/src" \
        -d "$dir/classes" -Xlint:-options @"$dir/sources.txt"
    while IFS= read -r -d '' resource; do
        relative=${resource#"$resources/"}
        [[ "$relative" != *.class ]] && [ ! -e "$dir/classes/$relative" ] || { echo "Resource collides with compiled output: $relative" >&2; exit 1; }
    done < <(find "$resources" -type f -print0)
    cp -R "$resources/." "$dir/classes/"
    java -cp "$work/tools" PackageJar "$dir/classes" "$dir/$output"
    cp "$dir/$output" "$JAVA_OUTPUT/$output"
    write_receipt "$JAVA_OUTPUT/$output" "$id"
}

# The same record scripts/build_java.sh wrote before the build moved into Docker. The checkout
# is not a git repository here, so docker.sh supplies the commit and dirty flag.
write_receipt() {
    local jar=$1 id=$2 host_dir=${CARPLAY_OUTPUT_DISPLAY:-$JAVA_OUTPUT}
    local size sum
    size=$(stat -c %s "$jar")
    sum=$(sha256sum "$jar" | cut -d ' ' -f 1)
    printf '{\n  "jar": "%s/%s",\n  "bytes": %s,\n  "sha256": "%s",\n  "build_id": "%s",\n  "commit": "%s",\n  "source_dirty": %s,\n  "deployed": false\n}\n' \
        "$host_dir" "$(basename "$jar")" "$size" "$sum" "$id" "${CARPLAY_COMMIT:-unknown}" "${CARPLAY_SOURCE_DIRTY:-false}" \
        > "${jar%.jar}-build.json"
}

build_variant main "$SOURCE_ROOT/java_patch" "$STOCK_JAR" "${BOOT_JAR:-}" carplay_hook.jar "$BUILD_ID"
if [ -n "${STOCK_CN_JAR:-}" ]; then
    # The CN firmware has its own class library; the car JCL passed for the main variant does not apply.
    build_variant cn "$CN_SOURCE_DIR" "$STOCK_CN_JAR" '' carplay_hook_cn.jar "$BUILD_ID-cn"
else
    rm -f "$JAVA_OUTPUT/carplay_hook_cn.jar" "$JAVA_OUTPUT/carplay_hook_cn-build.json"
fi
javac -version > "$JAVA_OUTPUT/javac-version.txt" 2>&1
