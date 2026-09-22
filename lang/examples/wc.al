// Count lines, words and bytes, like wc(1).
//   a wc.al file...
struct Counts { int lines; int words; int bytes; }

Counts count(str text) {
    Counts c = Counts{};
    c.bytes = len(text);
    bool in_word = false;
    for (int i = 0; i < len(text); i++) {
        int ch = text[i];
        bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        if (ch == '\n') { c.lines++; }
        if (!space && !in_word) { c.words++; }
        in_word = !space;
    }
    return c;
}

int main(str[] args) {
    if (len(args) < 2) {
        write(stderr, "usage: wc file...\n");
        return 2;
    }
    Counts total = Counts{};
    for (int i = 1; i < len(args); i++) {
        File f = open(args[i], "r");
        if (f == null) {
            write(stderr, "wc: " + args[i] + ": cannot open\n");
            continue;
        }
        Counts c = count(read(f));
        close(f);
        printf("%7d %7d %7d %s\n", c.lines, c.words, c.bytes, args[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (len(args) > 2) {
        printf("%7d %7d %7d total\n", total.lines, total.words, total.bytes);
    }
    return 0;
}
