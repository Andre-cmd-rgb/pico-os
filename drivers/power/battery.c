/*
 * Battery: how full the cell is, what it is doing, and how it wears.
 *
 * The board divides the cell voltage in two and feeds it to an ADC pin.
 * A task reads it every five seconds: it warns when the cell is low, and
 * below the critical level it syncs everything and goes into deep sleep,
 * which is the only way to stop a lithium cell from being run flat.
 *
 * The voltage alone is a poor guide to what is left. It sags while the
 * board draws current, by the current times the cell's own resistance,
 * and it stands high while the charger pushes current in: on a 200 mAh
 * cell the two moved the percentage by 40 points within ten minutes.
 * There is no current sensor, so this keeps instead:
 *
 *   an estimate of the current, from what the board is doing -- the
 *   backlight, the CPU, the radio -- or from the charger's set current;
 *   the cell's resistance, learned from how far the voltage steps when
 *   the charger comes or goes or the backlight moves a long way;
 *   the voltage with the sag put back, which is what the cell would read
 *   at rest, and what the table of a lithium cell's charge is made of;
 *   the charge in the cell, counted by the current and pulled slowly
 *   towards what the corrected voltage says, so the percentage neither
 *   jumps with the brightness nor drifts away over hours;
 *   whether a charger is there: certainly when a PC is talking over the
 *   USB port; otherwise from the step the cell's voltage takes when one
 *   is plugged in or pulled out, and failing that from the trend. A PC
 *   that puts an idle port to sleep has not been unplugged.
 *
 * A restart -- a reboot, a crash, flashing -- carries all of that over
 * in RTC memory, so the percentage does not move because the system did.
 * Only a power-on or a wake from deep sleep starts again from the
 * voltage, and after a sleep that is the voltage of a rested cell, the
 * best reading there is.
 *
 * And the cell's life. What has been counted out of it since it was
 * fitted, over its capacity, is its cycles. What it holds is measured on
 * the way down from a full charge: once the corrected voltage says it is
 * down to a fifth -- the steep end of the table, where the voltage says
 * most -- what was counted out since it was full, over the share of the
 * whole that is, is the whole. Its health is what it holds now against
 * what the first of those measurements found. Both rest on the estimated
 * current, but its error is much the same from one discharge to the next,
 * and the ratio mostly cancels it.
 *
 * /etc/battery keeps the capacity (`battery -c`), the resistance and the
 * cell's life.
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_BATTERY

#define SAMPLES		64	/* the ADC is noisy, and the radio's bursts worse */
#define PERIOD_MS	5000
#define WARN_EVERY_MS	60000
#define SMOOTH_SHIFT	3	/* an eighth of each new reading: ~40 s */
#define TREND_MINUTES	10	/* how far back the trend looks */
#define LEARN_FILE	"/etc/battery"
#define SAVE_EVERY_MS	600000
/*
 * No lithium cell ever reads above about 4.25 V, so anything higher is
 * the charger's own rail with nothing on it: the board is on USB and
 * there is no battery to talk about.
 */
#define USB_ONLY_MV	4300

/*
 * The charger (a TP4054) pushes its set current until the cell reaches
 * 4.2 V, then holds it there while the current falls away, and stops. It
 * starts again when the cell has fallen to 4.05 V. Near 4.2 V and no
 * longer rising is full, and full lasts until then.
 */
#define FULL_MV		4200
#define TAPER_MV	4150	/* above this its current falls away */
#define DONE_MV		4170
#define DONE_TREND	15	/* mV an hour: not rising any more */
#define RECHARGE_MV	4050

/*
 * A rise this steady, with nothing plugged in that the system knows of,
 * is a charger it missed the step of; a fall as steady on the charger
 * means there is none. A 2500 mAh cell moves too slowly for this to see:
 * the step is what finds a charger for it.
 */
#define TREND_CHARGER	60	/* mV an hour */
#define TREND_SAMPLES	5	/* minutes of history before it is believed */

/*
 * What the chip adds to the board's idle draw, from the ESP32-S3
 * datasheet (table 5-8): a core running at 240 MHz instead of waiting,
 * and 240 MHz rather than 80 with both cores waiting. The radio's share
 * is rougher: receiving costs 88 mA (table 5-7), and away from its
 * network the radio spends a good part of its time listening for it,
 * which a short run on a 200 mAh cell put at about this.
 */
#define CORE_BUSY_MA	18
#define CPU_FAST_MA	11
#define RADIO_MA	40

