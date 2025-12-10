#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>


LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

// static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct i2c_dt_spec dac_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(dac));

typedef struct {
	uint8_t command;
	uint16_t data;
} dac_i2c_write_op_t;
#define DAC_I2C_WRITE_DAC_INPUT_REGS_COMMAND 0x30
#define DAC_I2C_WRITE_DAC_INPUT_REGS_COMMAND_SIZE 3

#define DAC_VALUES_SIZE 5

void dac_write_i2c_command(uint8_t *data, dac_i2c_write_op_t dac_op) {
	data[0] = dac_op.command;
	sys_put_be16(dac_op.data, data+1);
}
 

int main(void) {
	// i2c_transfer
	// printk("welcome to the shell!");
	if (i2c_is_ready_dt(&dac_i2c)) {
		LOG_INF("DAC is ready!");
	} else {
		LOG_ERR("DAC is not ready. :(");
	}

	uint16_t dac_values[DAC_VALUES_SIZE] = {0xFFFF, 0xF000, 0xFFF, 0xFF, 0x0};


	while (1) {
		for (int i = 0; i < DAC_VALUES_SIZE; i++) {
			dac_i2c_write_op_t dac_i2c_command = {
				.command = DAC_I2C_WRITE_DAC_INPUT_REGS_COMMAND,
				.data = dac_values[i]
			};
			uint8_t i2c_command_bytes[DAC_I2C_WRITE_DAC_INPUT_REGS_COMMAND_SIZE]; 
			dac_write_i2c_command(i2c_command_bytes, dac_i2c_command);

			i2c_write_dt(&dac_i2c, i2c_command_bytes, DAC_I2C_WRITE_DAC_INPUT_REGS_COMMAND_SIZE);
			LOG_INF("writing DAC value = %d with command = 0x%x_%x_%x", dac_values[i], i2c_command_bytes[0], i2c_command_bytes[1], i2c_command_bytes[2]);
			k_sleep(K_MSEC(1000));
		}

		// uint8_t writedumb[3] = {0x10, 0xff, 0xff};
		// i2c_write_dt(&dac_i2c, writedumb, 3);

		// uint8_t writecontrolreg[3] = {0x10, 0xff, 0xff};
		// i2c_write_dt(&dac_i2c, writecontrolreg, 3);

		// uint8_t data[2];	
		// i2c_read_dt(&dac_i2c, data, 2);
		// LOG_INF("reading DAC values. got = %d, %d", data[0], data[1]);



	}
	return 0;
}