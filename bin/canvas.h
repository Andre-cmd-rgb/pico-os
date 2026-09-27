/*
 * Pictures on the panel, shared by the viewer and the video player.
 *
 * A canvas is the screen: a buffer of RGB565 in PSRAM that a decoder
 * writes into and that the panel's DMA then reads straight out of. The
 * picture being decoded has its own size, and is scaled to fit and
 * centred.
 */
#pragma once

#include "pt/sys.h"

struct canvas {
	uint8_t *px;			/* the panel, RGB565, high byte first */
	void	*mem;			/* what was allocated, px aligned inside it */
	int	 w, h;			/* the panel's size */
	int	 x0, y0, dw, dh;	/* where the picture goes in it */
	int	 sw, sh;		/* the picture's own size */
	bool	 placed;		/* the caller set the above; do not fit */
};

int	canvas_open(struct canvas *c);		/* -ENOMEM or -ENODEV */
void	canvas_close(struct canvas *c);
void	canvas_clear(struct canvas *c);
void	canvas_fit(struct canvas *c);		/* sw/sh in, x0/y0/dw/dh out */
void	canvas_blit(const struct canvas *c);	/* the whole panel */
void	canvas_blit_fit(const struct canvas *c);	/* only where the picture is */
/* The whole panel, without tearing: `native` is a 320x240x2 buffer, on a cache
 * line, for the turned copy. -ENOTSUP where the panel cannot be read. */
int	canvas_blit_native(const struct canvas *c, uint8_t *native);
/* The same in two halves: the turning, which is work for a core, and the
 * sending, which is mostly waiting on the panel. */
int	canvas_turn(const struct canvas *c, uint8_t *native);
int	canvas_send_native(const struct canvas *c, const uint8_t *native);

/*
 * Decoders. Each sets sw/sh, fits the picture and fills the canvas; the
 * caller blits when it is ready to. BMP is read here; JPEG is jpeg.c's,
 * and must be baseline, not progressive.
 *
 * With `placed` set the caller has already said where the picture goes
 * (x0, y0, dw, dh) and how big it is (sw, sh), and several canvases
 * sharing one set of pixels can decode into them at the same time: that
 * is how a video frame is split between the two cores.
 */
int	canvas_bmp(struct canvas *c, int fd);
int	canvas_jpeg(struct canvas *c, int fd);
int	canvas_jpeg_mem(struct canvas *c, const void *data, size_t len);


/* What the canvas is showing, as a BMP. Used by the s key in both. */
int	canvas_save(struct canvas *c, const char *dir, char *path, size_t size);