/*
 * A step in the current this big, from the charger or the backlight, is
 * enough to read the cell's resistance off the voltage.
 */
#define STEP_MA		40
#define STEP_BACKLIGHT	40	/* percentage points */
#define STEP_MIN_MV	10	/* a charger coming or going moves the cell more */
#define STEP_NEAR_MV	4	/* just after the PC went quiet: looked for closely */
#define WATCH		4	/* readings looked at for that step: two, then two */
#define STILL		10	/* permille against the current that is only noise */
#define CREEP		2	/* permille a reading it then moves by: 2.4 points a minute */

/*
 * The counted charge follows the corrected voltage with a time constant
 * of ten minutes: long enough to ride over work the current estimate
 * gets wrong, short enough that a wrong guess does not last. When the two
 * are far apart -- a new cell, a count that went wrong -- it follows ten
 * times faster.
 */
#define PULL		120	/* readings: ten minutes */
#define PULL_FAST	12
#define FAR_PERMILLE	150

/* The cell's life: see the top of the file. */
#define MEASURE_AT	200	/* permille left */
#define MEASURE_SPAN	500	/* or this much used, if the charger comes back first */
#define NEW_MEASURES	2	/* what it held new is the mean of the first ones */

/*
 * A lithium-polymer cell at rest, at 0, 5, ... 100 percent: the table
 * usually given for one. A particular cell differs from it by a few
 * points; the counting between readings smooths over that, but cannot
 * take it away.
 */
static const uint16_t curve[] = {
	3270, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820, 3840,
	3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200,
};

static struct {
	int	 raw;			/* the last reading */
	int	 smooth;		/* millivolts at the terminals, filtered */
	int	 rest;			/* millivolts with the sag put back, filtered */
	int	 trend;			/* millivolts an hour, signed */
	int	 history[TREND_MINUTES];
	int	 samples;
	int	 ma;			/* out of the cell; into it is negative */
	int	 ma_avg;		/* the same over a few minutes */
	bool	 usb;			/* a PC on the port, at the last reading */
	bool	 charger;		/* what we believe: one is plugged in */
	int	 seen_mv[WATCH];	/* the last readings, for the charger's step */
	int	 seen_ma[WATCH];	/* and the load's current at each */
	int	 nseen;
	enum battery_state state;
	int	 capacity;		/* mAh, on its label */
	int	 mohm;			/* the cell's resistance */
	int	 steps;			/* how many steps it was learned from */
	int	 charge;		/* µAh left in the cell */
	int	 shown;			/* permille; -1 before the first estimate */
	int	 near;			/* readings left to look closely for a step */
	int64_t	 counted;		/* when the charge was last counted, µs */
	bool	 reseed;		/* start the count again from the voltage */
	/* its life */
	int64_t	 used;			/* µAh counted out since it was fitted */
	int	 new_mah;		/* what it held new, measured; 0 until then */
	int	 holds_mah;		/* what it holds lately, measured */
	int	 measured;		/* discharges measured */
	bool	 from_full;		/* a measurement under way */
	int64_t	 since_full;		/* µAh counted out since it was full */
	int	 left;			/* permille by the voltage, discharging */
	bool	 dirty;
} bat = { .capacity = CONFIG_PT_BATTERY_MAH, .shown = -1, .reseed = true };

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t	 cali;
static adc_channel_t		 channel;
static SemaphoreHandle_t	 adc_lock;

/*
 * What a restart must not lose, kept where a restart does not reach. A
 * power-on leaves it random, so it is only believed after a restart, and
 * the check catches a brown-out that scrambled it.
 */
#define KEPT_MAGIC	0x42415432	/* "BAT2" */
static RTC_NOINIT_ATTR struct {
	uint32_t magic;
	int	 capacity;
	int	 charge;
	int	 shown;
	bool	 charger;
	bool	 full;
	bool	 from_full;
	int64_t	 used;
	int64_t	 since_full;
	uint32_t check;
} kept;

