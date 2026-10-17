#!/bin/bash
# Every default Java group (CHECK_JAVA_GROUP picks one) plus stock linkage, including the native RGI contract: the C half runs
# here on the host (Python 3 and a C compiler), the Java half in Docker. See README.md, "Building".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export RGD_CONTRACT_FRAMES=${RGD_CONTRACT_FRAMES:-${JAVA_OUTPUT:-$ROOT/build}/rgd-native-contract}
RGD_CONTRACT_STAGE=native RGD_CONTRACT_OUT="$RGD_CONTRACT_FRAMES" \
    python3 "$ROOT/tests/test_rgd_native_contract.py"
exec bash "$ROOT/scripts/java/docker.sh" test "${CHECK_JAVA_GROUP:-all}" "$@"
