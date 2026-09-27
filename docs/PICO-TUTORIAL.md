# Learning pico

pico is the language PocketType speaks: small, C-like, with types, and safe
— a mistake stops your program with the line it happened on, never the
board. This tutorial starts from nothing and ends with programs worth
keeping: digital rain, a snake game, the weather from the internet. Type the
examples on the board with `edit`, or write them on the PC and send them
with `make push`. The full reference is [LANGUAGE.md](LANGUAGE.md), and
`man pico` on the board is the short version.

## 1. The first program

```c
int main() {
    println("hello, pocket");
    return 0;
}
```

Save it as `hello.pico` (`edit hello.pico`, type, Fn S, Fn Q) and run it:

```
$ pico hello.pico
hello, pocket
```

`pico` compiles the file and runs it in one go. `picoc hello.pico` compiles
it to a program called `hello` that runs on its own, as `./hello`, which
starts faster. Every program has a `main`; the number it returns is its
exit status, 0 for "all went well".

`main` can take the words typed after the program's name:

```c
int main(str[] args) {
    str who = len(args) > 1 ? args[1] : "world";
    println("hello, ", who);
    return 0;
}
```

```
$ pico hello.pico andre
hello, andre
```

## 2. Values and variables

Every variable has a type, and says it:

```c
int count = 3;              // whole numbers
float price = 2.50;         // numbers with a point
bool done = false;          // true or false
str name = "PocketType";    // text
const int SIZE = 10;        // a constant: it cannot change
```

`println` prints anything, one thing after another; `printf` fills a
pattern, as C's does:

```c
println("count is ", count, " and price ", price);
printf("%s: %d items at %.2f\n", name, count, price);
```

Numbers do not turn into other types by themselves (except an `int` where a
`float` is wanted). To turn one into another, say so:

```c
str s = str(42);            // "42"
int n = to_int("17");       // 17 (0 if it is not a number)
int whole = int(3.9);       // 3
str msg = "n=" + n;         // + joins text, and turns the number into text
```

## 3. Deciding and repeating

```c
if (count > 5) {
    println("many");
} else if (count > 0) {
    println("some");
} else {
    println("none");
}

int i = 0;
while (i < 3) {
    println("while ", i);
    i++;
}

for (int j = 0; j < 3; j++) println("for ", j);
```

Conditions must be `bool`: write `if (n != 0)`, not `if (n)`.

To go through every item of a list, `for ... in` is shorter and cannot go
past the end:

```c
str[] fruit = ["apple", "fig", "kiwi"];
for (str f in fruit) println("I like ", f);
```

`break` leaves a loop, `continue` goes on with the next time round.

When one value decides between many things, `switch`:

```c
switch (key) {
case 'q', 'Q':
    println("quit");
case KEY_UP:
    y--;
default:
    println("some other key");
}
```

Only the matching case runs — there is no falling into the next one as in
C — and a case can list several values. `default` goes last.

## 4. Functions

```c
int square(int n) {
    return n * n;
}

void greet(str who, int times) {
    for (int i = 0; i < times; i++) println("hi ", who);
}

int main() {
    println(square(7));
    greet("you", 2);
    return 0;
}
```

A function can be used before it is written further down the file. One
that returns a value must return on every path; the compiler checks.

## 5. Lists and text

An array is a list that grows:

```c
int[] scores = [12, 7, 30];
push(scores, 18);           // [12, 7, 30, 18]
sort(scores);               // [7, 12, 18, 30]
println(len(scores), " scores, best ", scores[len(scores) - 1]);
int last = pop(scores);     // takes the last one off
```

Text has what you would expect:

```c
str line = "  the quick brown fox  ";
str[] words = split(trim(line));        // ["the", "quick", "brown", "fox"]
println(join(words, "-"));              // the-quick-brown-fox
println(upper("shout"), " ", find("hello", "ll"), " ", replace("a-b", "-", "+"));
for (str ch in chars("café")) print("[", ch, "]");   // [c][a][f][é]
```

`s[i]` is the byte at `i` as a number: `"A"[0]` is 65.

## 6. Structs and enums

A struct groups values under one name:

```c
struct Player { str name; int x; int y; int lives; }

int main() {
    Player p = Player{name: "ada", x: 1, y: 1, lives: 3};
    p.x += 2;
    println(p);             // Player{name: "ada", x: 3, y: 1, lives: 3}
    return 0;
}
```

