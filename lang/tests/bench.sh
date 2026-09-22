#!/bin/sh
# Timings for the benchmark programs, on this PC.
#
#   sh tests/bench.sh [build directory]
#
# The same programs run on the board with `a bench.al` and `a benchmark.al`.
set -u
dir=${1:-build-host}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
A="$root/$dir/a"

cd "$root/examples" || exit 1
"$A" bench.al 4 || exit 1
for group in cpu fpu mem; do
	"$A" benchmark.al -n 20 "$group" | grep -v '^(checks' || exit 1
done
