/*
 * The codec's own interface, used by audio.c. Everything outside the audio
 * driver goes through drivers/drivers.h instead.
 */
#pragma once

#include <stdbool.h>

int	es8311_init(int sda, int scl, int rate);
void	es8311_deinit(void);
int	es8311_set_rate(int rate);
int	es8311_power(bool on);		/* standby, and back */
int	es8311_set_volume(int percent);
int	es8311_set_mic_gain(int db);
int	es8311_set_alc(bool on, int max_db);
int	es8311_mute(bool on);
bool	es8311_present(void);
