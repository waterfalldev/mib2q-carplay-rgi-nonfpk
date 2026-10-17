#!/bin/bash
# Each Java variant must link cleanly against every firmware it is installed on:
#   build/carplay_hook.jar     vs STOCK_JAR (java_stock/US_MU1316_lsd.jar), STOCK_EU_JAR (EU_MU1329_lsd.jar)
#   build/carplay_hook_cn.jar  vs STOCK_CN_JAR (java_stock/CN_MU1003_lsd.jar)
# (tests/JavaStockLinkageAudit.java: patch->stock and stock->patch member links).
# Keep this matrix in step with java_jar() in the installer (E2P region US/ER/JP -> US jar,
# CN -> CN jar).  JP MU1017 has no java_stock jar: its replaced stock classes are
# byte-identical to US.
# Runs through Docker; without Docker or the stock JAR it skips, like the other optional tests.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "SKIP (audit_java_variants: needs Docker)"; exit 0
fi
if [ ! -f "${STOCK_JAR:-$ROOT/java_stock/US_MU1316_lsd.jar}" ]; then
    echo "SKIP (audit_java_variants: needs the stock firmware JARs in java_stock/, or STOCK_JAR)"; exit 0
fi
bash "$ROOT/scripts/java/docker.sh" build > /dev/null
JAVA_SKIP_BUILD=1 bash "$ROOT/scripts/java/docker.sh" test linkage
echo "Java variants link cleanly: PASS"
