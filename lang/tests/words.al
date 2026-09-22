// A little text pipeline: count words, find the longest, build a report.
struct Count { str word; int n; }

int index_of(Count[] cs, str w) {
    for (int i = 0; i < len(cs); i++) { if (cs[i].word == w) { return i; } }
    return -1;
}

int main() {
    str text = "the quick brown fox jumps over the lazy dog the fox";
    str[] words = split(text);
    Count[] counts = [];
    for (int i = 0; i < len(words); i++) {
        str w = words[i];
        int at = index_of(counts, w);
        if (at < 0) { push(counts, Count{word: w, n: 1}); } else { counts[at].n += 1; }
    }
    str longest = "";
    for (int i = 0; i < len(words); i++) { if (len(words[i]) > len(longest)) { longest = words[i]; } }
    println("words=", len(words), " unique=", len(counts), " longest=", longest);
    for (int i = 0; i < len(counts); i++) {
        if (counts[i].n > 1) { printf("%-6s %d\n", counts[i].word, counts[i].n); }
    }
    str[] sorted = [];
    for (int i = 0; i < len(counts); i++) { push(sorted, counts[i].word); }
    sort(sorted);
    println(join(sorted, ","));
    println(upper(substr(text, 0, 9)), "|", replace(text, "the", "THE"));
    return 0;
}
