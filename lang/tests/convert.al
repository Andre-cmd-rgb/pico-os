int main() {
    println(int(2.9), " ", int(-2.9), " ", int(true), " ", int(7), " ", int(1e30), " ", int(-1e30));
    println(float(3), " ", float(2.5), " ", float(-1) / 4.0);
    println(str(42), str(-1), " ", str(2.5), " ", str(true), " ", str("x"), " ", str([1, 2]), " ", str(null));
    println("n=" + str(3) + " f=" + str(0.25) + " b=" + str(false));
    println(to_int("10") + to_int("5"), " ", to_float("0.5") + 0.5);
    println(int(float(7) / 2.0), " ", float(int(2.9)));
    return 0;
}
