struct Point { int x; int y; }
struct Person { str name; int age; float score; bool member; Point home; str[] tags; }
struct Node { int v; Node next; }

int sum(Node n) { return n == null ? 0 : n.v + sum(n.next); }
void move(Point p, int dx) { p.x += dx; }

int main() {
    Point p = Point{x: 3, y: 4};
    println(p, " ", p.x, " ", p.y);
    p.x = 10;
    p.y += 5;
    move(p, 2);
    println(p);
    Person q = Person{name: "ann", age: 30};
    println(q);
    q.home = Point{};
    q.tags = ["a"];
    push(q.tags, "b");
    q.home.x = 1;
    println(q, " ", q.home.x, " ", q.tags[1]);
    Point same = p;
    same.x = 99;
    println(p.x, " ", p == same, " ", p == Point{x: 99, y: 11});
    Node list = Node{v: 1, next: Node{v: 2, next: Node{v: 3}}};
    println(sum(list), " ", list.next.next.v, " ", list.next.next.next == null);
    Point[] ps = [Point{x: 1, y: 1}, Point{x: 2, y: 2}];
    ps[1].x = 20;
    println(ps, " ", ps[1].x);
    return 0;
}
