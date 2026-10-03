/*
 * The ES8311: a small mono codec with one speaker output and one microphone
 * input, configured over I2C while the samples travel over I2S.
 *
 * The register sequence follows Espressif's own driver (esp_codec_dev,
 * device/es8311). Its clock table has one row per MCLK-to-rate ratio; we
 * always feed the codec MCLK = 256 x sample rate, and every 256x row of that
 * table is identical apart from the DAC oversampling, so clock_config()
 * below is the whole table for our case.
 */
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "es8311.h"
#include "levels.h"

#if CONFIG_PT_AUDIO

#define ADDR		0x18		/* CE tied low; 0x19 when tied high */
#define TIMEOUT_MS	200

/* Registers, named as in the datasheet. */
#define REG_RESET	0x00
#define REG_CLK1	0x01
#define REG_CLK2	0x02
#define REG_CLK3	0x03		/* ADC oversampling */
#define REG_CLK4	0x04		/* DAC oversampling */
#define REG_CLK5	0x05
#define REG_CLK6	0x06		/* BCLK divider */
#define REG_CLK7	0x07		/* LRCK divider, high */
#define REG_CLK8	0x08		/* LRCK divider, low */
#define REG_SDPIN	0x09		/* the DAC's serial port */
#define REG_SDPOUT	0x0a		/* the ADC's serial port */
#define REG_SYS0B	0x0b
#define REG_SYS0C	0x0c
#define REG_PWR		0x0d
#define REG_PWR2	0x0e
#define REG_SYS10	0x10
#define REG_SYS11	0x11
#define REG_DACEN	0x12
#define REG_OUT		0x13
#define REG_PGA		0x14		/* microphone: analogue or digital, gain */
#define REG_ADC_RAMP	0x15
#define REG_MIC_GAIN	0x16		/* bits 2:0 scale the analogue input */
#define REG_ADC_VOL	0x17		/* digital volume, or the ALC's ceiling */
#define REG_ALC		0x18
#define REG_ALC_LEVEL	0x19
#define REG_ADC_HPF1	0x1b
#define REG_ADC_HPF2	0x1c
#define REG_DAC_MUTE	0x31
#define REG_DAC_VOL	0x32
#define REG_DAC_RAMP	0x37
#define REG_GPIO	0x44
#define REG_GP		0x45
#define REG_ID1		0xfd		/* 0x83 */
#define REG_ID2		0xfe		/* 0x11 */

static i2c_master_dev_handle_t dev;

static int wr(uint8_t reg, uint8_t val)
{
	const uint8_t buf[2] = { reg, val };

	return i2c_master_transmit(dev, buf, 2, TIMEOUT_MS) ? -EIO : 0;
}

static int rd(uint8_t reg, uint8_t *val)
{
	return i2c_master_transmit_receive(dev, &reg, 1, val, 1, TIMEOUT_MS) ? -EIO : 0;
}

/*
 * MCLK is 256 x the sample rate, so: no pre-divider, no multiplier, ADC and
 * DAC clocks straight from it, 256 BCLK cycles per frame, and the DAC
 * oversampled 32x below 22 kHz where there is room for it, 16x above.
 */
static int clock_config(int rate)
{
	uint8_t reg;
	int ret = 0;

	ret |= wr(REG_CLK2, 0x00);			/* pre-divide 1, multiply 1 */
	ret |= wr(REG_CLK5, 0x00);			/* ADC and DAC dividers 1 */
	if (rd(REG_CLK3, &reg))
		return -EIO;
	ret |= wr(REG_CLK3, (reg & 0x80) | 0x10);	/* single speed, ADC osr 16 */
	if (rd(REG_CLK4, &reg))
		return -EIO;
	ret |= wr(REG_CLK4, (reg & 0x80) | (rate <= 16000 ? 0x20 : 0x10));
	if (rd(REG_CLK7, &reg))
		return -EIO;
	ret |= wr(REG_CLK7, reg & 0xc0);		/* LRCK divider 256 */
	ret |= wr(REG_CLK8, 0xff);
	if (rd(REG_CLK6, &reg))
		return -EIO;
	ret |= wr(REG_CLK6, (reg & 0xe0) | 0x03);	/* BCLK = MCLK / 4 */
	return ret ? -EIO : 0;
}