An enum names a handful of choices:

```c
enum Dir { UP, DOWN, LEFT, RIGHT }

Dir d = LEFT;
switch (d) {
case UP, DOWN: println("vertical");
default: println("horizontal");
}
```

Structs, arrays and text are passed around by reference: give a struct to a
function and the function changes the same struct.

## 7. Files

```c
File f = open("notes.txt", "w");
write(f, "first line\n");
close(f);

File g = open("notes.txt", "r");
for (;;) {
    str line = readline(g);
    if (line == "") break;
    print("read: ", line);
}
```

`exists(path)`, `listdir(dir)`, `mkdir`, `remove` and `rename` do what they
say; `~` is not expanded, so write `getenv("HOME") + "/notes"`.

## 8. The terminal

The screen is 53 columns by 23 rows (`term_cols()` and `term_rows()` say).
Colours and the cursor are escape sequences, which are ordinary text:
`\e[31m` red, `\e[1m` bold, `\e[0m` back to normal, `\e[2J` clears the
screen, `\e[5;10H` moves to row 5, column 10.

For a game, the keys have to arrive the moment they are pressed, without
Enter and without being printed: `raw_mode(true)`. Then `readkey(ms)` waits
at most that long for a key, and gives `KEY_NONE` if there was none — which
is what keeps a game moving while nobody presses anything:

```c
int main() {
    int x = 10;
    raw_mode(true);
    print("\e[2J\e[?25l");                      // clear, hide the cursor
    for (;;) {
        printf("\e[10;%dH o \e[H", x);          // draw the dot
        int key = readkey(100);
        if (key == 'q') break;
        if (key == KEY_LEFT && x > 1) x--;
        if (key == KEY_RIGHT && x < 50) x++;
    }
    print("\e[?25h\e[2J\e[H");                  // the cursor back
    raw_mode(false);
    return 0;
}
```

Arrow keys are `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`; Esc is
`KEY_ESC`. `sleep_ms(n)` waits, `random(n)` picks 0 to n-1, `uptime_ms()`
and `time()` tell the time.

## 9. The internet

With Wi-Fi on (`wifi`), `http_get(url)` fetches a page or what a web API
answers, as text — `""` if it could not. Most APIs answer in JSON, and
`json_get(text, path)` picks one value out of it: a path is names and
numbers joined by dots, and `#` counts:

```c
str j = http_get("https://api.open-meteo.com/v1/forecast?latitude=45.46" +
                 "&longitude=9.19&current=temperature_2m");
println("Milan: ", json_get(j, "current.temperature_2m"), " C");
```

For `{"items": [{"name": "a"}, {"name": "b"}]}`:
`json_get(j, "items.#")` is `"2"`, `json_get(j, "items.1.name")` is `"b"`,
and `json_get(j, "items.0")` is the whole `{"name": "a"}`, to look into
again.

`output(command, words...)` runs any command and gives back what it
printed — the board's other commands become part of your program:

```c
str when = output("date");
str[] files = split(output("ls", getenv("HOME")));
```

`run(command, words...)` runs one and lets it print, giving back its exit
status.

## 10. Programs to keep

These come with PocketType, in `~/pico` on the board (and `lang/examples`
in the source):

| | |
| --- | --- |
| `matrix.pico` | digital rain, the film's green glyphs falling down the screen |
| `snake.pico` | the game: arrows or WASD, eat, grow, don't crash |
| `weather.pico` | `pico weather.pico Tokyo`: the weather now, from the internet |
| `life.pico` | Conway's game of life, in colour |
| `mandel.pico` | a Mandelbrot zoom, drawn in coloured characters |
| `todo.pico` | a to-do list kept in a file |
| `guess.pico` | guess the number |
| `wc.pico` | counts lines, words and bytes, like `wc` |

Run one with `pico ~/pico/snake.pico`, read it with `more`, change it with
`edit`. Taking one apart is the quickest way to learn the rest.

## 11. When something goes wrong

The compiler says where, and often what to do:

```
hello.pico:4:13: error: expected int, got str; parse it with to_int(s)
    int n = "12";
            ^
```

A mistake while running stops the program with the line and how it got
there, and nothing else is harmed:

```
list.pico:6: runtime error: index 9 out of range (length 3)
    in main (list.pico:6)
```

`assert(condition, "why")` stops the program at once if something that must
be true is not, which finds a mistake where it starts rather than where it
shows. Fn C stops a program that runs away.
