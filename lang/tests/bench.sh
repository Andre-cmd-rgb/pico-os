#!/bin/sh
# Timings for the benchmark programs, on this PC.
#
#   sh tests/bench.sh [build directory]
#
# The same programs run on the board with `pico bench.pico` and `pico benchmark.pico`.
set -u
dir=${1:-build-host}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
A="$root/$dir/pico"

cd "$root/examples" || exit 1
"$A" bench.pico 4 || exit 1
for group in cpu fpu mem; do
	"$A" benchmark.pico -n 20 "$group" | grep -v '^(checks' || exit 1
done