int es8311_init(int sda, int scl, int rate)
{
	i2c_master_bus_handle_t bus;
	const i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = ADDR,
		.scl_speed_hz = 100000,
	};
	uint8_t id1 = 0, id2 = 0, reg;
	int ret = 0;

	if (!dev) {
		if ((ret = i2c_bus_get(sda, scl, (struct i2c_master_bus_t **)&bus)))
			return ret;
		if (i2c_master_bus_add_device(bus, &dev_cfg, &dev)) {
			dev = NULL;
			return -EIO;
		}
	}
	if (rd(REG_ID1, &id1) || rd(REG_ID2, &id2) || id1 != 0x83 || id2 != 0x11) {
		klog("es8311: no codec at 0x%02x on SDA %d / SCL %d", ADDR, sda, scl);
		i2c_master_bus_rm_device(dev);
		dev = NULL;
		return -ENODEV;
	}

	/* Power up the analogue side and let the chip settle. */
	ret |= wr(REG_PWR, 0xfa);
	ret |= wr(REG_GPIO, 0x08);	/* the codec ignores the very first write now and then */
	ret |= wr(REG_GPIO, 0x08);
	ret |= wr(REG_CLK1, 0x30);
	ret |= wr(REG_CLK2, 0x00);
	ret |= wr(REG_CLK3, 0x10);
	ret |= wr(REG_MIC_GAIN, 0x24);
	ret |= wr(REG_CLK4, 0x10);
	ret |= wr(REG_CLK5, 0x00);
	ret |= wr(REG_SYS0B, 0x00);
	ret |= wr(REG_SYS0C, 0x00);
	ret |= wr(REG_SYS10, 0x1f);
	ret |= wr(REG_SYS11, 0x7f);
	ret |= wr(REG_RESET, 0x80);
	if (ret)
		goto fail;

	/* We are the I2S master and we provide MCLK, so the codec is a slave. */
	if ((ret = rd(REG_RESET, &reg)))
		goto fail;
	ret |= wr(REG_RESET, reg & 0xbf);
	ret |= wr(REG_CLK1, 0x3f);		/* clocks on, MCLK from the MCLK pin */
	if (rd(REG_CLK6, &reg)) {
		ret = -EIO;
		goto fail;
	}
	ret |= wr(REG_CLK6, reg & ~0x20);	/* BCLK not inverted */
	ret |= wr(REG_OUT, 0x10);
	ret |= wr(REG_ADC_HPF1, 0x0a);
	ret |= wr(REG_ADC_HPF2, 0x6a);	/* high-pass filter: no DC offset on the mic */
	ret |= wr(REG_GPIO, 0x00);
	if (ret)
		goto fail;

	if ((ret = clock_config(rate)))
		goto fail;

	/* 16 bits per sample, I2S (Philips) framing, in both directions. */
	if ((ret = rd(REG_SDPIN, &reg)))
		goto fail;
	ret |= wr(REG_SDPIN, (reg & 0xfc) | 0x0c);
	if (rd(REG_SDPOUT, &reg)) {
		ret = -EIO;
		goto fail;
	}
	ret |= wr(REG_SDPOUT, (reg & 0xfc) | 0x0c);

	/* Start: un-mute both paths, power up the DAC, analogue microphone. */
	ret |= wr(REG_SDPIN, 0x0c);
	ret |= wr(REG_SDPOUT, 0x0c);
	ret |= wr(REG_ADC_VOL, 0xbf);	/* 0 dB */
	ret |= wr(REG_PWR2, 0x02);
	ret |= wr(REG_DACEN, 0x00);
	ret |= wr(REG_PGA, 0x1a);		/* analogue microphone, PGA on */
	ret |= wr(REG_PWR, 0x01);
	ret |= wr(REG_ADC_RAMP, 0x40);
	ret |= wr(REG_DAC_RAMP, 0x08);
	ret |= wr(REG_GP, 0x00);
	if (ret)
		goto fail;
	klog("es8311: codec at 0x%02x, %d Hz", ADDR, rate);
	return 0;
fail:
	es8311_deinit();
	return -EIO;
}

/*
 * Standby and back. Off is the sequence Espressif's own driver suspends
 * the codec with: both volumes to nothing, every analogue block powered
 * down, the references off. On is the end of es8311_init() again; the
 * volume, the gain and the ALC are the caller's to set once more.
 */
