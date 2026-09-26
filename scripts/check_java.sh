#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${STOCK_JAR:?Set STOCK_JAR to the external stock JAR}"
: "${CARPLAY_DEPENDENCIES:?Set CARPLAY_DEPENDENCIES to the dependency cache}"
: "${JAVA_HOME:?Set JAVA_HOME to JDK 8}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
pwsh -NoProfile -File "$ROOT/packaging/Check-Java.ps1" -StockJar "$STOCK_JAR" \
    -Dependencies "$CARPLAY_DEPENDENCIES" -JavaHome "$JAVA_HOME" -OutputRoot "$OUT/java"
