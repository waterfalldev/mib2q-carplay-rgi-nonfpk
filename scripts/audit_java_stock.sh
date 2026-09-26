#!/bin/bash
# Compatibility entry point; the shared runner builds and checks the same JAR.
exec bash "$(dirname "$0")/check_java.sh" "$@"
