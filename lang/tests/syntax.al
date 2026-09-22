struct Inner { int[] vals; }
struct Outer { Inner[] parts; }
void nothing() { }
int pick(Inner i) { return i.vals[0]; }
int main() {
    for (int i = 0, j = 10; i < j; i += 3) { print(i, " "); }
    println("");
    int n = 0;
    for (;;) { n++; if (n > 2) break; }
    for (int i = 0; ; i++) { if (i == 2) { break; } }
    Outer o = Outer{parts: [Inner{vals: [7, 8]}, Inner{vals: [9]}]};
    o.parts[0].vals[1] += 1;
    println(n, " ", o.parts[0].vals[1], " ", o.parts[1].vals[0], " ", pick(o.parts[0]));
    println(pick(Inner{vals: [42]}), " ", len(Outer{parts: []}.parts));
    int t = n > 1 ? (n > 2 ? 3 : 2) : 1;
    println(t);
    nothing();
    if (true) ; else ;
    { }
    return 0;
}
