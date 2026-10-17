#!/bin/bash
# Host-only investigation of real camera/display contracts; no HU or release writes.
set -euo pipefail
exec bash "$(dirname "$0")/java/docker.sh" test partial360 "$@"
