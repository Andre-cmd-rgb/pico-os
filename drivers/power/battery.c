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
 * The readings themselves can be a few percent off: the ADC's own
 * calibration and the divider's resistors each allow for that, and at
 * the top of a lithium cell's table 3% of the voltage is a third of its
 * charge. So they are scaled by what a meter says (`battery -m`), or,
 * failing a meter, by the charger: a TP4054 holds the cell at 4.20 V
 * once it is nearly full, so a voltage that rose on the charger and then
 * stood still is 4.20 V, whatever the reading says.
 *
 * On the Freenove board the USB supply runs the board through a diode
 * while a P-MOSFET cuts the cell off from the load, so on USB the cell
 * only ever sees the charger. The charger's status pin is not wired to
 * anything, so when it has finished is seen from the voltage: standing
 * still on USB, for long enough that charging at its current would have
 * moved it.
 *
 * Another charger on the cell (a TP4056 module on its wires, say) is
 * found by the step it makes, and the step, over the resistance learned,
 * says how much it pushes.
 *
 * /etc/battery keeps the capacity (`battery -c`), the resistance, the
 * calibration and the cell's life.
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_pm.h"
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
/*
 * The power log: a line at every start, every sleep and every half hour
 * awake, with the level and the voltage. A night's sleep, or where a day's
 * charge went, can be read back from it (`battery -l`). It is kept to a
 * few hundred lines: past that it starts again, the old one kept aside.
 */
#define LOG_FILE	"/etc/power.log"
#define LOG_OLD		"/etc/power.log.1"
#define LOG_MAX		24000		/* bytes */
#define LOG_EVERY_MS	(30 * 60 * 1000)
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
 * On USB, a voltage that has not moved by more than this in half an
 * hour is a charger that has stopped: at 300 mA into 2500 mAh even the
 * flat middle of the table rises 20 mV in that time. A cell that is
 * already high needs only ten minutes to say so.
 *
 * Stopped is not always full. The board's TP4054 does not start on a
 * cell above its recharge threshold, about 4.05 V: plugged in at 85%, it
 * leaves it at 85% (a meter read 4.075 V on a cell it would not touch).
 * Only a cell stopped near the charger's 4.20 V is full.
 */
#define FLAT_MINUTES	30
#define FLAT_HIGH_MIN	10
#define FLAT_HIGH_MV	4050
#define FLAT_MV		6
#define FLAT_FULL_MV	4150	/* stopped above this is finished */
#define ROSE_TREND	20	/* mV an hour: it was charging before it stood */
#define HELD_MV		4190	/* where the charger holds it: 4.20 V, less its wires */
#define UNSURE_MV	4000	/* above this, is the charger charging at all? */

/* A calibration outside this (in ten-thousandths) is a mistake, not a meter. */
#define CAL_ONE		10000
#define CAL_MIN		9000
#define CAL_MAX		11000

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
 * What light sleep saves: the chip waiting at 80 MHz draws about 20 mA
 * (table 5-8 again), and asleep with PSRAM kept it is nearer 2. The panel
 * asleep with its lamp out saves its own few milliamps.
 */
#define CHIP_WAIT_MA	18
#define PANEL_MA	5

/*
 * The cell's resistance is read off the step the board's charger makes,
 * whose current is known: 300 mA and the board's load, which moves to
 * USB. The backlight's current is then read off the steps it makes when
 * it dims or comes back, which are the only changes in the load big
 * enough and clean enough (nothing else moves at that moment). Learning
 * the resistance from the backlight instead, with its current guessed,
 * took the guess's error into the resistance: the lamp draws nearer
 * 80 mA than the 45 its resistor suggested, and the resistance came out
 * at twice its size.
 */
#define STEP_BACKLIGHT	40	/* percentage points */
#define MODEL		2	/* what /etc/battery's figures were learned by */
#define SLIDE		10	/* permille the level moves in a reading, at most */
#define STEP_MIN_MV	10	/* a charger coming or going moves the cell more */

/*
 * A charger lifts the cell above the voltage it would rest at, by its
 * current over the resistance: well over a hundred millivolts at a
 * module's 1 A. Nothing else can -- a load going away only lets the cell
 * come back up to rest, and a heavy one ending (a clip, the radio's
 * search) was taken for a charger when the thresholds were lower. So a
 * charger found by a step or a trend must have the cell LIFT_MIN_MV above
 * where the count says it rests, and one that does not keep it at least
 * half that for LIFT_LOST_MIN minutes, before LIFT_SURE_MIN have shown it
 * charging, is let go. In the first seconds after a start the load is
 * settling (the panel, the radio) and no step is believed.
 */
#define LIFT_MIN_MV	60
#define LIFT_LOST_MIN	3
#define LIFT_SURE_MIN	5
#define START_SETTLE_US	(20 * 1000000LL)
#define STEP_NEAR_MV	4	/* just after the PC went quiet: looked for closely */
#define WATCH		4	/* readings looked at for that step: two, then two */


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

