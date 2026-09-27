#!/bin/bash
# Focused build contracts. Requires Docker plus external stock/dependency inputs.
# All altered sources stay in temporary container storage; the checkout is read-only.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    : "${STOCK_JAR:?Set STOCK_JAR to an external firmware JAR}"
    : "${CARPLAY_DEPENDENCIES:?Set CARPLAY_DEPENDENCIES to the dependency directory}"
    output=${JAVA_BUILD_TEST_OUTPUT:-$ROOT/build/java-build-tests}
    mkdir -p "$output"
    host_path() {
        case "$(uname -s)" in
            MINGW*|MSYS*|CYGWIN*) cygpath -am "$1" ;;
            *) (cd "$(dirname "$1")" && printf '%s/%s\n' "$PWD" "$(basename "$1")") ;;
        esac
    }
    stock_target="/inputs/$(basename "$STOCK_JAR")"
    args=(run --rm --network none
        --mount "type=bind,source=$(host_path "$ROOT"),target=/src,readonly"
        --mount "type=bind,source=$(host_path "$STOCK_JAR"),target=$stock_target,readonly"
        --mount "type=bind,source=$(host_path "$CARPLAY_DEPENDENCIES"),target=/deps,readonly"
        --mount "type=bind,source=$(host_path "$output"),target=/results"
        -e "STOCK_JAR=$stock_target" -e CARPLAY_DEPENDENCIES=/deps)
    if [ -n "${STOCK_BOOT_JAR:-}" ]; then
        args+=(--mount "type=bind,source=$(host_path "$STOCK_BOOT_JAR"),target=/inputs/boot.jar,readonly" -e BOOT_JAR=/inputs/boot.jar)
    fi
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1 ;;
        *) args+=(--user "$(id -u):$(id -g)") ;;
    esac
    image=${CARPLAY_JAVA_IMAGE:-eclipse-temurin:8-jdk-jammy}
    docker image inspect "$image" >/dev/null 2>&1 || docker pull "$image"
    image=$(docker image inspect --format '{{.Id}}' "$image")
    exec docker "${args[@]}" "$image" bash /src/scripts/test_java_build.sh --inside
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source_fixture="$work/source tree with spaces"
mkdir -p "$source_fixture/scripts" /results
cp -R "$ROOT/java_patch" "$ROOT/java_resources" "$source_fixture/"
cp -R "$ROOT/scripts/java" "$source_fixture/scripts/"
export SOURCE_ROOT=$source_fixture CARPLAY_BUILD_ID=java-build-regression
original_boot=${BOOT_JAR:-$STOCK_JAR}
build() {
    local name=$1
    JAVA_OUTPUT="$work/$name" bash "$source_fixture/scripts/java/build.sh" > "/results/$name.log" 2>&1
}
expect_failure() {
    local name=$1 message=$2
    if build "$name"; then
        echo "FAIL $name: the build unexpectedly succeeded" >&2; exit 1
    fi
    if ! grep -Fq "$message" "/results/$name.log"; then
        cat "/results/$name.log" >&2
        echo "FAIL $name: missing expected diagnostic '$message'" >&2; exit 1
    fi
    if [ -e "$work/$name/carplay_hook.jar" ]; then
        echo "FAIL $name: published a JAR after failure" >&2; exit 1
    fi
    echo "PASS $name (failed before publishing a JAR)"
}

build 'reproducible first' || { cat '/results/reproducible first.log' >&2; exit 1; }
build 'reproducible second' || { cat '/results/reproducible second.log' >&2; exit 1; }
cmp "$work/reproducible first/carplay_hook.jar" "$work/reproducible second/carplay_hook.jar"
sha256sum "$work/reproducible first/carplay_hook.jar" | cut -d ' ' -f 1 > /results/reproducible-sha256.txt
echo 'PASS reproducible JAR (two fresh builds, same build ID, paths with spaces)'

