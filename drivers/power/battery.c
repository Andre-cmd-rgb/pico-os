/*
 * Battery level.
 *
 * The board divides the cell voltage in two and feeds it to an ADC pin.
 * A task samples it every few seconds: it warns when the cell is low, and
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
 *   at rest, and what a lithium cell's table of charge is made of;
 *   the charge in the cell, counted down by the current and pulled
 *   slowly towards what the corrected voltage says, so the percentage
 *   neither jumps with the brightness nor drifts away over hours;
 *   the trend in millivolts per minute, which says whether the cell is
 *   charging when no PC is there to say so.
 *
 * The capacity it counts against (`battery -c`) and the resistance it
 * has learned are kept in /etc/battery.
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_BATTERY

#define SAMPLES		8	/* the ADC is noisy; average a few */
#define PERIOD_MS	5000
#define WARN_EVERY_MS	60000
#define SMOOTH_SHIFT	3	/* an eighth of each new reading: ~40 s */
#define TREND_MINUTES	10	/* how far back the trend looks */
#define CHARGING_MV_MIN	2	/* rise per minute that means "charging" */
#define LEARN_FILE	"/etc/battery"
/*
 * No lithium cell ever reads above about 4.25 V, so anything higher is
 * the charger's own rail with nothing on it: the board is on USB and
 * there is no battery to talk about.
 */
#define USB_ONLY_MV	4300
#define SAVE_EVERY_MS	600000
#define FULL_MV		4200	/* where the charger holds a full cell */
#define TAPER_MV	4150	/* above this its current falls away */

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

/*
 * The counted charge follows the corrected voltage with a time constant
 * of ten minutes: long enough to ride over work the current estimate
 * gets wrong, short enough that a wrong guess does not last. When the two
 * are far apart -- a new cell, the first minutes after boot -- it follows
 * ten times faster.
 */
#define PULL		120	/* readings: ten minutes */
#define PULL_FAST	12
#define FAR_PERMILLE	150

/* A lithium cell at rest, at 0, 10, ... 100 percent. */
static const uint16_t curve[] = {
	3300, 3600, 3700, 3740, 3770, 3790, 3820, 3870, 3950, 4050, 4200,
};

static struct {
	int	 smooth;		/* millivolts at the terminals, filtered */
	int	 rest;			/* millivolts with the sag put back, filtered */
	int	 trend;			/* millivolts per minute, signed */
	int	 ma;			/* out of the cell; into it is negative */
	int	 ma_avg;		/* the same over a few minutes */
	int	 capacity;		/* mAh */
	int	 mohm;			/* the cell's resistance */
	int	 steps;			/* how many steps it was learned from */
	int	 charge;		/* µAh left in the cell */
	int	 shown;			/* percent; -1 before the first estimate */
	int64_t	 counted;		/* when the charge was last counted, µs */
	bool	 reseed;		/* start the count again from the voltage */
	enum battery_state state;
	int	 history[TREND_MINUTES];
	int	 samples;
	bool	 dirty;
} bat = { .capacity = CONFIG_PT_BATTERY_MAH, .shown = -1, .reseed = true };

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t	 cali;
static adc_channel_t		 channel;
static int			 last_mv;

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
			return i * 100 + (mv - curve[i]) * 100 / (curve[i + 1] - curve[i]);
	}
	return 1000;
}

/* What this cell is and has been seen to do, across reboots. */
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
		int v;

		if (sscanf(line, "capacity %d", &v) == 1 && v >= 50 && v <= 20000)
			bat.capacity = v;
		else if (sscanf(line, "resistance %d", &v) == 1 && v >= 20 && v <= 3000)
			bat.mohm = v;
		else if (sscanf(line, "steps %d", &v) == 1 && v >= 0)
			bat.steps = v;
	}
	fclose(f);
	if (!bat.steps)
		bat.mohm = default_mohm(bat.capacity);
	klog("battery: %d mAh cell, %d mohm%s", bat.capacity, bat.mohm,
	     bat.steps ? "" : " (a guess until it is measured)");
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
	fprintf(f, "# the cell: `battery -c` sets its capacity\n"
		"capacity %d\nresistance %d\nsteps %d\n", bat.capacity, bat.mohm, bat.steps);
	if (!fclose(f))
		bat.dirty = false;
}

int battery_millivolts(void)
{
	int total = 0;

	if (!adc)
		return -ENODEV;
	for (int i = 0; i < SAMPLES; i++) {
		int raw, mv;

		if (adc_oneshot_read(adc, channel, &raw))
			return -EIO;
		if (cali) {
			if (adc_cali_raw_to_voltage(cali, raw, &mv))
				return -EIO;
		} else {
			mv = raw * 3100 / 4095;	/* rough, uncalibrated chips */
		}
		total += mv;
	}
	last_mv = total / SAMPLES * CONFIG_PT_BATTERY_SCALE / 100;
	return last_mv;
}

int battery_percent(void)
{
	return bat.shown < 0 ? -ENODEV : bat.shown;
}

