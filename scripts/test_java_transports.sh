#!/bin/bash
# Local transports, AltScreen, lifecycle and parking regressions on a fresh build.
set -euo pipefail
exec bash "$(dirname "$0")/java/docker.sh" test transports "$@"
