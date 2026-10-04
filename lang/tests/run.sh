#!/bin/sh
# Run every tests/*.pico and compare against its .out (stdout) and .err
# (stderr, when the file exists). A "// status: N" line in the test says
# what exit status to expect, "// args: ..." what to pass.
#
#   sh tests/run.sh [build directory]
#
# Each test runs twice: compiled in memory by `pico`, and compiled to a file
# by `picoc` and run from it, which must behave the same. The second run leaves the
# instructions unfused (PICO_NOQUICKEN), so fused and plain execution are
# compared on every test too. Then `picoc -t` checks programs good and
# damaged, and source.
set -u
dir=${1:-build/host}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
A="$root/$dir/pico"
AC="$root/$dir/picoc"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
pass=0
fail=0

cd "$here" || exit 1
for t in *.pico; do
	name=${t%.pico}
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
	PICO_LEAKCHECK=1 PICO_TMP="$tmp" "$A" "$t" $args < "$tmp/in" > "$tmp/out" 2> "$tmp/err"
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
		PICO_NOQUICKEN=1 PICO_LEAKCHECK=1 PICO_TMP="$tmp" "$A" "$tmp/prog" $args < "$tmp/in" > "$tmp/out2" 2> "$tmp/err2"
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

# picoc -t FILE: the loader's checks, with nothing run, and nothing said
# when they pass. What it should say instead comes on stdin.
check() {	# expected status, file
	cat > "$tmp/want"
	"$AC" -t "$2" > "$tmp/out" 2> "$tmp/err"
	got=$?
	if [ "$got" = "$1" ] && [ ! -s "$tmp/out" ] && cmp -s "$tmp/want" "$tmp/err"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL picoc -t %s: status %s, expected %s\n' "$2" "$got" "$1"
		cat "$tmp/out" "$tmp/err" | sed 's/^/    /' | head -25
	fi
}

# A program, the same with a byte damaged and as a newer picoc's would be,
# and source, which is compiled in memory first.
"$AC" -o "$tmp/good" arith.pico
version=$(od -An -tu1 -j3 -N1 "$tmp/good" | tr -d ' ')
cp "$tmp/good" "$tmp/damaged"
printf '\377' | dd of="$tmp/damaged" bs=1 seek=5 conv=notrunc 2> /dev/null
cp "$tmp/good" "$tmp/newer"
printf '\377' | dd of="$tmp/newer" bs=1 seek=3 conv=notrunc 2> /dev/null
check 0 "$tmp/good" < /dev/null
check 1 "$tmp/damaged" <<EOF
$tmp/damaged: damaged executable (checksum mismatch)
EOF
check 1 "$tmp/newer" <<EOF
$tmp/newer: executable format version 255, this system runs version $version (compile it again with picoc)
EOF
check 0 arith.pico < /dev/null
check 1 bad_syntax.pico < bad_syntax.err

printf '%d/%d tests passed\n' "$pass" "$((pass + fail))"
[ "$fail" = 0 ]
