// A reference cycle: refcounting cannot free it, and the leak check says so.
struct Ring { str name; Ring next; }
int main() {
    Ring a = Ring{name: "a"};
    Ring b = Ring{name: "b", next: a};
    a.next = b;
    println(a.next.name, b.next.name);
    return 0;
}
