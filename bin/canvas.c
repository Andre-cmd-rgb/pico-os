/*
 * Pictures on the panel.
 *
 * BMP is read here: the format is a header and rows of bytes, and reading
 * it takes less code than explaining why it should not be. JPEG is handed
 * to the decompressor in the chip's own ROM, which costs no flash at all
 * and is already there whether it is used or not; a Huffman decoder and
 * an inverse DCT written by hand would be a week's work to arrive at
 * something slower and larger.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "canvas.h"
#include "util.h"

#if CONFIG_PT_LCD

#include "esp32s3/rom/tjpgd.h"

#define STRIP_ROWS	16		/* rows per transfer to the panel */
#define JPEG_POOL	8192		/* the decompressor's working memory */

int canvas_open(struct canvas *c)
{
	if (!vt_has_display())
		return -ENODEV;
	c->w = lcd_width();
	c->h = lcd_height();
	c->px = pt_malloc((size_t)c->w * c->h * 2);
	return c->px ? 0 : -ENOMEM;
}

void canvas_close(struct canvas *c)
{
	pt_free(c->px);
	c->px = NULL;
}

void canvas_clear(struct canvas *c)
{
	memset(c->px, 0, (size_t)c->w * c->h * 2);
}

void canvas_pixel(struct canvas *c, int dx, int dy, int r, int g, int b)
{
	unsigned v;
	uint8_t *p;

	if (dx < 0 || dy < 0 || dx >= c->w || dy >= c->h)
		return;
	v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
	p = c->px + ((size_t)dy * c->w + dx) * 2;
	p[0] = v >> 8;
	p[1] = v;
}

int canvas_x(const struct canvas *c, int sx)
{
	return c->dw == c->sw ? c->x0 + sx : c->x0 + (int)((int64_t)sx * c->dw / c->sw);
}

int canvas_y(const struct canvas *c, int sy)
{
	return c->dh == c->sh ? c->y0 + sy : c->y0 + (int)((int64_t)sy * c->dh / c->sh);
}

/* The largest the picture can be on this screen without distorting it. */
void canvas_fit(struct canvas *c)
{
	if ((int64_t)c->h * c->sw < (int64_t)c->w * c->sh) {
		c->dh = c->h;			/* height is the limit */
		c->dw = (int)((int64_t)c->sw * c->h / c->sh);
	} else {
		c->dw = c->w;
		c->dh = (int)((int64_t)c->sh * c->w / c->sw);
	}
	if (c->dw < 1)
		c->dw = 1;
	if (c->dh < 1)
		c->dh = 1;
	c->x0 = (c->w - c->dw) / 2;
	c->y0 = (c->h - c->dh) / 2;
}

/* `x, y, w, h` of the canvas, sent a strip at a time. */
static void blit_rect(const struct canvas *c, int x0, int y0, int w, int h)
{
	uint8_t *strip = lcd_alloc_buffer((size_t)w * STRIP_ROWS * 2);

	if (!strip) {
		lcd_draw(x0, y0, w, h, c->px);	/* in one go, if it can */
		return;
	}
	for (int y = 0; y < h; y += STRIP_ROWS) {
		int rows = h - y < STRIP_ROWS ? h - y : STRIP_ROWS;

		for (int r = 0; r < rows; r++)
			memcpy(strip + (size_t)r * w * 2,
			       c->px + ((size_t)(y0 + y + r) * c->w + x0) * 2,
			       (size_t)w * 2);
		lcd_draw(x0, y0 + y, w, rows, strip);
	}
	free(strip);
}

void canvas_blit(const struct canvas *c)
{
	blit_rect(c, 0, 0, c->w, c->h);
}

/*
 * Only the part the picture covers. For a clip whose shape does not
 * match the screen this is the difference between sending the black
 * bars sixty times a second and not sending them at all; the bars are
 * already on the panel from the first full blit.
 */
void canvas_blit_fit(const struct canvas *c)
{
	if (c->dw <= 0 || c->dh <= 0)
		return;
	blit_rect(c, c->x0, c->y0, c->dw, c->dh);
}

/*
 * Keeping a copy of what is on the screen. `screenshot` cannot be used
 * from a program that owns the panel -- taking one makes the terminal
 * paint itself over the picture -- so such a program arms the capture
 * and draws itself again into it.
 */
int canvas_save(struct canvas *c, const char *dir, char *path, size_t size)
{
	int ret;

	for (int n = 1; n < 1000; n++) {
		struct pt_stat st;

		snprintf(path, size, "%s/shot-%d.bmp", dir, n);
		if (pt_stat(path, &st))
			break;
	}
	if ((ret = lcd_capture_begin()))
		return ret;
	canvas_blit(c);
	ret = lcd_capture_save(path);
	lcd_capture_end();
	return ret;
}

/* ------------------------------------------------------------ BMP */

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A short read is not the end of the file, only the end of this one. */
static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		int got = pt_read(fd, p, n);

		if (got <= 0)
			return -EIO;
		p += got;
		n -= got;
	}
	return 0;
}