/*
 * On a charger the voltage at the terminals stands above the cell's by
 * more than its resistance explains: the chemistry lags the current, by
 * 50 to 150 mV at these currents, and it takes as long to settle once the
 * charger goes. Near 4 V that is 10 to 20 points, and a count pulled
 * towards it ran ahead while charging and dropped back after (65% to 70%
 * in three minutes, and back down to 59% once off). So while a charge
 * goes in, and for half an hour after, the count leads and the voltage
 * only nudges it; "full" sets it right at the end. On USB with the
 * charger idle the cell is at rest and its voltage is the best there is.
 */
#define PULL_CHARGING	720		/* readings: an hour */
#define SETTLE_US	(30 * 60 * 1000000LL)

/*
 * A charge that has finished is 100%, and stays near it: the cell's
 * voltage falls for a couple of hours after a charger stops, 4.20 V to
 * 4.13 on this 2500 mAh cell, and the table read that as 92% within the
 * hour ("the charger's light is green but it never got to 100"). So for
 * that long only the count moves it. At the top of the table a few
 * millivolts are several points, and the voltage's word there is only
 * ever a nudge.
 */
#define SETTLE_FULL_US	(2 * 60 * 60 * 1000000LL)
#define TOP_PERMILLE	900

/*
 * A charger ends at a tenth of its current: a TP4056 turns its light
 * green and stops there, and the board's own load moves onto the cell.
 * That step is small next to a charger pulled out at full current, so a
 * cell held full is watched for a smaller one; and a charger that stops
 * without a step to see still leaves the cell falling away from where
 * it was held.
 */
#define END_FRACTION	10
#define FULL_FALL_MV	20

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
	int64_t	 settle;		/* until then the voltage only nudges the count */
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
	/* the charger(s) */
	int	 charge_ma;		/* pushed into the cell, below the taper */
	int	 extra_ma;		/* another charger on top of the USB one */
	bool	 learnable;		/* charge_ma is known, not guessed */
	int	 flat[FLAT_MINUTES];	/* a reading a minute on USB, newest last */
	int	 nflat;
	bool	 rose;			/* seen charging on this USB session */
	bool	 sure;			/* the charger found has lifted the cell for minutes */
	uint8_t	 lifted_min, unlifted_min;
	bool	 stopped;		/* on USB, flat and low: not charging */
	bool	 usb_seen;		/* usb, as watch_charger last saw it */
	/* the readings */
	int	 cal;			/* ten-thousandths; CAL_ONE is none */
	bool	 cal_meter;		/* from a meter, not from the charger */
	int64_t	 slept_us;		/* light sleep, for the current estimate */
	int	 backlight_ma;		/* the lamp at full brightness, learned */
	int	 bl_steps;
	int	 pre_mv, pre_load;	/* just before a PC was plugged in */
	int	 pending_mohm;		/* read off that step, until charging is seen */
} bat = { .capacity = CONFIG_PT_BATTERY_MAH, .shown = -1, .reseed = true, .cal = CAL_ONE,
	  .charge_ma = CONFIG_PT_BATTERY_CHARGE_MA, .backlight_ma = CONFIG_PT_BATTERY_BACKLIGHT_MA };

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t	 cali;
static adc_channel_t		 channel;
static SemaphoreHandle_t	 adc_lock;

/*
 * What a restart must not lose, kept where a restart does not reach. A
 * power-on leaves it random, so it is only believed after a restart, and
 * the check catches a brown-out that scrambled it.
 */
#define KEPT_MAGIC	0x42415433	/* "BAT3" */
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
	int	 charge_ma;
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
	kept.charge_ma = bat.charge_ma;
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

/* The voltage the cell rests at with `pm` permille in it: the curve, read back. */
static int mv_of_permille(int pm)
{
	const int steps = sizeof(curve) / sizeof(curve[0]) - 1;
	int i;

	if (pm <= 0)
		return curve[0];
	if (pm >= 1000)
		return curve[steps];
	i = pm / 50;
	return curve[i] + (curve[i + 1] - curve[i]) * (pm - i * 50) / 50;
}

/* Whether `mv` stands `margin` above where the count says the cell rests. */
static bool above_rest(int mv, int margin)
{
	int full = holds_mah() * 1000;
	int pm = full > 0 ? (int)((int64_t)bat.charge * 1000 / full) : 0;

	return mv >= mv_of_permille(pm) + margin;
}

/* The current a charger found on the cell's wires would push: the board's, or the module's. */
static int expect_ma(void)
{
	if (CONFIG_PT_BATTERY_CHARGE_MA)
		return CONFIG_PT_BATTERY_CHARGE_MA;
	return CONFIG_PT_BATTERY_MODULE_MA ? CONFIG_PT_BATTERY_MODULE_MA : 300;
}

