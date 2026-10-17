#!/bin/bash
# Build the CarPlay Java patch: build/carplay_hook.jar, plus build/carplay_hook_cn.jar when a CN
# stock JAR is available. Java 8 comes from Docker (scripts/java/docker.sh); the default inputs
# are the java_stock/ layout, and STOCK_JAR, STOCK_CN_JAR, CARPLAY_DEPENDENCIES and JAVA_OUTPUT
# point elsewhere (README.md, "Building").
set -euo pipefail
[ "$#" -eq 0 ] || { echo 'usage: ./scripts/build_java.sh' >&2; exit 2; }
exec bash "$(dirname "$0")/java/docker.sh" build
