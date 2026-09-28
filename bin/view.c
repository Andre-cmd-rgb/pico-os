/*
 * view - look at a picture on the screen.
 *
 * The decoding is in canvas.c, which the video player uses too; this is
 * the program around it: which files, the keys, and putting the terminal
 * back afterwards. Switching to another terminal leaves the picture where
 * it is; coming back paints it again.
 */
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "canvas.h"
#include "util.h"

#if CONFIG_PT_LCD


static bool is_jpeg(int fd)
{
	uint8_t magic[2] = { 0 };

	pt_lseek(fd, 0, SEEK_SET);
	pt_read(fd, magic, 2);
	pt_lseek(fd, 0, SEEK_SET);
	return magic[0] == 0xff && magic[1] == 0xd8;
}

/* The picture, onto the panel -- if this terminal is the one in front. */
static void paint(struct canvas *c)
{
	if (vt_screen_begin())
		canvas_blit(c);
	vt_screen_end();
}

static int show(const char *path, struct canvas *c)
{
	int fd = pt_open(path, O_RDONLY);
	int ret;

	if (fd < 0)
		return fd;
	canvas_clear(c);
	ret = is_jpeg(fd) ? canvas_jpeg(c, fd) : canvas_bmp(c, fd);
	pt_close(fd);
	if (!ret)
		paint(c);
	return ret;
}

PT_COMPLETE(view, ": <file:.bmp.jpg.jpeg>\n*: <file:.bmp.jpg.jpeg>\n")

PT_PROGRAM_STACK(view, 8, "look at a picture\n"
		 "usage: view [file.bmp | file.jpg ...]\n"
		 "With no file, the pictures in ~/photos are offered as a list.\n"
		 "Left and right move between the files named, s saves a copy\n"
		 "of the screen, q or Esc quits.\n"
		 "BMP and baseline JPEG; anything larger is scaled to fit.")
{
	static const char *const exts[] = { ".bmp", ".jpg", ".jpeg", NULL };
	struct canvas c = { 0 };
	char chosen[PT_PATH_MAX], shot[PT_PATH_MAX], photos[64];
	char *picked[1] = { chosen };
	char **files = argv + 1;
	int n = argc - 1, at = 0, ret;

	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "view: there is no screen\n");
		return 1;
	}
	home_dir(photos, sizeof(photos), "photos");
	if (!n) {
		ret = pick_file(photos, exts, "pictures", chosen, sizeof(chosen));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("view", photos, ret);
		files = picked;
		n = 1;
	}
	if ((ret = canvas_open(&c)))
		return fail("view", NULL, ret);

	vt_hold_screen(true);
	pt_tty_raw(PT_STDIN, true);
	while (!(ret = show(files[at], &c))) {
		unsigned gen = vt_screen_gen();
		int key;

		/* back from another terminal, which painted over the picture */
		while ((key = pt_readkey_timeout(PT_STDIN, 200)) == PT_KEY_NONE)
			if (gen != vt_screen_gen()) {
				gen = vt_screen_gen();
				paint(&c);
			}

		if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c') ||
		    key == PT_KEY_EOF || key == '\r' || key == '\n')
			break;
		if ((key == PT_KEY_RIGHT || key == ' ') && at + 1 < n)
			at++;
		else if (key == PT_KEY_LEFT && at > 0)
			at--;
		else if (key == 's') {
			if (vt_screen_begin())
				canvas_save(&c, sd_mounted() ? photos : "/tmp", shot, sizeof(shot));
			vt_screen_end();
		}
	}
	pt_tty_raw(PT_STDIN, false);
	canvas_close(&c);
	vt_hold_screen(false);
	vt_redraw();
	return ret ? fail("view", files[at], ret) : 0;
}

#else

PT_PROGRAM(view, "look at a picture (no screen on this board)")
{
	pt_dprintf(PT_STDERR, "view: this board has no screen\n");
	return 1;
}

#endif
