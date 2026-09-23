/*
 * Pictures on the panel.
 *
 * BMP is read here: the format is a header and rows of bytes, and reading
 * it takes less code than explaining why it should not be. JPEG goes to
 * jpeg.c, which hands it back a band of RGB565 at a time. Either way the
 * picture is fitted to the screen, keeping its shape, and each pixel of
 * the screen is taken from the nearest pixel of the picture -- the screen
 * pixel's, not the picture's, so that one smaller than the screen is
 * stretched without leaving gaps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "pt/kernel.h"
#include "canvas.h"
#include "jpeg.h"
#include "util.h"

#if CONFIG_PT_LCD

#define PX_ALIGN	64		/* the largest data cache line */
#define JPEG_READ	4096		/* a photo is read from the card this much at a time */
#define MAX_W		480		/* the widest panel a column map is kept for */

int canvas_open(struct canvas *c)
{
	if (!vt_has_display())
		return -ENODEV;
	c->w = lcd_width();
	c->h = lcd_height();
	/*
	 * On a cache line, which is what lets the panel's DMA read the
	 * picture straight out of PSRAM; anywhere else and the SPI driver
	 * would copy it somewhere that is, 32 KB at a time.
	 */
	c->mem = pt_malloc((size_t)c->w * c->h * 2 + PX_ALIGN - 1);
	c->px = (uint8_t *)(((uintptr_t)c->mem + PX_ALIGN - 1) & ~(uintptr_t)(PX_ALIGN - 1));
	return c->mem ? 0 : -ENOMEM;
}

void canvas_close(struct canvas *c)
{
	pt_free(c->mem);
	c->mem = NULL;
	c->px = NULL;
}

void canvas_clear(struct canvas *c)
{
	memset(c->px, 0, (size_t)c->w * c->h * 2);
}

/*
 * Where each column of the screen takes its pixel from, for a picture
 * that is not being shown at its own size.
 */
static void column_map(const struct canvas *c, uint16_t *map)
{
	for (int x = 0; x < c->dw; x++)
		map[x] = (uint16_t)((int64_t)x * c->sw / c->dw);
}

/*
 * Rows `sy` to `sy + rows` of the picture, RGB565 high byte first, onto
 * the screen rows that come from them: none, one or several, depending on
 * which way the picture is scaled.
 */
