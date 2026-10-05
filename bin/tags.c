/*
 * Music tags (tags.h).
 *
 * Written against what downloaded libraries are really like, not only the
 * specifications: 24-bit FLAC with an LRC LYRICS comment and a 1400-pixel
 * cover, MP3 with ID3v2.4, its lyrics in USLT and a cover in APIC. Every
 * length in a file is checked against the file before it is used: a
 * hostile file gets an empty tag, never a read outside what it holds.
 */
#include <errno.h>
#include <string.h>

#include "tags.h"

#define HEAD	512		/* what is read of a frame or a comment to look at it */
#define LAYOUT	4096		/* where an MP3's first frame is looked for */

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
}

/* ID3's sizes: seven bits a byte, so that no 0xff appears in them. */
static uint32_t synchsafe(const uint8_t *p)
{
	return (uint32_t)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

/* Exactly `len` bytes at `off`, or false. */
static bool get(tag_read_fn read, void *ctx, uint64_t off, void *buf, size_t len)
{
	return read(ctx, off, buf, len) == (int)len;
}

/* ------------------------------------------------------------ text */

/* A code point onto the end of out[n], if it fits with a NUL after: the new n. */
static size_t put(char *out, size_t n, size_t size, uint32_t c)
{
	char b[4];
	size_t k;

	if (c < 0x80) {
		b[0] = c;
		k = 1;
	} else if (c < 0x800) {
		b[0] = 0xc0 | c >> 6;
		b[1] = 0x80 | (c & 0x3f);
		k = 2;
	} else if (c < 0x10000) {
		b[0] = 0xe0 | c >> 12;
		b[1] = 0x80 | (c >> 6 & 0x3f);
		b[2] = 0x80 | (c & 0x3f);
		k = 3;
	} else {
		b[0] = 0xf0 | c >> 18;
		b[1] = 0x80 | (c >> 12 & 0x3f);
		b[2] = 0x80 | (c >> 6 & 0x3f);
		b[3] = 0x80 | (c & 0x3f);
		k = 4;
	}
	if (n + k >= size)
		return size;		/* full: what does not fit is left off whole */
	memcpy(out + n, b, k);
	return n + k;
}

/* The next code point of UTF-8 at s[*i], moving *i past it; 0xfffd for a bad byte. */
static uint32_t next_utf8(const uint8_t *s, size_t len, size_t *i)
{
	uint32_t c = s[(*i)++];
	int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

	if (c < 0x80)
		return c;
	if (!more)
		return 0xfffd;		/* a continuation byte on its own */
	c &= 0x3f >> more;
	for (; more; more--) {
		if (*i >= len || (s[*i] & 0xc0) != 0x80)
			return 0xfffd;
		c = c << 6 | (s[(*i)++] & 0x3f);
	}
	return c;
}

/*
 * Text in one of ID3's encodings into UTF-8, up to the first NUL (a frame
 * may hold several values; the first is taken). With `lines` newlines and
 * tabs stay, as lyrics need; otherwise every control character goes.
 * Returns the length written.
 */
static size_t to_utf8(int enc, const uint8_t *s, size_t len, char *out, size_t size, bool lines)
{
	size_t n = 0, i = 0;
	bool big = enc == TAG_UTF16BE;

	if ((enc == TAG_UTF16 || enc == TAG_UTF16BE) && len >= 2) {
		if (s[0] == 0xff && s[1] == 0xfe) {
			big = false;
			i = 2;
		} else if (s[0] == 0xfe && s[1] == 0xff) {
			big = true;
			i = 2;
		}
	} else if (enc == TAG_UTF8 && len >= 3 && !memcmp(s, "\xef\xbb\xbf", 3)) {
		i = 3;
	}
	while (i < len && n < size) {
		uint32_t c;

		if (enc == TAG_LATIN1) {
			c = s[i++];
		} else if (enc == TAG_UTF8) {
			c = next_utf8(s, len, &i);
		} else {
			if (i + 1 >= len)
				break;
			c = big ? s[i] << 8 | s[i + 1] : s[i + 1] << 8 | s[i];
			i += 2;
			if (c >= 0xd800 && c < 0xdc00 && i + 1 < len) {	/* a surrogate pair */
				uint32_t lo = big ? s[i] << 8 | s[i + 1] : s[i + 1] << 8 | s[i];

				if (lo >= 0xdc00 && lo < 0xe000) {
					c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
					i += 2;
				}
			}
		}
		if (!c)
			break;
		if (c == 0xfeff || c == '\r' || (c < 0x20 && !(lines && (c == '\n' || c == '\t'))))
			continue;
		n = put(out, n, size, c);
	}
	if (n >= size)
		n = size ? size - 1 : 0;
	while (!lines && n && out[n - 1] == ' ')
		n--;
	if (size)
		out[n] = '\0';
	return n;
}

/* Into a field of struct tags, unless it already has something. */
static void set(char *field, int enc, const uint8_t *s, size_t len)
{
	char *p;

	if (field[0])
		return;
	to_utf8(enc, s, len, field, TAG_TEXT, false);
	for (p = field; *p == ' '; p++)
		;
	memmove(field, p, strlen(p) + 1);
}

/* "3/12" -> 3, "2024-05-01" -> 2024: the number at the start, or 0. */
static int number(const uint8_t *s, size_t len, int enc)
{
	char text[16];
	int v = 0;

	to_utf8(enc, s, len, text, sizeof(text), false);
	for (char *p = text; *p >= '0' && *p <= '9' && v < 100000; p++)
		v = v * 10 + (*p - '0');
	return v;
}

/* ID3 unsynchronisation undone: 0xff 0x00 back to 0xff. The new length. */
static size_t unsync(uint8_t *s, size_t len)
{
	size_t n = 0;

	for (size_t i = 0; i < len; i++) {
		s[n++] = s[i];
		if (s[i] == 0xff && i + 1 < len && s[i + 1] == 0x00)
			i++;
	}
	return n;
}

size_t tags_utf8(const struct tag_blob *b, char *buf, size_t len, size_t room)
{
	uint8_t *raw;

	if (!room)
		return 0;
	if (b->unsync)
		len = unsync((uint8_t *)buf, len);
	if (len * 2 + 1 > room)
		len = (room - 1) / 2;
	/* from the far end, so the text growing from the start never meets it */
	raw = (uint8_t *)buf + room - len;
	memmove(raw, buf, len);
	return to_utf8(b->enc, raw, len, buf, room, true);
}

void tags_title_from_name(const char *name, char *out, size_t size)
{
	const char *slash = strrchr(name, '/'), *dot, *dash, *p;
	size_t n;

	p = slash ? slash + 1 : name;
	dot = strrchr(p, '.');
	n = dot && dot != p ? (size_t)(dot - p) : strlen(p);
	/* a track number first, up to three digits and a mark: "01. ", "01 - ",
	 * "7) " -- but not "1999 Song", which is a title */
	if (n > 2 && p[0] >= '0' && p[0] <= '9') {
		size_t i = 0;

		while (i < n && i < 3 && p[i] >= '0' && p[i] <= '9')
			i++;
		while (i < n && p[i] == ' ')
			i++;
		if (i < n && (p[i] == '-' || p[i] == '.' || p[i] == '_' || p[i] == ')')) {
			for (i++; i < n && p[i] == ' '; i++)
				;
			if (i < n) {
				p += i;
				n -= i;
			}
		}
	}
	/* "Artist - Title": the title */
	for (dash = p; dash + 3 <= p + n; dash++)
		if (!memcmp(dash, " - ", 3)) {
			n -= dash + 3 - p;
			p = dash + 3;
			break;
		}
	if (!size)
		return;
	if (n >= size)
		n = size - 1;
	memcpy(out, p, n);
	out[n] = '\0';
}

/* ------------------------------------------------------------ FLAC */

static bool key_is(const uint8_t *s, size_t key, const char *name)
{
	size_t n = strlen(name);

	if (key != n)
		return false;
	for (size_t i = 0; i < n; i++) {
		uint8_t c = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i];

		if (c != (uint8_t)name[i])
			return false;
	}
	return true;
}

