int main() {
    println(abs(-5), " ", abs(5), " ", abs(-2.5), " ", min(3, 7), " ", max(3, 7), " ", min(1.5, 0.5), " ", max(2, 3.5));
    println(sqrt(16.0), " ", pow(2.0, 10.0), " ", floor(2.7), " ", ceil(2.1), " ", round(2.5), " ", round(-2.5));
    printf("%.4f %.4f %.4f\n", sin(0.0), cos(0.0), atan2(1.0, 1.0));
    println(PI > 3.14 && PI < 3.15);
    seed(42);
    int[] draws = [];
    for (int i = 0; i < 5; i++) { push(draws, random(10)); }
    bool in_range = true;
    for (int i = 0; i < len(draws); i++) { if (draws[i] < 0 || draws[i] > 9) { in_range = false; } }
    println("random in range: ", in_range, " count=", len(draws));
    seed(42);
    int first = random(10);
    seed(42);
    println("same seed, same draw: ", first == random(10));
    return 0;
}
