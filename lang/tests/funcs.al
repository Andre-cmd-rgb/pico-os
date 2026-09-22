int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }
void greet(str who) { println("hi ", who); }
int later(int x) { return helper(x) * 2; }
int helper(int x) { return x + 1; }
bool even(int n) { return n % 2 == 0; }
float half(float x) { return x / 2.0; }
str[] twice(str s) { return [s, s]; }
int main() {
    println(fact(10), " ", fib(15), " ", later(4), " ", even(7), " ", half(5.0), " ", twice("ab"));
    greet("andre");
    return 0;
}
