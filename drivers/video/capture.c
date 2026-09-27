/*
 * Writing a screenshot out.
 *
 * BMP, because every machine opens one without being asked twice, and
 * because writing it costs nothing: no compression, no second copy of
 * the image, one row at a time straight from the capture buffer.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_LCD

#define HEADER_BYTES	54

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

/*
 * What a failed write was, from the C library's errno: a full disk (a
 * few screenshots fill /tmp) says so, rather than "I/O error".
 */
static int write_error(void)
{
	return errno == ENOSPC ? -ENOSPC : -EIO;
}

int lcd_capture_save(const char *path)
{
	uint8_t header[HEADER_BYTES] = { 'B', 'M' };
	char vfs[PT_PATH_MAX + 16];
	const uint8_t *pixels;
	uint8_t *row;
	int w, h, ret = 0;
	FILE *f;

	pixels = lcd_capture_pixels(&w, &h);
	if (!pixels)
		return -ENODATA;
	if (!mount_resolve(path, vfs, sizeof(vfs)))
		return -ENOENT;

	put32(header + 2, HEADER_BYTES + (uint32_t)w * h * 3);
	put32(header + 10, HEADER_BYTES);
	put32(header + 14, 40);			/* DIB header size */
	put32(header + 18, w);
	put32(header + 22, h);			/* positive: rows bottom first */
	header[26] = 1;				/* one plane */
	header[28] = 24;			/* bits per pixel */
	put32(header + 34, (uint32_t)w * h * 3);

	f = fopen(vfs, "wb");
	if (!f)
		return write_error();
	errno = 0;
	row = malloc((size_t)w * 3);
	if (!row) {
		fclose(f);
		remove(vfs);
		return -ENOMEM;
	}
	if (fwrite(header, 1, sizeof(header), f) != sizeof(header))
		ret = write_error();
	for (int y = h - 1; y >= 0 && !ret; y--) {
		const uint8_t *src = pixels + (size_t)y * w * 2;

		for (int x = 0; x < w; x++) {
			/* stored high byte first: rrrrrggg gggbbbbb */
			unsigned c = src[x * 2] << 8 | src[x * 2 + 1];

			row[x * 3] = (c & 0x1f) << 3;		/* blue */
			row[x * 3 + 1] = (c >> 5 & 0x3f) << 2;	/* green */
			row[x * 3 + 2] = (c >> 11) << 3;	/* red */
		}
		if (fwrite(row, 3, w, f) != (size_t)w)
			ret = write_error();
	}
	free(row);
	if (fclose(f) && !ret)
		ret = write_error();
	if (ret)
		remove(vfs);		/* half a picture is no use to anyone */
	return ret;
}

#else

int lcd_capture_save(const char *path) { return -ENODEV; }

#endif
