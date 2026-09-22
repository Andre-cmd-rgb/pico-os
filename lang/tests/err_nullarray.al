// status: 1
struct Bag { int[] items; }
int main() {
    Bag b = Bag{};
    println(b.items == null);
    push(b.items, 1);
    return 0;
}
