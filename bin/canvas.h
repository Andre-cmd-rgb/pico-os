/*
 * Pictures on the panel, shared by the viewer and the video player.
 *
 * A canvas is the screen: a buffer of RGB565 in PSRAM that a decoder
 * writes into and that is then sent to the panel in strips through a
 * buffer the SPI hardware can reach. The picture being decoded has its
 * own size, and is scaled to fit and centred.
 */
#pragma once

#include "pt/sys.h"

struct canvas {
	uint8_t *px;			/* the panel, RGB565, high byte first */
	int	 w, h;			/* the panel's size */
	int	 x0, y0, dw, dh;	/* where the picture goes in it */
	int	 sw, sh;		/* the picture's own size */
};

int	canvas_open(struct canvas *c);		/* -ENOMEM or -ENODEV */
void	canvas_close(struct canvas *c);
void	canvas_clear(struct canvas *c);
void	canvas_fit(struct canvas *c);		/* sw/sh in, x0/y0/dw/dh out */
void	canvas_blit(const struct canvas *c);	/* the whole panel */
void	canvas_blit_fit(const struct canvas *c);	/* only where the picture is */

/* Where a pixel of the picture lands, and how to put it there. */
int	canvas_x(const struct canvas *c, int sx);
int	canvas_y(const struct canvas *c, int sy);
void	canvas_pixel(struct canvas *c, int dx, int dy, int r, int g, int b);

/*
 * Decoders. Each sets sw/sh, fits the picture and fills the canvas; the
 * caller blits when it is ready to. BMP is read here; JPEG goes to the
 * decompressor in the chip's ROM, and must be baseline, not progressive.
 */
int	canvas_bmp(struct canvas *c, int fd);
int	canvas_jpeg(struct canvas *c, int fd);
int	canvas_jpeg_mem(struct canvas *c, const void *data, size_t len);

/* What the canvas is showing, as a BMP. Used by the s key in both. */
int	canvas_save(struct canvas *c, const char *dir, char *path, size_t size);
