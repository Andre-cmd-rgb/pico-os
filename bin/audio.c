/*
 * Sound: play, rec, beep and volume.
 *
 * play reads WAV, MP3 and FLAC, or raw 16-bit little-endian mono at the
 * codec's rate, which is what /dev/audio takes too:
 *
 *	play tune.mp3          rec -t 5 note.wav
 *	play -n album.flac     cat tune.raw > /dev/audio
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "codec.h"
#include "sink.h"
#include "drivers/drivers.h"
#include "pt/kernel.h"
#include "util.h"

#if CONFIG_PT_AUDIO

#define DEFAULT_SECONDS	5
#define VOICE_REDUCE_DB	15		/* how far rec takes the hiss down */
#define VOICE_TARGET_DB	-20		/* and where it keeps the speech */
#define TONE_AMPLITUDE	8000		/* a third of full scale: no clipping */

static const int16_t sine64[64] = {
	     0,    784,   1561,   2322,   3061,   3771,   4445,   5075,
	  5657,   6184,   6652,   7055,   7391,   7656,   7846,   7961,
	  8000,   7961,   7846,   7656,   7391,   7055,   6652,   6184,
	  5657,   5075,   4445,   3771,   3061,   2322,   1561,    784,
	     0,   -784,  -1561,  -2322,  -3061,  -3771,  -4445,  -5075,
	 -5657,  -6184,  -6652,  -7055,  -7391,  -7656,  -7846,  -7961,
	 -8000,  -7961,  -7846,  -7656,  -7391,  -7055,  -6652,  -6184,
	 -5657,  -5075,  -4445,  -3771,  -3061,  -2322,  -1561,   -784,
};

static int no_codec(const char *prog)
{
	pt_dprintf(PT_STDERR, "%s: no audio codec on this board\n", prog);
	return 1;
}

/* ------------------------------------------------------------ play */

/*
 * minimp3, inside the MP3 decoder, keeps a 17 KB scratch buffer on the
 * stack, so `play` asks for a bigger stack than a program gets by default.
 * It was seen to use 18 KB of it; every kilobyte more is internal RAM
 * that a game started on another terminal then has to do without.
 */
#define PLAY_STACK_KB	24
static int play_file(const char *name, int fd, struct sink *s)
{
	struct codec *c;
	int32_t *pcm;
	int ret = codec_open(fd, &c);

	if (ret == -ENOTSUP)		/* nothing claimed it: samples and nothing else */
		ret = codec_open_raw(fd, audio_rate(), &c);
	if (ret)
		return fail("play", name, ret);
	if ((ret = sink_format(s, "play", name, c->rate, c->channels))) {
		codec_close(c);
		return ret == -ENOMEM ? fail("play", name, ret) : 1;
	}
	pcm = pt_malloc(SINK_PASS * c->channels * sizeof(*pcm));
	if (!pcm) {
		codec_close(c);
		return fail("play", name, -ENOMEM);
	}
	for (;;) {
		ssize_t got = codec_read(c, pcm, SINK_PASS);

		if (got <= 0) {
			ret = got < 0 ? fail("play", name, got) : 0;
			break;
		}
		if ((ret = sink_play(s, pcm, got, c->channels))) {
			ret = ret == -EINTR ? 0 : fail("play", name, ret);
			break;
		}
	}
	pt_free(pcm);
	if (s->dry)
		sink_report(s, name, c);
	codec_close(c);
	return ret;
}

/*
 * Either core: decoding MP3 is a sixth of a core, and on the programs'
 * core that sixth came out of a game on another terminal (57 frames a
 * second instead of 60). It waits on the speaker nearly all the time,
 * and without one (-n) it yields every few passes, so the kernel's core
 * still gets to idle.
 */
PT_COMPLETE(play, ": -n <file:.wav.flac.mp3.raw>\n*: <file:.wav.flac.mp3.raw>\n")

