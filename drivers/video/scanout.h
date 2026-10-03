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

#define SCANOUT_MAX_BANDS	14
#define SCANOUT_EDGE_ROWS	8	/* the first band and the last, at least */

struct scanout_band {
	int	first, n;	/* rows [first, first + n), in the order the refresh meets them */
};

/* Rows that bring a band's pixels to the next 64-byte line, for direct PSRAM DMA. */
static inline int scanout_unit(int width)
{
	int unit = 64, a = width * 2;

	while (a) {
		int rem = unit % a;

		unit = a;
		a = rem;
	}
	return 64 / unit;
}

/*
 * Rows [first, first + rows) cut into bands; their number, -1 if too
 * many. Each band begins a whole number of units from the picture's
 * first row, so its pixels go straight from PSRAM; the rows over go with
 * the band sent first when the far end goes first (reversed), with the
 * last otherwise.
 *
 * The bands grow from a short first one and shrink to a short last one.
 * A frame may start once the refresh has left its first band, and must
 * finish its last before the next refresh comes to it; and while a band
 * is sent the refresh, faster than the bus, gets further ahead. Short at
 * the ends and no more than doubling, then, no band holds the frame back
 * more than the first and the last, and a frame can take longer than a
 * refresh without one meeting it.
 */
static inline int scanout_bands(int width, int first, int rows, bool reversed,
				struct scanout_band *out)
{
	int unit = scanout_unit(width);
	int mid = unit > 32 ? unit : 32;
	int edge = (SCANOUT_EDGE_ROWS + unit - 1) / unit * unit;
	int odd = rows % unit;
	int last = edge + (reversed ? 0 : odd);
	int done = 0, nb = 0;

	while (done < rows) {
		int left = rows - done;
		int n = nb ? edge << (nb < 3 ? nb : 3) : edge + (reversed ? odd : 0);

		if (nb == SCANOUT_MAX_BANDS)
			return -1;
		if (nb && n > mid)
			n = mid;
		/* what is left ends in a band twice the last, then the last */
		if (left <= last)
			n = left;
		else if (left <= last + 2 * edge)
			n = left - last;
		else if (left - n < last + 2 * edge)
			n = left - last - 2 * edge;
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