/* One "KEY=value" comment, its head in `s`, the whole `len` long at `off`. */
static void comment(struct tags *t, const uint8_t *s, size_t got, uint64_t off, uint32_t len)
{
	const uint8_t *eq = memchr(s, '=', got);
	size_t key, vlen;
	const uint8_t *v;

	if (!eq)
		return;
	key = eq - s;
	v = eq + 1;
	vlen = got - key - 1;
	if (key_is(s, key, "TITLE")) {
		set(t->title, TAG_UTF8, v, vlen);
	} else if (key_is(s, key, "ARTIST")) {
		set(t->artist, TAG_UTF8, v, vlen);
	} else if (key_is(s, key, "ALBUM")) {
		set(t->album, TAG_UTF8, v, vlen);
	} else if (key_is(s, key, "ALBUMARTIST") || key_is(s, key, "ALBUM ARTIST") ||
		   key_is(s, key, "ALBUM_ARTIST")) {
		set(t->album_artist, TAG_UTF8, v, vlen);
	} else if (key_is(s, key, "TRACKNUMBER")) {
		t->track = t->track ? t->track : number(v, vlen, TAG_UTF8);
	} else if (key_is(s, key, "DISCNUMBER")) {
		t->disc = t->disc ? t->disc : number(v, vlen, TAG_UTF8);
	} else if (key_is(s, key, "DATE") || key_is(s, key, "YEAR") || key_is(s, key, "ORIGINALDATE")) {
		t->year = t->year ? t->year : number(v, vlen, TAG_UTF8);
	} else if ((key_is(s, key, "LYRICS") || key_is(s, key, "UNSYNCEDLYRICS") ||
		    key_is(s, key, "SYNCEDLYRICS")) && !t->lyrics.len && len > key + 1) {
		t->lyrics = (struct tag_blob){ .off = off + key + 1, .len = len - key - 1,
					       .enc = TAG_UTF8 };
	}
}

