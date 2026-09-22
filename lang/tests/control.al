int main() {
    int n = 3;
    if (n == 1) { println("one"); } else if (n == 3) { println("three"); } else { println("other"); }
    if (n > 0) println("positive");
    int i = 0;
    while (i < 5) { i++; if (i == 2) { continue; } if (i == 4) { break; } print(i, " "); }
    println("| i=", i);
    for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { if (k == j) { continue; } print(j, k, " "); } }
    println("");
    int count = 0;
    for (;;) { count++; if (count == 4) { break; } }
    while (true) { count++; break; }
    println("count=", count);
    { int scoped = 9; println(scoped); }
    int total = 0;
    for (int j = 10; j > 0; j -= 3) { total += j; }
    println("total=", total);
    return 0;
}
