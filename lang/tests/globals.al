const int LIMIT = 5;
const str NAME = "counter";
const float RATIO = 0.5;
int calls = 0;
int[] log = [];
str prefix = "> ";

int bump() { calls++; push(log, calls); return calls; }

int main() {
    println(NAME, " limit=", LIMIT, " ratio=", RATIO);
    while (bump() < LIMIT) { }
    println(prefix, calls, " ", log);
    prefix = "< ";
    println(prefix, len(log));
    return 0;
}
