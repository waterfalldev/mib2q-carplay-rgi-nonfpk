#!/bin/bash
# Test the existing shipping JAR (or an explicit PDC_PATCH_JAR) against the stock resource policy.
set -euo pipefail
export JAVA_SKIP_BUILD=1
if [ -n "${PDC_PATCH_JAR:-}" ]; then export CARPLAY_HOOK_JAR=$PDC_PATCH_JAR; fi
exec bash "$(dirname "$0")/java/docker.sh" test pdc "$@"