static void comments(struct tags *t, tag_read_fn read, void *ctx, uint64_t at, uint64_t end)
{
	uint8_t s[HEAD];
	uint32_t n;

	if (at + 4 > end || !get(read, ctx, at, s, 4))
		return;
	at += 4 + le32(s);			/* past the vendor string */
	if (at + 4 > end || !get(read, ctx, at, s, 4))
		return;
	n = le32(s);
	at += 4;
	for (uint32_t i = 0; i < n && i < 4096 && at + 4 <= end; i++) {
		uint32_t len;
		size_t got;

		if (!get(read, ctx, at, s, 4))
			return;
		len = le32(s);
		at += 4;
		if (len > end - at)
			return;
		got = len < sizeof(s) ? len : sizeof(s);
		if (!get(read, ctx, at, s, got))
			return;
		comment(t, s, got, at, len);
		at += len;
	}
}

/* A JPEG's first bytes: the only kind of picture music can show. */
static bool is_jpeg(tag_read_fn read, void *ctx, uint64_t off)
{
	uint8_t m[3];

	return get(read, ctx, off, m, 3) && m[0] == 0xff && m[1] == 0xd8 && m[2] == 0xff;
}

/* A cover found: the front cover wins over any other picture. */
static void cover(struct tags *t, bool *front, int type, uint64_t off, uint64_t len, bool sync)
{
	if (len < 100 || len > 16 * 1024 * 1024 || (t->cover.len && (*front || type != 3)))
		return;
	t->cover = (struct tag_blob){ .off = off, .len = len, .unsync = sync };
	*front = type == 3;
}

