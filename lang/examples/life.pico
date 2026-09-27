// Conway's life on the terminal. Shows raw mode, keys, escape sequences
// and sleep_ms. Press q to quit, space to restart, any other key to pause.
const str CLEAR = "\e[2J\e[H";
const str HIDE = "\e[?25l";
const str SHOW = "\e[?25h";
const str GREEN = "\e[32m";
const str RESET = "\e[0m";

int w = 0;
int h = 0;

bool[] fresh() {
    bool[] cells = [];
    resize(cells, w * h);
    for (int i = 0; i < w * h; i++) { cells[i] = random(5) == 0; }
    return cells;
}

int neighbours(bool[] cells, int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            int nx = (x + dx + w) % w;
            int ny = (y + dy + h) % h;
            if (cells[ny * w + nx]) { n++; }
        }
    }
    return n;
}

bool[] step(bool[] cells) {
    bool[] next = [];
    resize(next, w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int n = neighbours(cells, x, y);
            bool alive = cells[y * w + x];
            next[y * w + x] = n == 3 || (alive && n == 2);
        }
    }
    return next;
}

void draw(bool[] cells, int gen) {
    str out = CLEAR + GREEN;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) { out += cells[y * w + x] ? "#" : " "; }
        out += "\n";
    }
    print(out, RESET, "generation ", gen, "  (q quits)");
}

int main(str[] args) {
    w = min(term_cols(), 60);
    h = min(term_rows() - 2, 20);
    int limit = len(args) > 1 ? to_int(args[1], 100) : 100;
    seed(uptime_ms());
    bool[] cells = fresh();
    raw_mode(true);
    print(HIDE);
    for (int gen = 1; gen <= limit; gen++) {
        draw(cells, gen);
        cells = step(cells);
        int key = readkey(80);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) { break; }
        if (key == ' ') { cells = fresh(); }
        if (key != KEY_NONE) { readkey(0); }
    }
    print(SHOW, RESET, "\n");
    raw_mode(false);
    return 0;
}
