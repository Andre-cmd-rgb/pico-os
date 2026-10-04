#!/usr/bin/env python3
"""The text programs in bin/, checked on the PC against GNU's.

    python3 tools/programs_test.py          (part of make hosttest)
    python3 tools/programs_test.py -v       every case, not only failures
    python3 tools/programs_test.py --board /dev/ttyACM0   (make progtest)

The programs that need nothing but system calls -- grep, sed, sort, find,
cut, tr, printf, expr and the rest -- are built into one binary together with
tools/host_pt.c, which supplies those calls over POSIX, under the address
and undefined-behaviour sanitizers. Each case is a shell command line run
twice: once with this binary's programs first on PATH and once with the
PC's own, and the two must print the same and exit the same. Where ours
means to differ from GNU (a narrower feature, a different layout), the
case gives what ours should print instead.

Then random patterns: the regular expression engine against GNU grep,
basic and extended, on random lines, comparing what -o picks out.

With --board, the same cases run on a real board instead, through its own
shell, and are compared with what GNU's programs print here.

Needs cc and GNU coreutils, grep, findutils on PATH.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCES = [os.path.join(HERE, "host_pt.c")] + [
    os.path.join(ROOT, p) for p in (
        "bin/diff.c", "bin/filters.c", "bin/find.c", "bin/sums.c", "bin/regex.c", "bin/sed.c", "bin/shellutils.c", "bin/sort.c",
        "bin/textutils.c", "bin/util.c", "kernel/match.c", "bin/notes.c", "bin/pick.c",
        "bin/dates.c", "bin/todo.c", "bin/calendar.c", "bin/calc.c")]
PROGRAMS = ("basename cal calc calendar cksum cmp cut diff dirname du echo expr find grep head notes "
            "printf realpath sed seq sort tail tee todo tr uniq wc xargs yes").split()
VERBOSE = "-v" in sys.argv

# The fixtures every case can use, made in a fresh directory.
FILES = {
    "words": "banana\napple\ncherry\napple\nBanana\n\ndate\n  apple pie\nfig\n",
    "nums": "10\n9\n-3\n2.5\n0\n-0\n100\n007\n1e3\n  42\nabc\n",
    "sizes": "2K\n1M\n512\n3G\n1K\n1.5M\n",
    "table": "root:x:0:0:root:/root:/bin/sh\nandre:x:1000:1000::/home/user:/bin/sh\n"
             "nobody:x:65534:65534::/:/bin/false\nno colons here\n",
    "tabs": "a\tb\tc\nd\te\tf\n1\t2\n",
    "cols": "3 apple 10\n1 cherry 2\n2 banana 33\n3 apple 9\n1 fig 100\n",
    "dups": "a\na\nb\nB\nb\nc\nc\nc\nd\n",
    "noeol": "one\ntwo\nthree",
    "empty": "",
    "long": "x" * 5000 + "\n" + "y" * 3 + "\n",
    "utf8": "café\nCAFÉ\nnaïve\nüber alles\nplain\n",
    "lines": "".join(f"line {i}\n" for i in range(1, 31)),
    "code": "int main(void)\n{\n\treturn 0; /* done */\n}\n// the end\n",
    "note.md": "# Title\n\nSome **bold** and *italic* and `code`, a [link](http://x) and "
               "![pic](p.png).\nsnake_case and 2 * 3 stay, \\*escaped\\*.\n\n## List\n\n- one\n"
               "- two that is long enough to wrap over the line\n  1. nested\n- [ ] task\n\n"
               "> quoted\n> text\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n---\n\nSetext\n======\n\n"
               "hard  \nbreak\n\n```\ncode\tline\n```\n",
    "table.md": "| Anno | Evento |\n|---|--:|\n| 1469 | Nasce a Firenze da famiglia borghese |\n"
                "| 1513-17 | Scrive i *Discorsi*<br>e il **Principe** |\n\n"
                "| | Machiavelli | Guicciardini |\n|---|---|---|\n| Fortuna | Pesa per la metà | "
                "Vince lei, ha \"grandissima potestà\" |\n| Uomo | Malvagio | | extra |\n\n"
                "| Tipo | Caratteristiche |\n|:-:|---|\n| **Chierico** al servizio dei papi | È "
                "al servizio di papi e cardinali \\| colti |\nafter\n",
    "note.txt": "plain text line that is long enough to wrap around\n"
                "    indented line that is long enough to wrap too\n\nlast\n",
    "todo.md": "# To do\n\nsome notes, left alone\n- [ ] studiare storia @2026-09-25\n"
               "- [x] fatto\n- [ ] consegnare ricerca @2026-09-22\n* [ ] senza data\n",
    "diary.txt": "# the diary\n2026-09-24 10:00-11:00 Lab informatica\n"
                 "2026-09-25 09:30 verifica di storia !15\nweekly mo,we 15:00-16:30 Calcio\n"
                 "yearly 10-02 Compleanno di Marco !\nnot an event\n",
    "tree/a.txt": "alpha\n",
    "tree/b.log": "beta\n",
    "tree/sub/c.txt": "gamma alpha\n",
    "tree/sub/deeper/d.TXT": "delta\n",
    "tree/sub/deeper/e": "",
    "tree/other/f.txt": "phi alpha\n",
}
DIRS = ["tree/empty"]

# (command, expected) -- expected None: whatever GNU's does.
CASES = [
    # grep
    ("grep apple words", None),
    ("grep -n apple words", None),
    ("grep -c apple words", None),
    ("grep -v apple words", None),
    ("grep -i banana words", None),
    ("grep -w apple words", None),
    ("grep -x apple words", None),
    ("grep -o 'an*a' words", None),
    ("grep -E 'ap+le|fig' words", None),
    ("grep 'a\\(pp\\)\\{1\\}le' words", None),
    ("grep -e fig -e date words", None),
    ("grep -F '.*' code; echo $?", None),
    ("grep -F '*/' code", None),
    ("grep -l alpha tree/a.txt tree/b.log tree/sub/c.txt", None),
    ("grep -L alpha tree/a.txt tree/b.log tree/sub/c.txt", None),
    ("grep -q cherry words; echo $?", None),
    ("grep -q nothing words; echo $?", None),
    ("grep nothing nosuchfile; echo $?", None),
    ("grep -s nothing nosuchfile; echo $?", None),
    ("grep -m 2 apple words", None),
    ("grep -m 0 apple words; echo $?", None),
    ("grep -A 1 cherry words", None),
    ("grep -B 2 fig words", None),
    ("grep -C 1 -n 'line 1[05]' lines", None),
    ("grep -c '' empty; echo $?", None),
    ("grep '^$' words | wc -l", None),
    ("grep -n . noeol", None),
    ("grep -h alpha tree/a.txt tree/sub/c.txt", None),
    ("grep -H alpha tree/a.txt", None),
    ("grep -r alpha tree | sort", None),
    ("cd tree && grep -r alpha | sort", None),
    ("grep -i 'café' utf8", "café\nCAFÉ\n"),
    ("grep -o 'caf.' utf8", "café\n"),
    ("grep -E 'x{255}' long | wc -c", None),
    ("grep -E 'x{4999}' long 2>&1; echo $?", "grep: invalid repetition count\n2\n"),
    ("grep -ow 'a[a-z]*' words", None),
    ("grep '\\<b' words", None),
    ("grep 'e\\>' words", None),
    ("grep -E '(^| )apple( |$)' words", None),
    ("grep -E 'a{,2}p' words", None),
    ("printf 'x\\0y\\nzzz\\n' | grep y", "Binary file (standard input) matches\n"),
    ("printf 'x\\0y\\nzzz\\n' | grep -c y", None),
    ("grep '[[:upper:]]' words", None),
    ("grep '[^a-z ]' words", None),
    ("grep -e '' -c words", None),
    ("grep 'a\\|e' words | wc -l", None),
    ("grep -E 'a**' words | wc -l", None),
    # diff (headers aside: GNU puts the time to the nanosecond there)
    ("diff words words; echo $?", None),
    ("diff words dups; echo $?", None),
    ("diff lines noeol; echo $?", None),
    ("diff noeol lines", None),
    ("diff empty words", None),
    ("diff words empty", None),
    ("sed '3d;7s/e/E/;$a extra' lines > l2; diff lines l2", None),
    ("sed '1d;5,6d;20s/1/one/' lines > l2; diff lines l2", None),
    ("sed '3d;7s/e/E/;$a extra' lines > l2; diff -u lines l2 | tail -n +3", None),
    ("sed '1d;15,16d;28s/2/two/' lines > l2; diff -u lines l2 | tail -n +3", None),
    ("diff -u noeol lines | tail -n +3", None),
    ("diff -u words empty | tail -n +3", None),
    ("diff -q words dups; echo $?", None),
    ("diff -q words words; echo $?", None),
    ("diff words nosuch 2>/dev/null; echo $?", None),
    ("seq 500 > a; seq 500 | sed '100d;200s/0/o/;300a new' > b; diff a b", None),
    ("seq 500 > a; seq 500 | sed '100d;200s/0/o/;300a new' > b; diff -u a b | tail -n +3", None),
    ("sort words > s; diff words s", None),
    ("sort words > s; diff -u words s | tail -n +3", None),
    # checksums: md5sum and the rest use the chip's hashing, so on the PC
    # these compare GNU's with itself, and on the board with ours
    ("cksum words lines noeol", None),
    ("cksum < empty; cksum empty", None),
    ("cat long | cksum", None),
    ("md5sum words empty noeol", None),
    ("sha1sum words < lines; sha1sum < lines", None),
    ("sha256sum words lines long", None),
    ("sha512sum noeol", None),
    ("sha256sum words lines > s; sha256sum -c s; echo $?", None),
    ("echo x > f; md5sum words f > s; echo y > f; md5sum -c s 2>/dev/null; echo $?", None),
    ("sha256sum nosuch 2>/dev/null; echo $?", None),
    # sed
    ("sed 's/apple/APPLE/' words", None),
    ("sed 's/a/_/g' words", None),
    ("sed 's/a/_/2' words", None),
    ("sed 's/a/_/2g' words", None),
    ("sed -n 's/an/AN/p' words", None),
    ("sed 's/\\(a\\)\\(n\\)/\\2\\1/g' words", None),
    ("sed -E 's/(a)(n)/\\2\\1/g' words", None),
    ("sed 's/[aeiou]/<&>/g' words", None),
    ("sed 's/x*/-/g' words", None),
    ("echo baaac | sed 's/a*/x/g'", None),
    ("echo abc | sed 's/b*/x/2'", None),
    ("sed 's|/|:|g' table", None),
    ("sed 's/\\//|/g' table", None),
    ("sed 's/banana/split\\\nhere/' words", None),
    ("sed 's/a/\\&/g' words", None),
    ("sed 's/APPLE/x/I' words", None),
    ("sed -n '3p' lines", None),
    ("sed -n '$p' lines", None),
    ("sed -n '2,4p' lines", None),
    ("sed -n '/line 5/,/line 8/p' lines", None),
    ("sed -n '/line 25/,+2p' lines", None),
    ("sed -n '0~7p' lines", None),
    ("sed -n '3,~4p' lines", None),
    ("sed -n '0,/line/p' lines", None),
    ("sed -n '1,/line/p' lines", None),
    ("sed -n '5,3p' lines", None),
    ("sed '2,28d' lines", None),
    ("sed '/apple/!d' words", None),
    ("sed '1~2d' lines", None),
    ("sed '$d' lines", None),
    ("sed -n '/a/{/n/p}' words", None),
    ("sed '/apple/{s/a/A/;s/e/E/}' words", None),
    ("sed = noeol", None),
    ("sed -n '$=' lines", None),
    ("sed 'y/abc/xyz/' words", None),
    ("sed '2i\\\ninserted' noeol", None),
    ("sed '2a appended' noeol", None),
    ("sed '$a the end' noeol", None),
    ("sed '1i first' noeol", None),
    ("sed '2c changed' noeol", None),
    ("sed '2,3c changed' noeol", None),
    ("sed 'n;d' lines", None),
    ("sed '$!N;s/\\n/ /' lines", None),
    ("sed 'N;N;s/\\n/+/g' lines", None),
    ("sed -n 'h;n;G;p' lines", None),
    ("sed '1!G;h;$!d' noeol", None),
    ("sed -n '1!G;h;$p' lines", None),
    ("sed 'N;P;D' lines", None),
    ("sed '$!N;$!D' lines", None),
    ("sed ':a;N;$!ba;s/\\n/,/g' noeol", None),
    ("sed ':a;s/^.\\{1,9\\}$/ &/;ta' nums", None),
    ("sed -e :a -e '$!N;s/\\n//;ta' noeol", None),
    ("sed 's/a/A/;T;s/$/ (had a)/' words", None),
    ("sed '3q' lines", None),
    ("sed '3q5' lines; echo $?", None),
    ("sed '3Q' lines", None),
    ("sed -n '/cherry/{p;q}' words", None),
    ("sed 'z;s/^$/empty/' noeol", None),
    ("sed -n l code", None),
    ("printf 'a\\001b\\tc\\n' | sed -n l", None),
    ("sed -n 'l 0' long | wc -c", None),
    ("sed p noeol", None),
    ("sed -n p noeol", None),
    ("printf 'x' | sed 's/x/y/'", None),
    ("printf 'x' | sed '$a more'", None),
    ("sed -s -n '$p' words lines", None),
    ("sed -n '$p' words lines", None),
    ("sed -e 's/a/b/' -e 's/b/c/' words", None),
    ("printf 's/an/AN/g\\n/fig/d\\n' > script && sed -f script words", None),
    ("sed -n -e '/apple/{' -e 'p' -e '}' words", None),
    ("sed '/apple/w out' words > /dev/null && cat out", None),
    ("sed 's/apple/X/w out' words > /dev/null && cat out", None),
    ("sed '1r noeol' dups", None),
    ("sed '/b/r nosuch' dups", None),
    ("cp words w && sed -i 's/apple/pear/' w && cat w", None),
    ("cp words w && sed -i.bak '1d' w && cat w w.bak", None),
    ("cp words w && cp lines l && sed -i '1d;$d' w l && cat w l", None),
    ("sed 's/caf\u00e9/tea/' utf8", "tea\nCAF\u00c9\nna\u00efve\n\u00fcber alles\nplain\n"),
    ("sed 's/./X/g' utf8", "XXXX\nXXXX\nXXXXX\nXXXXXXXXXX\nXXXXX\n"),
    ("sed 's/[[:upper:]]/_/g' words", None),
    ("sed -E 's/(^| )a/\\1A/g' words", None),
    ("sed 's/^/> /' code", None),
    ("sed 's/$/;/' code", None),
    ("sed '/^$/d' words", None),
    ("sed -n '/^\\s*apple/p' words", None),
    ("sed 's/\\bapple\\b/X/g' words", None),
    ("sed 's/\\<a/X/g' words", None),
    ("sed -n '/a/,/e/{=;p}' words", None),
    ("sed '2{h;d};4{G}' lines | head -6", None),
    ("sed 's/a/b/;s//c/' words", None),
    ("sed '/apple/s//X/' words", None),
    ("sed 's/x/y/;s/q/z' words 2>&1; echo $?", "sed: char 13: unterminated s command\n1\n"),
    ("sed 'k' words 2>&1; echo $?", "sed: char 1: unknown command\n1\n"),
    ("sed 'b nowhere' words 2>&1; echo $?", "sed: can't find label for jump to `nowhere'\n1\n"),
    ("sed '{p' words 2>&1; echo $?", "sed: char 3: unmatched {\n1\n"),
    ("sed p nosuch words 2>&1 | head -2; sed p nosuch 2>/dev/null; echo $?", None),
    ("sed 's/a/\\3/' words 2>&1; echo $?", "sed: char 8: a reference to a group the pattern does not have\n1\n"),
    # sort
    ("sort words", None),
    ("sort -r words", None),
    ("sort -u words", None),
    ("sort -f words", None),
    ("sort -n nums", None),
    ("sort -rn nums", None),
    ("sort -h sizes", None),
    ("sort -t: -k3,3n table", None),
    ("sort -t: -k7 -k1,1 table", None),
    ("sort -k2,2 -k3,3nr cols", None),
    ("sort -k2 cols", None),
    ("sort -s -k1,1 cols", None),
    ("sort -k 2.2,2.3 cols", None),
    ("sort -b -k2 cols", None),
    ("sort -nu nums", None),
    ("sort -c words; echo $?", None),
    ("sort words | sort -c; echo $?", None),
    ("sort noeol empty", None),
    ("sort -o out words && cat out", None),
    ("cp words w2 && sort -o w2 w2 && cat w2", None),
    ("sort long | wc -c", None),
    # uniq
    ("uniq dups", None),
    ("uniq -c dups", None),
    ("uniq -d dups", None),
    ("uniq -u dups", None),
    ("uniq -i dups", None),
    ("uniq -ci dups", None),
    ("sort cols | uniq -f 1", None),
    ("uniq -s 1 dups", None),
    ("uniq dups out && cat out", None),
    # cut
    ("cut -d: -f1 table", None),
    ("cut -d: -f1,6- table", None),
    ("cut -d: -f3-4 -s table", None),
    ("cut -f2 tabs", None),
    ("cut -c1-3 words", None),
    ("cut -c3- words", None),
    ("cut -c-2 utf8", "ca\nCA\nna\nüb\npl\n"),
    ("cut -b1-4 noeol", None),
    ("cut -d: -f 9 table", None),
    # tr
    ("tr a-z A-Z < words", None),
    ("tr -d aeiou < words", None),
    ("tr -s 'a-z' < dups", None),
    ("tr -s '\\n' < words", None),
    ("tr -cd 'a-z\\n' < table", None),
    ("tr '[:lower:]' '[:upper:]' < code", None),
    ("tr -c '[:alnum:]\\n' '_' < table", None),
    ("tr abc 'x[y*]' < words", None),
    ("tr 'a-c' 'xy' < words", None),
    ("tr -t 'abc' 'xy' < words", None),
    ("tr -ds 'a' 'p' < words", None),
    ("echo hello | tr -s l", None),
    # head, tail
    ("head -3 lines", None),
    ("head -n 2 words lines", None),
    ("head -n 2 words lines; head -n 1 words lines", None),
    ("head -c 10 lines", None),
    ("tail -3 lines", None),
    ("tail -n +28 lines", None),
    ("tail -c 12 lines", None),
    ("cat lines | tail -c 12", None),
    ("tail -n 2 noeol", None),
    ("tail -n 0 lines", None),
    ("tail -n 1 words lines", None),
    ("head -n 1 < noeol", None),
    # wc
    ("wc -l lines", None),
    ("wc -w words", None),
    ("wc -c noeol", None),
    ("cat lines | wc -l", None),
    ("wc -l lines words", None),
    ("wc lines", None),
    ("wc -lw lines words", None),
    ("cat lines | wc", None),
    ("cat lines | wc -lc", None),
    ("realpath nosuch/deeper; echo $?", None),
    # cmp
    ("cmp words words; echo $?", None),
    ("cmp words dups; echo $?", None),
    ("cmp -s words dups; echo $?", None),
    ("cmp lines noeol; echo $?", None),
    # tee
    ("echo hi | tee t1 t2 && cat t1 t2", None),
    ("echo a > t3; echo b | tee -a t3 >/dev/null; cat t3", None),
    # printf
    ("printf '%s-%d\\n' a 1 b 2 c", None),
    ("printf '%5.2f|%-6s|%x|%o|%X\\n' 3.14159 ab 255 8 255", None),
    ("printf '%b\\n' 'tab\\there' 'oct\\0101'", None),
    ("printf '%c%c\\n' hello world", None),
    ("printf '%*d|%-*d|\\n' 5 42 4 7", None),
    ("printf '%.3s\\n' abcdef", None),
    ("printf 'no newline'", None),
    ("printf '%d\\n' \"'A\"", None),
    ("printf '%%\\n'", None),
    ("printf '%s\\n'", None),
    ("printf '\\x41\\101\\n'", None),
    ("printf 'a\\cb'; echo", None),
    ("printf '%d\\n' 12abc 2>&1; echo $?", "printf: '12abc': not a valid number\n12\n1\n"),
    ("printf '%i %u\\n' -5 7", None),
    ("printf '%e %g\\n' 12345.678 0.0001", None),
    # seq
    ("seq 5", None),
    ("seq 2 4", None),
    ("seq 1 3 10", None),
    ("seq 10 -3 1", None),
    ("seq 0 0.1 1", None),
    ("seq 0 1e-1 0.5", None),
    ("seq 1 0.5e0 2", None),
    ("seq 1e2 1e2 3e2", None),
    ("seq -w 8 11", None),
    ("seq -w -1 1", None),
    ("seq -s , 5", None),
    ("seq -f '%03g' 3", None),
    ("seq 5 1", None),
    ("seq 1.5 3", None),
    ("seq -3 -1", None),
    # expr
    ("expr 3 + 4 \\* 2", None),
    ("expr 7 % 3", None),
    ("expr 10 / 3", None),
    ("expr 1 / 0 2>&1; echo $?", "expr: division by zero\n2\n"),
    ("expr abc = abc", None),
    ("expr 10 \\> 9", None),
    ("expr abc \\< abd", None),
    ("expr hello : 'h.l'", None),
    ("expr notes.txt : '\\(.*\\)\\.txt'", None),
    ("expr 0 \\| 5", None),
    ("expr 0 \\& 5; echo $?", None),
    ("expr '' ; echo $?", None),
    ("expr \\( 1 + 2 \\) \\* 3", None),
    ("expr a + 1 2>&1; echo $?", "expr: not a number\n2\n"),
    ("expr -5 + 3", None),
    # names
    ("basename /a/b/c.txt .txt", None),
    ("basename /a/b/ ", None),
    ("basename /", None),
    ("basename c.txt c.txt", None),
    ("dirname /a/b/c", None),
    ("dirname a", None),
    ("dirname /a", None),
    ("dirname a/b//", None),
    ("dirname //", "/\n"),
    ("cd tree && realpath sub/../a.txt", "@/tree/a.txt\n"),
    ("cd tree && realpath nosuch | grep -c '/tree/nosuch$'", None),
    # yes
    ("yes | head -3", None),
    ("yes a b | head -2", None),
    # echo
    ("echo -n a; echo b", None),
    ("echo -e 'a\\tb\\n\\x41\\0101'", None),
    ("echo -e 'stop\\cnot this'; echo", None),
    ("echo -- -n", None),
    # find
    ("find tree | sort", None),
    ("find tree -name '*.txt' | sort", None),
    ("find tree -iname '*.txt' | sort", None),
    ("find tree -type d | sort", None),
    ("find tree -type f -empty", None),
    ("find tree -maxdepth 1 | sort", None),
    ("find tree -mindepth 2 -type f | sort", None),
    ("find tree -path '*sub*' -name '*.txt' | sort", None),
    ("find tree -name sub -prune -o -type f -print | sort", None),
    ("find tree ! -name '*.txt' -type f | sort", None),
    ("find tree \\( -name a.txt -o -name b.log \\) | sort", None),
    ("find tree -regex '.*/[ab]\\..*' | sort", None),
    ("find tree -regextype posix-extended -regex '.*/(a|c)\\.txt' | sort", None),
    ("find tree -size -1 -type f", None),
    ("find tree -type f -name '*.txt' -exec cat {} \\; | sort", None),
    ("find tree -type f -name '*.txt' -exec echo {} + | tr ' ' '\\n' | sort", None),
    ("find tree -name 'a*' -quit", None),
    ("find tree/a.txt tree/b.log", None),
    ("cd tree && find . -name '*.log'", None),
    ("cd tree && find -name '*.log'", None),
    ("find tree -print0 | tr '\\0' '\\n' | sort", None),
    # not cafe and café side by side: FAT's short names make them one file
    ("mkdir u && touch u/caf\u00e9 u/tea && find u -name 'caf?' | sort", None),
    ("mkdir u && touch u/caf\u00e9 u/cafe1 && find u -name 'caf[!e]'", None),
    ("mkdir u && touch u/caf\u00e9 u/tea && find u -name '???' | sort", None),
    ("find nosuch; echo $?", None),
    ("find tree -bogus 2>&1; echo $?", "find: -bogus: not a test or an action (try 'help find')\n1\n"),
    ("cp -r tree t && find t -name '*.txt' -delete && find t | sort", None),
    ("cp -r tree t && find t -delete; ls t 2>/dev/null || echo gone", None),
    # xargs
    ("echo a b c | xargs echo x", None),
    ("printf 'a\\nb\\nc\\n' | xargs -n 2 echo", None),
    ("printf 'one two\\nthree\\n' | xargs -I {} echo '<{}>'", None),
    ("printf \"'quoted arg' plain\\n\" | xargs -n 1 echo", None),
    ("printf 'a\\0b c\\0' | xargs -0 -n 1 echo", None),
    ("true | xargs echo nothing", None),
    ("true | xargs -r echo nothing", None),
    ("echo x | xargs -t echo 2>&1", None),
    ("echo x | xargs nosuchcommand 2>&1; echo $?", "xargs: nosuchcommand: no such command\n127\n"),
    ("echo x | xargs false; echo $?", None),
    ("find tree -name '*.txt' | sort | xargs cat", None),
    ("seq 3000 | xargs echo | wc -l", "3\n"),
    # notes: PocketType's own, laid out as the screen shows it (-p: no colour)
    ("notes -p -w 30 note.md",
     "Title\n" + "\u2014" * 30 + "\n\nSome bold and italic and code,\na link and [pic]. snake_case\n"
     "and 2 * 3 stay, *escaped*.\n\nList\n\n\u2022 one\n\u2022 two that is long enough to\n"
     "  wrap over the line\n  1. nested\n\u2022 [ ] task\n\n| quoted text\n\na  b\n"
     + "\u2014" * 4 + "\n1  2\n\n" + "\u2014" * 30 + "\n\nSetext\n" + "\u2014" * 30 +
     "\n\nhard\nbreak\n\n  code    line\n"),
    # tables: columns if they fit, the last one wrapped, or a card a row
    ("notes -p -w 36 table.md",
     "Anno                          Evento\n" + "\u2014" * 36 + "\n1469     Nasce a Firenze da "
     "famiglia\n                            borghese\n1513-17            Scrive i Discorsi\n"
     "                       e il Principe\n\nFortuna\n  Machiavelli: Pesa per la metà\n"
     "  Guicciardini: Vince lei, ha\n    \"grandissima potestà\"\n\nUomo\n  Machiavelli: "
     "Malvagio\n  extra\n\nChierico al servizio dei papi\n  È al servizio di papi e "
     "cardinali\n  | colti\n\nafter\n"),
    ("notes -p -w 30 table.md | head -3", "1469\n  Nasce a Firenze da famiglia\n  borghese\n"),
    ("notes -p -w 80 table.md | head -4",
     "Anno" + " " * 35 + "Evento\n" + "\u2014" * 45 + "\n"
     "1469     Nasce a Firenze da famiglia borghese\n1513-17" + " " * 21 + "Scrive i Discorsi\n"),
    ("notes -p -w 30 note.txt",
     "plain text line that is long\nenough to wrap around\n    indented line that is long\n"
     "    enough to wrap too\n\nlast\n"),
    ("notes -p note.txt | head -1", "plain text line that is long enough to wrap around\n"),
    ("notes -p 2>&1; echo $?", "usage: notes -p [-w cols] file...\n2\n"),
    ("notes -p -w 5 note.md 2>&1; echo $?", "notes: -w takes 10 to 250 columns\n2\n"),
    ("notes -p nosuch.md 2>&1; echo $?", "notes: nosuch.md: No such file or directory\n1\n"),
    # cal: util-linux's layout, as its cal prints it (Ubuntu's cal is BSD's)
    ("cal -m 9 2026", '   September 2026   \nMo Tu We Th Fr Sa Su\n    1  2  3  4  5  6\n 7  8  9 10 11 12 13\n14 15 16 17 18 19 20\n21 22 23 24 25 26 27\n28 29 30            \n                    \n'),
    ("cal -s 2 2026", '    February 2026   \nSu Mo Tu We Th Fr Sa\n 1  2  3  4  5  6  7\n 8  9 10 11 12 13 14\n15 16 17 18 19 20 21\n22 23 24 25 26 27 28\n                    \n                    \n'),
    ("cal -m 2 2026", '    February 2026   \nMo Tu We Th Fr Sa Su\n                   1\n 2  3  4  5  6  7  8\n 9 10 11 12 13 14 15\n16 17 18 19 20 21 22\n23 24 25 26 27 28   \n                    \n'),
    ("cal -m -3 1 2026", '    December 2025         January 2026          February 2026   \nMo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su  Mo Tu We Th Fr Sa Su\n 1  2  3  4  5  6  7            1  2  3  4                     1\n 8  9 10 11 12 13 14   5  6  7  8  9 10 11   2  3  4  5  6  7  8\n15 16 17 18 19 20 21  12 13 14 15 16 17 18   9 10 11 12 13 14 15\n22 23 24 25 26 27 28  19 20 21 22 23 24 25  16 17 18 19 20 21 22\n29 30 31              26 27 28 29 30 31     23 24 25 26 27 28   \n                                                                \n'),
    ("cal -s -3 12 2026", '    November 2026         December 2026         January 2027    \nSu Mo Tu We Th Fr Sa  Su Mo Tu We Th Fr Sa  Su Mo Tu We Th Fr Sa\n 1  2  3  4  5  6  7         1  2  3  4  5                  1  2\n 8  9 10 11 12 13 14   6  7  8  9 10 11 12   3  4  5  6  7  8  9\n15 16 17 18 19 20 21  13 14 15 16 17 18 19  10 11 12 13 14 15 16\n22 23 24 25 26 27 28  20 21 22 23 24 25 26  17 18 19 20 21 22 23\n29 30                 27 28 29 30 31        24 25 26 27 28 29 30\n                                            31                  \n'),
    ("cal -m 2026", '                               2026                               \n\n       January               February                 March       \nMo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su\n          1  2  3  4                      1                      1\n 5  6  7  8  9 10 11    2  3  4  5  6  7  8    2  3  4  5  6  7  8\n12 13 14 15 16 17 18    9 10 11 12 13 14 15    9 10 11 12 13 14 15\n19 20 21 22 23 24 25   16 17 18 19 20 21 22   16 17 18 19 20 21 22\n26 27 28 29 30 31      23 24 25 26 27 28      23 24 25 26 27 28 29\n                                              30 31               \n        April                   May                   June        \nMo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su\n       1  2  3  4  5                1  2  3    1  2  3  4  5  6  7\n 6  7  8  9 10 11 12    4  5  6  7  8  9 10    8  9 10 11 12 13 14\n13 14 15 16 17 18 19   11 12 13 14 15 16 17   15 16 17 18 19 20 21\n20 21 22 23 24 25 26   18 19 20 21 22 23 24   22 23 24 25 26 27 28\n27 28 29 30            25 26 27 28 29 30 31   29 30               \n                                                                  \n        July                  August                September     \nMo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su\n       1  2  3  4  5                   1  2       1  2  3  4  5  6\n 6  7  8  9 10 11 12    3  4  5  6  7  8  9    7  8  9 10 11 12 13\n13 14 15 16 17 18 19   10 11 12 13 14 15 16   14 15 16 17 18 19 20\n20 21 22 23 24 25 26   17 18 19 20 21 22 23   21 22 23 24 25 26 27\n27 28 29 30 31         24 25 26 27 28 29 30   28 29 30            \n                       31                                         \n       October               November               December      \nMo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su   Mo Tu We Th Fr Sa Su\n          1  2  3  4                      1       1  2  3  4  5  6\n 5  6  7  8  9 10 11    2  3  4  5  6  7  8    7  8  9 10 11 12 13\n12 13 14 15 16 17 18    9 10 11 12 13 14 15   14 15 16 17 18 19 20\n19 20 21 22 23 24 25   16 17 18 19 20 21 22   21 22 23 24 25 26 27\n26 27 28 29 30 31      23 24 25 26 27 28 29   28 29 30 31         \n                       30                                         \n'),
    ("cal -s 2024", '                               2024                               \n\n       January               February                 March       \nSu Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa\n    1  2  3  4  5  6                1  2  3                   1  2\n 7  8  9 10 11 12 13    4  5  6  7  8  9 10    3  4  5  6  7  8  9\n14 15 16 17 18 19 20   11 12 13 14 15 16 17   10 11 12 13 14 15 16\n21 22 23 24 25 26 27   18 19 20 21 22 23 24   17 18 19 20 21 22 23\n28 29 30 31            25 26 27 28 29         24 25 26 27 28 29 30\n                                              31                  \n        April                   May                   June        \nSu Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa\n    1  2  3  4  5  6             1  2  3  4                      1\n 7  8  9 10 11 12 13    5  6  7  8  9 10 11    2  3  4  5  6  7  8\n14 15 16 17 18 19 20   12 13 14 15 16 17 18    9 10 11 12 13 14 15\n21 22 23 24 25 26 27   19 20 21 22 23 24 25   16 17 18 19 20 21 22\n28 29 30               26 27 28 29 30 31      23 24 25 26 27 28 29\n                                              30                  \n        July                  August                September     \nSu Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa\n    1  2  3  4  5  6                1  2  3    1  2  3  4  5  6  7\n 7  8  9 10 11 12 13    4  5  6  7  8  9 10    8  9 10 11 12 13 14\n14 15 16 17 18 19 20   11 12 13 14 15 16 17   15 16 17 18 19 20 21\n21 22 23 24 25 26 27   18 19 20 21 22 23 24   22 23 24 25 26 27 28\n28 29 30 31            25 26 27 28 29 30 31   29 30               \n                                                                  \n       October               November               December      \nSu Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa\n       1  2  3  4  5                   1  2    1  2  3  4  5  6  7\n 6  7  8  9 10 11 12    3  4  5  6  7  8  9    8  9 10 11 12 13 14\n13 14 15 16 17 18 19   10 11 12 13 14 15 16   15 16 17 18 19 20 21\n20 21 22 23 24 25 26   17 18 19 20 21 22 23   22 23 24 25 26 27 28\n27 28 29 30 31         24 25 26 27 28 29 30   29 30 31            \n                                                                  \n'),
    ("cal 13 2026 2>&1; echo $?", "cal: 13: not a month (1-12)\n1\n"),
    # todo: our own; -d fixes "today". (The harness reads @ in an answer as
    # the directory, so the due dates go through tr.)
    # calc has no GNU namesake: what it should say
    ("calc 2+3", "5\n"),
    ("calc '2^10/4'", "256\n"),
    ("calc '2^10/3'", "341.3333333\n"),
    ("calc '-2^2'", "-4\n"),
    ("calc '2^3^2'", "512\n"),
    ("calc '(1+2)*3'", "9\n"),
    ("calc 7 % 3", "1\n"),
    ("calc 50%", "0.5\n"),
    ("calc '200*15%'", "30\n"),
    ("calc '5!'", "120\n"),
    ("calc 'sqrt(2)'", "1.414213562\n"),
    ("calc 'sin(pi)'", "0\n"),
    ("calc -d 'sin(30)'", "0.5\n"),
    ("calc -d 'atan(1)'", "45\n"),
    ("calc 0x1F + 0b101", "36\n"),
    ("calc 'max(3, 8, 2)'", "8\n"),
    ("calc 'min(3, 8, 2)'", "2\n"),
    ("calc 'root(-27, 3)'", "-3\n"),
    ("calc 'gcd(12, 18)'", "6\n"),
    ("calc 0.1+0.2", "0.3\n"),
    ("calc 1e20", "1e+20\n"),
    ("calc 1/0 2>&1; echo $?", "calc: 1/0: division by zero\n1\n"),
    ("calc '2+' 2>&1; echo $?", "calc: 2+: a number is missing at the end\n1\n"),
    ("calc '(1+2' 2>&1; echo $?", "calc: (1+2: a ) is missing\n1\n"),
    ("calc '1+2)' 2>&1; echo $?", "calc: 1+2): a ( is missing\n1\n"),
    ("calc 'foo(2)' 2>&1; echo $?", "calc: foo(2): no such function\n1\n"),
    ("calc 'sqrt(-1)' 2>&1; echo $?", "calc: sqrt(-1): outside what the function takes\n1\n"),
    ("printf '2*3\\nans+1\\nx = 4\\nx^2\\n' | calc", "6\n7\n4\n16\n"),
    ("printf 'deg\\ncos(60)\\nrad\\ncos(0)\\n' | calc", "0.5\n1\n"),
    ("todo -f todo.md -d 2026-09-24 ls",
     " 1  [ ] consegnare ricerca                                      late: Tue 22 Sep\n"
     " 2  [ ] studiare storia                                                 tomorrow\n"
     " 3  [ ] senza data\n 4  [x] fatto\n"),
    ("todo -f todo.md -d 2026-09-24 add ripassare latino @fri; tr @ % < todo.md",
     "3  [ ] ripassare latino\n# To do\n\nsome notes, left alone\n- [ ] studiare storia %2026-09-25\n"
     "- [x] fatto\n- [ ] consegnare ricerca %2026-09-22\n* [ ] senza data\n"
     "- [ ] ripassare latino %2026-09-25\n"),
    ("todo -f todo.md -d 2026-09-24 done 1 3 >/dev/null; todo -f todo.md clear; tr @ % < todo.md",
     "3 done things cleared\n# To do\n\nsome notes, left alone\n- [ ] studiare storia %2026-09-25\n"),
    ("todo -f todo.md add x @someday 2>&1 | tr @ %; echo $?",
     "todo: %someday: not a day (today, fri, 30/9, 2026-09-30, +3)\n0\n"),
    ("todo -f new.md add primo; todo -f new.md ls; cat new.md",
     "1  [ ] primo\n 1  [ ] primo\n# To do\n\n- [ ] primo\n"),
    # calendar: the diary, with what is due from ~/todo.md
    ("HOME=$PWD calendar -f diary.txt -d 2026-09-24 agenda 10",
     "Thu 24 Sep, today\n  10:00-11:00  Lab informatica\n  late, to do  consegnare ricerca\n"
     "Fri 25 Sep, tomorrow\n  09:30        verifica di storia (!)\n  to do        studiare storia\n"
     "Mon 28 Sep\n  15:00-16:30  Calcio\nWed 30 Sep\n  15:00-16:30  Calcio\n"
     "Fri 2 Oct\n  all day      Compleanno di Marco (!)\n"),
    ("HOME=$PWD calendar -f diary.txt -d 2026-09-24 add 30/9 9:00 dentista !30; tail -1 diary.txt",
     "2026-09-30 09:00 dentista !30\n2026-09-30 09:00 dentista !30\n"),
    ("HOME=$PWD calendar -f diary.txt -d 2026-09-24 rm 30/9; echo $?",
     "removed: weekly mo,we 15:00-16:30 Calcio\n0\n"),
    ("HOME=$PWD calendar -f diary.txt -d 2026-09-24 rm 1/9 2>&1; echo $?",
     "calendar: no such event\n1\n"),
    ("HOME=$PWD calendar -f diary.txt add someday x 2>&1; echo $?",
     "usage: calendar add DAY|every DAYS|yearly DAY [TIME[-END]] what [!N]\n"
     "  DAY: today, tomorrow, fri, 30/9, 2026-09-30   DAYS: mo, mo,we, mo-fr\n2\n"),
    ("seq 3000 | xargs echo | wc -w", None),
]


def build(tmp):
    exe = os.path.join(tmp, "host_pt")
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", os.path.join(ROOT, "kernel", "include"), "-I", os.path.join(ROOT, "bin"),
                    "-o", exe, *SOURCES, "-lm"], check=True)
    bindir = os.path.join(tmp, "bin")
    os.mkdir(bindir)
    for p in PROGRAMS:
        os.symlink(exe, os.path.join(bindir, p))
    return bindir


def fixtures(where):
    os.makedirs(where)
    for name, text in FILES.items():
        path = os.path.join(where, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    for d in DIRS:
        os.makedirs(os.path.join(where, d))


def run(cmd, work, path):
    env = dict(os.environ, LC_ALL="C.UTF-8", PATH=path,
               ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
    # the shell's own echo, printf, true and false would hide the programs
    script = "enable -n echo printf true false test; " + cmd
    p = subprocess.run(["bash", "-c", script], cwd=work, env=env, capture_output=True,
                       timeout=20)
    return p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"), p.returncode


def cases(tmp, bindir):
    failed = 0
    for i, (cmd, want) in enumerate(CASES):
        ours_dir, gnu_dir = os.path.join(tmp, f"o{i}"), os.path.join(tmp, f"g{i}")
        fixtures(ours_dir)
        out, err, rc = run(cmd, ours_dir, bindir + ":" + os.environ["PATH"])
        if want is None:
            fixtures(gnu_dir)
            gout, _, grc = run(cmd, gnu_dir, os.environ["PATH"])
            want, want_rc = gout, grc
            got = out
        else:
            # given by hand (2>&1 where the errors matter); @ is the directory
            want, want_rc = want.replace("@", ours_dir), None
            got = out
        bad = "runtime error" in err or "AddressSanitizer" in err
        if got != want or (want_rc is not None and rc != want_rc) or bad:
            failed += 1
            print(f"FAIL: {cmd}")
            print(f"  want ({want_rc}): {want!r}"[:600])
            print(f"  got  ({rc}): {got!r}"[:600])
            if err:
                print("  stderr: " + err.strip().replace("\n", "\n          ")[:2000])
        elif VERBOSE:
            print(f"ok: {cmd}")
    return failed


# ------------------------------------------------------------ random patterns

def random_pattern(rnd, ere):
    atoms = ["a", "b", "c", ".", "[ab]", "[^a]", "[a-c]", "x", "\\.", "[[:alpha:]]", " ",
             "\\w", "[]a]", "[^]b]", "[a-]"]
    # \< \> and \b only outside groups: repeated, GNU's regex gets them
    # wrong -- ([a-c]*\b *){2,3}[ab] matches all of "bab" there, and the
    # same written out twice does not
    anchors = ["\\<", "\\>", "\\b"]

    def piece(depth):
        r = rnd.random()
        if depth < 3 and r < 0.2:
            inner = alt(depth + 1)
            return f"({inner})" if ere else f"\\({inner}\\)"
        return rnd.choice(atoms)

    def quant(p):
        r = rnd.random()
        if r < 0.15:
            return p + "*"
        if r < 0.25:
            return p + ("+" if ere else "\\+")
        if r < 0.32:
            return p + ("?" if ere else "\\?")
        if r < 0.40:
            lo = rnd.randint(0, 3)
            hi = rnd.choice(["", str(lo + rnd.randint(0, 2))])
            body = f"{lo},{hi}" if rnd.random() < 0.6 else str(lo)
            return p + (f"{{{body}}}" if ere else f"\\{{{body}\\}}")
        return p

    def concat(depth):
        return "".join(rnd.choice(anchors) if not depth and rnd.random() < 0.1 else quant(piece(depth))
                       for _ in range(rnd.randint(1, 4)))

    def alt(depth):
        parts = [concat(depth) for _ in range(rnd.randint(1, 2 if depth else 3))]
        return ("|" if ere else "\\|").join(parts)

    p = alt(0)
    if rnd.random() < 0.15:
        p = "^" + p
    if rnd.random() < 0.15:
        p = p + "$"
    return p


def fuzz(tmp, bindir, rounds=400, seed=1234):
    rnd = random.Random(seed)
    lines = ["".join(rnd.choice("abcxAB. ") for _ in range(rnd.randint(0, 14))) for _ in range(40)]
    text = os.path.join(tmp, "fuzz.txt")
    with open(text, "w") as f:
        f.write("\n".join(lines) + "\n")
    ours = os.path.join(bindir, "grep")
    failed = 0
    # UTF-8, as ours is: GNU's -w takes a different path in the C locale
    # and gives different answers for patterns that can match nothing
    env = dict(os.environ, LC_ALL="C.UTF-8", ASAN_OPTIONS="detect_leaks=0")
    for _ in range(rounds):
        ere = rnd.random() < 0.5
        pat = random_pattern(rnd, ere)
        for opts in (["-o"], ["-c"], ["-x", "-c"], ["-i", "-o"], ["-w", "-n"], ["-v", "-c"]):
            args = (["-E"] if ere else []) + opts + ["-e", pat, text]
            g = subprocess.run(["grep", *args], capture_output=True, env=env)
            o = subprocess.run([ours, *args], capture_output=True, env=env)
            if g.returncode > 1:
                break			# GNU refuses it too: nothing to compare
            if (o.stdout, o.returncode) != (g.stdout, g.returncode) or o.stderr:
                failed += 1
                print(f"FAIL: grep {' '.join(args[:-1])}")
                print(f"  GNU  ({g.returncode}): {g.stdout[:300]!r}")
                print(f"  ours ({o.returncode}): {o.stdout[:300]!r} {o.stderr[:300]!r}")
                break
    return failed


# ------------------------------------------------------------ on the board

def parse_blocks(raw):
    """What each case printed, from the script's output: (stdout, stderr, status)."""
    results = []
    for block in raw.split("@@END\n"):
        head, _, err = block[block.find("@@RC "):].partition("\n@@ERR\n")
        rc_line, _, out = head.partition("\n")
        rc = int(rc_line[5:]) if rc_line[5:].isdigit() else None
        results.append((out, err[:-1], rc))
    return results


