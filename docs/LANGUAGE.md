# pico — the PocketType language

pico is a small statically typed language with C-like syntax. You write
`.pico` files, `picoc` compiles them to a compact bytecode executable, and a
virtual machine in the kernel runs it. The same compiler and VM build as PC
tools, so you can write and test on the laptop and run the result unchanged
on the board.

Memory is automatic (reference counting, no pointers). Array and string
accesses are bounds-checked, null is checked, and a mistake stops the
program with the source line, not the board.

```
$ picoc hello.pico          # writes ./hello
$ ./hello world        # the kernel finds the loader by the file's first bytes
$ pico hello.pico world     # compile in memory and run in one step
```

## Your first program

```c
// hello.pico
int main(str[] args) {
    str name = len(args) > 1 ? args[1] : "world";
    println("hello, ", name);
    for (int i = 1; i <= 3; i++) {
        printf("%d squared is %d\n", i, i * i);
    }
    return 0;
}
```

On the board:

```
$ pico hello.pico andre
hello, andre
1 squared is 1
...
```

On the PC, build the tools once:

```
$ make -C lang            # builds lang/build/host/picoc and lang/build/host/pico
$ lang/build/host/pico hello.pico andre
$ lang/build/host/picoc -o hello hello.pico      # the same executable the board runs
```

`make -C lang test` runs the test suite, `make -C lang bench` the timings.

`args[0]` is the program name, so `len(args) - 1` is the argument count. The
value `main` returns is the exit status; `void main()` and `int main()`
without arguments work too.

New to it? [PICO-TUTORIAL.md](PICO-TUTORIAL.md) teaches it from the start.

## Types

| type | what it is |
| --- | --- |
| `int` | 32-bit signed, wraps around on overflow |
| `float` | 32-bit IEEE single precision, the S3's FPU |
| `bool` | `true` or `false`, not a number |
| `str` | immutable UTF-8 text, any length |
| `T[]` | growable array of `T`: `int[]`, `str[]`, `Point[]`, `int[][]` |
| `struct` | named fields, declared at the top level |
| `enum` | named `int` values, declared at the top level |
| `File` | an open file (see the file built-ins) |
| `void` | the return type of a function that returns nothing |

`int` is 32 bits because the CPU is: the Xtensa LX7 is a 32-bit core, 64-bit
multiply and divide would become library calls, and one value fits in one
4-byte slot next to floats and references. If you need to count past two
billion, use `float` or split the value.

`str`, arrays, structs and `File` are **references**. Assigning one does not
copy it:

```c
int[] a = [1, 2];
int[] b = a;
push(b, 3);
println(a);        // [1, 2, 3] — a and b are the same array
```

Strings are immutable, so sharing them is invisible; `s + "x"` makes a new
string. `==` compares strings by content, arrays and structs by identity
(`a == b` is true only if they are the same object).

### null and default values

A variable you declare without a value gets: `0` for `int`, `0.0` for
`float`, `false` for `bool`, `""` for `str`, and `null` for arrays, structs
and `File`. Struct fields work the same way. So:

```c
struct Bag { str name; int[] items; }
Bag b = Bag{name: "tools"};   // b.items is null
push(b.items, 1);             // runtime error: push() to a null array
Bag c = Bag{name: "tools", items: []};   // this is what you want
```

Only arrays, structs and `File` can be null; `str` never is.

### Conversions

Numbers do not convert silently except `int` to `float`, which happens
wherever a float is expected (`float f = 1;`, `1 + 0.5`). Everything else is
explicit:

```c
int(2.9)        // 2, truncates toward zero, saturates at INT_MAX/INT_MIN
float(3)        // 3.0
str(42)         // "42", works for any value, including arrays and structs
to_int("42")    // parses, 0 (or a given fallback) if it is not a number
to_float("2.5")
```

## Declarations

