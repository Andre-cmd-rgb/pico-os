/*
 * Native scanout: the bands a frame goes out in, and the moments it may
 * start. Shared with the host checks; no hardware access here.
 *
 * The panel refreshes along its rows, `lines` a refresh counting the
 * porch, at `lines_per_us`; Get Scanline says where. A frame is a run of
 * bands queued back to back. A refresh that met a band half written would
 * show two frames, cut where it met it -- a seam. So every band must be
 * written after one refresh has read its rows and finished before the
 * next reads them, for the same pair of refreshes all through the frame:
 * then each refresh shows the old picture or the new one, whole.
 *
 * Time runs from the moment the line was read. The frame's pace is only
 * known within limits -- the bus's interrupts, the queueing, the panel's
 * clock -- so the earliest a band can be written (`fast`) is checked
 * against the refresh that must already have left it, and the latest
 * (`slow`) against the one that must not have reached it yet.
 */
#pragma once

#include <stdbool.h>

#define SCANOUT_MAX_BANDS	12

/* Whole bands keep direct PSRAM DMA aligned, including a cropped picture. */
static inline int scanout_band_rows(int width, int remaining, bool reversed)
{
	int bytes = width * 2, unit = 64, a = bytes;
	int n;

	while (a) {
		int rem = unit % a;

		unit = a;
		a = rem;
	}
	unit = 64 / unit;
	/* Start sending sooner after the first rows have passed. Odd widths
	 * still need 64 rows to keep the next DMA pointer aligned. */
	n = unit > 32 ? unit : 32;
	if (n > remaining)
		n = remaining;
	/* The far end comes first when the panel refreshes upwards. Send
	 * its short tail alone, rather than bouncing every full band. */
	if (reversed && remaining % unit)
		n = remaining % unit;
	return n;
}

struct scanout_band {
	int	first, n;	/* rows [first, first + n), in the order the refresh meets them */
};

/* Rows [first, first + rows) cut into whole bands; their number, -1 if too many. */
static inline int scanout_bands(int width, int first, int rows, bool reversed,
				struct scanout_band *out)
{
	int done = 0, nb = 0;

	while (done < rows) {
		int n = scanout_band_rows(width, rows - done, reversed);

		if (nb == SCANOUT_MAX_BANDS)
			return -1;
		out[nb].first = first + done;
		out[nb].n = n;
		nb++;
		done += n;
	}
	return nb;
}

struct scanout_pace {
	float	start_us;	/* from reading the line to the first band's pixels */
	float	band_us;	/* each band's own commands */
	float	bytes_per_us;	/* pixels on the wire */
};

struct scanout_panel {
	float	lines_per_us;
	int	lines;		/* a whole refresh, porch included */
	int	porch;		/* lines before the first row */
	int	margin;		/* lines kept clear of the refresh either side */
};

/* When band k starts and ends, in us from reading the line. */
static inline void scanout_band_times(const struct scanout_band *b, int k, int width,
				      const struct scanout_pace *p, float *start, float *end)
{
	float t = p->start_us;

	*start = *end = t;
	for (int i = 0; i <= k; i++) {
		float s = t + p->band_us;
		float e = s + b[i].n * width * 2 / p->bytes_per_us;

		if (i == k) {
			*start = s;
			*end = e;
		}
		t = e;
	}
}

/*
 * The lines at which a frame may start: up to two intervals, [lo, hi] in
 * Get Scanline's count, into `lo` and `hi`; their number. Either the
 * refresh going on when it starts has left each band before it is
 * written and the next has not come to it (it follows that refresh), or
 * that refresh has not reached any band before it is written (it runs
 * ahead of it, which a narrow picture, quick to send, can).
 */
static inline int scanout_window(const struct scanout_band *b, int nb, int width,
				 const struct scanout_panel *pn, const struct scanout_pace *fast,
				 const struct scanout_pace *slow, float lo[2], float hi[2])
{
	int found = 0;

	for (int pass = 0; pass < 2; pass++) {
		/* pass 0: behind the refresh in progress; 1: ahead of it */
		float low = 0, high = pn->lines - 1;

		for (int k = 0; k < nb; k++) {
			float s = 0, e = 0, unused = 0;
			float rows_end = b[k].first + b[k].n + pn->porch;
			float rows_start = b[k].first + pn->porch;

			scanout_band_times(b, k, width, fast, &s, &unused);
			scanout_band_times(b, k, width, slow, &unused, &e);
			/* the refresh before has read all of the band ... */
			float need = rows_end + pn->margin - pn->lines_per_us * s -
				     (pass ? pn->lines : 0);
			/* ... and the one after has not begun it */
			float limit = rows_start - pn->margin - pn->lines_per_us * e +
				      (pass ? 0 : pn->lines);

			if (need > low)
				low = need;
			if (limit < high)
				high = limit;
		}
		if (low <= high) {
			lo[found] = low;
			hi[found] = high;
			found++;
		}
	}
	return found;
}

/* Lines from `line` until a frame may start, going round the refresh. */
static inline float scanout_wait(float line, const struct scanout_panel *pn, int nw,
				 const float lo[2], const float hi[2])
{
	float best = -1;

	for (int i = 0; i < nw; i++) {
		float d;

		if (line >= lo[i] && line <= hi[i])
			return 0;
		d = lo[i] - line;
		if (d < 0)
			d += pn->lines;
		if (best < 0 || d < best)
			best = d;
	}
	return best;
}
