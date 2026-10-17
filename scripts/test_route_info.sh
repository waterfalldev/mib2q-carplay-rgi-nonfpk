#!/bin/bash
# Build fresh and exercise route guidance against the supplied stock firmware: the native RGI
# contract, then the Java route group (scripts/check_java.sh).
set -euo pipefail
CHECK_JAVA_GROUP=route exec bash "$(dirname "$0")/check_java.sh" "$@"
