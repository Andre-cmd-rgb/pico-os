name=world
cat <<EOF
hello $name
  kept "quotes" and \$dollar and \"esc\"
sum $((1+2)) and $(echo sub)
EOF
cat <<'EOF'
literal $name $(echo no)
EOF
cat <<-EOF
	tabs gone $name
	EOF
cat <<A; cat <<B
first A
A
second B
B
while read a b; do echo "[$a|$b]"; done <<EOF
1 one
2 two
EOF
f() {
	cat <<EOF
in function $1
EOF
}
f arg
cat <<EOF | tr a-z A-Z
piped $name
EOF
( cat <<EOF )
subshell
EOF
x=$(cat <<EOF
captured
EOF
)
echo "x=$x"
set -o pipefail
false | true; echo pipefail=$?
set +o pipefail
false | true; echo nopipefail=$?
set -u
echo ${undefined:-default}
( echo $undefined_var; echo not reached ) 2>&1
set +u
set -x
echo traced 2>&1
set +x
type cd ls f nosuch; echo type=$?
command -v ls
g() { echo func; }
command echo notfunc
( set -e; true && false; echo not reached ); echo e1=$?
( set -e; false || true; echo reached ); echo e2=$?
( set -e; if false; then :; fi; ! true; echo reached2 ); echo e3=$?
( set -e; while false; do :; done; f2() { false; echo no; }; f2; echo no2 ); echo e4=$?
test /tmp -nt /nonexistent && echo newer
echo done
sh -c 'trap "echo bye \$?" EXIT; echo hi; false'; echo rc=$?
sh -c 'trap "echo caught INT" INT; kill -INT $$; echo after'
sh -c 'trap "" INT; kill -INT $$; echo ignored'
sh -c 'trap "echo cleanup" EXIT; exit 3'; echo rc=$?
sh -c 'set -e; trap "echo on-exit" EXIT; false; echo not'; echo rc=$?
trap 'echo x' EXIT; trap; trap - EXIT; trap
alias hi='echo hello'; alias hi; unalias hi; alias hi 2>/dev/null || echo gone
sleep 5 & kill %1; wait %1; echo st=$?
echo end