static void picture(struct tags *t, bool *front, tag_read_fn read, void *ctx, uint64_t at, uint64_t end)
{
	uint8_t s[8];
	uint64_t p = at;
	uint32_t type;

	if (p + 8 > end || !get(read, ctx, p, s, 8))
		return;
	type = be32(s);
	p += 8 + (uint64_t)be32(s + 4);		/* past the MIME type */
	if (p + 4 > end || !get(read, ctx, p, s, 4))
		return;
	p += 4 + (uint64_t)be32(s) + 16;	/* the description, the size and depth */
	if (p + 4 > end || !get(read, ctx, p, s, 4))
		return;
	if (be32(s) <= end - p - 4 && is_jpeg(read, ctx, p + 4))
		cover(t, front, type, p + 4, be32(s), false);
}

static int read_flac(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint8_t h[34];
	uint64_t at = 4;
	bool last = false, front = false;

	t->flac = true;
	for (int blocks = 0; !last && blocks < 256 && at + 4 <= size; blocks++) {
		uint64_t body, len;
		int type;

		if (!get(read, ctx, at, h, 4))
			break;
		last = h[0] & 0x80;
		type = h[0] & 0x7f;
		len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
		body = at + 4;
		if (len > size - body)
			break;
		if (type == 0 && len >= 34 && get(read, ctx, body, h, 34)) {
			uint64_t total = (uint64_t)(h[13] & 0xf) << 32 | be32(h + 14);

			t->rate = (uint32_t)h[10] << 12 | h[11] << 4 | h[12] >> 4;
			t->channels = ((h[12] >> 1) & 7) + 1;
			t->bits = (((h[12] & 1) << 4) | (h[13] >> 4)) + 1;
			if (t->rate)
				t->ms = total * 1000 / t->rate;
		} else if (type == 4) {
			comments(t, read, ctx, body, body + len);
		} else if (type == 6) {
			picture(t, &front, read, ctx, body, body + len);
		}
		at = body + len;
	}
	return 0;
}

/* ------------------------------------------------------------ MP3 */

/* Past a string ending in the encoding's NUL: the index after it, or len. */
static size_t past_nul(const uint8_t *s, size_t i, size_t len, int enc)
{
	if (enc == TAG_UTF16 || enc == TAG_UTF16BE) {
		for (; i + 1 < len; i += 2)
			if (!s[i] && !s[i + 1])
				return i + 2;
		return len;
	}
	for (; i < len; i++)
		if (!s[i])
			return i + 1;
	return len;
}

static void id3_frame(struct tags *t, bool *front, tag_read_fn read, void *ctx, const char *id,
		      uint64_t at, uint32_t len, bool sync)
{
	uint8_t s[HEAD];
	size_t got = len < sizeof(s) ? len : sizeof(s), n;
	int enc;

	if (!len || !get(read, ctx, at, s, got))
		return;
	n = sync ? unsync(s, got) : got;
	enc = s[0] <= TAG_UTF8 ? s[0] : TAG_LATIN1;
	if (!memcmp(id, "TIT2", 4)) {
		set(t->title, enc, s + 1, n - 1);
	} else if (!memcmp(id, "TPE1", 4)) {
		set(t->artist, enc, s + 1, n - 1);
	} else if (!memcmp(id, "TPE2", 4)) {
		set(t->album_artist, enc, s + 1, n - 1);
	} else if (!memcmp(id, "TALB", 4)) {
		set(t->album, enc, s + 1, n - 1);
	} else if (!memcmp(id, "TRCK", 4)) {
		t->track = t->track ? t->track : number(s + 1, n - 1, enc);
	} else if (!memcmp(id, "TPOS", 4)) {
		t->disc = t->disc ? t->disc : number(s + 1, n - 1, enc);
	} else if (!memcmp(id, "TYER", 4) || !memcmp(id, "TDRC", 4) || !memcmp(id, "TDOR", 4)) {
		t->year = t->year ? t->year : number(s + 1, n - 1, enc);
	} else if (!memcmp(id, "USLT", 4) && !t->lyrics.len) {
		/* encoding, language, a description, then the words: located in
		 * the bytes as they are in the file, which is where they are read */
		size_t text = past_nul(s, 4, n, enc);

		if (text < n)
			t->lyrics = (struct tag_blob){ .off = at + text, .len = len - text,
						       .enc = enc, .unsync = sync };
	} else if (!memcmp(id, "TXXX", 4) && !t->lyrics.len) {
		/* a description and a value: some writers keep the lyrics so */
		char what[24];
		size_t value = past_nul(s, 1, n, enc);

		to_utf8(enc, s + 1, value - 1, what, sizeof(what), false);
		for (char *c = what; *c; c++)
			*c = *c >= 'a' && *c <= 'z' ? *c - 32 : *c;
		if (value < n && (!strcmp(what, "LYRICS") || !strcmp(what, "USLT") ||
				  !strcmp(what, "UNSYNCEDLYRICS") || !strncmp(what, "LYRICS-", 7)))
			t->lyrics = (struct tag_blob){ .off = at + value, .len = len - value,
						       .enc = enc, .unsync = sync };
	} else if (!memcmp(id, "APIC", 4)) {
		size_t p = past_nul(s, 1, n, TAG_LATIN1);	/* the MIME type */
		int type;

		if (p >= n)
			return;
		type = s[p];
		p = past_nul(s, p + 1, n, enc);			/* the description */
		if (p < n && p < len && is_jpeg(read, ctx, at + p))
			cover(t, front, type, at + p, len - p, sync);
	}
}

