int main(str[] args) {
    // strings
    println(contains("hello world", "o w"), " ", contains("abc", "z"), " ", contains("abc", ""));
    println(repeat("ab", 3), " ", repeat("x", 0), "|");
    // arrays
    int[] a = [1, 2, 3, 4];
    reverse(a);
    println(a);
    str[] s = ["one", "two"];
    reverse(s);
    println(s);
    int[] empty = [];
    reverse(empty);
    println(len(empty));
    // maths
    printf("%.3f %.3f %.3f %.3f\n", tan(0.0), ln(2.718281828), exp(1.0), hypot(3.0, 4.0));
    // assert passes quietly
    assert(1 + 1 == 2);
    assert(len(a) == 4, "array kept its length");
    println("done");
    return 0;
}