/* Everything the task has worked out, for `battery` and `power`. */
int battery_status(struct battery_status *out)
{
	int mv = bat.smooth ? bat.smooth : battery_millivolts();

	memset(out, 0, sizeof(*out));
	if (mv < 0)
		return mv;
	out->mv = mv;
	out->trend = bat.trend;
	out->state = bat.state;
	out->percent = bat.shown < 0 ? 0 : bat.shown;
	out->ma = bat.ma_avg;
	out->capacity_mah = bat.capacity;
	out->mohm = bat.mohm;
	out->minutes_left = -1;
	out->minutes_full = -1;
	if (bat.state != BATTERY_CHARGING && bat.ma_avg > 5)
		out->minutes_left = bat.charge / 1000 * 60 / bat.ma_avg;
	else if (bat.state == BATTERY_CHARGING && bat.ma_avg < -5)
		out->minutes_full = (bat.capacity * 1000 - bat.charge) / 1000 * 60 / -bat.ma_avg;
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

/*
 * The current out of the cell, estimated from what the board is doing.
 * On USB the board runs from the charger's supply, so all the cell sees
 * is the charge going in, or nothing once it is full.
 */
static int draw_ma(int mv, bool usb)
{
	int busy[portNUM_PROCESSORS], most = 0, all = 0, ma;

	cpu_busy(busy);			/* every time, so its window is one reading */
	if (bat.state == BATTERY_CHARGING)
		return -charge_ma(mv);
	if (usb)
		return 0;
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
 * The cell's resistance, from a step in the current: the voltage before
 * the step and after it, five seconds apart, which is too short for the
 * charge in the cell to move. A few steps are averaged, and once there
 * are some, a reading far from the rest (a burst from the radio at the
 * wrong moment) is left out.
 */
static void learn_resistance(int mv, int ma, bool step)
{
	static int before_mv, before_ma;
	int dma = ma - before_ma, sample;

	if (step && before_mv && abs(dma) >= STEP_MA && mv < TAPER_MV && before_mv < TAPER_MV) {
		sample = (before_mv - mv) * 1000 / dma;
		if (sample >= 20 && sample <= 3000 &&
		    (bat.steps < 3 || (sample < bat.mohm * 3 && sample > bat.mohm / 3))) {
			bat.mohm += (sample - bat.mohm) / (bat.steps < 3 ? 2 : 4);
			bat.steps++;
			bat.dirty = true;
		}
	}
	before_mv = mv;
	before_ma = ma;
}

/*
 * Count the charge by the current, then pull it towards the corrected
 * voltage. The percentage shown only goes down while the cell is
 * discharging and only up while it charges: a figure that wanders both
 * ways is not believed.
 */
static void count_charge(void)
{
	int cap = bat.capacity * 1000;		/* µAh */
	int64_t now = esp_timer_get_time();
	int target, pct, ms = (int)((now - bat.counted) / 1000);
	bool far;

	bat.counted = now;
	target = bat.state == BATTERY_FULL ? cap : permille_of(bat.rest) * bat.capacity;
	if (bat.reseed) {
		bat.charge = target;		/* nothing counted yet: the voltage's word */
	} else {
		bat.charge -= (int)((int64_t)bat.ma * ms / 3600);
		far = abs(target - bat.charge) > cap / 1000 * FAR_PERMILLE;
		bat.charge += (target - bat.charge) / (far ? PULL_FAST : PULL);
	}
	bat.charge = bat.charge < 0 ? 0 : bat.charge > cap ? cap : bat.charge;

	pct = bat.charge / (bat.capacity * 10);
	if (!bat.reseed && bat.ma > 0 && pct > bat.shown)
		pct = bat.shown;
	else if (!bat.reseed && bat.ma < 0 && pct < bat.shown)
		pct = bat.shown;
	bat.shown = pct;
	bat.reseed = false;
}

/* Fold one reading in: the current, the resistance, the charge. */
static void estimate(int mv, bool usb, bool step)
{
	int rest;

	bat.ma = draw_ma(mv, usb);
	learn_resistance(mv, bat.ma, step);
	rest = mv + bat.ma * bat.mohm / 1000;
	bat.rest = bat.rest ? bat.rest + ((rest - bat.rest) >> SMOOTH_SHIFT) : rest;
	bat.ma_avg = bat.reseed ? bat.ma : bat.ma_avg + (bat.ma - bat.ma_avg) / 24;
	count_charge();
}

int battery_set_capacity(int mah)
{
	if (mah < 50 || mah > 20000)
		return -EINVAL;
	bat.capacity = mah;
	bat.mohm = default_mohm(mah);	/* a new cell: what was learned was the old one's */
	bat.steps = 0;
	bat.reseed = true;		/* and start again from its voltage */
	bat.dirty = true;
	learn_save();
	return 0;
}

/*
 * Once a minute: look at where the voltage was ten minutes ago, and
 * decide what the cell is doing.
 */
static void update_trend(int mv)
{
	int then;

	int span;

	memmove(bat.history, bat.history + 1, sizeof(bat.history) - sizeof(bat.history[0]));
	bat.history[TREND_MINUTES - 1] = mv;
	if (bat.samples < TREND_MINUTES)
		bat.samples++;
	if (bat.samples < 2)
		return;			/* one reading is not a trend */
	/* Use as much history as there is, so the state settles after two
	 * minutes rather than ten. */
	span = bat.samples;
	then = bat.history[TREND_MINUTES - span];
	bat.trend = (mv - then) / span;
	if (bat.trend >= CHARGING_MV_MIN)
		bat.state = BATTERY_CHARGING;
	else if (mv >= FULL_MV - 60 && bat.trend > -CHARGING_MV_MIN)
		bat.state = BATTERY_FULL;
	else
		bat.state = BATTERY_DISCHARGING;
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
	int64_t last_minute = 0, last_save = 0;
	int last_backlight = lcd_backlight_get();
	bool last_usb = usb_serial_jtag_is_connected();

	vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));	/* / is mounted by now */
	learn_load();
	bat.reseed = true;			/* estimate again, for this cell */
	for (;; vTaskDelay(pdMS_TO_TICKS(PERIOD_MS))) {
		int mv = battery_millivolts();
		int64_t now = esp_timer_get_time();
		int backlight = lcd_backlight_get();
		bool usb = usb_serial_jtag_is_connected();
		bool step = usb != last_usb || abs(backlight - last_backlight) >= STEP_BACKLIGHT;

		last_backlight = backlight;
		if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV) {
			bat.state = BATTERY_USB;
			last_usb = usb;
			continue;	/* no cell: the board is on USB */
		}
		bat.smooth = bat.smooth ? bat.smooth + ((mv - bat.smooth) >> SMOOTH_SHIFT) : mv;
		if (bat.state == BATTERY_NONE || bat.state == BATTERY_USB)
			bat.state = BATTERY_UNKNOWN;	/* until the trend says */
		if (now - last_minute >= 60 * 1000000LL) {
			last_minute = now;
			update_trend(bat.smooth);
		}
		/*
		 * A cell with the charger plugged in is charging, whatever
		 * the trend says about the last few minutes: there is no
		 * warning to give and nothing to switch off. This has to
		 * come after the trend, which would otherwise call it
		 * discharging again once a minute -- as it did. And the
		 * moment the PC goes, the cell is carrying the board, before
		 * the trend has seen the voltage fall.
		 */
		if (usb && bat.state != BATTERY_FULL)
			bat.state = BATTERY_CHARGING;
		else if (!usb && last_usb)
			bat.state = BATTERY_DISCHARGING;
		last_usb = usb;
		estimate(mv, usb, step);
		if (now - last_save >= SAVE_EVERY_MS * 1000LL) {
			last_save = now;
			learn_save();
		}
		mv = bat.smooth;
		if (bat.state == BATTERY_CHARGING)
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
			learn_save();
			power_quiesce();
			vTaskDelay(pdMS_TO_TICKS(1500));
			esp_deep_sleep_start();
		}
		if (mv <= CONFIG_PT_BATTERY_WARN_MV &&
		    esp_timer_get_time() - last_warn > WARN_EVERY_MS * 1000LL) {
			last_warn = esp_timer_get_time();
			say("low at %d.%02d V: charge it soon", mv);
		}
	}
}