/* A charger just found: it has to show it is lifting the cell (see LIFT_MIN_MV). */
static void charger_found(void)
{
	bat.sure = false;
	bat.lifted_min = bat.unlifted_min = 0;
}

/*
 * The charger went while it held the cell full: it has finished (or was
 * taken away at the very end, which is as good). The count is full, the
 * level says so at once, and the voltage's fall that follows is let be.
 */
static void charge_finished(void)
{
	bat.charge = holds_mah() * 1000;
	bat.shown = 1000;
	bat.settle = esp_timer_get_time() + SETTLE_FULL_US;
	klog("battery: charged: the charger has finished");
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
	int model = 1;

	while (fgets(line, sizeof(line), f)) {
		long long ll;
		int v;

		if (sscanf(line, "model %d", &v) == 1)
			model = v;
		else if (sscanf(line, "backlight %d", &v) == 1 && v >= 20 && v <= 250)
			bat.backlight_ma = v;
		else if (sscanf(line, "capacity %d", &v) == 1 && v >= 50 && v <= 20000)
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
		else if (sscanf(line, "calibration %d", &v) == 1 && v >= CAL_MIN && v <= CAL_MAX) {
			bat.cal = v;
			bat.cal_meter = strstr(line, "meter") != NULL;
		}
	}
	fclose(f);
	if (model < MODEL) {
		/* learned the old way, from the backlight's guessed current */
		bat.steps = 0;
		bat.backlight_ma = CONFIG_PT_BATTERY_BACKLIGHT_MA;
		bat.dirty = true;
	}
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
		"model %d\ncapacity %d\nresistance %d\nsteps %d\nbacklight %d\n"
		"# its life: mAh used since it was fitted, and what it held\n"
		"# new and lately, measured over this many discharges\n"
		"used %lld\nnew %d\nholds %d\nmeasured %d\n"
		"# readings times this over 10000: from a meter (`battery -m`)\n"
		"# or from where the charger holds a full cell\n"
		"calibration %d %s\n",
		MODEL, bat.capacity, bat.mohm, bat.steps, bat.backlight_ma,
		(long long)(bat.used / 1000), bat.new_mah,
		bat.holds_mah, bat.measured, bat.cal,
		bat.cal == CAL_ONE ? "none" : bat.cal_meter ? "meter" : "charger");
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
 * which lands at one end, is left out rather than averaged in. This is
 * what the pin says, before the calibration.
 */
static int read_raw(void)
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

static int calibrated(int raw)
{
	return (int)((int64_t)raw * bat.cal / CAL_ONE);
}

int battery_millivolts(void)
{
	int raw = read_raw();

	return raw < 0 ? raw : calibrated(raw);
}

/*
 * The history was made at the old scale: the level starts again from
 * the voltage at the new one, as after a power-on.
 */
static void rescale(int from, int to)
{
	bat.smooth = (int)((int64_t)bat.smooth * to / from);
	bat.rest = bat.smooth;
	bat.samples = 0;
	bat.nflat = 0;
	bat.nseen = 0;
	bat.reseed = true;
}

/*
 * What a meter on the cell's wires says it is now: the readings are
 * scaled to agree from here on. Several readings, so one unlucky one
 * does not set it.
 */