```c
const int WIDTH = 40;          // global constant
str greeting = "hi";           // global variable, set up before main runs
struct Point { int x; int y; } // struct, usable before its declaration
enum Dir { UP, DOWN, LEFT = 10, RIGHT }   // UP 0, DOWN 1, LEFT 10, RIGHT 11

int dist2(Point p) { return p.x * p.x + p.y * p.y; }

int main() {
    int a = 1, b = 2;          // several at once
    const float RATIO = 0.5;   // local constant
    Point p = Point{x: 3, y: 4};    // struct literal, missing fields get defaults
    int[] xs = [1, 2, 3];      // array literal
    str[][] grid = [];         // empty array, type from the declaration
    return dist2(p);
}
```

An enum's values are `int` constants, each one more than the one before
unless it says (`= 10`); its name, `Dir` here, is a type that means `int`
(`Dir d = LEFT;`). The name can be left out: `enum { A, B }`.

Functions can be called before they are declared. A function that returns a
value must return on every path; the compiler says so if it can fall off the
end.

## Statements

```c
if (cond) { ... } else if (other) { ... } else { ... }
while (cond) { ... }
for (int i = 0; i < n; i++) { ... }     // any part may be empty: for (;;)
for (str s in names) { ... }            // each item of an array, in order
switch (x) { case 1, 2: ... case 3: ... default: ... }
break; continue; return; return value;
{ ... }                                  // a block has its own scope
x = 1;  x += 2;  x++;  ++x;  a[i] *= 3;  p.x -= 1;   // assignments
f(x);                                    // a call on its own
```

`for (T x in a)` runs once for each item of the array `a`, `x` a new
variable each time; the length is looked at every time round, so an array
that grows in the loop is gone through to its end. For a string's
characters, `for (str ch in chars(s))`.

`switch` compares its value with each case's values in order and runs the
first case that matches, then goes on after the switch: there is no
falling through. A case can list several values (`case 'q', KEY_ESC:`),
and they can be any expressions of the switch's type: `int`, `str`,
`float`, `bool` or an enum. `default`, if there is one, goes last. Each
case is a scope of its own, so it can declare variables; `break` leaves
the switch, and `continue` goes on with the loop around it.

Conditions must be `bool`: `if (n)` is an error, write `if (n != 0)`.
Assignments are statements, not expressions, so `if (x = 1)` cannot happen.
A one-statement body may skip the braces, but not for a declaration.

## Operators

From loosest to tightest:

| operators | notes |
| --- | --- |
| `? :` | `cond ? a : b`, both sides the same type |
| `\|\|` `&&` | `bool` only, short-circuit |
| `\|` `^` `&` | `int` or `bool` |
| `==` `!=` | numbers, `bool`, `str` (content), references (identity), `null` |
| `<` `<=` `>` `>=` | numbers and `str` (byte order) |
| `<<` `>>` | `int`, the shift count is taken modulo 32, `>>` keeps the sign |
| `+` `-` | `+` also joins strings: `"n=" + str(n)`, or `"n=" + n` |
| `*` `/` `%` | `/` and `%` by zero is a runtime error; `%` on floats is `fmod` |
| `-x` `!x` `~x` | |
| `f(x)` `a[i]` `p.field` | `s[i]` gives the byte at `i` as an `int` |

