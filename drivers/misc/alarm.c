/*
 * Alarms: the clock that rings.
 *
 * `alarm` sets them and this keeps them in /etc/alarms, on the flash, so
 * they are there with or without the card. A task here rings the one that
 * comes due, whatever else is running: beeps through the speaker that grow
 * louder, the backlight pulsing, and the status bar saying what it is. Any
 * key snoozes it and Esc stops it -- the keys go no further while it rings
 * -- and so does the side button: a press snoozes, holding it stops. Left
 * ringing, it snoozes by itself, three times, then gives up.
 *
 * Reminders from the calendar are alarms too, of the gentler kind: a chime,
 * once, and the status bar showing what they are about for a minute.
 *
 * `suspend` asks when the next one is and sets the chip's timer to wake a
 * moment before it; the boot that follows finds it due and rings. The chip
 * keeps time through deep sleep on its own oscillator, which can drift a
 * little over a night; the network puts the clock right when it connects.
 *
 * A snooze is an alarm like any other, for that one day, so it is in the
 * list, it survives a suspend, and it goes once it has rung.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "drivers/drivers.h"

#define ALARM_FILE	"/etc/alarms"
#define PLAUSIBLE	1704067200	/* 2024: before this the clock was never set */
#define SNOOZE_S	(9 * 60)
#define RING_S		(3 * 60)	/* then it snoozes by itself */
#define AUTO_SNOOZES	3		/* and after this many, gives up */
#define CHIME_S		60		/* a reminder stays on the bar this long */
#define RAMP_S		30		/* from quiet to loud */
#define ON_TIME_S	120		/* later than this, an alarm was missed */
#define CATCH_UP_S	(10 * 60)	/* after the timer woke the chip */
#define TONE_HZ		1760
#define BEEP_MS		110
#define GAP_MS		70

enum answer { ANSWER_NONE, ANSWER_SNOOZE, ANSWER_STOP };

static struct {
	SemaphoreHandle_t lock;
	TaskHandle_t	 task;
	struct alarm	 a[ALARMS_MAX];
	int		 n;
	time_t		 checked;	/* everything due up to here has had its turn */
	int		 grace;		/* how late a due one may still ring */
	struct alarm	 now;		/* the one ringing */
	volatile bool	 ringing;
	volatile int	 answer;
} al;

static const char *const day_names[7] = { "mo", "tu", "we", "th", "fr", "sa", "su" };

/* ------------------------------------------------------------ the file */

static void days_text(const struct alarm *a, char *out, size_t size)
{
	size_t n = 0;

	if (a->date) {
		snprintf(out, size, "%04d-%02d-%02d", a->date / 10000, a->date / 100 % 100,
			 a->date % 100);
		return;
	}
	if (!a->days) {
		strlcpy(out, "once", size);
		return;
	}
	if (a->days == ALARM_DAILY) {
		strlcpy(out, "daily", size);
		return;
	}
	out[0] = '\0';
	for (int d = 0; d < 7; d++)
		if (a->days & (1 << d))
			n += snprintf(out + n, n < size ? size - n : 0, "%s%s", n ? "," : "",
				      day_names[d]);
}

static bool days_parse(const char *s, struct alarm *a)
{
	int y, m, d;

	a->days = 0;
	a->date = 0;
	if (!strcmp(s, "once"))
		return true;
	if (!strcmp(s, "daily")) {
		a->days = ALARM_DAILY;
		return true;
	}
	if (sscanf(s, "%d-%d-%d", &y, &m, &d) == 3) {
		a->date = y * 10000 + m * 100 + d;
		return y > 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
	}
	for (const char *p = s; *p; p += *p == ',') {
		int i;

		for (i = 0; i < 7 && strncmp(p, day_names[i], 2); i++)
			;
		if (i == 7)
			return false;
		a->days |= 1 << i;
		p += 2;
	}
	return a->days != 0;
}

/* Called with the lock held. */
static void save(void)
{
	char path[64], days[32];
	FILE *f;

	if (!mount_resolve(ALARM_FILE, path, sizeof(path)) || !(f = fopen(path, "w")))
		return;
	fprintf(f, "# on/off time days kind snoozes label -- `alarm` writes this\n");
	for (int i = 0; i < al.n; i++) {
		const struct alarm *a = &al.a[i];

		days_text(a, days, sizeof(days));
		fprintf(f, "%s %02d:%02d:%02d %s %s %d %s\n", a->on ? "on" : "off", a->hour,
			a->min, a->sec, days, a->kind == ALARM_CHIME ? "chime" : "ring", a->snoozes,
			a->label[0] ? a->label : "-");
	}
	fclose(f);
}

