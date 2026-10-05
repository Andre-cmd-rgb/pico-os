/*
 * Baseline JPEG, decoded to RGB565 for the panel.
 *
 * jpeg_open() reads the headers; the picture's size is then known and the
 * caller picks a scale (1, 1/2, 1/4 or 1/8) and a buffer for one band,
 * which is a row of MCUs: 8 or 16 lines of the picture. jpeg_decode()
 * fills the band a row of MCUs at a time and hands each to a callback
 * that puts it where it belongs, so a picture of any size needs only one
 * band of memory, and the band can live in the fast internal RAM while
 * the picture goes to PSRAM.
 *
 * Plain C with nothing from the rest of the system, so that it builds on
 * the PC too, where tools/jpeg_test.c checks it against ffmpeg.
 *
 * What it takes: baseline and extended sequential Huffman JPEG (SOF0 and
 * SOF1) with 8-bit samples, greyscale or YCbCr, luma sampled 1x1, 2x1,
 * 1x2 or 2x2 with chroma at 1x1 -- which is what cameras, phones, the web
 * and ffmpeg make -- with or without restart markers.
 *
 * Progressive Huffman JPEG (SOF2), the same shapes, only at an eighth:
 * that is one sample a block, which its first scans -- the DC ones -- hold
 * whole, so the rest of the file is passed over. The caller gives it the
 * memory for one value a block (jpeg_dc_size(), set in `blocks` after
 * jpeg_open()); a 1400-pixel cover needs under 100 KB. Arithmetic-coded
 * files, CMYK and 12-bit samples are -ENOTSUP.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define JPEG_FAST	9		/* bits looked up at once in a Huffman table */

struct jpeg_huff {
	uint8_t		fast[1 << JPEG_FAST];	/* symbol index for a short code, 255 if longer */
	uint8_t		size[256];		/* code length of each symbol index */
	uint8_t		values[256];
	uint32_t	maxcode[18];		/* first code past each length, top-aligned */
	int32_t		delta[17];		/* symbol index minus code, for each length */
};

struct jpeg_comp {
	int		id, h, v;		/* sampling factors */
	int		tq, td, ta;		/* its quantisation, DC and AC tables */
	int		pred;			/* the previous block's DC */
	/* For the scale being decoded: each block is averaged down by
	 * 2^rx x 2^ry, to bw samples a line, and each sample then covers
	 * 2^ux x 2^uy pixels of the picture. */
	int		rx, ry, ux, uy, bw;
};

struct jpeg {
	/* Where the bytes come from. `refill` is called when p reaches end,
	 * and sets both again; it returns 0 at the end of the file. */
	const uint8_t	*p, *end;
	int		(*refill)(void *ctx, const uint8_t **p, const uint8_t **end);
	void		*ctx;

	int		width, height;		/* set by jpeg_open() */
	int		ncomp, hmax, vmax, mcux, mcuy, restart;
	uint8_t		progressive;
	int16_t		*blocks;		/* progressive: the caller's, one value a block */
	/* The scan being read: which components, which coefficients, which bits. */
	int		scan_n, ss, se, ah, al;
	uint8_t		scan[3];
	struct jpeg_comp comp[3];
	uint16_t	qt[4][64];		/* in natural order */
	uint32_t	qs[4][64];		/* the same with the IDCT's scaling in */
	struct jpeg_huff dc[2], ac[2];
	int16_t		fast_ac[2][1 << JPEG_FAST];
	uint8_t		have_dc, have_ac, have_qt;

	/* The entropy decoder: the next bit is the top bit of `bits`. */
	uint32_t	bits;
	int		nbits, marker;
	uint8_t		eof;

	/* Word-aligned: they are cleared and filled a word at a time. */
	_Alignas(4) int16_t coef[64];
	_Alignas(4) uint8_t y[256];		/* one MCU, after the IDCT */
	_Alignas(4) uint8_t cb[64];
	_Alignas(4) uint8_t cr[64];
	/* Red, green and blue from -256 to 511, clamped and already in their
	 * places in an RGB565 pixel: a pixel is three loads and two ORs. */
	uint16_t	rgb[3][768];
	/* What each chroma value adds, worked out once: for red and blue
	 * straight to where in those tables to look, for green its two
	 * halves, Cb's and Cr's. */
	const uint16_t	*red[256], *blue[256];
	int16_t		gcb[256], gcr[256];
};

/*
 * The callback for each band: `rows` lines of the picture starting at line
 * `y`, each `w` pixels of RGB565 high byte first, lines `stride` bytes
 * apart. Returning nonzero stops the decode with that value.
 */
typedef int (*jpeg_band_fn)(void *ctx, int y, int rows, const uint8_t *px, size_t stride);

/* The headers, up to the start of the picture: 0, -EINVAL or -ENOTSUP. */
int	jpeg_open(struct jpeg *j);

/* The picture's size at `scale` (0 is full size, 3 is an eighth). */
int	jpeg_scaled_w(const struct jpeg *j, int scale);
int	jpeg_scaled_h(const struct jpeg *j, int scale);

/* How big the band buffer handed to jpeg_decode() must be. */
size_t	jpeg_band_size(const struct jpeg *j, int scale);

/* Bytes for `blocks` that a progressive picture needs; 0 for any other. */
size_t	jpeg_dc_size(const struct jpeg *j);

/* The picture, band by band: 0, a callback's value, or -EINVAL if damaged;
 * -ENOTSUP for a progressive one at any scale but 3, or with no `blocks`. */
int	jpeg_decode(struct jpeg *j, int scale, uint8_t *band, jpeg_band_fn fn, void *ctx);
