int calls = 0;
bool yes() { calls++; return true; }
bool no() { calls++; return false; }
int main() {
    println(1 < 2, 2 <= 2, 3 > 4, 4 >= 4, 5 == 5, 5 != 5);
    println(1.5 < 2.0, 2.0 <= 2.0, "a" < "b", "abc" == "abc", "abc" != "abd", "b" > "abc");
    println(!true, " ", true && false, " ", true || false);
    // short circuit: the right side must not run
    calls = 0;
    bool r = no() && yes();
    println(r, " calls=", calls);
    calls = 0;
    r = yes() || no();
    println(r, " calls=", calls);
    println(1 == 1 && 2 == 2 || 3 == 4);
    int[] a = [1], b = [1];
    println(a == b, " ", a == a, " ", a != b, " ", a == null, " ", null == a);
    return 0;
}