Integer overflow wraps (two's complement). `INT_MIN / -1` is `INT_MIN`.

## Built-in functions

Types in the signatures: `T` is any type, `N` is `int` or `float`.
Arguments in brackets may be left out.

### Printing

| | |
| --- | --- |
| `printf(str fmt, ...)` | C-style formatting, checked at compile time when `fmt` is a literal |
| `format(str fmt, ...) -> str` | the same, as a string |
| `print(...)`, `println(...)` | print any values one after another, `println` adds a newline |

Conversions: `%d %i` int, `%u %x %X %o` int as unsigned, `%c` int as a
character (UTF-8), `%f %e %E %g %G` float (an int is accepted), `%s` any
value, `%%` a percent sign. Flags `-+ 0#`, a width and a `.precision` work as
in C; `*` does not.

`print` and `println` write arrays and structs the way the language writes
them: `[1, 2, 3]`, `Point{x: 3, y: 4}`, with strings inside quoted.

### Strings

| | |
| --- | --- |
| `len(str s) -> int` | length in bytes |
| `substr(str s, int start[, int count]) -> str` | to the end without `count` |
| `find(str s, str part[, int from]) -> int` | index, or -1 |
| `split(str s[, str sep]) -> str[]` | without `sep`, splits on runs of whitespace |
| `join(str[] parts, str sep) -> str` | |
| `trim(str s) -> str` | strips whitespace at both ends |
| `upper(str s)`, `lower(str s) -> str` | ASCII only |
| `replace(str s, str old, str new) -> str` | every occurrence |
| `starts_with(str s, str p)`, `ends_with(str s, str p) -> bool` | |
| `contains(str s, str part) -> bool` | |
| `repeat(str s, int times) -> str` | `repeat("-", 20)` for a rule |
| `chars(str s) -> str[]` | the characters, each a whole UTF-8 sequence |
| `chr(int code) -> str` | a code point as UTF-8 |
| `ord(str s) -> int` | the first code point |
| `to_int(str s[, int fallback]) -> int` | accepts `-12`, `0x1f`, surrounding spaces |
| `to_float(str s[, float fallback]) -> float` | |
| `str(any)`, `int(N or bool)`, `float(N)` | conversions |

`s[i]` is the byte at `i` (an `int`), bounds-checked. `len` counts bytes, so
non-ASCII characters count more than one.

### Arrays

| | |
| --- | --- |
| `len(T[] a) -> int` | |
| `push(T[] a, T v)` | append |
| `pop(T[] a) -> T` | remove and return the last item |
| `insert(T[] a, int i, T v)` | |
| `remove_at(T[] a, int i) -> T` | |
| `resize(T[] a, int n)` | grow with default values, or cut |
| `slice(T[] a, int start, int end) -> T[]` | a new array with copies of the references |
| `sort(T[] a)` | in place, for `int[]`, `float[]` and `str[]` |
| `reverse(T[] a)` | in place, for any array |
| `find(T[] a, T v) -> int` | index, or -1 |

### Math

`abs(N) -> N`, `min(N, N) -> N`, `max(N, N) -> N`, `sqrt(float) -> float`,
`pow(float, float) -> float`, `sin(float)`, `cos(float)`, `tan(float)`,
`atan2(float, float)`, `hypot(float, float)`, `exp(float)`,
`ln(float)` (the natural logarithm -- not `log`, because programs call
their own things that), `floor(float) -> int`, `ceil(float) -> int`,
`round(float) -> int`, `random(int n) -> int` (0 to n-1),
`seed(int)`. `PI`, `INT_MAX` and `INT_MIN` are constants.

### Checking things

`assert(bool cond[, str message])` stops the program where the mistake
is, with the file and line, rather than letting it carry on with
something impossible:

```
assert(len(rows) > 0, "the file had no rows");
```

### Files

| | |
| --- | --- |
| `open(str path, str mode) -> File` | mode `"r"`, `"w"` or `"a"`; `null` if it fails |
| `close(File f)` | also happens when the last reference goes away |
| `read(File f[, int n]) -> str` | the rest of the file, or at most `n` bytes |
| `readline(File f) -> str` | with the newline; `""` at the end of the file |
| `write(File f, str s) -> bool` | |
| `exists(str path)`, `is_dir(str path) -> bool` | |
| `remove(str path)`, `mkdir(str path)`, `rename(str from, str to) -> bool` | |
| `listdir(str path) -> str[]` | names only, sorted, without `.` and `..` |

`stdin`, `stdout` and `stderr` are `File` constants:
`write(stderr, "oops\n")`, `readline(stdin)`.

### Processes, time, terminal

| | |
| --- | --- |
| `run(str cmd, ...) -> int` | run a command, wait, return its status (-1 if it cannot start) |
| `run(str[] argv) -> int` | the same with the arguments in an array |
| `output(str cmd, ...) -> str` | run a command and return what it printed (at most 1 MB); `""` if it cannot start. Also `output(str[] argv)` |
| `exit(int status)` | stop now |
| `getenv(str name) -> str` | `""` when unset |
| `sleep_ms(int ms)` | |
| `uptime_ms() -> int` | milliseconds since boot (wraps after 24 days) |
| `time() -> int` | seconds since 1970 |
| `date() -> str` | local time as `YYYY-MM-DD HH:MM:SS` |
| `raw_mode(bool on)` | no echo, keys arrive immediately |
| `readkey([int timeout_ms]) -> int` | a byte, a `KEY_*` value, or `KEY_NONE` after the timeout |
| `term_cols()`, `term_rows() -> int` | 80x24 when the output is not a terminal |

### The internet

| | |
| --- | --- |
| `http_get(str url) -> str` | what a web address answers (`curl -sL`, so https and redirects work); `""` if it fails |
| `json_get(str json, str path) -> str` | the value at `path` in a JSON text |

A `json_get` path is names and array indexes joined by dots:
`"main.temp"`, `"list.0.name"`. The value comes back as text: a string
without its quotes and escapes (`\u00e9` becomes `é`), a number or `true`,
`false`, `null` as written, an object or array as its JSON, to look into
again. `#` as the last step is how many items or members there are. A path
that leads nowhere gives `""`.

```c
str j = http_get("https://api.open-meteo.com/v1/forecast?latitude=45.46" +
                 "&longitude=9.19&current=temperature_2m");
println(json_get(j, "current.temperature_2m"), " C");
```

### Keys

Key constants: `KEY_UP KEY_DOWN KEY_LEFT KEY_RIGHT KEY_HOME KEY_END KEY_PGUP
KEY_PGDN KEY_INSERT KEY_DELETE KEY_ESC KEY_F1..KEY_F4 KEY_NONE KEY_EOF`.
Ctrl-key presses arrive as their control byte (Ctrl-C is 3 in raw mode).

Colors are escape sequences in ordinary strings; `\e` is escape:

```c
println("\e[1mbold\e[0m \e[31mred\e[0m \e[32mgreen\e[0m \e[2mdim\e[0m");
print("\e[2J\e[H");        // clear the screen, cursor home
print("\e[?25l");          // hide the cursor, \e[?25h shows it
printf("\e[%d;%dH", row, col);   // move the cursor (1-based)
```

## Memory

Every string, array, struct and `File` is freed as soon as the last
reference to it goes away — no pause, no garbage collector. Reference
*cycles* (a struct that points to itself, directly or through others) are
never freed; the memory comes back when the program exits. The host tools
report cycles when you run them with `PICO_LEAKCHECK=1`, which the test suite
does.

## Errors

Compile errors name the file, line and column, show the line and stop after
five:

```
hello.pico:4:13: error: expected int, got str; parse it with to_int(s)
    int n = "12";
            ^
```

Runtime errors name the line and the call chain, and the program exits with
status 1:

```
hello.pico:6: runtime error: index 9 out of range (length 3)
    in at (hello.pico:2)
    in main (hello.pico:6)
```

What is checked at runtime: array and string indexes, null arrays, structs
and files, division by zero, `pop` of an empty array, bad arguments to the
built-ins, the call depth limit (10000 frames), and running out of memory.
Ctrl-C stops a running program at the next backward jump or call.

Exit statuses: what `main` returns (or `exit(n)`), `1` after a compile or
runtime error, `2` for wrong command usage, `130` after Ctrl-C.

## Limits

| | |
| --- | --- |
| locals per function | 250 (parameters included) |
| fields per struct | 255 |
| nesting depth in one statement | 24 |
| code per function | 32 KB |
| strings, functions, globals, structs per program | 65535 |
| call depth | 10000 frames |
| array length | 67 million items |

Compiling a 700-line program uses about 80 KB of heap and under 12 KB of
stack, so it fits on the device with room to spare.

## The executable format

`picoc` writes a file that starts with the bytes `7f 41 4c` (`\x7fAL`) and a
format version, which is how the kernel picks the loader. Everything is
little-endian:

```
header  32 bytes: magic, version, then u16 counts of strings, structs,
        globals, functions, the index of main, of the global initialiser,
        of the source file name, a reserved word, then u32 counts of
        fields, line entries and code bytes
strings u32 length, then the bytes
structs u16 name, u16 field count
fields  u16 name, u16 type descriptor
globals u8 kind
funcs   u32 code offset, length, first line entry, line count,
        u16 name, u16 stack needed, u8 parameters, locals, returns, pad
lines   u16 code offset, u16 source line
code    the instructions
crc     u32 CRC-32 of everything before it
```

The loader checks the CRC first, so a file damaged in transfer or on the card
is refused. Then it checks every count, offset, instruction and jump target,
and walks each function to check that the operand stack is used consistently
and never deeper than the function claims. It cannot check that values have
the *types* the instructions expect: an executable edited by hand, with its
CRC fixed up, can still confuse the VM. Compile from source if you are not
sure where a file came from.

Format version 2 added the CRC. A program compiled by an older `picoc` is
refused with "compile it again with picoc".

`picoc -t program` makes all those checks on a compiled program without
running it: it prints nothing and exits 0 if the program passes, and
otherwise prints what the loader would and exits 1; source is refused, as
not a compiled program. The pico-os-packages build runs it on every program
it publishes, and `pkg` on every one it installs.

`picoc -d file.pico` prints the instructions, which is the quickest way to see
what the compiler did with a piece of code.

## How it is built

Everything is in `lang/`, one ESP-IDF component and one plain `make`
project:

| file | |
| --- | --- |
| `lex.c` | source to tokens |
| `compile.c` | four passes over the tokens, straight to bytecode, no syntax tree |
| `image.c` | instruction table, loader, verifier, disassembler |
| `quicken.c` | fuses common instruction sequences once a program is loaded |
| `vm.c` | the interpreter loop |
| `runtime.c` | memory pool, objects, strings, formatting, errors |
| `builtins.c` | the built-in functions |
| `driver.c` | the `picoc` and `pico` commands |
| `port_pt.c`, `port_host.c` | the only files that know about the platform |

### Speed

Dispatching an instruction (fetch the opcode, find its handler, jump) is
most of what an interpreter spends on the ESP32-S3, so after a program is
verified `quicken.c` rewrites the first opcode of common sequences into
fused instructions that do the whole sequence at once: a for loop's step and
test, a compare and branch on locals, `a = b + c` on locals. Only that one
byte changes, so everything else, jumps included, stays valid. A counted loop
runs about twice as fast for it; set `PICO_NOQUICKEN=1` to run without, which
the tests do to check that both behave the same.

Rough costs on the board at 240 MHz, per loop iteration: an empty loop
150 ns, `s += i` 230 ns, `s += a[i]` 490 ns, a call to a small function
0.6 to 1.3 us, `"x" + str(i)` 4 us. `sin` and `cos`
cost about 1.4 us within a few turns and ten times that past about 200
radians, where the C library switches to a slow method; keep angles small.

The core never calls a `pt_` function directly and keeps no mutable global
state, because on PocketType two programs can run at once in one address
space.

Examples are in `lang/examples/`: `hello.pico`, `wc.pico`, `guess.pico`,
`todo.pico`, `life.pico` (the terminal, raw keys, colors), `mandel.pico` (an
animated Mandelbrot zoom), `matrix.pico` (digital rain), `snake.pico` (the
game: enum, switch, for-in), `weather.pico` (http_get and json_get), and
`bench.pico` and `benchmark.pico` (timings, with `--help`). Tests are `lang/tests/*.pico` with their expected output next
to them; `make -C lang test` runs them twice, once compiled in memory and
once through a compiled file, `make -C lang asan` does the same under
AddressSanitizer, and `make -C lang bench` times the benchmarks on the PC.