resource="$source_fixture/java_resources/com/luka/carplay/rgd/vc-text.bin"
mv "$resource" "$work/vc-text.bin"
expect_failure missing-resource 'Missing vc-text.bin'
mv "$work/vc-text.bin" "$resource"
mkdir -p "$source_fixture/java_resources/com/luka/carplay/core"
printf 'invalid resource\n' > "$source_fixture/java_resources/com/luka/carplay/core/CarPlayApp.class"
expect_failure resource-collision 'Resource collides with compiled output'
rm "$source_fixture/java_resources/com/luka/carplay/core/CarPlayApp.class"

export BOOT_JAR="$work/nonexistent-boot.jar"
expect_failure missing-boot 'nonexistent-boot.jar'
mkdir "$work/empty"
jar cf "$work/no-bootstrap-classes.jar" -C "$work/empty" .
export BOOT_JAR="$work/no-bootstrap-classes.jar"
expect_failure invalid-boot 'Stock Java library missing'
export BOOT_JAR=$original_boot

# Exercise the original Mac directory conventions without requiring or claiming
# execution on macOS. Fake Docker records exact arguments; Java remains in Docker.
layout="$work/upstream workspace with spaces"
project="$layout/Repositories/fork"
tools="$layout/Tools/jxe2jar"
mkdir -p "$project/scripts/java" "$tools/out" "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316" "$tools/tools/uninline/lib" "$work/launcher-bin"
cp "$ROOT/scripts/java/docker.sh" "$project/scripts/java/docker.sh"
touch "$tools/out/MU1316-final.jar" "$tools/out/MU1316-combined.jar" \
    "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar" \
    "$tools/tools/uninline/lib/asm-9.7.jar" "$tools/tools/uninline/lib/asm-tree-9.7.jar"
cat > "$work/launcher-bin/docker" <<'DOCKER'
#!/bin/bash
if [ "$1" = image ]; then echo sha256:fixture-image; exit 0; fi
if [ "$1" = run ]; then printf '%s\n' "$@" > /results/launcher-arguments.txt; exit 0; fi
echo "Unexpected Docker action: $*" >&2; exit 1
DOCKER
cat > "$work/launcher-bin/uname" <<'UNAME'
#!/bin/bash
echo Darwin
UNAME
for command in java javac pwsh; do
    printf '#!/bin/bash\necho "Unexpected host tool: %s" >&2\nexit 1\n' "$command" > "$work/launcher-bin/$command"
done
chmod +x "$work/launcher-bin/"*
(
    unset STOCK_JAR STOCK_BOOT_JAR STOCK_RUNTIME_JAR CARPLAY_DEPENDENCIES CARPLAY_TOOLS_DIR JAVA_OUTPUT CARPLAY_HOOK_JAR ASM_JAR ASM_TREE_JAR RGD_CONTRACT_FRAMES VC_UNICODE_TEST_DIR
    export PATH="$work/launcher-bin:$PATH"
    bash "$project/scripts/java/docker.sh" test transports
)
for expected in \
    'STOCK_JAR=/inputs/stock/MU1316-final.jar' \
    'BOOT_JAR=/inputs/boot/jcl.jar' \
    'STOCK_RUNTIME_JAR=/inputs/runtime/MU1316-combined.jar' \
    'ASM_JAR=/inputs/asm/asm-9.7.jar' \
    'ASM_TREE_JAR=/inputs/asm-tree/asm-tree-9.7.jar'; do
    grep -Fxq "$expected" /results/launcher-arguments.txt || { echo "FAIL launcher: $expected" >&2; exit 1; }
done
grep -Fxq "type=bind,source=$tools/out/MU1316-final.jar,target=/inputs/stock/MU1316-final.jar,readonly" /results/launcher-arguments.txt
grep -Fxq "type=bind,source=$project/build,target=/out" /results/launcher-arguments.txt
echo 'PASS upstream launcher defaults (stock basename, boot/runtime JARs, transport ASM, paths with spaces; no host Java or PowerShell)'
echo 'Java build contracts: PASS'
