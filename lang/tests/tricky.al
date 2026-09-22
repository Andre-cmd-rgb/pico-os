// Corners of the code generator: captured loop conditions, scopes with
// references, break and continue out of scopes, globals during start-up.
struct Box { str tag; int[] nums; Box inner; }

str[] log = [];
str banner = greeting() + "!";
int started = count_up();

str greeting() { return "ready"; }
int count_up() { push(log, "init"); return len(log); }

Box make(str tag, int n) {
    Box b = Box{tag: tag, nums: []};
    for (int i = 0; i < n; i++) { push(b.nums, i); }
    return b;
}

str describe(Box b) {
    str out = b.tag;
    int i = 0;
    while (i < len(b.nums)) {
        str piece = "," + str(b.nums[i]);
        if (b.nums[i] == 2) { i++; continue; }
        out += piece;
        i++;
    }
    return out;
}

int main() {
    println(banner, " ", started, " ", log);
    Box b = make("box", 5);
    println(describe(b), " ", b.nums);
    // references held in inner scopes, left by break and continue
    for (int i = 0; i < 4; i++) {
        str label = "row" + str(i);
        if (i == 1) { continue; }
        {
            str deeper = label + "!";
            if (i == 3) { push(log, deeper); break; }
            push(log, deeper);
        }
    }
    println(log);
    // nested boxes, reassignment, and dropping a chain
    Box outer = Box{tag: "a", nums: [1], inner: Box{tag: "b", nums: [2], inner: make("c", 2)}};
    println(outer.inner.inner.tag, " ", outer.inner.inner.nums, " ", describe(outer.inner));
    outer.inner = null;
    println(outer);
    // arrays of arrays, modified through a local alias
    int[][] rows = [];
    for (int i = 0; i < 3; i++) {
        int[] row = [];
        for (int k = 0; k <= i; k++) { push(row, i * 10 + k); }
        push(rows, row);
    }
    int[] alias = rows[1];
    alias[0] += 5;
    rows[2][1] *= 2;
    println(rows, " ", len(rows[2]));
    // strings built in loops with conditions captured by the compiler
    str s = "";
    int n = 0;
    while (len(s) < 10) { s += "ab"; n++; }
    println(s, " ", n, " ", len(s));
    for (int i = 0; i < 3; i++) { s = substr(s, 1); }
    println(s);
    // ternaries returning references, and null handling
    Box maybe = len(s) > 100 ? b : null;
    println(maybe == null, " ", (maybe == null ? "none" : maybe.tag));
    return 0;
}
