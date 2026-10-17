#!/bin/bash
# Focused build contracts. Requires Docker plus external stock/dependency inputs.
# All altered sources stay in temporary container storage; the checkout is read-only.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    export JAVA_OUTPUT=${JAVA_BUILD_TEST_OUTPUT:-$ROOT/build/java-build-tests}
    exec bash "$ROOT/scripts/java/docker.sh" build-tests
fi

export BUILD_TEST_RESULTS=${JAVA_OUTPUT:?}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source_fixture="$work/source tree with spaces"
mkdir -p "$source_fixture/scripts" "$BUILD_TEST_RESULTS"
cp -R "$ROOT/java_patch" "$ROOT/java_resources" "$source_fixture/"
cp -R "$ROOT/scripts/java" "$source_fixture/scripts/"
export SOURCE_ROOT=$source_fixture CARPLAY_BUILD_ID=java-build-regression
original_boot=${BOOT_JAR:-$STOCK_JAR}
build() {
    local name=$1
    JAVA_OUTPUT="$work/$name" bash "$source_fixture/scripts/java/build.sh" > "$BUILD_TEST_RESULTS/$name.log" 2>&1
}
expect_failure() {
    local name=$1 message=$2
    if build "$name"; then
        echo "FAIL $name: the build unexpectedly succeeded" >&2; exit 1
    fi
    if ! grep -Fq "$message" "$BUILD_TEST_RESULTS/$name.log"; then
        cat "$BUILD_TEST_RESULTS/$name.log" >&2
        echo "FAIL $name: missing expected diagnostic '$message'" >&2; exit 1
    fi
    if [ -e "$work/$name/carplay_hook.jar" ]; then
        echo "FAIL $name: published a JAR after failure" >&2; exit 1
    fi
    echo "PASS $name (failed before publishing a JAR)"
}

build 'reproducible first' || { cat "$BUILD_TEST_RESULTS/reproducible first.log" >&2; exit 1; }
build 'reproducible second' || { cat "$BUILD_TEST_RESULTS/reproducible second.log" >&2; exit 1; }
cmp "$work/reproducible first/carplay_hook.jar" "$work/reproducible second/carplay_hook.jar"
sha256sum "$work/reproducible first/carplay_hook.jar" | cut -d ' ' -f 1 > "$BUILD_TEST_RESULTS/reproducible-sha256.txt"
echo 'PASS reproducible JAR (two fresh builds, same build ID, paths with spaces)'

receipt="$work/reproducible first/carplay_hook-build.json"
grep -Fq "\"sha256\": \"$(cat "$BUILD_TEST_RESULTS/reproducible-sha256.txt")\"" "$receipt"
grep -Fq '"build_id": "java-build-regression"' "$receipt"
if [ -e "$work/reproducible first/carplay_hook_cn.jar" ]; then
    echo 'FAIL cn-variant: built a CN JAR without a CN stock JAR' >&2; exit 1
fi
echo 'PASS build receipt (JAR hash and build ID; no CN JAR without a CN stock JAR)'

# The CN variant takes its patched sources from docker.sh; an unpatched copy still exercises the path.
(
    export STOCK_CN_JAR=$STOCK_JAR CN_SOURCE_DIR="$source_fixture/java_patch"
    build cn-variant
) || { cat "$BUILD_TEST_RESULTS/cn-variant.log" >&2; exit 1; }
cmp -s "$work/reproducible first/carplay_hook.jar" "$work/cn-variant/carplay_hook.jar"
test -f "$work/cn-variant/carplay_hook_cn.jar"
grep -Fq '"build_id": "java-build-regression-cn"' "$work/cn-variant/carplay_hook_cn-build.json"
echo 'PASS CN variant (second JAR and receipt, main JAR unchanged)'

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

# Neither the stock JAR nor an input supplies the car's class library: build
# as the original script did, against JDK 8's classes.
mkdir "$work/stock-no-jcl"
(cd "$work/stock-no-jcl" && jar xf "$STOCK_JAR" && rm -rf java)
jar cf "$work/stock-no-jcl.jar" -C "$work/stock-no-jcl" .
(
    unset BOOT_JAR
    export STOCK_JAR="$work/stock-no-jcl.jar"
    build no-car-library
) || { cat "$BUILD_TEST_RESULTS/no-car-library.log" >&2; exit 1; }
grep -Fq 'boot classes: JDK 8' "$BUILD_TEST_RESULTS/no-car-library.log" || {
    echo 'FAIL no-car-library: JDK 8 fallback not reported' >&2; exit 1;
}
if cmp -s "$work/reproducible first/carplay_hook.jar" "$work/no-car-library/carplay_hook.jar"; then
    same='same JAR as the car-library build'
else
    same='JAR differs from the car-library build'
fi
echo "PASS no-car-library (JDK 8 boot classes; $same)"

# Exercise the default input layout (java_stock/, in-tree dependencies, ../../Tools/jxe2jar)
# without requiring or claiming execution on macOS. Fake Docker records exact arguments;
# Java remains in Docker.
layout="$work/upstream workspace with spaces"
project="$layout/Repositories/fork"
tools="$layout/Tools/jxe2jar"
mkdir -p "$project/scripts/java" "$project/java_stock" "$project/toolchain/java" "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316" "$work/launcher-bin"
cp "$ROOT/scripts/java/docker.sh" "$project/scripts/java/docker.sh"
cp "$ROOT/scripts/test_java_build.sh" "$project/scripts/test_java_build.sh"
touch "$project/java_stock/US_MU1316_lsd.jar" "$project/java_stock/EU_MU1329_lsd.jar" \
    "$project/toolchain/java/asm-9.7.jar" "$project/toolchain/java/asm-tree-9.7.jar" \
    "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar"
