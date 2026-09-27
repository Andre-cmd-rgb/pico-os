// Builds and drops a lot of objects; the leak check at exit must be silent.
struct Item { str name; int[] values; Item next; }

Item chain(int n) {
    Item head = null;
    for (int i = 0; i < n; i++) {
        int[] v = [];
        for (int k = 0; k < 3; k++) { push(v, i * k); }
        head = Item{name: "item" + str(i), values: v, next: head};
    }
    return head;
}

int total(Item it) {
    int sum = 0;
    while (it != null) {
        for (int i = 0; i < len(it.values); i++) { sum += it.values[i]; }
        it = it.next;
    }
    return sum;
}

int main() {
    for (int round = 0; round < 50; round++) {
        Item head = chain(20);
        if (round == 0) { println("sum=", total(head), " name=", head.name); }
    }
    str[] strings = [];
    for (int i = 0; i < 100; i++) { push(strings, "s" + str(i)); }
    while (len(strings) > 0) { pop(strings); }
    str s = "";
    for (int i = 0; i < 200; i++) { s = s + "x"; }
    str[][] nested = [];
    for (int i = 0; i < 20; i++) { push(nested, ["a", "b"]); }
    resize(nested, 5);
    println(len(s), " ", len(nested), " ", len(strings));
    return 0;
}
