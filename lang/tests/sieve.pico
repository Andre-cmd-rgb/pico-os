int[] primes_below(int n) {
    bool[] composite = [];
    resize(composite, n);
    int[] primes = [];
    for (int i = 2; i < n; i++) {
        if (composite[i]) { continue; }
        push(primes, i);
        for (int k = i * i; k < n; k += i) { composite[k] = true; }
    }
    return primes;
}
int main() {
    int[] p = primes_below(100);
    println(len(p), " primes: ", p);
    int[] big = primes_below(10000);
    println("under 10000: ", len(big), " last: ", big[len(big) - 1]);
    return 0;
}
