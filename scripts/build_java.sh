#!/bin/bash
# Build the Java 1.4 patch against an external stock JAR. Requires PowerShell and JDK 8.
# STOCK_JAR, CARPLAY_DEPENDENCIES and JAVA_HOME select local inputs.
set -euo pipefail
PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
: "${STOCK_JAR:?Set STOCK_JAR to your reconstructed stock JAR}"
: "${CARPLAY_DEPENDENCIES:?Set CARPLAY_DEPENDENCIES to an external dependency directory}"
: "${JAVA_HOME:?Set JAVA_HOME to JDK 8}"
BUILD_ID=${CARPLAY_BUILD_ID:-$(git -C "$PROJECT_DIR" describe --always --dirty)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
command -v pwsh >/dev/null || { echo 'PowerShell 7 (pwsh) is required.' >&2; exit 1; }
pwsh -NoProfile -File "$PROJECT_DIR/packaging/Build-Java.ps1" -SourceRoot "$PROJECT_DIR" \
    -StockJar "$STOCK_JAR" -Dependencies "$CARPLAY_DEPENDENCIES" -JavaHome "$JAVA_HOME" \
    -OutputRoot "$OUT/java" -BuildId "$BUILD_ID"
mkdir -p "$PROJECT_DIR/build"
cp "$OUT/java/carplay_hook.jar" "$PROJECT_DIR/build/carplay_hook.jar"
