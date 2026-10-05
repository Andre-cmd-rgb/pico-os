/*
 * bin/jpeg.c on the PC, driven by tools/jpeg_test.py.
 *
 *   jpeg_test FILE [SCALE]    decode FILE; write "W H" and a line, then
 *                             RGB888 (RGB565 widened) to stdout
 *   jpeg_test -f FILE N       damage FILE N different ways and decode each,
 *                             reporting only whether anything went wrong
 *   jpeg_test -t FILE N       decode FILE N times and print the time each
 *
 * Built with the sanitizers by the script, so "went wrong" includes any
 * read or write outside what the decoder was given.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../bin/jpeg.h"

struct image {
	int		w, h;
	uint8_t		*rgb;
};

static int band(void *ctx, int y, int rows, const uint8_t *px, size_t stride)
{
	struct image *im = ctx;

	for (int r = 0; r < rows; r++) {
		const uint8_t *s = px + r * stride;
		uint8_t *d = im->rgb + ((size_t)(y + r) * im->w) * 3;

		for (int x = 0; x < im->w; x++, s += 2, d += 3) {
			unsigned v = s[0] << 8 | s[1];
			unsigned r5 = v >> 11, g6 = v >> 5 & 63, b5 = v & 31;

			d[0] = (uint8_t)(r5 << 3 | r5 >> 2);
			d[1] = (uint8_t)(g6 << 2 | g6 >> 4);
			d[2] = (uint8_t)(b5 << 3 | b5 >> 2);
		}
	}
	return 0;
}

static int decode(const uint8_t *data, size_t len, int scale, struct image *im)
{
	static struct jpeg j;
	uint8_t *strip;
	int ret;

	memset(&j, 0xa5, sizeof(j));		/* nothing may rely on it being zero */
	j.p = data;
	j.end = data + len;
	j.refill = NULL;
	if ((ret = jpeg_open(&j)))
		return ret;
	im->w = jpeg_scaled_w(&j, scale);
	im->h = jpeg_scaled_h(&j, scale);
	im->rgb = calloc((size_t)im->w * im->h, 3);
	strip = malloc(jpeg_band_size(&j, scale));
	if (jpeg_dc_size(&j))			/* a progressive one's block values */
		j.blocks = malloc(jpeg_dc_size(&j));
	if (!im->rgb || !strip || (jpeg_dc_size(&j) && !j.blocks)) {
		free(strip);
		free(j.blocks);
		return -ENOMEM;
	}
	ret = jpeg_decode(&j, scale, strip, band, im);
	free(strip);
	free(j.blocks);
	return ret;
}

static uint8_t *load(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
		return NULL;
	buf = malloc((size_t)n ? (size_t)n : 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
		return NULL;
	fclose(f);
	*len = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	struct image im = { 0 };
	uint8_t *data;
	size_t len;
	int ret;

	if (argc >= 4 && !strcmp(argv[1], "-f")) {
		int n = atoi(argv[3]);

		if (!(data = load(argv[2], &len)))
			return 2;
		srand(12345);
		for (int i = 0; i < n; i++) {
			uint8_t *bad = malloc(len);
			size_t cut = len;

			memcpy(bad, data, len);
			for (int k = rand() % 8 + 1; k--;)
				bad[rand() % len] ^= (uint8_t)(1 << rand() % 8);
			if (i % 4 == 0)
				cut = (size_t)rand() % len;	/* truncated, too */
			decode(bad, cut, rand() % 4, &im);
			free(im.rgb);
			im.rgb = NULL;
			free(bad);
		}
		free(data);
		printf("%d damaged decodes survived\n", n);
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "-t")) {
		int n = atoi(argv[3]);
		struct timespec a, b;

		if (!(data = load(argv[2], &len)))
			return 2;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (int i = 0; i < n; i++) {
			decode(data, len, 0, &im);
			free(im.rgb);
		}
		clock_gettime(CLOCK_MONOTONIC, &b);
		free(data);
		printf("%.3f ms a decode\n",
		       ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / n);
		return 0;
	}
	if (argc < 2 || !(data = load(argv[1], &len))) {
		fprintf(stderr, "usage: jpeg_test FILE [SCALE] | -f FILE N | -t FILE N\n");
		return 2;
	}
	ret = decode(data, len, argc > 2 ? atoi(argv[2]) : 0, &im);
	free(data);
	if (ret) {
		free(im.rgb);
		printf("error %d\n", ret);
		return 1;
	}
	printf("%d %d\n", im.w, im.h);
	fwrite(im.rgb, 3, (size_t)im.w * im.h, stdout);
	free(im.rgb);
	return 0;
}