static void load(void)
{
	char path[64], line[128];
	FILE *f;

	if (!mount_resolve(ALARM_FILE, path, sizeof(path)) || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f) && al.n < ALARMS_MAX) {
		struct alarm a = { 0 };
		char state[4], days[32], kind[8];
		int h, m, s, snoozes, at = 0;

		if (line[0] == '#' ||
		    sscanf(line, "%3s %d:%d:%d %31s %7s %d %n", state, &h, &m, &s, days, kind,
			   &snoozes, &at) < 7 || !at)
			continue;
		line[strcspn(line, "\r\n")] = '\0';
		if (!days_parse(days, &a) || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
			continue;
		a.on = !strcmp(state, "on");
		a.hour = h;
		a.min = m;
		a.sec = s;
		a.kind = !strcmp(kind, "chime") ? ALARM_CHIME : ALARM_RING;
		a.snoozes = snoozes;
		if (strcmp(line + at, "-"))
			strlcpy(a.label, line + at, sizeof(a.label));
		al.a[al.n++] = a;
	}
	fclose(f);
}

/* ------------------------------------------------------------ when */

time_t alarm_next(const struct alarm *a, time_t after)
{
	struct tm tm;

	if (!a->on)
		return 0;
	if (a->date) {
		struct tm day = {
			.tm_year = a->date / 10000 - 1900, .tm_mon = a->date / 100 % 100 - 1,
			.tm_mday = a->date % 100, .tm_hour = a->hour, .tm_min = a->min,
			.tm_sec = a->sec, .tm_isdst = -1,
		};
		time_t t = mktime(&day);

		return t > after ? t : 0;
	}
	localtime_r(&after, &tm);
	for (int d = 0; d <= 7; d++) {
		struct tm day = tm;
		time_t t;

		day.tm_mday += d;
		day.tm_hour = a->hour;
		day.tm_min = a->min;
		day.tm_sec = a->sec;
		day.tm_isdst = -1;
		t = mktime(&day);		/* and day.tm_wday with it */
		if (t > after && (!a->days || (a->days & (1 << (day.tm_wday + 6) % 7))))
			return t;
	}
	return 0;
}

/* The one that rings first after `after`, with the lock held. */
static int soonest(time_t after, time_t *when)
{
	int best = -1;

	*when = 0;
	for (int i = 0; i < al.n; i++) {
		time_t t = alarm_next(&al.a[i], after);

		if (t && (!*when || t < *when)) {
			*when = t;
			best = i;
		}
	}
	return best;
}

time_t alarm_next_any(struct alarm *which)
{
	time_t when;
	int i;

	if (!al.lock)
		return 0;
	xSemaphoreTake(al.lock, portMAX_DELAY);
	i = soonest(time(NULL), &when);
	if (i >= 0 && which)
		*which = al.a[i];
	xSemaphoreGive(al.lock);
	return i >= 0 ? when : 0;
}

uint32_t alarm_seconds_until(void)
{
	time_t now = time(NULL), when = alarm_next_any(NULL);

	return when > now ? (uint32_t)(when - now) : 0;
}

/* ------------------------------------------------------------ the list */

int alarm_list(struct alarm *out, int max)
{
	int n;

	if (!al.lock)
		return -ENODEV;
	xSemaphoreTake(al.lock, portMAX_DELAY);
	n = al.n < max ? al.n : max;
	memcpy(out, al.a, n * sizeof(*out));
	xSemaphoreGive(al.lock);
	return n;
}

static void changed(void)
{
	save();
	if (al.task)
		xTaskNotifyGive(al.task);	/* look again: the next one may be sooner */
}

int alarm_add(const struct alarm *a)
{
	int i;

	if (!al.lock)
		return -ENODEV;
	if (a->hour > 23 || a->min > 59 || a->sec > 59)
		return -EINVAL;
	xSemaphoreTake(al.lock, portMAX_DELAY);
	if (al.n == ALARMS_MAX) {
		xSemaphoreGive(al.lock);
		return -ENOSPC;
	}
	i = al.n++;
	al.a[i] = *a;
	changed();
	xSemaphoreGive(al.lock);
	return i;
}

int alarm_remove(int i)
{
	if (!al.lock)
		return -ENODEV;
	xSemaphoreTake(al.lock, portMAX_DELAY);
	if (i < 0 || i >= al.n) {
		xSemaphoreGive(al.lock);
		return -ENOENT;
	}
	memmove(&al.a[i], &al.a[i + 1], (al.n - i - 1) * sizeof(al.a[0]));
	al.n--;
	changed();
	xSemaphoreGive(al.lock);
	return 0;
}