int battery_calibrate(int meter_mv)
{
	int total = 0, n = 0, cal, was = bat.cal;

	if (!adc)
		return -ENODEV;
	if (meter_mv < 2500 || meter_mv > 4400)
		return -EINVAL;
	for (int i = 0; i < 8; i++) {
		int raw = read_raw();

		if (raw > 0) {
			total += raw;
			n++;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	if (!n)
		return -EIO;
	cal = (int)((int64_t)meter_mv * CAL_ONE * n / total);
	if (cal < CAL_MIN || cal > CAL_MAX)
		return -ERANGE;
	bat.cal = cal;
	bat.cal_meter = true;
	bat.dirty = true;
	rescale(was, cal);
	learn_save();
	klog("battery: calibrated against a meter: %d.%02d V, the pin said %d.%02d V",
	     meter_mv / 1000, meter_mv % 1000 / 10, total / n / 1000, total / n % 1000 / 10);
	return 0;
}

/*
 * Whole points, for the status line: rounded, so a cell just charged
 * reads 100 for its first half point and not for a moment, but never
 * 100 while a charge is still going in.
 */
static int whole_percent(int pm)
{
	int pc = (pm + 5) / 10;

	return pc > 99 && bat.state == BATTERY_CHARGING ? 99 : pc;
}

int battery_percent(void)
{
	return bat.shown < 0 ? -ENODEV : whole_percent(bat.shown);
}

static int charge_ma(int mv);

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
	out->percent = whole_percent(out->permille);
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
	out->cal = bat.cal;
	out->cal_meter = bat.cal_meter;
	out->charge_ma = bat.charger && bat.state == BATTERY_CHARGING ? charge_ma(bat.rest) : 0;
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
	case BATTERY_IDLE:	  return "on USB, not charging";	/* resting, or above 4.05 V */
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
 * The charger's current into the cell: what it was found to push, until
 * the cell reaches the charger's voltage, then less and less as it fills.
 */
/*
 * Whether the board's own charger charges the cell. On the Freenove board
 * it does not, as far as anything can see (the board file says why): on
 * USB the board runs from the PC and the cell rests, and the only charger
 * is a module on the cell's wires, found by the step it makes.
 */
#define BOARD_CHARGES	(CONFIG_PT_BATTERY_CHARGE_MA > 0)

static int charge_ma(int mv)
{
	if (mv <= TAPER_MV)
		return bat.charge_ma;
	if (mv >= FULL_MV)
		return 0;
	return bat.charge_ma * (FULL_MV - mv) / (FULL_MV - TAPER_MV);
}

/*
 * Time the chip spent in light sleep, counted as it wakes. The callback
 * runs in an idle task, so it only adds.
 */
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t woke(int64_t slept_us, void *arg)
{
	bat.slept_us += slept_us;
	return ESP_OK;
}

static void count_sleep(void)
{
	esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = woke };

	esp_pm_light_sleep_register_cbs(&cbs);
}
#else
static void count_sleep(void) { }
#endif

/* Permille of the time since the last call spent in light sleep. */
static int asleep_permille(void)
{
	static int64_t last_slept, last_at;
	int64_t now = esp_timer_get_time(), slept = bat.slept_us;
	int pm = 0;

	if (last_at && now > last_at)
		pm = (int)((slept - last_slept) * 1000 / (now - last_at));
	last_slept = slept;
	last_at = now;
	return pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
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
	ma = CONFIG_PT_BATTERY_IDLE_MA + bat.backlight_ma * lcd_backlight_now() / 100;
	ma += CPU_FAST_MA * most / 1000 + CORE_BUSY_MA * all / 1000;
	ma -= CHIP_WAIT_MA * asleep_permille() / 1000;
	if (!lcd_panel_on())
		ma -= PANEL_MA;
	if (wifi_radio_on())
		ma += RADIO_MA;
	ma += modem_load_ma();		/* a SIM module on the cell's wires */
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
	/* on USB the board runs from it, but a SIM module on the cell does not */
	if (bat.state == BATTERY_IDLE)
		return modem_load_ma();
	/*
	 * Just plugged into a PC with a cell high enough that the charger
	 * may not have started: nothing is counted in until the voltage is
	 * seen to rise, rather than counting 300 mA that are not there and
	 * then walking back down.
	 */
	if (bat.usb && !bat.rose && !bat.extra_ma && mv >= UNSURE_MV)
		return 0;
	return bat.charger ? -charge_ma(mv) : load;
}

static int max2(int a, int b) { return a > b ? a : b; }
static int min2(int a, int b) { return a < b ? a : b; }

static void learn_sample(int sample)
{
	if (sample >= 20 && sample <= 3000 &&
	    (bat.steps < 3 || (sample < bat.mohm * 2 && sample > bat.mohm / 2))) {
		bat.mohm += (sample - bat.mohm) / (bat.steps < 3 ? 2 : 4);
		bat.steps++;
		bat.dirty = true;
		klog("battery: the cell's resistance is about %d mohm (%d mohm this time)",
		     bat.mohm, sample);
	}
}

/*
 * The current a charger found by its step pushes into the cell. The
 * cell went from giving the load its current to taking the charger's
 * less the load's, so the step over the resistance is the charger's
 * current; take the load off and what is left goes into the cell. That
 * holds whether the charger feeds the board as well (a module on the
 * cell's wires) or the board moved onto USB (a charger with no PC).
 */
static int step_ma(int lift, int load)
{
	int ma = lift * 1000 / bat.mohm - load;

	/* with the resistance still a guess, a module's own rating says more */
	if (bat.steps < 3 && CONFIG_PT_BATTERY_MODULE_MA)
		ma = CONFIG_PT_BATTERY_MODULE_MA - load;
	return ma < 20 ? 20 : ma > 2000 ? 2000 : ma;
}

/*
 * Is a charger there? A PC talking over the port says yes. Without one,
 * a charger plugged in lifts the cell by its current and the load's times
 * the resistance, within a reading or two, and pulling it out drops it as
 * much: two readings are compared with the two before, the load's own
 * changes (the backlight) taken off. The lowest of the two after must be
 * above the highest of the two before, so the dip of a burst from the
 * radio in one reading is not mistaken for a charger.
 *
 * Only a step whose current is known says what the cell's resistance is:
 * the board's own charger, pulled out while it was still pushing its set
 * current. Any other charger's current is what its step says, over the
 * resistance learned. On USB a step up is a second charger on the cell's
 * wires, adding to the board's.
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
	if (usb && !bat.usb_seen) {
		klog(BOARD_CHARGES ? "battery: on the charger (a PC is on USB)" :
		     "battery: on USB: the board runs from it, the cell rests");
		/* the two readings before this one were on the cell alone */
		bat.pre_mv = BOARD_CHARGES && !bat.charger && bat.nseen >= 3 ?
			     max2(v[bat.nseen - 3], v[bat.nseen - 2]) : 0;
		bat.pre_load = bat.nseen >= 2 ? l[bat.nseen - 2] : 0;
		bat.pending_mohm = 0;
		if (BOARD_CHARGES) {
			bat.charger = true;
			bat.charge_ma = CONFIG_PT_BATTERY_CHARGE_MA;
			bat.extra_ma = 0;
			bat.learnable = true;
		} else if (bat.charger) {
			bat.extra_ma = bat.charge_ma;	/* the module, still going: see it go */
		}
		bat.nseen = 0;
	}
	bat.usb_seen = usb;
	/*
	 * Two readings on USB after it came: the lift is the board's load
	 * moving off the cell and the charger's current going in, if the
	 * charger started. Kept until it is seen charging.
	 */
	if (usb && bat.pre_mv && bat.nseen == 2) {
		int lift = min2(v[0], v[1]) - bat.pre_mv;

		if (lift > 0 && bat.pre_mv < TAPER_MV)
			bat.pending_mohm = lift * 1000 / (CONFIG_PT_BATTERY_CHARGE_MA + bat.pre_load);
		bat.pre_mv = 0;
	}
	if (bat.nseen < WATCH || esp_timer_get_time() < START_SETTLE_US)
		return;
	dload = (l[2] + l[3] - l[0] - l[1]) / 2;
	thr = max2(STEP_MIN_MV, (expect_ma() + l[3]) * bat.mohm / 2000);
	/*
	 * The PC going quiet is the moment to look: it has put the port to
	 * sleep, or it has been unplugged, and only the second drops the
	 * cell. Knowing when, a smaller step will do.
	 */
	if (bat.near) {
		int was_ma = bat.state == BATTERY_FULL ? 0 : charge_ma(bat.rest);

		thr = max2(STEP_NEAR_MV, (was_ma + l[3]) * bat.mohm / 3000);
		bat.near--;
	}
	if (bat.state == BATTERY_FULL)
		thr = min2(thr, max2(STEP_MIN_MV,
				     (expect_ma() / END_FRACTION + l[3]) * bat.mohm / 2000));
	/* the lift above what the load's change alone would give */
	lifted = min2(v[2], v[3]) - (max2(v[0], v[1]) - dload * bat.mohm / 1000);
	dropped = min2(v[0], v[1]) - max2(v[2], v[3]);
	if (usb) {
		/* the board's load is on USB: only another charger moves the cell */
		thr = max2(STEP_MIN_MV * 2, BOARD_CHARGES ? bat.mohm * 150 / 1000 :
						    expect_ma() * bat.mohm / 2000);
		if (bat.state == BATTERY_FULL && !BOARD_CHARGES)
			thr = max2(STEP_MIN_MV, expect_ma() / END_FRACTION * bat.mohm / 1000);
		if (!bat.extra_ma && lifted > thr && above_rest(min2(v[2], v[3]), LIFT_MIN_MV)) {
			if (!BOARD_CHARGES && CONFIG_PT_BATTERY_MODULE_MA && bat.smooth < TAPER_MV)
				learn_sample(lifted * 1000 / CONFIG_PT_BATTERY_MODULE_MA);
			bat.extra_ma = step_ma(lifted, 0);
			bat.charge_ma = CONFIG_PT_BATTERY_CHARGE_MA + bat.extra_ma;
			bat.charger = true;
			charger_found();
			klog(BOARD_CHARGES ? "battery: a second charger, by the step (+%d mV, about %d mA)" :
			     "battery: a charger, by the step (+%d mV, about %d mA into the cell)",
			     lifted, bat.extra_ma);
			bat.nseen = 0;
		} else if (bat.extra_ma && dropped > thr) {
			klog("battery: the %scharger is gone (-%d mV)", BOARD_CHARGES ? "second " : "",
			     dropped);
			if (bat.state == BATTERY_FULL && !BOARD_CHARGES)
				charge_finished();
			if (!BOARD_CHARGES && CONFIG_PT_BATTERY_MODULE_MA && bat.rest < TAPER_MV)
				learn_sample(dropped * 1000 / CONFIG_PT_BATTERY_MODULE_MA);
			bat.extra_ma = 0;
			bat.charge_ma = CONFIG_PT_BATTERY_CHARGE_MA;
			bat.charger = BOARD_CHARGES;
			bat.nseen = 0;
		}
		return;
	}
	if (!bat.charger && lifted > thr && above_rest(min2(v[2], v[3]), LIFT_MIN_MV)) {
		/* a module's rated current, over the step: the cell's resistance */
		if (!BOARD_CHARGES && CONFIG_PT_BATTERY_MODULE_MA && bat.smooth < TAPER_MV)
			learn_sample(lifted * 1000 / CONFIG_PT_BATTERY_MODULE_MA);
		bat.charger = true;
		bat.charge_ma = step_ma(lifted, l[1]);
		bat.extra_ma = 0;
		bat.learnable = false;
		charger_found();
		klog("battery: a charger, by the step (+%d mV, about %d mA into the cell)", lifted,
		     bat.charge_ma);
		bat.nseen = 0;
	} else if (bat.charger && dropped > thr) {
		bat.charger = false;
		klog("battery: off the charger, by the step (-%d mV)", dropped);
		if (bat.state == BATTERY_FULL)
			charge_finished();
		if (!BOARD_CHARGES && CONFIG_PT_BATTERY_MODULE_MA && bat.rest < TAPER_MV)
			learn_sample(dropped * 1000 / CONFIG_PT_BATTERY_MODULE_MA);
		if (bat.learnable && bat.rose && bat.state == BATTERY_CHARGING && bat.rest < TAPER_MV)
			learn_sample((v[0] + v[1] - v[2] - v[3]) / 2 * 1000 / (bat.charge_ma + l[3]));
		bat.nseen = 0;
	}
}

