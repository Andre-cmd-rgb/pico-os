/* Whole bands keep direct PSRAM DMA aligned, including a cropped picture. */
#pragma once

#include <stdbool.h>

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
