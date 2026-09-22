// Write a small CSV, read it back, check the numbers.
int main() {
    str path = getenv("AL_TMP") + "/roundtrip.csv";
    File out = open(path, "w");
    if (out == null) { println("cannot write"); return 1; }
    for (int i = 1; i <= 5; i++) { write(out, format("row%d,%d\n", i, i * i)); }
    close(out);
    File in = open(path, "r");
    int total = 0, rows = 0;
    while (true) {
        str line = trim(readline(in));
        if (line == "") { break; }
        str[] parts = split(line, ",");
        total += to_int(parts[1]);
        rows++;
        if (rows == 1) { println("first row: ", parts[0], " -> ", parts[1]); }
    }
    close(in);
    println("rows=", rows, " total=", total);
    println(remove(path), " ", exists(path));
    return 0;
}
