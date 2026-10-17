#!/bin/bash
# Linux/Docker host checks for the maneuver renderer (see README.md).
#
#   run.sh [tree]                     build + run the three renderer suites
#   run.sh [tree] check               fingerprint the tree, compare with fingerprint.sum
#   run.sh [tree] digest              print the per-unit digest (to refresh fingerprint.sum)
#   run.sh [tree] fingerprint         print the full geometry fingerprint on stdout
#   FP_VERTS=full run.sh tree fingerprint   ... with every vertex of every draw
#
# [tree] defaults to this repo; any checkout or git worktree works.  It is mounted
# read-only and nothing is written outside the container.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "${1:-$HERE/../..}" && pwd)"
MODE=${2:-tests}
case "$MODE" in tests|check|digest|fingerprint) ;; *) echo "usage: run.sh [tree] [tests|check|digest|fingerprint]" >&2; exit 2;; esac
BASE_IMG=${QNX_TOOLCHAIN_IMAGE:-qnx65-armv7-toolchain:latest}
IMG=${HOST_TEST_IMAGE:-carplay-rgi-host-tests:local}
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        export MSYS_NO_PATHCONV=1      # keep MSYS from rewriting the container paths
        HERE_D="$(cygpath -m "$HERE")"; SRC_D="$(cygpath -m "$SRC_DIR")" ;;
    *) HERE_D="$HERE"; SRC_D="$SRC_DIR" ;;
esac
[ -d "$SRC_DIR/maneuver_render" ] || { echo "ERROR: $SRC_DIR has no maneuver_render/" >&2; exit 2; }
docker image inspect "$BASE_IMG" >/dev/null 2>&1 || { echo "ERROR: docker image '$BASE_IMG' not found" >&2; exit 1; }
# The QNX toolchain image has host gcc but no g++ (scene/*.cpp needs it): the host-test image
# (tests/Dockerfile.host) adds it, built once.
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "building $IMG from tests/Dockerfile.host (once)..." >&2
    docker build -q --platform=linux/amd64 -t "$IMG" -f "$HERE_D/../Dockerfile.host" "$HERE_D/.." >&2
fi
run() {
    docker run --rm --platform=linux/amd64 -e FP_VERTS="${FP_VERTS:-}" \
        -v "$SRC_D":/src:ro -v "$HERE_D":/h:ro "$IMG" bash /h/inner.sh "$1"
}
[ "$MODE" = check ] || { run "$MODE"; exit; }

got=$(run digest | tr -d '\r')
want=$(tr -d '\r' < "$HERE/fingerprint.sum")
if [ "$got" = "$want" ]; then
    echo "renderer fingerprint: $(printf '%s\n' "$got" | wc -l | tr -d ' ') units match fingerprint.sum"
    exit 0
fi
echo "renderer fingerprint differs from fingerprint.sum in these units:"
diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed -n 's/^[<>] [0-9a-f]* /  /p' | sort -u
echo "Compare full fingerprints of the two trees to see what changed; if intended,"
echo "refresh with: tests/renderer/run.sh . digest > tests/renderer/fingerprint.sum"
exit 1
