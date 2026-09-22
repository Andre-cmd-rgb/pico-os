void nothing() { }
int main() {
    // terminal size falls back to 80x24 when not a terminal
    println(term_cols(), "x", term_rows());
    // uptime and time move forward, but never print them
    println(uptime_ms() >= 0, " ", time() > 1600000000, " ", len(date()));
    sleep_ms(1);
    println("getenv: [", getenv("AL_DOES_NOT_EXIST"), "]");
    println("run: ", run("true"), " ", run("false"), " ", run("sh", "-c", "exit 5"));
    nothing();
    println(run("this-command-does-not-exist") != 0);
    return 0;
}
