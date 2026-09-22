#!/bin/sh
# Run every tests/*.al and compare against its .out (stdout) and .err
# (stderr, when the file exists). A "// status: N" line in the test says
# what exit status to expect, "// args: ..." what to pass.
#
#   sh tests/run.sh [build directory]
#
# Each test runs twice: compiled in memory by `a`, and compiled to a file by
# `ac` and run from it, which must behave the same. The second run leaves the
# instructions unfused (AL_NOQUICKEN), so fused and plain execution are
# compared on every test too.
set -u
dir=${1:-build-host}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
A="$root/$dir/a"
AC="$root/$dir/ac"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
pass=0
fail=0

cd "$here" || exit 1
for t in *.al; do
	name=${t%.al}
	args=$(sed -n 's|^// args: ||p' "$t")
	status=$(sed -n 's|^// status: ||p' "$t")
	[ -n "$status" ] || status=0
	stdin=$(sed -n 's|^// stdin: ||p' "$t")

	if [ -n "$stdin" ]; then
		printf '%s\n' "$stdin" > "$tmp/in"
	else
		: > "$tmp/in"
	fi

	# shellcheck disable=SC2086
	AL_LEAKCHECK=1 AL_TMP="$tmp" "$A" "$t" $args < "$tmp/in" > "$tmp/out" 2> "$tmp/err"
	got=$?
	ok=1
	if [ -f "$name.out" ]; then
		diff -u "$name.out" "$tmp/out" > "$tmp/diff" 2>&1 || ok=0
	elif [ -s "$tmp/out" ]; then
		ok=0
		printf 'unexpected output:\n' > "$tmp/diff"
		cat "$tmp/out" >> "$tmp/diff"
	fi
	if [ "$ok" = 1 ]; then
		if [ -f "$name.err" ]; then
			diff -u "$name.err" "$tmp/err" > "$tmp/diff" 2>&1 || ok=0
		elif [ -s "$tmp/err" ]; then
			ok=0
			printf 'unexpected stderr:\n' > "$tmp/diff"
			cat "$tmp/err" >> "$tmp/diff"
		fi
	fi
	if [ "$ok" = 1 ] && [ "$got" != "$status" ]; then
		ok=0
		printf 'exit status %s, expected %s\n' "$got" "$status" > "$tmp/diff"
	fi

	# the same program, compiled to a file first
	if [ "$ok" = 1 ] && "$AC" -o "$tmp/prog" "$t" > "$tmp/acerr" 2>&1; then
		# shellcheck disable=SC2086
		AL_NOQUICKEN=1 AL_LEAKCHECK=1 AL_TMP="$tmp" "$A" "$tmp/prog" $args < "$tmp/in" > "$tmp/out2" 2> "$tmp/err2"
		got2=$?
		if ! cmp -s "$tmp/out" "$tmp/out2" || ! cmp -s "$tmp/err" "$tmp/err2" || [ "$got2" != "$got" ]; then
			ok=0
			printf 'the compiled program behaved differently (status %s):\n' "$got2" > "$tmp/diff"
			diff -u "$tmp/out" "$tmp/out2" >> "$tmp/diff" 2>&1
			diff -u "$tmp/err" "$tmp/err2" >> "$tmp/diff" 2>&1
		fi
	fi

	if [ "$ok" = 1 ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL %s\n' "$t"
		sed 's/^/    /' "$tmp/diff" | head -25
	fi
done

printf '%d/%d tests passed\n' "$pass" "$((pass + fail))"
[ "$fail" = 0 ]
