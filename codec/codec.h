/*
 * Sound file decoders.
 *
 * One interface for every format, the way a filesystem driver plugs into
 * the VFS: open a file descriptor, ask what it is, then read 16-bit frames
 * until the end. The decoders are in codec/<format>.c and nothing outside
 * this directory knows how any of them work.
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

struct codec_ops {
	const char *name;
	/* Reads interleaved 16-bit frames: how many, 0 at the end, -errno. */
	ssize_t	(*read)(struct codec *c, int16_t *pcm, size_t frames);
	void	(*close)(struct codec *c);
};

struct codec {
	const struct codec_ops	*ops;
	int			 fd;
	int			 rate;		/* Hz */
	int			 channels;	/* 1 or 2 */
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

ssize_t	codec_read(struct codec *c, int16_t *pcm, size_t frames);
void	codec_close(struct codec *c);
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
