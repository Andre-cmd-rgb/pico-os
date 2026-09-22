// Animated Mandelbrot zoom, colored by escape time.
//
//   a mandel.al [frames]      q / Esc quits, any other key pauses until the next
//
// Starts already in the "seahorse valley" and zooms in a frame at a time.
// It is a good FPU workout: every cell iterates z = z^2 + c until it
// escapes, so the whole frame is ~cols*rows*maxit float ops. 32-bit float
// precision bottoms out around frame ~100 (span ~1e-7); pass fewer frames
// to stop sooner.

const str CLEAR = "\e[2J\e[H";
const str HIDE  = "\e[?25l";
const str SHOW  = "\e[?25h";
const str RESET = "\e[0m";

int main(str[] args) {
    int cols = min(term_cols(), 96);
    int rows = min(term_rows() - 1, 44);
    int maxit = 48;
    int frames = len(args) > 1 ? to_int(args[1], 100) : 100;

    // The seahorse valley, and how much of the plane each frame shows.
    float cx = -0.74364389;
    float cy = 0.13182590;
    float span = 0.06;
    float zoom = 1.12;

    // Escape-time colors, far-outside -> boundary, and a matching density
    // ramp. Points that never escape (the set itself) stay black.
    str[] C = ["\e[34m", "\e[36m", "\e[32m", "\e[33m", "\e[37m"];
    const str RAMP = " .:*#";

    raw_mode(true);
    print(HIDE);
    for (int f = 0; f < frames; f++) {
        float hw = span / 2.0;
        float hh = hw * float(rows) / float(cols);
        str out = CLEAR;
        for (int y = 0; y < rows; y++) {
            float ci = cy + (float(y) / float(rows - 1) - 0.5) * 2.0 * hh;
            for (int x = 0; x < cols; x++) {
                float cr = cx + (float(x) / float(cols - 1) - 0.5) * 2.0 * hw;
                float zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
                int it = 0;
                while (it < maxit && zr2 + zi2 <= 4.0) {
                    zi = 2.0 * zr * zi + ci;
                    zr = zr2 - zi2 + cr;
                    zr2 = zr * zr;
                    zi2 = zi * zi;
                    it++;
                }
                if (it >= maxit) {
                    out += " ";                       // inside the set: black
                } else {
                    int k = it * len(C) / maxit;
                    out += C[k] + substr(RAMP, k, 1);
                }
            }
            out += "\n";
        }
        out += RESET + "  frame " + str(f) + "  span " + format("%.4g", span)
               + "  (q quits)";
        print(out);
        span = span / zoom;
        int key = readkey(40);
        if (key != KEY_NONE && key != 'q' && key != 'Q' && key != KEY_ESC) {
            key = readkey();                          // paused until the next key
        }
        if (key == 'q' || key == 'Q' || key == KEY_ESC || key < 0 && key != KEY_NONE) { break; }
    }
    print(SHOW, RESET, "\n");
    raw_mode(false);
    return 0;
}
