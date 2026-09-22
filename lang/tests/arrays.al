int main() {
    int[] a = [3, 1, 2];
    println(a, " ", len(a), " ", a[0], " ", a[len(a) - 1]);
    push(a, 4);
    insert(a, 0, 0);
    println(a, " popped=", pop(a), " removed=", remove_at(a, 1), " ", a);
    sort(a);
    println(a, " find2=", find(a, 2), " findX=", find(a, 99), " slice=", slice(a, 1, 3), " ", slice(a, 0, 99));
    int[] b = [];
    resize(b, 4);
    b[0] = 7;
    b[3] += 2;
    println(b, " ", len(b));
    resize(b, 2);
    println(b);
    // arrays are references
    int[] c = b;
    push(c, 5);
    println(b, " ", b == c, " ", b == [7, 0, 5]);
    // nested
    int[][] grid = [[1, 2], [3, 4]];
    grid[0][1] = 9;
    push(grid, [5]);
    println(grid, " ", grid[2][0], " ", len(grid));
    // strings and floats and bools
    str[] names = ["pear", "fig", "apple"];
    sort(names);
    println(names, " ", find(names, "fig"));
    float[] fs = [2.5, 1.5];
    sort(fs);
    println(fs);
    bool[] flags = [true, false];
    println(flags, " ", flags[1]);
    // array of structs stays alive through the array
    int[] empty = [];
    println(empty, " ", len(empty));
    return 0;
}