PT_PROGRAM_ANYCORE(play, PLAY_STACK_KB, "play a sound file\n"
	   "usage: play [-n] file...\n"
	   "WAV (8 to 32-bit, or float), FLAC, MP3, or raw 16-bit\n"
	   "mono at the codec's rate.\n"
	   "  -n  decode without playing; report the speed and\n"
	   "      a checksum\n"
	   "Headphones get every bit, 24 of a FLAC or an MP3, at\n"
	   "the file's rate up to 96 kHz (176.4 and 192 kHz play\n"
	   "at half). The speaker is mono, at 48 kHz at most.\n"
	   "Ctrl-C stops.")
{
	uint32_t flags;
	int i = parse_flags("play", argc, argv, "n", &flags);
	bool dry = FLAG(flags, 'n');
	int ret = 0;

	if (i < 0 || i == argc) {
		pt_dprintf(PT_STDERR, "usage: play [-n] file...\n");
		return 2;
	}
	if (!dry && !audio_present())
		return no_codec("play");
	/* Ctrl-C is seen by sink_play, which stops cleanly: the amplifier
	 * goes off and the files are closed, rather than the program just
	 * ending at its next read */
	pt_sigcatch(true);
	for (; i < argc && !pt_interrupted(); i++) {
		struct sink s;
		int fd = pt_open(argv[i], O_RDONLY);

		if (fd < 0) {
			ret = fail("play", argv[i], fd);
			continue;
		}
		if (sink_open(&s, dry)) {
			pt_close(fd);
			return fail("play", argv[i], -ENOMEM);
		}
		ret |= play_file(argv[i], fd, &s);
		sink_close(&s);
		pt_close(fd);
	}
	return ret ? 1 : 0;
}

/* ------------------------------------------------------------ rec */

/*
 * Where a recording goes. A name with a directory in it goes where it
 * says; a bare name, or none (the date and time then), goes in
 * ~/recordings, so that a term's lessons end up in one place and not
 * wherever the shell happened to be. Without a card: the name as given,
 * or /tmp.
 */
