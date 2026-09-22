// The first program: run it with `a hello.al yourname`.
struct Point {
    int x;
    int y;
}

int dist2(Point p) {
    return p.x * p.x + p.y * p.y;
}

int main(str[] args) {
    str name = len(args) > 1 ? args[1] : "world";
    Point p = Point{x: 3, y: 4};
    for (int i = 0; i < 3; i++) {
        printf("hello %s #%d, dist2 = %d\n", name, i, dist2(p));
    }
    int[] squares = [];
    for (int i = 1; i <= 5; i++) {
        push(squares, i * i);
    }
    println("squares: ", squares);
    File f = open(getenv("HOME") + "/hello.log", "a");
    if (f != null) {
        write(f, "ran with " + name + "\n");
        close(f);
    }
    return 0;
}
