/*
 * Sound file decoders.
 *
 * One interface for every format, the way a filesystem driver plugs into
 * the VFS: open a file descriptor, ask what it is, then read frames until
 * the end. The decoders are in codec/<format>.c and nothing outside this
 * directory knows how any of them work.
 *
 * Samples come out as 24 bits in an int32, CODEC_FULL being full scale,
 * whatever the file had: a 16-bit one shifted up, a 24-bit FLAC as it is,
 * MP3 straight from the decoder's floating point. What an MP3 overshoots
 * full scale by is kept, not clipped, for the mixer to bring down.
 *
 *	struct codec *c;
 *	if (!codec_open(fd, &c)) {
 *		while ((n = codec_read(c, pcm, FRAMES)) > 0)
 *			play(pcm, n);
 *		codec_close(c);
 *	}
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct codec;

#define CODEC_FULL	8388607		/* = the mixer's MIX_FULL */
#define CODEC_OVER	(1 << 28)	/* how far over it a sample may go */

struct codec_ops {
	const char *name;
	/* Reads interleaved frames: how many, 0 at the end, -errno. */
	ssize_t	(*read)(struct codec *c, int32_t *pcm, size_t frames);
	/* Moves to a frame: where reading starts now, or -errno. May be NULL. */
	int64_t	(*seek)(struct codec *c, uint64_t frame);
	void	(*close)(struct codec *c);
};

struct codec {
	const struct codec_ops	*ops;
	int			 fd;
	int			 rate;		/* Hz */
	int			 channels;	/* 1 or 2 */
	int			 bits;		/* in the file; 0 when it is not PCM (MP3) */
	uint64_t		 frames;	/* 0 when the file does not say */
};

/*
 * Reads the first bytes of `fd`, picks a decoder and sets it up. Returns 0,
 * or -errno; -ENOTSUP when nothing recognises the file. Takes over the file
 * position but not the descriptor: the caller still closes it.
 */
int	codec_open(int fd, struct codec **out);

/* Raw 16-bit mono at `rate`, for a file no decoder claims. */
int	codec_open_raw(int fd, int rate, struct codec **out);

ssize_t	codec_read(struct codec *c, int32_t *pcm, size_t frames);
void	codec_close(struct codec *c);

/*
 * Moves to `frame` (one sample of every channel; a frame past the end
 * means the end) and returns the frame reading starts at now: exactly
 * that one for WAV and FLAC, and for MP3 where the file's own table of
 * contents, or failing that its bitrate, puts it -- MP3 frames carry no
 * numbers. -ENOTSUP for raw samples.
 */
int64_t	codec_seek(struct codec *c, uint64_t frame);
const char *codec_name(const struct codec *c);

/*
 * The 44-byte header of a 16-bit PCM WAV file, for `rec`. Write it before
 * the samples with `bytes` = 0, then again with the real length once the
 * recording has stopped.
 */
#define WAV_HEADER_BYTES	44
void	wav_header(uint8_t *out, int rate, int channels, uint32_t bytes);

/* Each format's constructor; codec.c is the only caller. */
int	wav_open(int fd, const uint8_t *head, size_t n, struct codec **out);
int	flac_open(int fd, const uint8_t *head, size_t n, struct codec **out);
int	mp3_open(int fd, const uint8_t *head, size_t n, struct codec **out);

/*
 * A recorded voice cleaned as it comes in (voice.c): rumble, hiss, level
 * and peaks. `reduce_db` is how far the hiss may be taken down, 15 being
 * about right; `target_db` the speech's level, -20 being loud. voice_run
 * works in place on 16-bit mono and hands the sound back 16 ms late.
 */
struct voice;
struct voice *voice_new(int rate, float reduce_db, float target_db);
void	voice_run(struct voice *v, int16_t *pcm, size_t n);
void	voice_free(struct voice *v);