static uint32_t kept_check(void)
{
	const uint8_t *p = (const uint8_t *)&kept;
	uint32_t h = 2166136261u;		/* FNV-1a */

	for (size_t i = 0; i < offsetof(typeof(kept), check); i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void keep(void)
{
	kept.magic = KEPT_MAGIC;
	kept.capacity = bat.capacity;
	kept.charge = bat.charge;
	kept.shown = bat.shown;
	kept.charger = bat.charger;
	kept.full = bat.state == BATTERY_FULL;
	kept.from_full = bat.from_full;
	kept.used = bat.used;
	kept.since_full = bat.since_full;
	kept.check = kept_check();
}

static bool restarted(void)
{
	esp_reset_reason_t why = esp_reset_reason();

	return why != ESP_RST_POWERON && why != ESP_RST_DEEPSLEEP && why != ESP_RST_BROWNOUT &&
	       why != ESP_RST_UNKNOWN;
}

/*
 * Until a cell has shown its resistance: pouch cells come to roughly
 * 100 ohm-mAh, so half an ohm for 200 mAh and 0.04 for 2500, and the
 * wires and the socket add a little.
 */
static int default_mohm(int mah)
{
	return 50 + 100000 / mah;
}

/* Charge at rest, in permille, from the table. */
static int permille_of(int mv)
{
	const int steps = sizeof(curve) / sizeof(curve[0]) - 1;

	if (mv <= curve[0])
		return 0;
	for (int i = 0; i < steps; i++) {
		if (mv < curve[i + 1])
			return i * 50 + (mv - curve[i]) * 50 / (curve[i + 1] - curve[i]);
	}
	return 1000;
}

/* What it holds, as far as anyone knows: measured, or its label. */
static int holds_mah(void)
{
	return bat.holds_mah ? bat.holds_mah : bat.capacity;
}

/* What this cell is and has been seen to do, across power cuts. */
static void learn_load(void)
{
	char path[64], line[64];
	FILE *f;

	if (!mount_resolve(LEARN_FILE, path, sizeof(path)))
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		long long ll;
		int v;

		if (sscanf(line, "capacity %d", &v) == 1 && v >= 50 && v <= 20000)
			bat.capacity = v;
		else if (sscanf(line, "resistance %d", &v) == 1 && v >= 20 && v <= 3000)
			bat.mohm = v;
		else if (sscanf(line, "steps %d", &v) == 1 && v >= 0)
			bat.steps = v;
		else if (sscanf(line, "used %lld", &ll) == 1 && ll >= 0)
			bat.used = ll * 1000;
		else if (sscanf(line, "new %d", &v) == 1 && v >= 0)
			bat.new_mah = v;
		else if (sscanf(line, "holds %d", &v) == 1 && v >= 0)
			bat.holds_mah = v;
		else if (sscanf(line, "measured %d", &v) == 1 && v >= 0)
			bat.measured = v;
	}
	fclose(f);
	if (!bat.steps)
		bat.mohm = default_mohm(bat.capacity);
	if (!bat.new_mah || !bat.holds_mah)
		bat.new_mah = bat.holds_mah = bat.measured = 0;
}

static void learn_save(void)
{
	char path[64];
	FILE *f;

	if (!bat.dirty || !mount_resolve(LEARN_FILE, path, sizeof(path)))
		return;
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "# the cell: `battery -c MAH` when a new one is fitted\n"
		"capacity %d\nresistance %d\nsteps %d\n"
		"# its life: mAh used since it was fitted, and what it held\n"
		"# new and lately, measured over this many discharges\n"
		"used %lld\nnew %d\nholds %d\nmeasured %d\n",
		bat.capacity, bat.mohm, bat.steps, (long long)(bat.used / 1000), bat.new_mah,
		bat.holds_mah, bat.measured);
	if (!fclose(f))
		bat.dirty = false;
}

void battery_save(void)
{
	if (adc) {
		bat.dirty = true;
		learn_save();
	}
}

/* The ADC, from whoever reads it first: the empty check at boot, or init. */
static int adc_setup(void)
{
	adc_unit_t unit;
	adc_oneshot_unit_init_cfg_t unit_cfg;
	const adc_oneshot_chan_cfg_t chan_cfg = {
		.atten = ADC_ATTEN_DB_12,	/* up to about 3.1 V at the pin */
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	adc_cali_curve_fitting_config_t cali_cfg = {
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};

	if (adc)
		return 0;
	if (adc_oneshot_io_to_channel(CONFIG_PT_BATTERY_ADC_GPIO, &unit, &channel)) {
		klog("battery: GPIO%d is not an ADC pin", CONFIG_PT_BATTERY_ADC_GPIO);
		return -EINVAL;
	}
	unit_cfg = (adc_oneshot_unit_init_cfg_t){ .unit_id = unit };
	if (adc_oneshot_new_unit(&unit_cfg, &adc) || adc_oneshot_config_channel(adc, channel, &chan_cfg)) {
		klog("battery: ADC setup failed");
		adc = NULL;
		return -EIO;
	}
	cali_cfg.unit_id = unit;
	cali_cfg.chan = channel;
	if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali))
		cali = NULL;		/* uncalibrated chip: readings are rough */
	adc_lock = xSemaphoreCreateMutex();
	return 0;
}