static void rec_path(const char *name, char *out, size_t size)
{
	char dir[64];
	struct pt_stat st;
	struct tm tm;
	time_t now = time(NULL);
	int len;

	if (name && (strchr(name, '/') || !sd_mounted())) {
		strlcpy(out, name, size);
		return;
	}
	if (sd_mounted())
		pt_mkdir(home_dir(dir, sizeof(dir), "recordings"));
	else
		strlcpy(dir, "/tmp", sizeof(dir));
	if (name) {
		snprintf(out, size, "%s/%s", dir, name);
		return;
	}
	localtime_r(&now, &tm);
	len = snprintf(out, size, "%s/%04d-%02d-%02d-%02d%02d", dir, tm.tm_year + 1900,
		       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
	for (int n = 1; n < 100 && len > 0 && (size_t)len < size; n++) {
		snprintf(out + len, size - len, n == 1 ? ".wav" : "-%d.wav", n);
		if (pt_stat(out, &st))
			break;
	}
}

#define REC_USAGE	"usage: rec [-n] [-t seconds] [-r rate] [-g gain_db] [file]\n"

PT_COMPLETE(rec, ": -n -t -r -g <file:.wav>\n*: <file:.wav>\n")

PT_PROGRAM(rec, "record from the microphone into a WAV file\n"
	   REC_USAGE
	   "Records 5 seconds at 16 kHz by default; Ctrl-C stops early and\n"
	   "keeps what it has. The voice is cleaned as it comes in -- rumble\n"
	   "and hiss out, the level evened, no clipping -- unless -n asks for\n"
	   "the sound as the microphone heard it. Unlike sox's rec, a name\n"
	   "without a / goes in ~/recordings, and with no name the date and\n"
	   "time are the name.")
{
	int seconds = DEFAULT_SECONDS, rate = 0, gain = -1, i = 1;
	const char *given;
	char name[PT_PATH_MAX];
	uint8_t header[WAV_HEADER_BYTES], *buf;
	uint32_t written = 0, want;
	struct voice *voice = NULL;
	int64_t cleaning = 0;		/* µs spent in voice_run */
	bool raw = false;
	int fd, ret = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		char opt = argv[i][1];
		const char *val;

		if (opt == 'n' && !argv[i][2]) {
			raw = true;
			continue;
		}
		val = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : NULL);
		if (!val || !strchr("trg", opt)) {
			pt_dprintf(PT_STDERR, REC_USAGE);
			return 2;
		}
		if (opt == 't')
			seconds = atoi(val);
		else if (opt == 'r')
			rate = atoi(val);
		else
			gain = atoi(val);
	}
	given = i < argc ? argv[i] : NULL;
	if (i + 1 < argc || seconds <= 0) {
		pt_dprintf(PT_STDERR, REC_USAGE);
		return 2;
	}
	if (!audio_present())
		return no_codec("rec");
	/* the codec's own rate, which is plenty for speech; left alone, the
	 * recording would take whatever rate the last song played at */
	if (!rate)
		rate = CONFIG_PT_AUDIO_RATE;
	if ((ret = audio_set_rate(rate)))
		return fail("rec", "sample rate", ret);
	rec_path(given, name, sizeof(name));
	if (gain >= 0)
		audio_set_mic_gain(gain);
	rate = audio_rate();

	fd = pt_open(name, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0)
		return fail("rec", name, fd);
	/* Ctrl-C ends the loop below, so the header gets its length */
	pt_sigcatch(true);
	buf = pt_malloc(BUF_SIZE);
	if (!raw)
		voice = voice_new(rate, VOICE_REDUCE_DB, VOICE_TARGET_DB);
	if (!buf || (!raw && !voice)) {
		pt_free(buf);
		pt_close(fd);
		return fail("rec", name, -ENOMEM);
	}
	if (voice)
		audio_mic_alc_hold(true);	/* two levellers fight: voice_run's is the one */
	wav_header(header, rate, 1, 0);
	pt_write(fd, header, sizeof(header));

	want = (uint32_t)rate * 2 * seconds;
	pt_printf("recording %d s at %d Hz%s, Ctrl-C to stop\n", seconds, rate,
		  raw ? ", as it comes" : "");
	while (written < want && !pt_interrupted()) {
		size_t n = want - written < BUF_SIZE ? want - written : BUF_SIZE;
		ssize_t got = audio_read(buf, n);

		if (got < 0) {
			ret = fail("rec", name, got);
			break;
		}
		if (voice) {
			int64_t t = pt_uptime_us();

			voice_run(voice, (int16_t *)buf, got / 2);
			cleaning += pt_uptime_us() - t;
		}
		if (write_all(fd, (char *)buf, got)) {
			ret = 1;
			break;
		}
		written += got;
	}
	audio_stop();
	pt_free(buf);
	if (voice)
		audio_mic_alc_hold(false);
	voice_free(voice);
	if (voice && written >= 2) {
		int permille = (int)(cleaning * 1000 / ((int64_t)written / 2 * 1000000 / rate));

		klog("rec: cleaning the voice took %d.%d%% of a core", permille / 10, permille % 10);
	}

	wav_header(header, rate, 1, written);	/* now we know how long it is */
	if (pt_lseek(fd, 0, SEEK_SET) == 0)
		pt_write(fd, header, sizeof(header));
	pt_close(fd);
	if (!ret)
		pt_printf("%s: %d samples, %d.%d s\n", name, (int)(written / 2),
			  (int)(written / 2 / rate), (int)(written / 2 % rate * 10 / rate));
	return ret;
}

/* ------------------------------------------------------------ beep */