static void place_rows(struct canvas *c, const uint16_t *map, int sy, int rows,
		       const uint8_t *px, size_t stride)
{
	int first, end;

	if (c->dw == c->sw && c->dh == c->sh) {		/* shown at its own size */
		for (int r = 0; r < rows; r++)
			memcpy(c->px + ((size_t)(c->y0 + sy + r) * c->w + c->x0) * 2,
			       px + r * stride, (size_t)c->sw * 2);
		return;
	}
	/* screen row d shows picture row d * sh / dh */
	first = (int)(((int64_t)sy * c->dh + c->sh - 1) / c->sh);
	end = (int)(((int64_t)(sy + rows) * c->dh + c->sh - 1) / c->sh);
	for (int d = first; d < end && d < c->dh; d++) {
		const uint16_t *src = (const uint16_t *)(px + ((int64_t)d * c->sh / c->dh - sy) * stride);
		uint16_t *dst = (uint16_t *)(c->px + ((size_t)(c->y0 + d) * c->w + c->x0) * 2);

		for (int x = 0; x < c->dw; x++)
			dst[x] = src[map[x]];
	}
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

/*
 * Rows `y` to `y + h` of the canvas. Whole rows are one run of memory,
 * so the DMA takes them from where they are, with nothing copied and no
 * processor time spent: the call returns when the panel has them.
 */
static void blit_rows(const struct canvas *c, int y, int h)
{
	if (h > 0)
		lcd_draw(0, y, c->w, h, c->px + (size_t)y * c->w * 2);
}

void canvas_blit(const struct canvas *c)
{
	blit_rows(c, 0, c->h);
}

/*
 * Only the rows the picture covers. For a clip wider than the screen's
 * shape this is the difference between sending the black bars thirty
 * times a second and not sending them at all; the bars are already on
 * the panel from the first full blit. A picture narrower than the screen
 * is still sent in whole rows, bars and all: that is bus time, which is
 * spent while the next frame decodes, where cutting the bars out would
 * be processor time spent copying.
 */
void canvas_blit_fit(const struct canvas *c)
{
	if (c->dw > 0 && c->dh > 0)
		blit_rows(c, c->y0, c->dh);
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
	int bpp, stride, top_down = 0, ret = 0;
	uint8_t *row;
	uint16_t *line, *map;
	int32_t h;

	if (pt_lseek(fd, 0, SEEK_SET) < 0 || read_full(fd, head, sizeof(head)))
		return -EIO;
	if (head[0] != 'B' || head[1] != 'M')
		return -ENOTSUP;
	offset = le32(head + 10);
	if (le32(head + 14) < 40)
		return -ENOTSUP;		/* the old OS/2 header */
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
	if (c->sw <= 0 || c->sh <= 0 || c->sw > 1 << 15 || c->sh > 1 << 15 ||
	    (bpp != 16 && bpp != 24 && bpp != 32))
		return -ENOTSUP;

	canvas_fit(c);
	stride = (c->sw * bpp / 8 + 3) & ~3;
	row = pt_malloc(stride);
	line = pt_malloc((size_t)c->sw * 2);
	map = pt_malloc((size_t)c->dw * 2);
	if (!row || !line || !map) {
		ret = -ENOMEM;
		goto out;
	}
	column_map(c, map);
	if (pt_lseek(fd, offset, SEEK_SET) < 0) {
		ret = -EIO;
		goto out;
	}

	/*
	 * Straight through the file, never seeking: rows are usually
	 * stored bottom first, and asking a FAT driver for them in
	 * reverse costs more than the whole rest of the decode. Rows the
	 * scaling does not use are read but not converted.
	 */
	for (int file_row = 0; file_row < c->sh; file_row++) {
		int sy = top_down ? file_row : c->sh - 1 - file_row;
		int64_t first = ((int64_t)sy * c->dh + c->sh - 1) / c->sh;
		int64_t end = ((int64_t)(sy + 1) * c->dh + c->sh - 1) / c->sh;

		if (read_full(fd, row, stride)) {
			ret = -EIO;
			break;
		}
		if (first == end)
			continue;		/* no screen row shows this one */
		for (int sx = 0; sx < c->sw; sx++) {
			const uint8_t *p = row + sx * (bpp / 8);
			unsigned r, g, b, v;

			if (bpp == 16) {		/* X1R5G5B5, as BMP has it */
				v = p[0] | p[1] << 8;
				r = (v >> 10 & 0x1f) << 3;
				g = (v >> 5 & 0x1f) << 3;
				b = (v & 0x1f) << 3;
			} else {
				b = p[0];
				g = p[1];
				r = p[2];
			}
			v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
			line[sx] = (uint16_t)(v >> 8 | v << 8);
		}
		place_rows(c, map, sy, 1, (const uint8_t *)line, 0);
	}
out:
	pt_free(row);
	pt_free(line);
	pt_free(map);
	return ret;
}

/* ------------------------------------------------------------ JPEG */

/*
 * Everything a decode needs, in one allocation: the decoder's tables, the
 * column map and, for a file, what has been read of it -- all of it in
 * the fast internal memory when there is room, because every block of the
 * picture goes through it, and a buffer there is one the card can fill
 * without a bounce.
 */
struct jpeg_io {
	struct jpeg	 j;
	struct canvas	*c;
	int		 fd;		/* -1 when the picture is in memory */
	uint16_t	 map[MAX_W];
	uint8_t		 buf[];		/* JPEG_READ of the file, when there is one */
};

static int jpeg_refill(void *ctx, const uint8_t **p, const uint8_t **end)
{
	struct jpeg_io *io = ctx;
	int n = pt_read(io->fd, io->buf, JPEG_READ);

	if (n <= 0)
		return 0;
	*p = io->buf;
	*end = io->buf + n;
	return 1;
}

static int jpeg_band(void *ctx, int y, int rows, const uint8_t *px, size_t stride)
{
	struct jpeg_io *io = ctx;

	place_rows(io->c, io->map, y, rows, px, stride);
	return 0;
}

/* Internal memory if there is room, else any. Tracked, so a program
 * stopped halfway through a picture does not keep it for good. */
static void *fast_alloc(size_t n)
{
	void *p = pt_malloc_caps(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

	return p ? p : pt_malloc(n);
}

static int jpeg_decode_into(struct canvas *c, struct jpeg_io *io)
{
	struct jpeg *j = &io->j;
	uint8_t *band;
	int ret, scale = 0;

	if ((ret = jpeg_open(j)))
		return ret;
	if (c->placed) {
		/* The caller said where this goes; it had better be the
		 * size it said, or the picture would land crooked. */
		if (j->width != c->sw || j->height != c->sh)
			return -EINVAL;
	} else {
		c->sw = j->width;
		c->sh = j->height;
		canvas_fit(c);
		/*
		 * The decoder shrinks by averaging, which is what shrinking
		 * should be, so let it do as much as it can: the smallest
		 * of its sizes that is still no smaller than the screen's.
		 */
		while (scale < 3 && jpeg_scaled_w(j, scale + 1) >= c->dw &&
		       jpeg_scaled_h(j, scale + 1) >= c->dh)
			scale++;
		c->sw = jpeg_scaled_w(j, scale);
		c->sh = jpeg_scaled_h(j, scale);
	}
	column_map(c, io->map);		/* a placed picture can be scaled too */
	if (!(band = fast_alloc(jpeg_band_size(j, scale))))
		return -ENOMEM;
	ret = jpeg_decode(j, scale, band, jpeg_band, io);
	pt_free(band);
	return ret;
}

static int jpeg_run(struct canvas *c, int fd, const void *data, size_t len)
{
	struct jpeg_io *io;
	int ret;

	if (c->w > MAX_W)
		return -ENOTSUP;
	if (!(io = fast_alloc(sizeof(*io) + (fd >= 0 ? JPEG_READ : 0))))
		return -ENOMEM;
	io->c = c;
	io->fd = fd;
	io->j.p = data;
	io->j.end = (const uint8_t *)data + len;
	io->j.refill = fd >= 0 ? jpeg_refill : NULL;
	io->j.ctx = io;
	ret = jpeg_decode_into(c, io);
	pt_free(io);
	return ret;
}

int canvas_jpeg(struct canvas *c, int fd)
{
	if (pt_lseek(fd, 0, SEEK_SET) < 0)
		return -EIO;
	return jpeg_run(c, fd, NULL, 0);
}

int canvas_jpeg_mem(struct canvas *c, const void *data, size_t len)
{
	return jpeg_run(c, -1, data, len);
}

#endif /* CONFIG_PT_LCD */
