/* Production set_screen() with a panel which can reject sleep or wake. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

#define DIM_DIVIDE 4
#define DIM_MIN 5
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

enum screen_state { SCREEN_ON, SCREEN_DIM, SCREEN_OFF };
static volatile enum screen_state state;
static atomic_bool screen_changing;
static bool slept_before;
static void *task = (void *)1;
static int panel_width, brightness, light;
static bool panel_awake, quiet, blank, idle_sleep;
static bool reject_sleep, reject_wake, key_during_power;
static unsigned power_calls, sleep_policy_calls, wifi_calls, notifications;
static int64_t clock_now, last_activity;

void power_activity(void);

static int lcd_width(void) { return panel_width; }
static int lcd_backlight_get(void) { return brightness; }
static void lcd_light(int level) { light = level < 0 ? brightness : level; }
static void proc_set_quiet(bool on) { quiet = on; }
static void vt_blank(bool on) { blank = on; }
static bool lcd_panel_on(void) { return panel_awake; }
static bool cpufreq_idle_sleep(void) { return idle_sleep; }
static int cpufreq_set_idle_sleep(bool on)
{
	++sleep_policy_calls;
	idle_sleep = on;
	return 0;
}
static void wifi_retry_soon(void) { ++wifi_calls; }
static int64_t now_us(void) { return ++clock_now; }
static void activity_record(int64_t now, bool local)
{
	assert(local);
	last_activity = now;
}
static void xTaskNotifyGive(void *notified)
{
	assert(notified == task);
	++notifications;
}

static void lcd_panel_power(bool on)
{
	++power_calls;
	/* State must not claim a completed power command while it is pending. */
	assert(screen_changing);
	assert((state == SCREEN_OFF) == on);
	assert(quiet && blank && light == 0);
	if (key_during_power) {
		key_during_power = false;
		power_activity();
	}
	if (panel_width && !(on ? reject_wake : reject_sleep))
		panel_awake = on;
}

#include "idle_screen_under_test.h"

static void reset(bool policy, int width)
{
	state = SCREEN_ON;
	screen_changing = false;
	slept_before = false;
	panel_width = width;
	brightness = light = 80;
	panel_awake = width > 0;
	quiet = blank = false;
	idle_sleep = policy;
	reject_sleep = reject_wake = key_during_power = false;
	power_calls = sleep_policy_calls = wifi_calls = notifications = 0;
	clock_now = last_activity = 0;
}

static void visible(enum screen_state expected, bool policy)
{
	assert(state == expected && !screen_changing);
	assert(!quiet && !blank);
	assert(light == (expected == SCREEN_DIM ? 20 : 80));
	assert(idle_sleep == policy);
	assert(!panel_width || panel_awake);
}

static void dark(void)
{
	assert(state == SCREEN_OFF && !screen_changing);
	assert(quiet && blank && light == 0);
	assert(idle_sleep);
	assert(!panel_width || !panel_awake);
}

static void successful_transitions(bool policy)
{
	reset(policy, 320);
	assert(set_screen(SCREEN_ON));
	assert(!power_calls && !sleep_policy_calls);
	assert(set_screen(SCREEN_DIM));
	visible(SCREEN_DIM, policy);
	assert(!power_calls);
	assert(set_screen(SCREEN_OFF));
	dark();
	assert(slept_before == policy);
	assert(sleep_policy_calls == (policy ? 0 : 1));
	assert(set_screen(SCREEN_OFF));
	assert(power_calls == 1);
	assert(set_screen(SCREEN_ON));
	visible(SCREEN_ON, policy);
	assert(wifi_calls == 1);
	assert(sleep_policy_calls == (policy ? 0 : 2));
	assert(set_screen(SCREEN_OFF));
	dark();
	assert(set_screen(SCREEN_DIM));
	visible(SCREEN_DIM, policy);
	assert(wifi_calls == 2);
}

static void sleep_failure(bool policy, enum screen_state from)
{
	reset(policy, 320);
	assert(set_screen(from));
	reject_sleep = true;
	assert(!set_screen(SCREEN_OFF));
	visible(from, policy);
	assert(!sleep_policy_calls && !wifi_calls);
	assert(!set_screen(SCREEN_OFF));
	visible(from, policy);
	reject_sleep = false;
	assert(set_screen(SCREEN_OFF));
	dark();
	assert(set_screen(SCREEN_ON));
	visible(SCREEN_ON, policy);
}

static void wake_failure(bool policy, enum screen_state to)
{
	reset(policy, 320);
	assert(set_screen(SCREEN_OFF));
	reject_wake = true;
	assert(!set_screen(to));
	dark();
	assert(slept_before == policy && !wifi_calls);
	assert(sleep_policy_calls == (policy ? 0 : 3));
	assert(!set_screen(to));
	dark();
	assert(sleep_policy_calls == (policy ? 0 : 5));
	reject_wake = false;
	assert(set_screen(to));
	visible(to, policy);
	assert(wifi_calls == 1);
	assert(sleep_policy_calls == (policy ? 0 : 6));
}

static void unavailable_panel(bool policy)
{
	reset(policy, 0);
	/* Disabled LCD stubs never change panel_awake; serial idle still works. */
	reject_sleep = reject_wake = true;
	assert(set_screen(SCREEN_OFF));
	dark();
	assert(set_screen(SCREEN_ON));
	visible(SCREEN_ON, policy);
}

static void local_key_during_transition(void)
{
	reset(false, 320);
	power_activity();
	assert(!notifications && last_activity);
	key_during_power = true;
	assert(set_screen(SCREEN_OFF));
	assert(notifications == 1 && last_activity == 2);
	dark();
	/* The notification survives until idle_task re-evaluates activity. */
	assert(set_screen(SCREEN_ON));
	visible(SCREEN_ON, false);
}

int main(void)
{
	for (int policy = 0; policy <= 1; ++policy) {
		successful_transitions(policy);
		sleep_failure(policy, SCREEN_ON);
		sleep_failure(policy, SCREEN_DIM);
		wake_failure(policy, SCREEN_ON);
		wake_failure(policy, SCREEN_DIM);
		unavailable_panel(policy);
	}
	local_key_during_transition();
	assert(idle_wait_ticks(INT64_MAX, true) == portMAX_DELAY);
	assert(idle_wait_ticks(INT64_MAX, false) == 250);
	assert(idle_wait_ticks(5000000, false) == 250);
	assert(idle_wait_ticks(5000, false) == 25);
	assert(idle_wait_ticks(5000000, true) == 5020);
	puts("idle screen: sleep/wake failure rollback, retry, policy and serial fallback passed");
	return 0;
}