/* The ID3v2 tag at the start: where the audio begins after it. */
static uint64_t read_id3v2(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint8_t h[10];
	uint64_t p = 10, end, audio;
	bool sync, front = false;
	int version;

	if (!get(read, ctx, 0, h, 10) || memcmp(h, "ID3", 3) || ((h[6] | h[7] | h[8] | h[9]) & 0x80))
		return 0;
	version = h[3];
	audio = 10 + (uint64_t)synchsafe(h + 6) + (h[5] & 0x10 ? 10 : 0);
	end = audio < size ? audio : size;
	if (version < 3 || version > 4)
		return audio;			/* 2.2's three-letter frames: ID3v1 may say */
	sync = h[5] & 0x80;
	/*
	 * 2.3 unsynchronises the tag as a whole, and readers disagree about
	 * whether its frames' sizes count the bytes before or after: rather
	 * than read a title from the wrong place, ID3v1 and the name say.
	 */
	if (sync && version == 3)
		return audio;
	if (h[5] & 0x40) {			/* an extended header */
		if (!get(read, ctx, p, h, 4))
			return audio;
		p += version == 4 ? synchsafe(h) : be32(h) + 4;
	}
	for (int frames = 0; frames < 2048 && p + 10 <= end; frames++) {
		uint64_t body;
		uint32_t len;
		bool frame_sync;
		char id[4];

		if (!get(read, ctx, p, h, 10))
			break;
		memcpy(id, h, 4);
		for (int i = 0; i < 4; i++)
			if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
				return audio;	/* the padding after the last frame */
		len = version == 4 ? synchsafe(h + 4) : be32(h + 4);
		/* some writers put 2.3's plain sizes in a 2.4 tag */
		if (version == 4 && len > end - p - 10 && be32(h + 4) && be32(h + 4) <= end - p - 10)
			len = be32(h + 4);
		if (!len || len > end - p - 10)
			break;
		body = p + 10;
		frame_sync = sync || (version == 4 && (h[9] & 0x02));
		if (version == 4 && (h[9] & 0x01)) {	/* a data length first */
			body += 4;
			len = len > 4 ? len - 4 : 0;
		}
		/* compressed or encrypted frames are passed over */
		if (!(version == 4 ? h[9] & 0x0c : h[9] & 0xc0))
			id3_frame(t, &front, read, ctx, id, body, len, frame_sync);
		p = body + len;
	}
	return audio;
}

/* The 128 bytes at the very end, for whatever ID3v2 left empty. */
static bool read_id3v1(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint8_t s[128];

	if (size < 128 || !get(read, ctx, size - 128, s, 128) || memcmp(s, "TAG", 3))
		return false;
	set(t->title, TAG_LATIN1, s + 3, 30);
	set(t->artist, TAG_LATIN1, s + 33, 30);
	set(t->album, TAG_LATIN1, s + 63, 30);
	if (!t->year)
		t->year = number(s + 93, 4, TAG_LATIN1);
	if (!t->track && !s[125] && s[126])
		t->track = s[126];
	return true;
}

