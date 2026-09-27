// stdin: hi
int main() {
    // raw_mode does nothing useful when the input is a file, but must not fail
    raw_mode(true);
    int a = readkey();
    int b = readkey(50);
    println(a, " ", b, " ", chr(a) + chr(b));
    println(readkey(0) == KEY_NONE || readkey(0) == KEY_EOF);
    raw_mode(false);
    println(KEY_UP, " ", KEY_ESC, " ", KEY_NONE, " ", KEY_EOF, " ", term_cols() > 0);
    str s = "a" + chr(0) + "b";
    println(len(s), " ", s[1], " ", len("\u{1F600}"));
    return 0;
}
