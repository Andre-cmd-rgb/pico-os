/*
 * view - look at a picture on the screen.
 *
 * The decoding is in canvas.c, which the video player uses too; this is
 * the program around it: which files, the keys, and putting the terminal
 * back afterwards.
 */
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "canvas.h"
#include "util.h"

#if CONFIG_PT_LCD

#define PHOTOS		"/home/" CONFIG_PT_USERNAME "/photos"

static bool is_jpeg(int fd)
{
	uint8_t magic[2] = { 0 };

	pt_lseek(fd, 0, SEEK_SET);
	pt_read(fd, magic, 2);
	pt_lseek(fd, 0, SEEK_SET);
	return magic[0] == 0xff && magic[1] == 0xd8;
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
		canvas_blit(c);
	return ret;
}

PT_PROGRAM_STACK(view, 8, "look at a picture\n"
		 "usage: view [file.bmp | file.jpg ...]\n"
		 "With no file, the pictures in ~/photos are offered as a list.\n"
		 "Left and right move between the files named, s saves a copy\n"
		 "of the screen, q or Esc quits.\n"
		 "BMP and baseline JPEG; anything larger is scaled to fit.")
{
	static const char *const exts[] = { ".bmp", ".jpg", ".jpeg", NULL };
	struct canvas c = { 0 };
	char chosen[PT_PATH_MAX], shot[PT_PATH_MAX];
	char *picked[1] = { chosen };
	char **files = argv + 1;
	int n = argc - 1, at = 0, ret;

	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "view: there is no screen\n");
		return 1;
	}
	if (!n) {
		ret = pick_file(PHOTOS, exts, "pictures", chosen, sizeof(chosen));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("view", PHOTOS, ret);
		files = picked;
		n = 1;
	}
	if ((ret = canvas_open(&c)))
		return fail("view", NULL, ret);

	vt_hold_screen(true);
	pt_tty_raw(PT_STDIN, true);
	while (!(ret = show(files[at], &c))) {
		int key = pt_readkey(PT_STDIN);

		if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c') ||
		    key == PT_KEY_EOF || key == '\r' || key == '\n')
			break;
		if ((key == PT_KEY_RIGHT || key == ' ') && at + 1 < n)
			at++;
		else if (key == PT_KEY_LEFT && at > 0)
			at--;
		else if (key == 's')
			canvas_save(&c, sd_mounted() ? PHOTOS : "/tmp", shot, sizeof(shot));
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
