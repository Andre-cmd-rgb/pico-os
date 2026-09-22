// Guess the number. Shows random(), readline(stdin) and string to number.
int main() {
    seed(uptime_ms() + time());
    int secret = random(100) + 1;
    int tries = 0;
    println("I am thinking of a number between 1 and 100.");
    while (true) {
        print("your guess? ");
        str line = trim(readline(stdin));
        if (line == "" || line == "q") {
            println("the number was ", secret);
            return 0;
        }
        int guess = to_int(line, -1);
        if (guess < 1 || guess > 100) {
            println("that is not a number between 1 and 100");
            continue;
        }
        tries++;
        if (guess < secret) {
            println("higher");
        } else if (guess > secret) {
            println("lower");
        } else {
            println("right, in ", tries, " tries");
            return 0;
        }
    }
}
