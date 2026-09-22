int main() {
    str s = "Hello, World";
    println(len(s), " ", len(""), " ", s + "!", " ", "a" + 1 + true + 2.5);
    println(substr(s, 7), "|", substr(s, 0, 5), "|", substr(s, 5, 100), "|", substr(s, 12), "|");
    println(find(s, "o"), " ", find(s, "o", 5), " ", find(s, "zz"), " ", find(s, ""), " ", find(s, "World", 20));
    println(split("a,b,,c", ","), " ", split("one two\tthree"), " ", split("", ","), " ", split("ab", ""));
    println(join(["x", "y", "z"], "-"), "|", join([], ","), "|", join(["solo"], "+"));
    println("[", trim("  spaced \t\n"), "][", trim("none"), "][", trim("   "), "]");
    println(upper("mIxEd"), " ", lower("mIxEd"), " ", replace("banana", "an", "AN"), " ", replace("aaa", "a", ""));
    println(starts_with(s, "Hello"), ends_with(s, "World"), starts_with(s, "x"), ends_with("ab", "abc"));
    println(s[0], " ", s[len(s) - 1], " ", chr(65), chr(233), chr(0x2603), " ", ord("A"), " ", ord("é"), " ", ord("☃"));
    println(to_int("42"), " ", to_int("-7"), " ", to_int("0x1f"), " ", to_int(" 8 "), " ", to_int("nope"), " ", to_int("nope", -1));
    println(to_float("2.5"), " ", to_float("1e3"), " ", to_float("x", 9.5), " ", to_int("2147483648", 0));
    str built = "";
    for (int i = 0; i < 5; i++) { built += str(i); }
    println(built, " ", len(built));
    println("tab\there\nnew \"quoted\" \\ \x41 \u{1F600}");
    return 0;
}