def case_script(cmds):
    """Each command in a fresh copy of the fixtures, its output fenced off."""
    script = []
    for cmd in cmds:
        script += ["cd /tmp/pt; rm -rf c; cp -r f c; cd c", "{ " + cmd, "} >/tmp/pt/o 2>/tmp/pt/e",
                   'echo "@@RC $?"', "cat /tmp/pt/o; echo; echo @@ERR",
                   "cat /tmp/pt/e; echo; echo @@END"]
    return "\n".join(script) + "\n"


def on_board(port):
    """The same cases on a real board: the fixtures pushed over, every case
    run by one script, and what each printed compared with GNU's here.

    The USB console can drop a byte now and then under a long burst of
    output, which spoils one case's comparison without anything being
    wrong on the board; so a case that disagrees is run again on its own,
    and only one that disagrees twice counts."""
    sys.path.insert(0, HERE)
    from board import Board, clean
    import xfer

    tmp = tempfile.mkdtemp(prefix="programs_test.")
    wants = []
    for i, (cmd, want) in enumerate(CASES):
        if want is None:
            gnu = os.path.join(tmp, f"g{i}")
            fixtures(gnu)
            want, _, want_rc = run(cmd, gnu, os.environ["PATH"])
        else:
            want, want_rc = want.replace("@", "/tmp/pt/c"), None
        wants.append((want, want_rc))

    def agrees(i, result):
        out, _, rc = result
        want, want_rc = wants[i]
        return out == want and (want_rc is None or rc == want_rc)

    def push(local, remote):
        for attempt in range(3):
            if xfer.push(b, local, remote, os.path.basename(remote)):
                return True
            b.send(b"\x15\r")			# a fresh prompt, and again
            b.read_until(xfer.PROMPT, 10)
        return False

    failed = []
    b = Board(port)
    try:
        fixtures(os.path.join(tmp, "f"))
        b.send(b"\x15\r")
        b.read_until(xfer.PROMPT, 10)
        dirs = sorted({"/tmp/pt/f/" + os.path.dirname(n) for n in FILES if "/" in n} |
                      {"/tmp/pt/f/" + d for d in DIRS})
        b.run("rm -rf /tmp/pt; mkdir -p " + " ".join(dirs))
        with open(os.path.join(tmp, "cases.sh"), "w") as f:
            f.write(case_script(cmd for cmd, _ in CASES))
        for name in FILES:
            if not push(os.path.join(tmp, "f", name), "/tmp/pt/f/" + name):
                return 1
        if not push(os.path.join(tmp, "cases.sh"), "/tmp/pt/cases.sh"):
            return 1
        b.send(b"sh /tmp/pt/cases.sh; echo @@DONE\r")
        results = parse_blocks(clean(b.read_until(rb"@@DONE\r?\n", 900)))
        for i, (cmd, _) in enumerate(CASES):
            result = results[i] if i < len(results) else ("", "", None)
            if agrees(i, result):
                if VERBOSE:
                    print(f"ok: {cmd}")
                continue
            # once more, alone
            with open(os.path.join(tmp, "again.sh"), "w") as f:
                f.write(case_script([cmd]))
            again = None
            if push(os.path.join(tmp, "again.sh"), "/tmp/pt/again.sh"):
                b.send(b"sh /tmp/pt/again.sh; echo @@DONE\r")
                again = parse_blocks(clean(b.read_until(rb"@@DONE\r?\n", 60)))[0]
            if again and agrees(i, again):
                print(f"(once garbled on the console, fine run again: {cmd})")
                continue
            failed.append((cmd, again or result))
        b.run("rm -rf /tmp/pt; cd")
    finally:
        b.close()

    for cmd, (out, err, rc) in failed:
        want, want_rc = wants[[c for c, _ in CASES].index(cmd)]
        print(f"FAIL: {cmd}")
        print(f"  want ({want_rc}): {want!r}"[:600])
        print(f"  got  ({rc}): {out!r}"[:600])
        if err.strip():
            print("  stderr: " + err.strip()[:600])
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"programs on the board: {len(CASES) - len(failed)} of {len(CASES)} cases agree with GNU")
    return 1 if failed else 0


def main():
    if "--board" in sys.argv:
        return on_board(sys.argv[sys.argv.index("--board") + 1])
    tmp = tempfile.mkdtemp(prefix="programs_test.")
    try:
        bindir = build(tmp)
        failed = cases(tmp, bindir)
        print(f"programs: {len(CASES) - failed} of {len(CASES)} cases agree with GNU")
        fuzzed = fuzz(tmp, bindir)
        print(f"regex: {fuzzed} of 400 random patterns disagree with GNU grep")
        return 1 if failed or fuzzed else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