/*
 * The backlight's current at full brightness, from a step in it -- the
 * dimmer's, or a `backlight` command -- once the resistance is known:
 * the voltage before and after, five seconds apart, too short a time for
 * the charge in the cell to move. Only on the cell alone, and only when
 * nothing else changed at that moment: not the panel waking, not the
 * radio.
 */
static void learn_backlight(int mv, int percent, bool clean)
{
	static int before_mv, before_pct;
	int dpct = percent - before_pct;

	if (clean && before_mv && abs(dpct) >= STEP_BACKLIGHT && bat.steps >= 2 && !bat.charger &&
	    mv < TAPER_MV && before_mv < TAPER_MV) {
		int sample = (before_mv - mv) * 1000 * 100 / bat.mohm / dpct;

		if (sample >= 20 && sample <= 250 &&
		    (bat.bl_steps < 3 || (sample < bat.backlight_ma * 2 &&
					  sample > bat.backlight_ma / 2))) {
			bat.backlight_ma += (sample - bat.backlight_ma) / (bat.bl_steps < 3 ? 2 : 4);
			bat.bl_steps++;
			bat.dirty = true;
		}
	}
	before_mv = mv;
	before_pct = percent;
}

/*
 * What the cell is doing. With a charger: full once it is at the
 * charger's voltage and no longer rising, and until the charger would
 * start again; charging otherwise. Without: discharging, or not known
 * yet before there is any history.
 */