PT_PROGRAM(beep, "play a tone\n"
	   "usage: beep [hz] [ms]\n"
	   "Default 1000 Hz for 200 ms.")
{
	int hz = argc > 1 ? atoi(argv[1]) : 1000;
	int ms = argc > 2 ? atoi(argv[2]) : 200;
	int rate = audio_rate();
	uint32_t phase = 0, step;
	int16_t *buf;
	int samples;

	if (argc > 3 || hz < 20 || hz > 10000 || ms < 1 || ms > 10000) {
		pt_dprintf(PT_STDERR, "usage: beep [20-10000 hz] [1-10000 ms]\n");
		return 2;
	}
	if (!audio_present())
		return no_codec("beep");
	samples = rate * ms / 1000;
	buf = pt_malloc(samples * sizeof(*buf));
	if (!buf)
		return fail("beep", NULL, -ENOMEM);
	step = (uint32_t)((uint64_t)hz * 64 * 65536 / rate);
	for (int i = 0; i < samples; i++, phase += step) {
		int fade = 1000;		/* ramp the ends: no click */

		buf[i] = sine64[(phase >> 16) & 63];
		if (i < fade)
			buf[i] = (int32_t)buf[i] * i / fade;
		else if (samples - i < fade)
			buf[i] = (int32_t)buf[i] * (samples - i) / fade;
	}
	audio_write(buf, samples * sizeof(*buf), 1);
	audio_stop();
	pt_free(buf);
	return 0;
}

/* ------------------------------------------------------------ volume */

static const char *const outputs[] = { "auto", "speaker", "jack" };

PT_COMPLETE(volume, ": speaker jack auto\n")

PT_PROGRAM(volume, "show or set the volume, and where sound goes\n"
	   "usage: volume [0-100] [speaker | jack | auto]\n"
	   "The number is the volume of wherever the sound goes.\n"
	   "With a headphone jack, auto (the start) sends it to\n"
	   "the jack while a plug is in and speaker keeps it on\n"
	   "the speaker. jack forces it to the jack only where\n"
	   "the socket cannot tell a plug is in, or in a build\n"
	   "made for trying it out.\n"
	   "The speaker and the headphones keep a volume each,\n"
	   "the headphones starting low; all of it is kept in\n"
	   "/etc/power. An alarm always rings on the speaker.")
{
	long percent = -1;
	int out = -1;

	for (int i = 1; i < argc; i++) {
		int k = 0;

		while (k < 3 && strcmp(argv[i], outputs[k]))
			k++;
		if (k < 3 && out < 0) {
			out = k;
		} else if (k < 3 || percent >= 0 || parse_long(argv[i], &percent) ||
			   percent < 0 || percent > 100) {
			pt_dprintf(PT_STDERR, "usage: volume [0-100] [speaker | jack | auto]\n");
			return 2;
		}
	}
	if (!audio_present())
		return no_codec("volume");
	if (out > 0 && !audio_has_jack()) {
		pt_dprintf(PT_STDERR, "volume: there is no headphone jack (menuconfig:\n"
			   "  Device drivers, Sound)\n");
		return 1;
	}
	/* where first: `volume 30 jack` is the headphones at 30 */
	if (out >= 0 && audio_has_jack() && audio_set_output((enum audio_out)out) == -EPERM) {
		pt_dprintf(PT_STDERR, "volume: the headphones follow the plug here: auto\n"
			   "  or speaker\n");
		return 1;
	}
	if (percent >= 0)
		audio_set_volume((int)percent);
	if (percent >= 0 || out >= 0)
		power_levels_changed();		/* kept in /etc/power */
	if (!audio_has_jack()) {
		pt_printf("volume %d%%\n", audio_volume());
		return 0;
	}
	pt_printf("volume %d%% (%s)\n", audio_volume(), audio_to_jack() ? "headphones" : "speaker");
	/* only when it is not the plug choosing, which is the usual */
	if (audio_output() != AUDIO_OUT_AUTO && audio_jack_switch())
		pt_printf("kept on the %s: `volume auto` lets the plug choose\n",
			  audio_to_jack() ? "headphones" : "speaker");
	return 0;
}

#endif /* CONFIG_PT_AUDIO */