int canvas_bmp(struct canvas *c, int fd)
{
	uint8_t head[54];
	uint32_t offset;
	int bpp, stride, top_down = 0;
	uint8_t *row;
	int32_t h;

	if (pt_lseek(fd, 0, SEEK_SET) < 0 || read_full(fd, head, sizeof(head)))
		return -EIO;
	if (head[0] != 'B' || head[1] != 'M')
		return -ENOTSUP;
	offset = le32(head + 10);
	if (le32(head + 14) < 12)
		return -EINVAL;
	c->sw = (int)le32(head + 18);
	h = (int32_t)le32(head + 22);
	if (h < 0) {
		top_down = 1;			/* a positive height means bottom first */
		h = -h;
	}
	c->sh = h;
	bpp = head[28] | head[29] << 8;
	if (le32(head + 30) != 0)
		return -ENOTSUP;		/* compressed: not this decoder */
	if (c->sw <= 0 || c->sh <= 0 || (bpp != 16 && bpp != 24 && bpp != 32))
		return -ENOTSUP;

	canvas_fit(c);
	stride = (c->sw * bpp / 8 + 3) & ~3;
	row = pt_malloc(stride);
	if (!row)
		return -ENOMEM;
	if (pt_lseek(fd, offset, SEEK_SET) < 0) {
		pt_free(row);
		return -EIO;
	}

	/*
	 * Straight through the file, never seeking: rows are usually
	 * stored bottom first, and asking a FAT driver for them in
	 * reverse costs more than the whole rest of the decode. Each row
	 * is placed where it belongs, and the ones the scaling throws
	 * away are read but not converted.
	 */
	for (int file_row = 0; file_row < c->sh; file_row++) {
		int sy = top_down ? file_row : c->sh - 1 - file_row;
		int dy = canvas_y(c, sy);

		if (read_full(fd, row, stride)) {
			pt_free(row);
			return -EIO;
		}
		if (file_row && dy == canvas_y(c, top_down ? sy - 1 : sy + 1))
			continue;
		for (int sx = 0; sx < c->sw; sx++) {
			const uint8_t *p = row + sx * (bpp / 8);
			int r, g, b;

			if (bpp == 16) {
				unsigned v = p[0] | p[1] << 8;

				r = (v >> 11 & 0x1f) << 3;
				g = (v >> 5 & 0x3f) << 2;
				b = (v & 0x1f) << 3;
			} else {
				b = p[0];
				g = p[1];
				r = p[2];
			}
			canvas_pixel(c, canvas_x(c, sx), dy, r, g, b);
		}
	}
	pt_free(row);
	return 0;
}

/* ------------------------------------------------------------ JPEG */

struct jpeg_io {
	struct canvas	*c;
	int		 fd;		/* -1 when the picture is in memory */
	const uint8_t	*data;
	size_t		 len, at;
};

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len)
{
	struct jpeg_io *io = jd->device;

	if (io->fd >= 0) {
		if (!buf)
			return pt_lseek(io->fd, len, SEEK_CUR) < 0 ? 0 : len;
		int n = pt_read(io->fd, buf, len);
		return n > 0 ? (UINT)n : 0;
	}
	if (len > io->len - io->at)
		len = io->len - io->at;
	if (buf)
		memcpy(buf, io->data + io->at, len);
	io->at += len;
	return len;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
	struct jpeg_io *io = jd->device;
	struct canvas *c = io->c;
	const uint8_t *src = bitmap;		/* RGB888: the ROM's format */
	int rw = rect->right - rect->left + 1;

	/*
	 * A clip made for this screen arrives at the size it will be
	 * shown, which is the case worth being quick about: a rectangle
	 * of the picture is then a rectangle of the canvas, and the
	 * pixels go straight in without a multiply and a divide each.
	 * The picture is centred, so the whole rectangle is on screen.
	 */
	if (c->dw == c->sw && c->dh == c->sh) {
		for (int y = rect->top; y <= rect->bottom; y++, src += rw * 3) {
			uint8_t *dst = c->px + ((size_t)(c->y0 + y) * c->w +
						c->x0 + rect->left) * 2;
			const uint8_t *p = src;

			for (int x = 0; x < rw; x++, p += 3) {
				unsigned v = (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3;

				*dst++ = v >> 8;
				*dst++ = v;
			}
		}
		return 1;
	}
	for (int y = rect->top; y <= rect->bottom; y++) {
		int dy = canvas_y(c, y);

		for (int x = rect->left; x <= rect->right; x++, src += 3)
			canvas_pixel(c, canvas_x(c, x), dy, src[0], src[1], src[2]);
	}
	return 1;
}

static int jpeg_decode(struct canvas *c, struct jpeg_io *io)
{
	JDEC jd;
	void *pool = pt_malloc(JPEG_POOL);
	JRESULT r;
	BYTE scale = 0;

	if (!pool)
		return -ENOMEM;
	r = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL, io);
	if (r != JDR_OK) {
		pt_free(pool);
		return r == JDR_FMT3 ? -ENOTSUP : -EINVAL;
	}
	/*
	 * The decoder can halve the picture up to three times as it goes,
	 * which is quicker and kinder to memory than decoding a photo at
	 * full size only to throw most of it away. What is left over is
	 * scaled the rest of the way as the pixels arrive.
	 */
	while (scale < 3 && ((jd.width >> scale) > (UINT)c->w * 2 ||
			     (jd.height >> scale) > (UINT)c->h * 2))
		scale++;
	c->sw = (int)(jd.width >> scale);
	c->sh = (int)(jd.height >> scale);
	if (c->sw < 1 || c->sh < 1) {
		pt_free(pool);
		return -ENOTSUP;
	}
	canvas_fit(c);
	r = jd_decomp(&jd, jpeg_out, scale);
	pt_free(pool);
	return r == JDR_OK ? 0 : -EIO;
}

int canvas_jpeg(struct canvas *c, int fd)
{
	struct jpeg_io io = { .c = c, .fd = fd };

	if (pt_lseek(fd, 0, SEEK_SET) < 0)
		return -EIO;
	return jpeg_decode(c, &io);
}

int canvas_jpeg_mem(struct canvas *c, const void *data, size_t len)
{
	struct jpeg_io io = { .c = c, .fd = -1, .data = data, .len = len };

	return jpeg_decode(c, &io);
}

#endif /* CONFIG_PT_LCD */