static void decide_state(void)
{
	if (!bat.charger && bat.usb)
		bat.state = BATTERY_IDLE;	/* the board runs from USB, the cell rests */
	else if (!bat.charger)
		bat.state = bat.samples >= 2 ? BATTERY_DISCHARGING : BATTERY_UNKNOWN;
	else if (bat.state == BATTERY_FULL && bat.smooth >= RECHARGE_MV)
		;
	else if (bat.smooth >= DONE_MV && bat.samples >= 3 && bat.trend <= DONE_TREND)
		bat.state = BATTERY_FULL;
	else if (bat.stopped)
		bat.state = bat.smooth >= FLAT_FULL_MV ? BATTERY_FULL : BATTERY_IDLE;
	else
		bat.state = BATTERY_CHARGING;
}

/*
 * Once a minute on USB: has the voltage stood still for long enough that
 * the charger cannot be pushing its current? Then it has finished, or is
 * holding the cell at its 4.20 V, or never started on a cell above its
 * recharge threshold. If it had been seen rising on this charger first,
 * the voltage it stopped at is the charger's 4.20 V, and unless a meter
 * has spoken, the readings are scaled to agree.
 */
static void watch_flat(int mv)
{
	int lo = mv, hi = mv, need;

	if (!bat.usb) {
		bat.nflat = 0;
		bat.rose = bat.stopped = false;
		return;
	}
	if (bat.nflat == FLAT_MINUTES) {
		memmove(bat.flat, bat.flat + 1, (FLAT_MINUTES - 1) * sizeof(bat.flat[0]));
		bat.nflat--;
	}
	bat.flat[bat.nflat++] = mv;
	if (bat.samples >= TREND_SAMPLES && bat.trend >= ROSE_TREND && !bat.rose) {
		bat.rose = true;
		/* it is charging: the step when the PC came says the resistance */
		if (bat.pending_mohm)
			learn_sample(bat.pending_mohm);
		bat.pending_mohm = 0;
	}
	need = mv >= FLAT_HIGH_MV ? FLAT_HIGH_MIN : FLAT_MINUTES;
	if (bat.nflat < need)
		return;
	for (int i = bat.nflat - need; i < bat.nflat; i++) {
		lo = min2(lo, bat.flat[i]);
		hi = max2(hi, bat.flat[i]);
	}
	if (hi - lo > FLAT_MV) {
		bat.stopped = false;
		return;
	}
	if (!bat.stopped)
		klog("battery: the charger has stopped at %d.%02d V", mv / 1000, mv % 1000 / 10);
	bat.stopped = true;
	if (bat.rose && !bat.cal_meter && abs(mv - HELD_MV) > FLAT_MV) {
		int was = bat.cal, cal = (int)((int64_t)bat.cal * HELD_MV / mv);

		if (cal >= CAL_MIN && cal <= CAL_MAX) {
			bat.cal = cal;
			bat.dirty = true;
			klog("battery: calibrated against the charger: it read %d.%02d V at its 4.20 V",
			     mv / 1000, mv % 1000 / 10);
			rescale(was, cal);
			bat.rose = false;
			learn_save();
		}
	}
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
	int target, pm, pull, ms = bat.counted ? (int)((now - bat.counted) / 1000) : 0;
	int moved = (int)((int64_t)bat.ma * ms / 3600);	/* µAh out */
	bool far;

	bat.counted = now;
	if (bat.ma < 0)
		bat.settle = now + SETTLE_US;	/* a charge is going in */
	target = bat.state == BATTERY_FULL ? full : permille_of(bat.rest) * holds_mah();
	if (bat.reseed) {
		bat.charge = target;		/* nothing counted yet: the voltage's word */
	} else {
		bat.charge -= moved;
		far = abs(target - bat.charge) > full / 1000 * FAR_PERMILLE;
		if (bat.state != BATTERY_FULL && now < bat.settle)
			pull = PULL_CHARGING;
		else if (target > full / 1000 * TOP_PERMILLE && bat.charge > full / 1000 * TOP_PERMILLE)
			pull = PULL_CHARGING;	/* the top of the table: see SETTLE_FULL_US */
		else
			pull = far ? PULL_FAST : PULL;
		bat.charge += (target - bat.charge) / pull;
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
	 * does, and never against it: on a charger it does not fall, on the
	 * cell it does not rise. When the estimate is on the wrong side of
	 * it -- too low on the charger, too high off it -- the level waits
	 * where it is for the estimate to come round, which it does as the
	 * cell fills or empties. (It used to walk towards the estimate, and
	 * a level falling while the charger was in looked like a fault.)
	 * With no current either way it follows the estimate freely. And it
	 * moves a point a reading at most, so a correction is a quick slide
	 * rather than a jump.
	 */
	pm = (int)((int64_t)bat.charge * 1000 / full);
	if (!bat.reseed && bat.shown >= 0) {
		if ((bat.ma > 0 && pm > bat.shown) || (bat.ma < 0 && pm < bat.shown))
			pm = bat.shown;
		else if (abs(pm - bat.shown) > SLIDE)
			pm = bat.shown + (pm > bat.shown ? SLIDE : -SLIDE);
	}
	if (pm >= 1000 && bat.state == BATTERY_CHARGING)
		pm = 999;		/* 100 is for when the charger says so */
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
	learn_backlight(mv, lcd_backlight_now(), step);
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
	/*
	 * The average `battery -v` shows starts again when a charger comes
	 * or goes: carried over, it said "into it" beside "charger no" for
	 * minutes after a charger turned out not to be one.
	 */
	bat.ma_avg = bat.reseed || !bat.ma_avg || bat.charger != was ? bat.ma :
		     bat.ma_avg + (bat.ma - bat.ma_avg) / 24;
	count_charge();
	wear();
	keep();
}

void battery_note(const char *what)
{
	char path[64], old[64];
	time_t now = time(NULL);
	struct tm tm;
	FILE *f;

	if (!adc || !mount_resolve(LOG_FILE, path, sizeof(path)))
		return;
	f = fopen(path, "a");
	if (!f)
		return;
	if (ftell(f) > LOG_MAX && mount_resolve(LOG_OLD, old, sizeof(old))) {
		fclose(f);
		remove(old);
		rename(path, old);
		if (!(f = fopen(path, "a")))
			return;
	}
	localtime_r(&now, &tm);
	if (bat.state == BATTERY_NONE || bat.state == BATTERY_USB)
		fprintf(f, "%04d-%02d-%02d %02d:%02d  no cell   %s\n", tm.tm_year + 1900,
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, what);
	else
		fprintf(f, "%04d-%02d-%02d %02d:%02d %3d.%d%% %d.%02dV %s: %s\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
			bat.shown / 10, bat.shown % 10, bat.smooth / 1000, bat.smooth % 1000 / 10,
			battery_state_name(bat.state), what);
	fclose(f);
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
	watch_flat(mv);
	/*
	 * A charger on the cell's wires found by a step or a trend, still to
	 * show it is charging: it lifts the cell above rest, or it is let go
	 * (a load that went away, not a charger). Near full the lift falls
	 * with the current, so that is left to the charger's own end.
	 */
	if (bat.charger && !(bat.usb && BOARD_CHARGES) && !bat.sure &&
	    bat.state != BATTERY_FULL && bat.smooth < TAPER_MV) {
		if (above_rest(bat.smooth, LIFT_MIN_MV / 2)) {
			bat.unlifted_min = 0;
			bat.sure = ++bat.lifted_min >= LIFT_SURE_MIN;
		} else if (++bat.unlifted_min >= LIFT_LOST_MIN) {
			bat.charger = false;
			bat.extra_ma = 0;
			bat.rest = 0;
			klog("battery: not charging after all: the cell is not above rest");
			return;
		}
	}
	if (bat.charger && bat.state == BATTERY_FULL && !(bat.usb && BOARD_CHARGES) &&
	    bat.smooth < DONE_MV - FULL_FALL_MV) {
		charge_finished();
		bat.charger = false;
		bat.extra_ma = 0;
		bat.rest = 0;
		klog("battery: off the charger: the cell is falling from where it was held");
		return;
	}
	if (bat.samples < TREND_SAMPLES || bat.state == BATTERY_FULL)
		return;
	if (!bat.charger && bat.trend >= TREND_CHARGER && above_rest(bat.smooth, LIFT_MIN_MV)) {
		bat.charger = true;
		charger_found();
		/* a guess, with no step to go by: the module, if there is one */
		bat.charge_ma = CONFIG_PT_BATTERY_MODULE_MA ? CONFIG_PT_BATTERY_MODULE_MA :
				CONFIG_PT_BATTERY_CHARGE_MA;
		if (bat.usb)
			bat.extra_ma = bat.charge_ma;	/* so that its going is seen */
		bat.learnable = false;
		bat.rest = 0;			/* made again, under the charger */
		klog("battery: a charger, by the trend (%+d mV an hour)", bat.trend);
	} else if (bat.charger && (!bat.usb || !BOARD_CHARGES) && bat.trend <= -TREND_CHARGER) {
		bat.charger = false;
		bat.extra_ma = 0;
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
	int64_t last_minute = 0, last_save = esp_timer_get_time(), last_note = 0;
	bool last_panel = lcd_panel_on(), last_radio = wifi_radio_on();

	for (;; vTaskDelay(pdMS_TO_TICKS(PERIOD_MS))) {
		int mv = battery_millivolts();
		int64_t now = esp_timer_get_time();
		bool usb = usb_serial_jtag_is_connected();
		bool panel = lcd_panel_on(), radio = wifi_radio_on();
		/* nothing but the lamp changed since the last reading */
		bool step = panel && last_panel && radio == last_radio;

		last_panel = panel;
		last_radio = radio;
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
		if (!last_note || now - last_note >= LOG_EVERY_MS * 1000LL) {
			static const char *const screens[] = { "awake, screen on", "awake, dimmed",
							       "awake, screen dark" };
			char what[96];

			if (!last_note)
				snprintf(what, sizeof(what), "started: %s", power_start_reason());
			else
				snprintf(what, sizeof(what), "%s", screens[power_screen()]);
			last_note = now;
			battery_note(what);
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
	bat.charger = usb && BOARD_CHARGES;
	if (restarted() && kept.magic == KEPT_MAGIC && kept.check == kept_check() &&
	    kept.capacity == bat.capacity) {
		bat.charge = kept.charge;
		bat.shown = kept.shown;
		/*
		 * On USB the board's charger is there or it is not; a module
		 * still charging is found again by the trend in a few minutes.
		 */
		bat.charger = usb ? BOARD_CHARGES : kept.charger;
		bat.state = kept.full && bat.charger ? BATTERY_FULL : BATTERY_UNKNOWN;
		bat.from_full = kept.from_full;
		bat.since_full = kept.since_full;
		if (kept.charge_ma >= 20 && kept.charge_ma <= 3000)
			bat.charge_ma = kept.charge_ma;
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
	count_sleep();
	ktask_create(battery_task, "kbattery", 4096, NULL, 1, NULL, 0);
	return 0;
}

#else

int battery_init(void) { return -ENODEV; }
int battery_millivolts(void) { return -ENODEV; }
int battery_early_millivolts(void) { return -ENODEV; }
int battery_percent(void) { return -ENODEV; }
int battery_status(struct battery_status *out) { return -ENODEV; }
int battery_set_capacity(int mah) { return -ENODEV; }
int battery_calibrate(int meter_mv) { return -ENODEV; }
void battery_save(void) { }
void battery_note(const char *what) { }
const char *battery_state_name(enum battery_state state) { return "no cell"; }

#endif