int alarm_enable(int i, bool on)
{
	if (!al.lock)
		return -ENODEV;
	xSemaphoreTake(al.lock, portMAX_DELAY);
	if (i < 0 || i >= al.n) {
		xSemaphoreGive(al.lock);
		return -ENOENT;
	}
	al.a[i].on = on;
	changed();
	xSemaphoreGive(al.lock);
	return 0;
}

/* ------------------------------------------------------------ ringing */

static int16_t sine[64];

/*
 * `ms` of a tone at `hz` into buf, faded in and out so it does not click;
 * `decay` makes it die away like a struck bell rather than stop.
 */
static int tone(int16_t *buf, int rate, int hz, int ms, bool decay)
{
	int samples = rate * ms / 1000, fade = rate / 200;
	uint32_t phase = 0, step = (uint32_t)((uint64_t)hz * 64 * 65536 / rate);

	for (int i = 0; i < samples; i++, phase += step) {
		int32_t v = sine[(phase >> 16) & 63];

		if (decay)
			v = v * (samples - i) / samples;
		if (i < fade)
			v = v * i / fade;
		else if (samples - i < fade)
			v = v * (samples - i) / fade;
		buf[i] = v;
	}
	return samples;
}

/* Wait up to `ms` for an answer; true if one came. Other news (an alarm
 * added meanwhile) wakes the task too, and is not an answer. */
static bool wait_answer(int ms)
{
	int64_t end = esp_timer_get_time() + ms * 1000LL;

	while (al.answer == ANSWER_NONE) {
		int64_t left = end - esp_timer_get_time();

		if (left <= 0)
			break;
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left / 1000 + 1));
	}
	return al.answer != ANSWER_NONE;
}

/* One run of four beeps, louder as the ringing goes on. */
static void beeps(int16_t *buf, int seconds)
{
	int rate = audio_rate(), n;

	audio_set_volume(35 + (seconds < RAMP_S ? 65 * seconds / RAMP_S : 65));
	for (int k = 0; k < 4 && al.answer == ANSWER_NONE; k++) {
		n = tone(buf, rate, TONE_HZ, BEEP_MS, false);
		memset(buf + n, 0, rate * GAP_MS / 1000 * sizeof(*buf));
		audio_write(buf, (n + rate * GAP_MS / 1000) * sizeof(*buf), 1);
	}
}

/* Three notes rising, like a doorbell: for reminders. */
static void chime(int16_t *buf)
{
	static const int notes[] = { 1047, 1319, 1568 };
	int rate = audio_rate();

	audio_set_volume(70);
	for (int k = 0; k < 3 && al.answer == ANSWER_NONE; k++)
		audio_write(buf, tone(buf, rate, notes[k], 260, true) * sizeof(*buf), 1);
}

static void snooze(const struct alarm *a, int count)
{
	time_t at = time(NULL) + SNOOZE_S;
	struct tm tm;
	struct alarm s = *a;

	localtime_r(&at, &tm);
	s.on = true;
	s.days = 0;
	s.date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
	s.hour = tm.tm_hour;
	s.min = tm.tm_min;
	s.sec = tm.tm_sec;
	s.snoozes = count;
	alarm_add(&s);
	klog("alarm: snoozed until %02d:%02d", s.hour, s.min);
}

static void ring(const struct alarm *a)
{
	int volume = 0, light = lcd_backlight_get();
	int limit = a->kind == ALARM_CHIME ? CHIME_S : RING_S;
	int64_t start = esp_timer_get_time();
	bool sound = audio_present();
	int16_t *buf = sound ? malloc(audio_rate() * 300 / 1000 * sizeof(*buf)) : NULL;
	int seconds;

	al.now = *a;
	al.answer = ANSWER_NONE;
	al.ringing = true;
	power_activity();		/* light a dark screen to ring */
	klog("alarm: %02d:%02d %s", a->hour, a->min, a->label[0] ? a->label : "(no label)");
	if (buf) {
		/* the speaker's, now: an alarm rings there, headphones or not */
		audio_claim(true);
		volume = audio_volume();
	}
	if (a->kind == ALARM_CHIME) {
		if (buf) {
			chime(buf);
			wait_answer(400);
			chime(buf);
		}
		wait_answer(limit * 1000);
	} else {
		do {
			seconds = (esp_timer_get_time() - start) / 1000000;
			lcd_backlight_set(100);
			if (buf)
				beeps(buf, seconds);
			else
				wait_answer(640);
			lcd_backlight_set(15);
		} while (!wait_answer(600) && esp_timer_get_time() - start < limit * 1000000LL);
	}
	al.ringing = false;
	if (buf) {
		audio_stop();
		audio_set_volume(volume);
		audio_claim(false);
		free(buf);
	}
	lcd_backlight_set(light);

	if (a->kind == ALARM_CHIME)
		return;
	if (al.answer == ANSWER_SNOOZE)
		snooze(a, 1);				/* someone is awake: count afresh */
	else if (al.answer == ANSWER_NONE && a->snoozes < 1 + AUTO_SNOOZES)
		snooze(a, a->snoozes + 1);
	else if (al.answer == ANSWER_NONE)
		klog("alarm: nobody answered; it has stopped");
}