int battery_early_millivolts(void)
{
	int err = adc_setup();

	return err ? err : battery_millivolts();
}

static int by_value(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/*
 * A reading: the middle half of many samples, so a burst from the radio,
 * which lands at one end, is left out rather than averaged in.
 */
int battery_millivolts(void)
{
	int mv[SAMPLES], total = 0, err = 0;

	if (!adc)
		return -ENODEV;
	xSemaphoreTake(adc_lock, portMAX_DELAY);
	for (int i = 0; i < SAMPLES && !err; i++) {
		int raw;

		if (adc_oneshot_read(adc, channel, &raw))
			err = -EIO;
		else if (!cali)
			mv[i] = raw * 3100 / 4095;	/* rough, uncalibrated chips */
		else if (adc_cali_raw_to_voltage(cali, raw, &mv[i]))
			err = -EIO;
	}
	xSemaphoreGive(adc_lock);
	if (err)
		return err;
	qsort(mv, SAMPLES, sizeof(mv[0]), by_value);
	for (int i = SAMPLES / 4; i < SAMPLES * 3 / 4; i++)
		total += mv[i];
	return total / (SAMPLES / 2) * CONFIG_PT_BATTERY_SCALE / 100;
}

int battery_percent(void)
{
	return bat.shown < 0 ? -ENODEV : bat.shown / 10;
}

/* Everything the task has worked out, for `battery`, `power` and the bar. */
int battery_status(struct battery_status *out)
{
	int mv = bat.smooth ? bat.smooth : battery_millivolts();
	int holds = holds_mah();

	memset(out, 0, sizeof(*out));
	if (mv < 0)
		return mv;
	out->mv = mv;
	out->rest = bat.rest;
	out->raw = bat.raw;
	out->trend = bat.trend;
	out->state = bat.state;
	out->permille = bat.shown < 0 ? 0 : bat.shown;
	out->percent = out->permille / 10;
	out->ma = bat.ma_avg;
	out->capacity_mah = bat.capacity;
	out->holds_mah = bat.holds_mah;
	out->mohm = bat.mohm;
	out->mohm_guessed = !bat.steps;
	out->charger = bat.charger;
	out->usb = bat.usb;
	out->used_mah = (int)(bat.used / 1000);
	out->cycles10 = (int)(bat.used / 100 / bat.capacity);
	out->health = bat.new_mah ? bat.holds_mah * 100 / bat.new_mah : -1;
	if (out->health > 100)
		out->health = 100;		/* measured a little high: it is not better than new */
	out->measured = bat.measured;
	out->minutes_left = -1;
	out->minutes_full = -1;
	if (bat.state == BATTERY_DISCHARGING && bat.ma_avg > 5)
		out->minutes_left = bat.charge / 1000 * 60 / bat.ma_avg;
	else if (bat.state == BATTERY_CHARGING && bat.ma_avg < -5)
		out->minutes_full = (holds * 1000 - bat.charge) / 1000 * 60 / -bat.ma_avg;
	return 0;
}

const char *battery_state_name(enum battery_state state)
{
	switch (state) {
	case BATTERY_CHARGING:	  return "charging";
	case BATTERY_FULL:	  return "full";
	case BATTERY_DISCHARGING: return "on battery";
	case BATTERY_UNKNOWN:	  return "working it out";
	default:		  return "no battery fitted";
	}
}

/* How busy each core has been since the last call, in permille. */
static void cpu_busy(int busy[portNUM_PROCESSORS])
{
	static uint64_t last_idle[portNUM_PROCESSORS];
	static int64_t last_time;
	int64_t now = esp_timer_get_time();

	for (int core = 0; core < portNUM_PROCESSORS; core++) {
		uint64_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
		uint64_t waited = idle - last_idle[core];
		int64_t span = now - last_time;

		busy[core] = 0;
		if (last_time && span > 0)
			busy[core] = 1000 - (int)(waited >= (uint64_t)span ? 1000 : waited * 1000 / span);
		last_idle[core] = idle;
	}
	last_time = now;
}

/*
 * The charger's current into the cell: what its resistor sets, until the
 * cell reaches the charger's voltage, then less and less as it fills.
 */
static int charge_ma(int mv)
{
	if (mv <= TAPER_MV)
		return CONFIG_PT_BATTERY_CHARGE_MA;
	if (mv >= FULL_MV)
		return 0;
	return CONFIG_PT_BATTERY_CHARGE_MA * (FULL_MV - mv) / (FULL_MV - TAPER_MV);
}

/* What the board draws, estimated from what it is doing. */
static int load_ma(void)
{
	int busy[portNUM_PROCESSORS], most = 0, all = 0, ma;

	cpu_busy(busy);			/* every time, so its window is one reading */
	for (int core = 0; core < portNUM_PROCESSORS; core++) {
		most = busy[core] > most ? busy[core] : most;
		all += busy[core];
	}
	ma = CONFIG_PT_BATTERY_IDLE_MA + CONFIG_PT_BATTERY_BACKLIGHT_MA * lcd_backlight_get() / 100;
	ma += CPU_FAST_MA * most / 1000 + CORE_BUSY_MA * all / 1000;
	if (wifi_started())
		ma += RADIO_MA;
	return ma;
}

/*
 * The current out of the cell. With a charger the board runs from its
 * supply, so all the cell sees is the charge going in, or nothing once
 * it is full.
 */
static int draw_ma(int mv, int load)
{
	if (bat.state == BATTERY_FULL)
		return 0;
	return bat.charger ? -charge_ma(mv) : load;
}

static int max2(int a, int b) { return a > b ? a : b; }
static int min2(int a, int b) { return a < b ? a : b; }

static void learn_sample(int sample)
{
	if (sample >= 20 && sample <= 3000 &&
	    (bat.steps < 3 || (sample < bat.mohm * 3 && sample > bat.mohm / 3))) {
		bat.mohm += (sample - bat.mohm) / (bat.steps < 3 ? 2 : 4);
		bat.steps++;
		bat.dirty = true;
	}
}

/*
 * Is a charger there? A PC talking over the port says yes. Without one,
 * a charger plugged in lifts the cell by its current and the load's times
 * the resistance, within a reading or two, and pulling it out drops it as
 * much: two readings are compared with the two before, the load's own
 * changes (the backlight) taken off. The lowest of the two after must be
 * above the highest of the two before, so the dip of a burst from the
 * radio in one reading is not mistaken for a charger. A step found also
 * says what the cell's resistance is.
 */
static void watch_charger(int mv, int load, bool usb)
{
	int *v = bat.seen_mv, *l = bat.seen_ma, thr, dload, lifted, dropped;

	if (bat.nseen == WATCH) {
		memmove(v, v + 1, (WATCH - 1) * sizeof(*v));
		memmove(l, l + 1, (WATCH - 1) * sizeof(*l));
		bat.nseen--;
	}
	v[bat.nseen] = mv;
	l[bat.nseen++] = load;
	if (usb) {
		if (!bat.charger)
			klog("battery: on the charger (a PC is on USB)");
		bat.charger = true;
		return;
	}
	if (bat.nseen < WATCH)
		return;
	dload = (l[2] + l[3] - l[0] - l[1]) / 2;
	thr = max2(STEP_MIN_MV, (CONFIG_PT_BATTERY_CHARGE_MA + l[3]) * bat.mohm / 2000);
	/*
	 * The PC going quiet is the moment to look: it has put the port to
	 * sleep, or it has been unplugged, and only the second drops the
	 * cell. Knowing when, a smaller step will do.
	 */
	if (bat.near) {
		thr = max2(STEP_NEAR_MV, (CONFIG_PT_BATTERY_CHARGE_MA + l[3]) * bat.mohm / 3000);
		bat.near--;
	}
	/* the lift above what the load's change alone would give */
	lifted = min2(v[2], v[3]) - (max2(v[0], v[1]) - dload * bat.mohm / 1000);
	dropped = min2(v[0], v[1]) - max2(v[2], v[3]);
	if (!bat.charger && lifted > thr) {
		bat.charger = true;
		klog("battery: a charger, by the step (+%d mV)", lifted);
		learn_sample(((v[2] + v[3] - v[0] - v[1]) / 2 + dload * bat.mohm / 1000) * 1000 /
			     (CONFIG_PT_BATTERY_CHARGE_MA + l[3]));
		bat.nseen = 0;
	} else if (bat.charger && dropped > thr) {
		bat.charger = false;
		klog("battery: off the charger, by the step (-%d mV)", dropped);
		learn_sample((v[0] + v[1] - v[2] - v[3]) / 2 * 1000 /
			     (CONFIG_PT_BATTERY_CHARGE_MA + l[3]));
		bat.nseen = 0;
	}
}

/*
 * The cell's resistance, from a step in the backlight's current (the
 * charger's are read in watch_charger): the voltage before the step and
 * after it, five seconds apart, which is too short for the charge in the
 * cell to move. A few steps are averaged, and once there are some, a
 * reading far from the rest (a burst from the radio at the wrong moment)
 * is left out.
 */
static void learn_resistance(int mv, int ma, bool step)
{
	static int before_mv, before_ma;
	int dma = ma - before_ma;

	if (step && before_mv && abs(dma) >= STEP_MA && mv < TAPER_MV && before_mv < TAPER_MV)
		learn_sample((before_mv - mv) * 1000 / dma);
	before_mv = mv;
	before_ma = ma;
}

/*
 * What the cell is doing. With a charger: full once it is at the
 * charger's voltage and no longer rising, and until the charger would
 * start again; charging otherwise. Without: discharging, or not known
 * yet before there is any history.
 */
static void decide_state(void)
{
	if (!bat.charger)
		bat.state = bat.samples >= 2 ? BATTERY_DISCHARGING : BATTERY_UNKNOWN;
	else if (bat.state == BATTERY_FULL && bat.smooth >= RECHARGE_MV)
		;
	else if (bat.smooth >= DONE_MV && bat.samples >= 3 && bat.trend <= DONE_TREND)
		bat.state = BATTERY_FULL;
	else
		bat.state = BATTERY_CHARGING;
}

/* What a discharge from full says the cell holds: see the top of the file. */
static void measure(int left)
{
	int used = 1000 - left, mah;

	bat.from_full = false;
	mah = (int)(bat.since_full / used);	/* µAh over permille is mAh */
	if (mah < bat.capacity / 2 || mah > bat.capacity * 3 / 2) {
		klog("battery: a discharge made it %d mAh, which cannot be right", mah);
		return;
	}
	bat.measured++;
	if (bat.measured <= NEW_MEASURES)
		bat.new_mah += (mah - bat.new_mah) / bat.measured;
	bat.holds_mah = bat.holds_mah ? bat.holds_mah + (mah - bat.holds_mah) / 4 : mah;
	bat.dirty = true;
	klog("battery: holds about %d mAh, by a discharge of %d%%", mah, used / 10);
}

/*
 * Count the charge by the current, then pull it towards the corrected
 * voltage; and count what goes out, for the cell's life.
 */
static void count_charge(void)
{
	int full = holds_mah() * 1000;		/* µAh */
	int64_t now = esp_timer_get_time();
	int target, pm, ms = bat.counted ? (int)((now - bat.counted) / 1000) : 0;
	int moved = (int)((int64_t)bat.ma * ms / 3600);	/* µAh out */
	bool far;

	bat.counted = now;
	target = bat.state == BATTERY_FULL ? full : permille_of(bat.rest) * holds_mah();
	if (bat.reseed) {
		bat.charge = target;		/* nothing counted yet: the voltage's word */
	} else {
		bat.charge -= moved;
		far = abs(target - bat.charge) > full / 1000 * FAR_PERMILLE;
		bat.charge += (target - bat.charge) / (far ? PULL_FAST : PULL);
	}
	bat.charge = bat.charge < 0 ? 0 : bat.charge > full ? full : bat.charge;

	if (moved > 0) {
		bat.used += moved;
		if (bat.from_full)
			bat.since_full += moved;
		bat.dirty = true;		/* the next save, within ten minutes */
	}

	/*
	 * The level shown, in tenths of a point, goes the way the current
	 * does at once. Against it -- the estimate was too high on the
	 * charger, or too low off it -- a point is noise and is left alone,
	 * and more is walked towards a little every reading: never stuck on
	 * a figure that is wrong, and never a jump. 100% is for a charger
	 * that has finished.
	 */
	pm = (int)((int64_t)bat.charge * 1000 / full);
	if (!bat.reseed && bat.shown >= 0 &&
	    ((bat.ma > 0 && pm > bat.shown) || (bat.ma < 0 && pm < bat.shown))) {
		if (abs(pm - bat.shown) <= STILL)
			pm = bat.shown;
		else
			pm = bat.shown + (pm > bat.shown ? CREEP : -CREEP);
	}
	if (pm >= 1000 && bat.state != BATTERY_FULL)
		pm = 999;
	bat.shown = pm;
	bat.reseed = false;
}

/* The cell's life, once a reading has been counted. */
static void wear(void)
{
	if (bat.state == BATTERY_FULL) {
		bat.from_full = true;		/* a measurement starts here */
		bat.since_full = 0;
		bat.left = 1000;
	} else if (!bat.from_full) {
		;
	} else if (bat.charger) {
		/* back on the charger: measure if enough went, or give up */
		if (1000 - bat.left >= MEASURE_SPAN)
			measure(bat.left);
		bat.from_full = false;
	} else if (bat.state == BATTERY_DISCHARGING) {
		bat.left = permille_of(bat.rest);
		if (bat.left <= MEASURE_AT)
			measure(bat.left);
	}
}

/* Fold one reading in: the charger, the current, the resistance, the charge. */
static void estimate(int mv, bool usb, bool step)
{
	int rest, load = load_ma();
	bool was = bat.charger;

	bat.raw = mv;
	if (bat.usb && !usb)
		bat.near = WATCH / 2;		/* the two readings after it */
	bat.usb = usb;
	watch_charger(mv, load, usb);
	decide_state();
	bat.ma = draw_ma(mv, load);
	learn_resistance(mv, bat.ma, step);
	rest = mv + bat.ma * bat.mohm / 1000;
	/*
	 * The filtered rest voltage was made under what was believed about
	 * the charger; when that changes, or the count starts again, it is
	 * made again from the filtered terminal voltage. Left to catch up, it
	 * pulls the charge the wrong way for a minute, and the level shown,
	 * which does not go down on the charger, stands still until a reboot.
	 */
	if (bat.reseed || !bat.rest || bat.charger != was)
		bat.rest = bat.smooth + bat.ma * bat.mohm / 1000;
	else
		bat.rest += (rest - bat.rest) >> SMOOTH_SHIFT;
	bat.ma_avg = bat.reseed || !bat.ma_avg ? bat.ma : bat.ma_avg + (bat.ma - bat.ma_avg) / 24;
	count_charge();
	wear();
	keep();
}

int battery_set_capacity(int mah)
{
	if (mah < 50 || mah > 20000)
		return -EINVAL;
	/* A new cell: nothing learned from the old one applies to it. */
	bat.capacity = mah;
	bat.mohm = default_mohm(mah);
	bat.steps = 0;
	bat.used = 0;
	bat.new_mah = bat.holds_mah = bat.measured = 0;
	bat.from_full = false;
	bat.since_full = 0;
	bat.reseed = true;		/* and the count starts from its voltage */
	bat.dirty = true;
	learn_save();
	klog("battery: a new %d mAh cell", mah);
	return 0;
}

/*
 * Once a minute: where the voltage has gone over the last ten minutes,
 * and what that says about a charger.
 */
static void update_trend(int mv)
{
	int then, minutes;

	memmove(bat.history, bat.history + 1, sizeof(bat.history) - sizeof(bat.history[0]));
	bat.history[TREND_MINUTES - 1] = mv;
	if (bat.samples < TREND_MINUTES)
		bat.samples++;
	if (bat.samples < 2)
		return;			/* one reading is not a trend */
	/* as much history as there is, so it means something after two minutes */
	then = bat.history[TREND_MINUTES - bat.samples];
	minutes = bat.samples - 1;
	bat.trend = (mv - then) * 60 / minutes;
	if (bat.samples < TREND_SAMPLES || bat.state == BATTERY_FULL)
		return;
	if (!bat.charger && bat.trend >= TREND_CHARGER) {
		bat.charger = true;
		bat.rest = 0;			/* made again, under the charger */
		klog("battery: a charger, by the trend (%+d mV an hour)", bat.trend);
	} else if (bat.charger && !bat.usb && bat.trend <= -TREND_CHARGER) {
		bat.charger = false;
		bat.rest = 0;
		klog("battery: off the charger, by the trend (%+d mV an hour)", bat.trend);
	}
}

/*
 * Into the log, and nowhere else: the state of the battery is on the
 * status bar where it can be glanced at, and a warning printed into
 * whatever somebody is doing helps nobody.
 */
static void say(const char *fmt, int mv)
{
	char line[96];
	int n = snprintf(line, sizeof(line), fmt, mv / 1000, mv % 1000 / 10);

	klog("battery: %.*s", n, line);
}

static void battery_task(void *arg)
{
	int64_t last_warn = -WARN_EVERY_MS * 1000LL;
	int64_t last_minute = 0, last_save = esp_timer_get_time();
	int last_backlight = lcd_backlight_get();

	for (;; vTaskDelay(pdMS_TO_TICKS(PERIOD_MS))) {
		int mv = battery_millivolts();
		int64_t now = esp_timer_get_time();
		int backlight = lcd_backlight_get();
		bool usb = usb_serial_jtag_is_connected();
		bool step = abs(backlight - last_backlight) >= STEP_BACKLIGHT;

		last_backlight = backlight;
		if (mv < 0)
			continue;
		bat.raw = mv;
		if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV) {
			bat.state = BATTERY_USB;
			continue;	/* no cell: the board is on USB */
		}
		bat.smooth = bat.smooth ? bat.smooth + ((mv - bat.smooth) >> SMOOTH_SHIFT) : mv;
		if (now - last_minute >= 60 * 1000000LL) {
			last_minute = now;
			update_trend(bat.smooth);
		}
		estimate(mv, usb, step);
		if (now - last_save >= SAVE_EVERY_MS * 1000LL) {
			last_save = now;
			learn_save();
		}
		mv = bat.smooth;
		if (bat.charger)
			continue;	/* on the charger: nothing to warn about */
		/*
		 * Powering off is the last thing this system should ever do
		 * by itself, so it takes more than one low reading: the cell
		 * has to be falling, and there has to be enough history to
		 * know that it is. A flat cell on a charger reads low for
		 * minutes while it fills, and switching the machine off
		 * under someone at that moment is unforgivable.
		 */
		if (CONFIG_PT_BATTERY_OFF_MV && mv <= CONFIG_PT_BATTERY_OFF_MV &&
		    bat.state == BATTERY_DISCHARGING && bat.samples >= 3 && bat.trend < 0) {
			say("empty at %d.%02d V: saving and powering off", mv);
			battery_save();
			power_off_empty();
		}
		if (mv <= CONFIG_PT_BATTERY_WARN_MV &&
		    esp_timer_get_time() - last_warn > WARN_EVERY_MS * 1000LL) {
			last_warn = esp_timer_get_time();
			say("low at %d.%02d V: charge it soon", mv);
		}
	}
}

