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


#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>


LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define CYCLES_PER_BIT 8
#define SIDESET_BIT_COUNT 2

#define DT_DRV_COMPAT robin_pico_uart_pio // required to use DT_INST_FOREACH_STATUS_OKAY

struct pio_uart_config {
	const struct device *piodev;
	const struct pinctrl_dev_config *pcfg;
	const uint32_t tx_pin;
	uint32_t baudrate;
};

struct pio_uart_data {
	size_t tx_sm;
};


RPI_PICO_PIO_DEFINE_PROGRAM(uart_tx, 0, 3,
		/* .wrap_target */
	0x9fa0, /*  0: pull   block           side 1 [7]  */
	0xf727, /*  1: set    x, 7            side 0 [7]  */
	0x6001, /*  2: out    pins, 1                     */
	0x0642, /*  3: jmp    x--, 2                 [6]  */
		/* .wrap */
);

static int pio_uart_tx_init(PIO pio, uint32_t sm, uint32_t tx_pin, float div)
{
	uint32_t offset;
	pio_sm_config sm_config;

	if (!pio_can_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_tx))) {
		return -EBUSY;
	}

	offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(uart_tx));
	sm_config = pio_get_default_sm_config();

	sm_config_set_sideset(&sm_config, SIDESET_BIT_COUNT, true, false);
	sm_config_set_out_shift(&sm_config, true, false, 0);
	sm_config_set_out_pins(&sm_config, tx_pin, 1);
	sm_config_set_sideset_pins(&sm_config, tx_pin);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&sm_config, div);
	sm_config_set_wrap(&sm_config,
			   offset + RPI_PICO_PIO_GET_WRAP_TARGET(uart_tx),
			   offset + RPI_PICO_PIO_GET_WRAP(uart_tx));

	pio_sm_set_pins_with_mask(pio, sm, BIT(tx_pin), BIT(tx_pin));
	pio_sm_set_pindirs_with_mask(pio, sm, BIT(tx_pin), BIT(tx_pin));
	pio_sm_init(pio, sm, offset, &sm_config);
	pio_sm_set_enabled(pio, sm, true);

	return 0;
}

static void pio_uart_poll_out(const struct device *dev, unsigned char c)
{
	const struct pio_uart_config *config = dev->config;
	struct pio_uart_data *data = dev->data;

	pio_sm_put_blocking(pio_rpi_pico_get_pio(config->piodev), data->tx_sm, (uint32_t)c);
}

static int pio_uart_init(const struct device *dev)
{
	const struct pio_uart_config *config = dev->config;
	struct pio_uart_data *data = dev->data;
	float sm_clock_div;
	size_t tx_sm;
	int retval;
	PIO pio;

	pio = pio_rpi_pico_get_pio(config->piodev);
	sm_clock_div = (float)clock_get_hz(clk_sys) / (CYCLES_PER_BIT * config->baudrate);

	retval = pio_rpi_pico_allocate_sm(config->piodev, &tx_sm);


	if (retval < 0) {
		LOG_ERR("pio_rpi_pico_allocate_sm failed with ret = %d", retval);
		return retval;
	}

	data->tx_sm = tx_sm;

	retval = pio_uart_tx_init(pio, tx_sm, config->tx_pin, sm_clock_div);
	if (retval < 0) {
		LOG_ERR("pio_uart_tx_init failed with ret = %d", retval);
		return retval;
	}


	retval = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (retval < 0) {
		LOG_ERR("pinctrl_apply_state failed with ret = %d", retval);
	}

	return retval;
}

#define PIO_UART_INIT(idx)									\
	PINCTRL_DT_INST_DEFINE(idx);								\
	static const struct pio_uart_config pio_uart##idx##_config = {				\
		.piodev = DEVICE_DT_GET(DT_INST_PARENT(idx)),					\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(idx),					\
		.tx_pin = DT_INST_RPI_PICO_PIO_PIN_BY_NAME(idx, default, 0, tx_pins, 0),	\
		.baudrate = DT_INST_PROP(idx, current_speed),					\
	};											\
	static struct pio_uart_data pio_uart##idx##_data;					\
												\
	DEVICE_DT_INST_DEFINE(idx, pio_uart_init, NULL, &pio_uart##idx##_data,			\
			      &pio_uart##idx##_config, POST_KERNEL,				\
			      CONFIG_SERIAL_INIT_PRIORITY,					\
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(PIO_UART_INIT)



int main(void) {
	int ret;

	k_sleep(K_MSEC(10000));	

	const struct device *uart0 = DEVICE_DT_GET(DT_NODELABEL(pio1_uart0));

	char data = 0b01010101;
	int i = 0;
	// ret = pio_uart_init(my_config);

	while (1) {
		LOG_INF("Lmao lets a gooo (%d) ret = %d", i++, ret);
		pio_uart_poll_out(uart0, data);
		k_sleep(K_MSEC(100));	
	}

	return 0;
}