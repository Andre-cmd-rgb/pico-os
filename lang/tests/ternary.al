struct P { int x; }
int main() {
    int n = 5;
    println(n > 3 ? "big" : "small", " ", n > 3 ? 1 : 0, " ", n < 3 ? 1.5 : 2, " ", n > 3 ? 2 : 1.5);
    str s = n == 5 ? "five" : n == 4 ? "four" : "other";
    println(s);
    int[] a = n > 0 ? [1, 2] : [];
    println(a);
    P p = n > 0 ? null : P{x: 1};
    println(p, " ", n > 0 ? null == p : false);
    println(true ? (false ? 1 : 2) : 3);
    float f = true ? 1 : 2;
    println(f);
    return 0;
}