/*
 * Called once / is mounted, so the first estimate is made for the cell
 * that is fitted, with what was learned about it, rather than for the
 * default one and corrected with a jump five seconds later.
 */
int battery_init(void)
{
	int mv, err;
	bool usb, carried = false;

	if ((err = adc_setup()))
		return err;
	learn_load();
	mv = battery_millivolts();
	usb = usb_serial_jtag_is_connected();
	bat.smooth = mv > 0 ? mv : 0;
	bat.charger = usb;
	if (restarted() && kept.magic == KEPT_MAGIC && kept.check == kept_check() &&
	    kept.capacity == bat.capacity) {
		bat.charge = kept.charge;
		bat.shown = kept.shown;
		bat.charger = usb || kept.charger;
		bat.state = kept.full && bat.charger ? BATTERY_FULL : BATTERY_UNKNOWN;
		bat.from_full = kept.from_full;
		bat.since_full = kept.since_full;
		if (kept.used > bat.used)
			bat.used = kept.used;	/* counted since the file was last saved */
		bat.reseed = false;
		carried = true;
	}
	bat.raw = mv;
	if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV) {
		bat.state = BATTERY_USB;
		klog("battery: none fitted (the sense pin reads %d.%02d V)", mv / 1000, mv % 1000 / 10);
	} else {
		if (bat.state != BATTERY_FULL)
			bat.state = bat.charger ? BATTERY_CHARGING : BATTERY_UNKNOWN;
		estimate(mv, usb, false);
		klog("battery: %d.%02d V, %d%%%s; a %d mAh cell, %d mohm%s, %d.%d cycles",
		     mv / 1000, mv % 1000 / 10, bat.shown / 10, carried ? " (kept over the restart)" : "",
		     bat.capacity, bat.mohm, bat.steps ? "" : " (a guess)",
		     (int)(bat.used / 100 / bat.capacity / 10), (int)(bat.used / 100 / bat.capacity % 10));
	}
	xTaskCreatePinnedToCore(battery_task, "kbattery", 4096, NULL, 1, NULL, 0);
	return 0;
}

#else

int battery_init(void) { return -ENODEV; }
int battery_millivolts(void) { return -ENODEV; }
int battery_early_millivolts(void) { return -ENODEV; }
int battery_percent(void) { return -ENODEV; }
int battery_status(struct battery_status *out) { return -ENODEV; }
int battery_set_capacity(int mah) { return -ENODEV; }
void battery_save(void) { }
const char *battery_state_name(enum battery_state state) { return "no cell"; }

#endif
