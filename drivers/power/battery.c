/*
 * Battery level.
 *
 * The board divides the cell voltage in two and feeds it to an ADC pin.
 * A task samples it every few seconds: it warns when the cell is low, and
 * below the critical level it syncs everything and goes into deep sleep,
 * which is the only way to stop a lithium cell from being run flat.
 *
 * A single reading is a poor guide -- it jumps about with load and it
 * reads high while charging -- so this keeps three things instead:
 *
 *   a smoothed voltage, so the number does not dance;
 *   the trend in millivolts per minute, which says whether the cell is
 *   charging or discharging, and how long it has left at this rate;
 *   the ends of the range as actually observed on this cell, kept in
 *   /etc/battery, so the percentage stops being a guess from a table and
 *   becomes this battery's own curve.
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_timer.h"
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

static const uint16_t curve[] = {
	3300, 3600, 3700, 3740, 3770, 3790, 3820, 3870, 3950, 4050, 4200,
};

static struct {
	int	 smooth;		/* millivolts, filtered */
	int	 trend;			/* millivolts per minute, signed */
	int	 full_mv, empty_mv;	/* what this cell really does */
	enum battery_state state;
	int	 history[TREND_MINUTES];
	int	 samples;
	bool	 dirty;
} bat = { .full_mv = 4200, .empty_mv = 3300 };

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t	 cali;
static adc_channel_t		 channel;
static int			 last_mv;

/*
 * The table is the shape of a lithium cell's discharge at 0, 10, ... 100
 * percent. The reading is first stretched onto the ends this cell has
 * actually been seen to reach, and a cell on charge reads high, so that
 * is taken off first.
 */
static int percent_of(int mv, enum battery_state state)
{
	const int steps = sizeof(curve) / sizeof(curve[0]) - 1;
	int span = bat.full_mv - bat.empty_mv;

	if (state == BATTERY_CHARGING)
		mv -= 80;		/* the charger holds it above its rest */
	if (span > 200)			/* stretch onto the learned ends */
		mv = curve[0] + (mv - bat.empty_mv) * (curve[steps] - curve[0]) / span;
	if (mv <= curve[0])
		return 0;
	for (int i = 0; i < steps; i++) {
		if (mv < curve[i + 1])
			return i * 10 + (mv - curve[i]) * 10 / (curve[i + 1] - curve[i]);
	}
	return 100;
}

/* What this particular cell has been seen to do, across reboots. */
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

		if (sscanf(line, "full %d", &v) == 1 && v > 3500 && v < 4400)
			bat.full_mv = v;
		else if (sscanf(line, "empty %d", &v) == 1 && v > 2800 && v < 3800)
			bat.empty_mv = v;
	}
	fclose(f);
	klog("battery: this cell runs %d.%02d V to %d.%02d V",
	     bat.empty_mv / 1000, bat.empty_mv % 1000 / 10,
	     bat.full_mv / 1000, bat.full_mv % 1000 / 10);
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
	fprintf(f, "# what this cell has been seen to do\nfull %d\nempty %d\n",
		bat.full_mv, bat.empty_mv);
	if (!fclose(f))
		bat.dirty = false;
}

/* Widen the learned range when the cell proves it goes further. */
static void learn_from(int mv, enum battery_state state)
{
	if (state == BATTERY_FULL && mv > bat.full_mv + 10) {
		bat.full_mv = mv;
		bat.dirty = true;
	}
	if (state == BATTERY_DISCHARGING && mv < bat.empty_mv - 10 && mv > 2800) {
		bat.empty_mv = mv;
		bat.dirty = true;
	}
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
	int mv = bat.smooth ? bat.smooth : battery_millivolts();

	return mv < 0 ? mv : percent_of(mv, bat.state);
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
	out->percent = percent_of(mv, bat.state);
	out->minutes_left = -1;
	if (bat.state == BATTERY_DISCHARGING && bat.trend < -1)
		out->minutes_left = (mv - bat.empty_mv) / -bat.trend;
	return 0;
}

const char *battery_state_name(enum battery_state state)
{
	switch (state) {
	case BATTERY_CHARGING:	  return "charging";
	case BATTERY_FULL:	  return "full or on USB";
	case BATTERY_DISCHARGING: return "on battery";
	case BATTERY_UNKNOWN:	  return "working it out";
	case BATTERY_USB:	  return "on USB power";
	default:		  return "no cell";
	}
}

/*
 * Once a minute: fold the reading into the average, look at where the
 * voltage was ten minutes ago, and decide what the cell is doing.
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
	else if (mv >= bat.full_mv - 60 && bat.trend > -CHARGING_MV_MIN)
		bat.state = BATTERY_FULL;
	else
		bat.state = BATTERY_DISCHARGING;
	learn_from(mv, bat.state);
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

	learn_load();
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
		int mv = battery_millivolts();
		int64_t now = esp_timer_get_time();

		if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV) {
			bat.state = BATTERY_USB;
			continue;	/* no cell: the board is on USB */
		}
		/*
		 * A cell with the charger plugged in is charging, whatever
		 * the trend says about the last few minutes: there is no
		 * warning to give and nothing to switch off.
		 */
		if (usb_serial_jtag_is_connected() && bat.state != BATTERY_FULL)
			bat.state = BATTERY_CHARGING;
		bat.smooth = bat.smooth ? bat.smooth + ((mv - bat.smooth) >> SMOOTH_SHIFT) : mv;
		if (bat.state == BATTERY_NONE)
			bat.state = BATTERY_UNKNOWN;	/* until the trend says */
		if (now - last_minute >= 60 * 1000000LL) {
			last_minute = now;
			update_trend(bat.smooth);
		}
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
			say("\nbattery empty (%d.%02d V): saving and powering off\n", mv);
			bat.empty_mv = mv;	/* this is where this cell ends */
			bat.dirty = true;
			learn_save();
			power_quiesce();
			vTaskDelay(pdMS_TO_TICKS(1500));
			esp_deep_sleep_start();
		}
		if (mv <= CONFIG_PT_BATTERY_WARN_MV && bat.state != BATTERY_CHARGING &&
		    esp_timer_get_time() - last_warn > WARN_EVERY_MS * 1000LL) {
			last_warn = esp_timer_get_time();
			say("\nbattery low (%d.%02d V): charge it soon\n", mv);
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

	/*
	 * Judge the very first reading the way the task will, so the status
	 * bar shows the cell from the moment the screen comes up instead of
	 * "usb" for the five seconds before the task has an opinion.
	 */
	bat.smooth = mv > 0 ? mv : 0;
	if (mv < BATTERY_NO_CELL_MV || mv > USB_ONLY_MV)
		bat.state = BATTERY_USB;
	else
		bat.state = usb_serial_jtag_is_connected() ? BATTERY_CHARGING : BATTERY_UNKNOWN;
	if (bat.state == BATTERY_USB)
		klog("battery: none fitted, running from USB");
	else
		klog("battery: %d.%02d V (%d%%) on GPIO%d%s", mv / 1000, mv % 1000 / 10,
		     percent_of(mv, BATTERY_FULL), CONFIG_PT_BATTERY_ADC_GPIO,
		     cali ? "" : ", uncalibrated");
	xTaskCreatePinnedToCore(battery_task, "kbattery", 3584, NULL, 1, NULL, 0);
	return 0;
}

#else

int battery_init(void) { return -ENODEV; }
int battery_millivolts(void) { return -ENODEV; }
int battery_percent(void) { return -ENODEV; }
int battery_status(struct battery_status *out) { return -ENODEV; }
const char *battery_state_name(enum battery_state state) { return "no cell"; }

#endif
