/*
 * The decoders' cost on the ESP32-S3's own instruction set, run under
 * QEMU by tools/perfbench.py. QEMU with -icount counts instructions, not
 * the board's cycles -- no cache, no PSRAM waits -- so the numbers are for
 * comparing one version of the code with another, and every result
 * carries a CRC-32 of what was decoded, which a change that only makes
 * things faster must leave alone.
 *
 * The media come packed in one file (media.bin, made by the script):
 *
 *	"PBMEDIA1" count  { name[32] kind offset length }  data...
 *
 * Entries of one name add up to one result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#include "codec.h"
#include "jpeg.h"
#include "pt/sys.h"

#include "half.h"		/* sink.c's 2:1 filter, cut out by the script */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "levels.h"		/* the mixer's output stage */
#include "mixer.h"		/* its walk through a stream, cut out of audio.c */

#define PASS		1024	/* frames asked for at a time, as sink.c does */

enum { AUDIO = 1, JPEG_SLICE = 2, JPEG_DC = 3 };

struct entry {
	char		name[32];
	uint32_t	kind, offset, length;
};

extern const uint8_t media_start[] asm("_binary_media_bin_start");

static struct entry *entries;
static uint32_t count;

struct result {
	const char	*name;
	uint64_t	 cycles, units;
	uint32_t	 crc;
};

static struct result results[32];
static int nresults;

static struct result *result_for(const char *name)
{
	for (int i = 0; i < nresults; i++)
		if (!strcmp(results[i].name, name))
			return &results[i];
	results[nresults].name = name;
	return &results[nresults++];
}

static uint32_t now(void)
{
	return esp_cpu_get_cycle_count();
}

/* ------------------------------------------------------------ files */

static const struct entry *open_entry;
static off_t open_pos;

void *pt_malloc(size_t n) { return malloc(n); }
void *pt_calloc(size_t count, size_t n) { return calloc(count, n); }
void pt_free(void *p) { free(p); }

ssize_t pt_read(int fd, void *buf, size_t n)
{
	size_t left = open_entry->length - open_pos;

	if (n > left)
		n = left;
	memcpy(buf, media_start + open_entry->offset + open_pos, n);
	open_pos += n;
	return n;
}

off_t pt_lseek(int fd, off_t off, int whence)
{
	off_t base = whence == SEEK_END ? (off_t)open_entry->length :
		     whence == SEEK_CUR ? open_pos : 0;

	if (base + off < 0)
		return -EINVAL;
	open_pos = base + off;
	return open_pos;
}

/* ------------------------------------------------------------ sound */

static void audio(const struct entry *e)
{
	struct result *r = result_for(e->name);
	struct codec *c;
	int32_t *pcm = heap_caps_malloc(PASS * 2 * sizeof(*pcm), MALLOC_CAP_INTERNAL);
	uint32_t t;
	ssize_t got;

	open_entry = e;
	open_pos = 0;
	t = now();
	if (!pcm || codec_open(3, &c)) {
		printf("PB-ERROR %s: cannot open\n", e->name);
		return;
	}
	r->cycles += now() - t;
	for (;;) {
		t = now();
		got = codec_read(c, pcm, PASS);
		r->cycles += now() - t;
		if (got <= 0)
			break;
		r->units += got;
		r->crc = esp_rom_crc32_le(r->crc, (const uint8_t *)pcm, got * c->channels * sizeof(*pcm));
	}
	if (got < 0)
		printf("PB-ERROR %s: %d\n", e->name, (int)got);
	codec_close(c);
	heap_caps_free(pcm);
}

/* 192 kHz stereo halved, as sink.c plays it: a sweep and some noise. */
static void halving(void)
{
	struct result *r = result_for("half_run 192k");
	struct half *h = calloc(1, sizeof(*h));
	int32_t *pcm = heap_caps_malloc(PASS * 2 * sizeof(*pcm), MALLOC_CAP_INTERNAL);
	uint32_t seed = 1, phase = 0;

	for (int pass = 0; pass < 192000 * 2 / PASS; pass++) {
		uint32_t t;
		size_t out;

		for (int i = 0; i < PASS; i++) {
			seed = seed * 1103515245 + 12345;
			phase += 0x01000000 + pass * 0x1000;
			pcm[2 * i] = (int32_t)(phase >> 9) - (1 << 22) + (int32_t)(seed >> 16) - 32768;
			pcm[2 * i + 1] = -pcm[2 * i] / 2;
		}
		t = now();
		out = half_run(h, pcm, PASS, 2);
		r->cycles += now() - t;
		r->units += PASS;
		r->crc = esp_rom_crc32_le(r->crc, (const uint8_t *)pcm, out * 2 * sizeof(*pcm));
	}
	free(h);
	heap_caps_free(pcm);
}

/*
 * The same fixed work every time, whatever the decoders do: if its count
 * moves, the run is not comparable with the others (see perfbench.py).
 */
static void calibration(void)
{
	struct result *r = result_for("calibration");
	uint32_t x = 1, t = now();

	for (int i = 0; i < 1000000; i++)
		x = x * 1664525 + 1013904223 + (x >> 13);
	r->cycles += now() - t;
	r->units = 1;
	r->crc = x;
}

/*
 * The mixer, as it runs with one stream at the wire's rate (music on the
 * speaker or the headphones): a block from the stream's ring in PSRAM,
 * then the speaker's output stage -- the two sides folded, the limiter,
 * 16 bits -- or the jack's, 32 bits at its volume. The writer's side,
 * filling the ring, is not counted.
 */