cat > "$work/launcher-bin/docker" <<'DOCKER'
#!/bin/bash
if [ "$1" = image ]; then echo sha256:fixture-image; exit 0; fi
if [ "$1" = run ]; then printf '%s\n' "$@" > "$BUILD_TEST_RESULTS/launcher-arguments.txt"; exit 0; fi
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
clear_inputs() {
    unset STOCK_JAR STOCK_EU_JAR STOCK_CN_JAR STOCK_BOOT_JAR STOCK_RUNTIME_JAR CARPLAY_DEPENDENCIES CARPLAY_TOOLS_DIR JXE2JAR \
        JAVA_OUTPUT JAVA_BUILD_TEST_OUTPUT CARPLAY_HOOK_JAR CARPLAY_HOOK_CN_JAR ASM_JAR ASM_TREE_JAR RGD_CONTRACT_FRAMES VC_UNICODE_TEST_DIR
}
(
    clear_inputs
    export PATH="$work/launcher-bin:$PATH"
    bash "$project/scripts/java/docker.sh" test transports
)
for expected in \
    'STOCK_JAR=/inputs/stock/US_MU1316_lsd.jar' \
    'STOCK_EU_JAR=/inputs/stock-eu/EU_MU1329_lsd.jar' \
    'BOOT_JAR=/inputs/boot/jcl.jar' \
    'ASM_JAR=/inputs/asm/asm-9.7.jar' \
    'ASM_TREE_JAR=/inputs/asm-tree/asm-tree-9.7.jar'; do
    grep -Fxq "$expected" "$BUILD_TEST_RESULTS/launcher-arguments.txt" || { echo "FAIL launcher: $expected" >&2; exit 1; }
done
grep -Fxq "type=bind,source=$project/java_stock/US_MU1316_lsd.jar,target=/inputs/stock/US_MU1316_lsd.jar,readonly" "$BUILD_TEST_RESULTS/launcher-arguments.txt"
grep -Fxq "type=bind,source=$project/toolchain/java,target=/deps,readonly" "$BUILD_TEST_RESULTS/launcher-arguments.txt"
grep -Fxq "type=bind,source=$project/build,target=/out" "$BUILD_TEST_RESULTS/launcher-arguments.txt"
if grep -q 'STOCK_CN_JAR\|/cn-src' "$BUILD_TEST_RESULTS/launcher-arguments.txt"; then
    echo 'FAIL launcher: CN variant requested without a CN stock JAR' >&2; exit 1
fi
echo 'PASS default launcher layout (java_stock/, in-tree dependencies, car library, transport ASM, paths with spaces; no host Java or PowerShell)'

# An explicit STOCK_JAR builds only that firmware, whatever else java_stock/ holds.
(
    clear_inputs
    export PATH="$work/launcher-bin:$PATH" STOCK_JAR="$project/java_stock/EU_MU1329_lsd.jar"
    bash "$project/scripts/java/docker.sh" build
)
grep -Fxq 'STOCK_JAR=/inputs/stock/EU_MU1329_lsd.jar' "$BUILD_TEST_RESULTS/launcher-arguments.txt"
if grep -q 'STOCK_EU_JAR\|STOCK_CN_JAR\|BOOT_JAR' "$BUILD_TEST_RESULTS/launcher-arguments.txt"; then
    echo 'FAIL launcher: explicit STOCK_JAR picked up default companions' >&2; exit 1
fi
echo 'PASS explicit STOCK_JAR (no default EU, CN or car-library inputs)'

# Without the default jcl.jar, the build must not require it.
mv "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar" "$work/jcl.jar.fixture"
(
    clear_inputs
    export PATH="$work/launcher-bin:$PATH"
    bash "$project/scripts/java/docker.sh" build
)
mv "$work/jcl.jar.fixture" "$tools/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar"
grep -Fxq 'STOCK_JAR=/inputs/stock/US_MU1316_lsd.jar' "$BUILD_TEST_RESULTS/launcher-arguments.txt"
if grep -q 'BOOT_JAR\|/inputs/boot' "$BUILD_TEST_RESULTS/launcher-arguments.txt"; then
    echo 'FAIL launcher: optional car library was required' >&2; exit 1
fi
echo 'PASS default layout without jcl.jar (build does not require the car library)'

# The public regression command must accept the default layout without explicit inputs.
(
    clear_inputs
    export PATH="$work/launcher-bin:$PATH"
    bash "$project/scripts/test_java_build.sh"
)
grep -Fxq 'STOCK_JAR=/inputs/stock/US_MU1316_lsd.jar' "$BUILD_TEST_RESULTS/launcher-arguments.txt"
grep -Fxq "type=bind,source=$project/build/java-build-tests,target=/out" "$BUILD_TEST_RESULTS/launcher-arguments.txt"
grep -Fxq '/src/scripts/test_java_build.sh' "$BUILD_TEST_RESULTS/launcher-arguments.txt"
echo 'PASS build regression command with the default layout'
echo 'Java build contracts: PASS'