static const int mp3_rates[3][3] = {
	{ 11025, 12000, 8000 },		/* MPEG 2.5 */
	{ 22050, 24000, 16000 },	/* MPEG 2 */
	{ 44100, 48000, 32000 },	/* MPEG 1 */
};
static const short mp3_kbps[2][16] = {
	{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
	{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
};

/* The first frame: the rate, and the time from a Xing table or the bitrate. */
static bool read_mpeg(struct tags *t, tag_read_fn read, void *ctx, uint64_t start, uint64_t audio_end)
{
	uint8_t s[LAYOUT];
	int n;

	if (start >= audio_end)
		return false;
	n = read(ctx, start, s, sizeof(s));
	for (int p = 0; n > 0 && p + 4 < n; p++) {
		int version, side, x;
		bool mpeg1, mono;

		if (s[p] != 0xff || (s[p + 1] & 0xe0) != 0xe0 || ((s[p + 1] >> 1) & 3) != 1)
			continue;
		version = (s[p + 1] >> 3) & 3;
		if (version == 1 || (s[p + 2] >> 4) == 15 || !(s[p + 2] >> 4) || ((s[p + 2] >> 2) & 3) == 3)
			continue;
		mpeg1 = version == 3;
		mono = (s[p + 3] >> 6) == 3;
		t->rate = mp3_rates[version == 0 ? 0 : version - 1][(s[p + 2] >> 2) & 3];
		t->channels = mono ? 1 : 2;
		t->kbps = mp3_kbps[!mpeg1][s[p + 2] >> 4];
		side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		x = p + 4 + side;
		if (x + 12 <= n && (!memcmp(s + x, "Xing", 4) || !memcmp(s + x, "Info", 4)) &&
		    (be32(s + x + 4) & 1)) {
			uint64_t frames = be32(s + x + 8);

			t->ms = frames * (mpeg1 ? 1152 : 576) * 1000 / t->rate;
		} else if (t->kbps) {
			t->ms = (audio_end - start - p) * 8 / t->kbps;
		}
		return true;
	}
	return false;
}

static int read_mp3(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint64_t start = read_id3v2(t, read, ctx, size);
	bool v1 = read_id3v1(t, read, ctx, size);

	if (!read_mpeg(t, read, ctx, start, v1 ? size - 128 : size) && !start)
		return -EINVAL;			/* no tag and no frame: not an MP3 */
	return 0;
}

/* ------------------------------------------------------------ WAV */

static int read_wav(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint8_t s[24];
	uint64_t at = 12;
	uint32_t bytes_per_second = 0;

	for (int chunks = 0; chunks < 64 && at + 8 <= size; chunks++) {
		uint32_t len;

		if (!get(read, ctx, at, s, 8))
			break;
		len = le32(s + 4);
		if (!memcmp(s, "fmt ", 4) && len >= 16 && get(read, ctx, at + 8, s, 16)) {
			t->channels = s[2] | s[3] << 8;
			t->rate = le32(s + 4);
			bytes_per_second = le32(s + 8);
			t->bits = s[14] | s[15] << 8;
		} else if (!memcmp(s, "data", 4)) {
			uint64_t data = len < size - at - 8 ? len : size - at - 8;

			if (bytes_per_second)
				t->ms = data * 1000 / bytes_per_second;
			break;
		}
		at += 8 + (uint64_t)len + (len & 1);
	}
	return 0;
}

int tags_read(struct tags *t, tag_read_fn read, void *ctx, uint64_t size)
{
	uint8_t head[12];

	memset(t, 0, sizeof(*t));
	if (size < 12 || !get(read, ctx, 0, head, sizeof(head)))
		return -EINVAL;
	if (!memcmp(head, "fLaC", 4))
		return read_flac(t, read, ctx, size);
	if (!memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4))
		return read_wav(t, read, ctx, size);
	return read_mp3(t, read, ctx, size);
}