static void mixing(const char *name, bool jack)
{
	struct result *r = result_for(name);
	struct stream s = { .rate = 44100, .size = 44100, .used = true };
	int32_t *acc = heap_caps_malloc(BLOCK * 2 * sizeof(*acc), MALLOC_CAP_INTERNAL);
	int16_t *wire = heap_caps_malloc(BLOCK * 2 * sizeof(*wire), MALLOC_CAP_INTERNAL);
	int32_t gain = 32768, jack_gain = jack_gain_for(40), x = 0;

	s.ring = heap_caps_malloc(s.size * 2 * sizeof(*s.ring), MALLOC_CAP_SPIRAM);
	for (int block = 0; block < 44100 * 4 / BLOCK; block++) {
		uint32_t t;
		int n = 0;

		while (filled(&s) < s.size) {		/* the writer keeps it full */
			int32_t *f = &s.ring[s.head % s.size * 2];

			x = x * 1103515245 + 12345;
			f[0] = (x >> 8) % (MIX_FULL / 2);
			f[1] = -f[0] / 3;
			s.head++;
		}
		t = now();
		memset(acc, 0, BLOCK * 2 * sizeof(*acc));
#ifdef HAVE_MIX_STREAM
		n = mix_stream(&s, ONE, acc, false);
#else
		for (int v[2]; n < BLOCK && pull(&s, ONE, v); n++) {
			acc[2 * n] += v[0];
			acc[2 * n + 1] += v[1];
		}
#endif
		if (jack) {
			gain = mix_gain(acc, BLOCK, 2, 44100, gain);
			for (int k = 0; k < BLOCK * 2; k++)
				acc[k] = jack_sample(acc[k], gain, jack_gain);
		} else {
			for (int k = 0; k < BLOCK; k++)
				acc[k] = (int32_t)(((int64_t)acc[2 * k] + acc[2 * k + 1]) / 2);
			gain = mix_gain(acc, BLOCK, 1, 44100, gain);
			for (int k = 0; k < BLOCK; k++)
				wire[2 * k] = wire[2 * k + 1] = speaker_sample(acc[k], gain);
		}
		r->cycles += now() - t;
		r->units += n;
		r->crc = jack ? esp_rom_crc32_le(r->crc, (const uint8_t *)acc, BLOCK * 2 * sizeof(*acc))
			      : esp_rom_crc32_le(r->crc, (const uint8_t *)wire, BLOCK * 2 * sizeof(*wire));
	}
	heap_caps_free(s.ring);
	heap_caps_free(acc);
	heap_caps_free(wire);
}

/* ------------------------------------------------------------ pictures */

struct page {
	uint8_t	*px;
	int	 w, h, y0;
};

static int band(void *ctx, int y, int rows, const uint8_t *px, size_t stride)
{
	struct page *p = ctx;

	for (int r = 0; r < rows && p->y0 + y + r < p->h; r++)
		memcpy(p->px + (size_t)(p->y0 + y + r) * p->w * 2, px + r * stride,
		       (size_t)p->w * 2);
	return 0;
}

/* A clip's frame is two slices, one over the other, as mkvideo.py makes them. */
static void picture(const struct entry *e, int index, int scale)
{
	static struct page page;
	struct result *r = result_for(e->name);
	struct jpeg *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_INTERNAL);
	uint8_t *strip;
	uint32_t t;
	int w, h, ret;

	j->p = media_start + e->offset;
	j->end = j->p + e->length;
	t = now();
	if (!j || jpeg_open(j)) {
		printf("PB-ERROR %s: cannot open\n", e->name);
		return;
	}
	r->cycles += now() - t;
	w = jpeg_scaled_w(j, scale);
	h = jpeg_scaled_h(j, scale);
	if (!page.px || page.w != w) {
		free(page.px);
		page.w = w;
		page.h = scale ? h : 2 * h;
		page.px = calloc((size_t)page.w * page.h, 2);
	}
	page.y0 = scale ? 0 : index % 2 * h;
	strip = heap_caps_malloc(jpeg_band_size(j, scale), MALLOC_CAP_INTERNAL);
	if (jpeg_dc_size(j))
		j->blocks = malloc(jpeg_dc_size(j));
	t = now();
	ret = jpeg_decode(j, scale, strip, band, &page);
	r->cycles += now() - t;
	if (ret)
		printf("PB-ERROR %s: %d\n", e->name, ret);
	r->units++;
	r->crc = esp_rom_crc32_le(r->crc, page.px + (size_t)page.y0 * page.w * 2,
				  (size_t)page.w * (scale ? page.h : h) * 2);
	free(j->blocks);
	heap_caps_free(strip);
	heap_caps_free(j);
}

void app_main(void)
{
	int slice = 0;

	calibration();
	if (memcmp(media_start, "PBMEDIA1", 8)) {
		printf("PB-ERROR no media\n");
		return;
	}
	memcpy(&count, media_start + 8, 4);
	entries = malloc(count * sizeof(*entries));	/* the blob need not be aligned */
	memcpy(entries, media_start + 12, count * sizeof(*entries));
	for (uint32_t i = 0; i < count; i++) {
		const struct entry *e = &entries[i];

		if (e->kind == AUDIO)
			audio(e);
		else if (e->kind == JPEG_SLICE)
			picture(e, slice++, 0);
		else if (e->kind == JPEG_DC)
			picture(e, 0, 3);
	}
	halving();
	mixing("mixer 44.1k speaker", false);
	mixing("mixer 44.1k jack", true);
	for (int i = 0; i < nresults; i++)
		printf("PB %s|%llu|%llu|%08lx\n", results[i].name,
		       (unsigned long long)results[i].cycles, (unsigned long long)results[i].units,
		       (unsigned long)results[i].crc);
	printf("PB-DONE\n");
}
