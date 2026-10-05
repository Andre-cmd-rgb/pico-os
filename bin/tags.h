/*
 * What a music file says about itself: FLAC's Vorbis comments and
 * pictures, MP3's ID3v2 (2.3 and 2.4) and ID3v1, and how long it plays.
 *
 * The file is read through a callback, a few small reads at a time: the
 * cover and the lyrics are only found -- where they are and how long --
 * not read, so a whole library is scanned quickly, and music reads them
 * when a song is played. Plain C with nothing from the rest of the system,
 * so that it builds on the PC too, where tools/tags_test.c checks it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAG_TEXT	96	/* bytes kept of a title, an artist or an album, as UTF-8 */

enum tag_enc {			/* ID3's text encodings; FLAC is always UTF-8 */
	TAG_LATIN1,
	TAG_UTF16,		/* with a byte-order mark */
	TAG_UTF16BE,
	TAG_UTF8,
};

/* A piece of the file found but not read: a cover, or the lyrics. */
struct tag_blob {
	uint64_t off;
	uint32_t len;		/* 0: there is none */
	uint8_t	 enc;		/* the lyrics' enum tag_enc */
	bool	 unsync;	/* ID3 unsynchronisation to undo once read */
};

struct tags {
	char	 title[TAG_TEXT], artist[TAG_TEXT], album[TAG_TEXT], album_artist[TAG_TEXT];
	int	 track, disc, year;	/* 0 when the file does not say */
	uint32_t ms;			/* how long it plays; 0 if it cannot be told */
	int	 rate, channels;
	int	 bits;			/* a FLAC's; 0 for MP3 */
	int	 kbps;			/* an MP3's first frame */
	bool	 flac;
	struct tag_blob cover;		/* JPEG: the front cover if one is marked so */
	struct tag_blob lyrics;
};

/* Reads `len` bytes at `off`: how many, fewer only at the end, or -errno. */
typedef int (*tag_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

/*
 * The tags of a file of `size` bytes: 0, or -EINVAL when it is neither
 * FLAC nor MP3. A damaged tag is read as far as it makes sense; whatever
 * could not be read is left empty.
 */
int	tags_read(struct tags *t, tag_read_fn read, void *ctx, uint64_t size);

/*
 * A text blob once read (the lyrics), turned into UTF-8 in place: ID3's
 * unsynchronisation undone, a byte-order mark and NULs dropped. `buf`
 * needs room for half again `len` (Latin-1 grows) plus a NUL; the new
 * length.
 */
size_t	tags_utf8(const struct tag_blob *b, char *buf, size_t len, size_t room);

/* "01. Artist - Title.flac" -> "Title": for a file with no title tag. */
void	tags_title_from_name(const char *name, char *out, size_t size);