int battery_init(void)
{
	adc_unit_t unit;
	adc_oneshot_unit_init_cfg_t unit_cfg;
	const adc_oneshot_chan_cfg_t chan_cfg = {
		.atten = ADC_ATTEN_DB_12,	/* up to about 3.1 V at the pin */
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};

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

	const adc_cali_curve_fitting_config_t cali_cfg = {
		.unit_id = unit,
		.chan = channel,
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali))
		cali = NULL;		/* uncalibrated chip: readings are rough */

	int mv = battery_millivolts();
	bool usb = usb_serial_jtag_is_connected();

	/*
	 * Judge the very first reading the way the task will, so the status
	 * bar shows the cell from the moment the screen comes up instead of
	 * nothing for the five seconds before the task has an opinion.
	 */
	bat.mohm = default_mohm(bat.capacity);
	bat.smooth = mv > 0 ? mv : 0;
	if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV)
		bat.state = BATTERY_USB;
	else
		bat.state = usb ? BATTERY_CHARGING : BATTERY_UNKNOWN;
	if (bat.state == BATTERY_USB) {
		klog("battery: none fitted");
	} else {
		estimate(mv, usb, false);
		klog("battery: %d.%02d V (%d%%) on GPIO%d%s", mv / 1000, mv % 1000 / 10,
		     bat.shown, CONFIG_PT_BATTERY_ADC_GPIO, cali ? "" : ", uncalibrated");
	}
	xTaskCreatePinnedToCore(battery_task, "kbattery", 3584, NULL, 1, NULL, 0);
	return 0;
}

#else

int battery_init(void) { return -ENODEV; }
int battery_millivolts(void) { return -ENODEV; }
int battery_percent(void) { return -ENODEV; }
int battery_status(struct battery_status *out) { return -ENODEV; }
int battery_set_capacity(int mah) { return -ENODEV; }
const char *battery_state_name(enum battery_state state) { return "no cell"; }

#endif
