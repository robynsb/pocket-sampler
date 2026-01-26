#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/dac.h>
#include <math.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

// static const struct device *const dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac_adafruit_ad5693r));
static const struct device *const dac_dev = DEVICE_DT_GET(DT_ALIAS(mydac));

static const struct dac_channel_cfg dac_ch_cfg = {
	.channel_id  = 0,
	.resolution  = 16,
	.buffered = true, // I think this variable is unused
};

void infinite_ladder() {
	double x_degrees = 0;
	double pitch = 1;
	int i = 0;
	int j = 0;
	int ret;

	while (1) {
		i++;

		if(i > 2000) {
			i = 0;
			j++;
			pitch *= 1.0594630;
			LOG_INF("pitch=%lf", pitch);
			if(j > 4) {
				j = 0;
				pitch = 1;
			}
		}
		x_degrees += 22*pitch;

		if(x_degrees > 360) x_degrees = 0;

		double x_radians = x_degrees * 3.14 / 180.0;
		double result = (1+sin(x_radians))/2;

        uint16_t dac_value = (uint16_t) (result * UINT16_MAX * 0.001);
		ret = dac_write_value(dac_dev, 0, dac_value);

		k_sleep(K_USEC((uint16_t) (70*pitch)));
	}

}

int main(void) {
	k_sleep(K_MSEC(2000));	
	int ret;
	if (!device_is_ready(dac_dev)) {
		LOG_ERR("DAC device %s is not ready\n", dac_dev->name);
		return 0;
	}
	ret = dac_channel_setup(dac_dev, &dac_ch_cfg);

	if (ret != 0) {
		LOG_ERR("Setting up of DAC channel failed with code %d\n", ret);
		return 0;
	}
	infinite_ladder();

	return 0;
}