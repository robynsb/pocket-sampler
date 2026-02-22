#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/i2s.h>

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>


LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

int main(void) {
	int ret;

	k_sleep(K_MSEC(500));	

	const struct device *i2s0 = DEVICE_DT_GET(DT_NODELABEL(pio1_i2s0));

    // Dummy write to start I2S, no configurable implentation yet
    i2s_write(i2s0, NULL, 0);

	int i = 0;
	while (1) {
        LOG_INF("test %d", i++);
		k_sleep(K_MSEC(1000));	
	}

	return 0;
}