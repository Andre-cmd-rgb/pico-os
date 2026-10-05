/*
 * The way to the speaker that play and music share (sink.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec.h"

#define SINK_PASS	1024	/* frames a decoder is asked for at a time */

struct half;

/*
 * Everything that plays goes through here as 24-bit frames, stereo as it
 * came: the mixer folds it for the speaker and keeps both sides, and all
 * the bits, for headphones.
 *
 * The checksum (of the frames mixed down to one and cut to 16 bits, as it
 * always was) and the timing are what `play -n` reports: it decodes
 * without touching the codec, which is how a file -- and the speed of the
 * decoder -- can be checked on a board with no speaker attached.
 */
struct sink {
	bool	 dry;			/* decode only: the checksum and the time */
	int	 rate, channels;	/* the file's */
	struct half *half;		/* when it is played at half its rate */
	uint64_t frames;
	uint32_t crc;
	int64_t	 started;
	int16_t	*mono;
};

int	sink_open(struct sink *s, bool dry);
void	sink_close(struct sink *s);		/* what is queued plays out */
/* The file's rate and channels, as they come: -EINVAL (and a message as
 * `prog`) for a rate it cannot play. */
int	sink_format(struct sink *s, const char *prog, const char *name, int rate, int channels);
/* Waits while a second is queued; -EINTR once a signal is pending. */
int	sink_play(struct sink *s, int32_t *pcm, size_t frames, int channels);
void	sink_report(const struct sink *s, const char *name, const struct codec *c);