int es8311_power(bool on)
{
	int ret = 0;

	if (!dev)
		return -ENODEV;
	if (!on) {
		ret |= wr(REG_DAC_VOL, 0x00);
		ret |= wr(REG_ADC_VOL, 0x00);
		ret |= wr(REG_PWR2, 0xff);
		ret |= wr(REG_DACEN, 0x02);
		ret |= wr(REG_PGA, 0x00);
		ret |= wr(REG_PWR, 0xfa);
		ret |= wr(REG_ADC_RAMP, 0x00);
		ret |= wr(REG_DAC_RAMP, 0x08);
		ret |= wr(REG_GP, 0x01);
		return ret ? -EIO : 0;
	}
	ret |= wr(REG_GP, 0x00);
	ret |= wr(REG_ADC_VOL, 0xbf);
	ret |= wr(REG_PWR2, 0x02);
	ret |= wr(REG_DACEN, 0x00);
	ret |= wr(REG_PGA, 0x1a);
	ret |= wr(REG_PWR, 0x01);
	ret |= wr(REG_ADC_RAMP, 0x40);
	ret |= wr(REG_DAC_RAMP, 0x08);
	return ret ? -EIO : 0;
}

/* A failed audio startup gives the shared I2C bus its device slot back. */
void es8311_deinit(void)
{
	if (!dev)
		return;
	es8311_power(false);
	i2c_master_bus_rm_device(dev);
	dev = NULL;
}

int es8311_set_rate(int rate)
{
	return dev ? clock_config(rate) : -ENODEV;
}

/*
 * The DAC volume register is 0.5 dB per step: 0 is silence, 0xbf is 0 dB.
 * Percent maps onto the top 40 dB, including the full 0 dB setting.
 */
int es8311_set_volume(int percent)
{
	int reg = speaker_volume_reg(percent);

	return dev ? wr(REG_DAC_VOL, reg) : -ENODEV;
}

/* The analogue scale in front of the converter: 0 to 42 dB, in sixes. */
int es8311_set_mic_gain(int db)
{
	int step = db / 6;
	uint8_t reg;

	if (step < 0)
		step = 0;
	if (step > 7)
		step = 7;
	if (!dev)
		return -ENODEV;
	if (rd(REG_MIC_GAIN, &reg))		/* bit 5 is the clock mode: keep it */
		return -EIO;
	return wr(REG_MIC_GAIN, (reg & ~0x07) | step);
}

/*
 * Automatic level control: the codec turns its own digital gain up when
 * the room is quiet and down before anything clips, which is what makes a
 * MEMS microphone in a room usable at all. `max_db` is how far up it may
 * go; with ALC on, the volume register means that ceiling rather than a
 * fixed gain.
 */
int es8311_set_alc(bool on, int max_db)
{
	int ceiling = 0xbf + max_db * 2;	/* 0xbf is 0 dB, half a dB a step */
	int ret;

	if (!dev)
		return -ENODEV;
	if (ceiling < 0)
		ceiling = 0;
	if (ceiling > 0xff)
		ceiling = 0xff;
	if (!on) {
		ret = wr(REG_ALC, 0x00);
		ret |= wr(REG_ADC_VOL, 0xbf);	/* plain 0 dB again */
		return ret ? -EIO : 0;
	}
	ret = wr(REG_ADC_VOL, ceiling);
	/* Target between -10 dB and -7 dB of full scale: loud, with room
	 * left for a door slamming. Window 6 is a quarter of a dB every
	 * 128 samples, so it follows speech without pumping. */
	ret |= wr(REG_ALC_LEVEL, (13 << 4) | 9);
	ret |= wr(REG_ALC, 0x80 | 0x06);
	return ret ? -EIO : 0;
}

int es8311_mute(bool on)
{
	uint8_t reg;

	if (!dev)
		return -ENODEV;
	if (rd(REG_DAC_MUTE, &reg))
		return -EIO;
	return wr(REG_DAC_MUTE, (reg & 0x9f) | (on ? 0x60 : 0x00));
}

bool es8311_present(void)
{
	return dev != NULL;
}

#endif /* CONFIG_PT_AUDIO */