bool alarm_ringing(struct alarm *which)
{
	if (al.ringing && which)
		*which = al.now;
	return al.ringing;
}

void alarm_answer(bool stop)
{
	if (!al.ringing)
		return;
	al.answer = stop ? ANSWER_STOP : ANSWER_SNOOZE;
	if (al.task)
		xTaskNotifyGive(al.task);
}

/* Esc or q stop it; any other key snoozes. The key itself goes nowhere. */
bool alarm_key(const char *s, size_t n)
{
	if (!al.ringing || !n)
		return false;
	alarm_answer(n == 1 && (s[0] == 0x1b || s[0] == 'q' || s[0] == 'Q'));
	return true;
}

bool alarm_button(bool held)
{
	if (!al.ringing)
		return false;
	alarm_answer(held);
	return true;
}

/* ------------------------------------------------------------ the task */

/* A one-off has had its turn: a dated one goes, a "once" is switched off. */
static void spent(int i)
{
	if (al.a[i].date) {
		memmove(&al.a[i], &al.a[i + 1], (al.n - i - 1) * sizeof(al.a[0]));
		al.n--;
		save();
	} else if (!al.a[i].days) {
		al.a[i].on = false;
		save();
	}
}

static void alarm_task(void *arg)
{
	for (;;) {
		time_t now = time(NULL), due;
		struct alarm a;
		int i, wait_ms = 60000;

		if (now < PLAUSIBLE) {			/* no clock, no alarms */
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
			continue;
		}
		if (!al.checked || al.checked > now + 60)
			al.checked = now;		/* first time, or the clock went back */
		xSemaphoreTake(al.lock, portMAX_DELAY);
		i = soonest(al.checked, &due);
		if (i >= 0 && due <= now) {
			a = al.a[i];
			spent(i);
		}
		xSemaphoreGive(al.lock);

		if (i >= 0 && due <= now) {
			al.checked = due;
			if (now - due <= al.grace)
				ring(&a);
			else
				klog("alarm: missed %02d:%02d %s", a.hour, a.min, a.label);
			continue;
		}
		al.grace = ON_TIME_S;
		if (i >= 0 && due - now < 60)
			wait_ms = (due - now) * 1000;
		/* as close to the second as the tick allows */
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms ? wait_ms : 10));
	}
}

int alarm_test(int kind)
{
	struct alarm a = { .on = true, .kind = kind, .snoozes = 1 + AUTO_SNOOZES };
	time_t now = time(NULL) + 1;
	struct tm tm;

	localtime_r(&now, &tm);
	a.date = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
	a.hour = tm.tm_hour;
	a.min = tm.tm_min;
	a.sec = tm.tm_sec;
	strlcpy(a.label, "test", sizeof(a.label));
	return alarm_add(&a) < 0 ? -ENOSPC : 0;
}

int alarm_init(void)
{
	uint32_t causes = esp_sleep_get_wakeup_causes();

	for (int i = 0; i < 64; i++)
		sine[i] = (int16_t)(12000 * sinf(i * 6.2831853f / 64));
	al.lock = xSemaphoreCreateMutex();
	if (!al.lock)
		return -ENOMEM;
	load();
	al.grace = ON_TIME_S;
	/* The timer woke the chip: that was for an alarm, perhaps a little
	 * early or late by the sleeping clock. Look back for it. */
	if (esp_reset_reason() == ESP_RST_DEEPSLEEP && (causes & BIT(ESP_SLEEP_WAKEUP_TIMER))) {
		al.checked = time(NULL) - CATCH_UP_S;
		al.grace = CATCH_UP_S + ON_TIME_S;
	}
	if (xTaskCreatePinnedToCore(alarm_task, "kalarm", 4096, NULL, 3, &al.task, 0) != pdPASS)
		return -ENOMEM;
	if (al.n)
		klog("alarm: %d set", al.n);
	return 0;
}
