#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);


int main(void) {
	// printk("welcome to the shell!");

	// while (1) {
	// 	printk("hello team!\n");
	// 	k_sleep(K_MSEC(2000));
	// }
	return 0;
}