#!/bin/bash
# Shipping US/EU/CN classes against real stock interfaces; seeded IPC and raw-delta fuzz.
# CARPLAY_FUZZ_ITERATIONS=100000 CARPLAY_FUZZ_SEEDS="32 47426351 0x8b338d86 0x4ccb85a6" bash scripts/test_route_context_fuzz.sh
# Replay a failure with the printed seed; each run's output persists in build/tests/.
set -euo pipefail
exec bash "$(dirname "$0")/java/docker.sh" test fuzz "$@"
