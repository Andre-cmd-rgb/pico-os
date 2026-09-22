int main() {
    printf("[%d] [%5d] [%-5d] [%05d] [%+d]\n", 42, 42, 42, 42, 42);
    printf("[%x] [%X] [%o] [%u]\n", 255, 255, 8, -1);
    printf("[%f] [%.2f] [%8.3f] [%e] [%g]\n", 1.5, 1.5, 1.5, 1500.0, 1500.0);
    printf("[%s] [%8s] [%-8s] [%.2s]\n", "text", "text", "text", "text");
    printf("[%c][%c][%c]\n", 65, 10, 0x263A);
    printf("100%% sure\n");
    printf("%d items, first is %s, total %.1f\n", 3, "apple", 4.5);
    printf("%d\n", true);
    printf("%f from int\n", 2);
    printf("%s %s %s %s\n", [1, 2], true, 2.5, null);
    println(format("%s=%d", "x", 1), " ", format("no args"), " ", len(format("%s", "abc")));
    printf("");
    printf("%s\n", "");
    return 0;
}
