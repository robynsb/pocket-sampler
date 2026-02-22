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
#include <math.h>


LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define SAMPLE_LENGTH 128
#define NUM_BLOCKS 20
#define BLOCK_SIZE (SAMPLE_LENGTH * sizeof(int32_t))
K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, BLOCK_SIZE, NUM_BLOCKS, 32);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int32_t sine_wave_table[SAMPLE_LENGTH];

static void generate_sine_table(void)
{
    float volume = 0.15f;
    for (int i = 0; i < SAMPLE_LENGTH; i++) {
        int16_t val = (int16_t)(32767.0f * volume * cosf(i * 2.0f * (float)(M_PI / SAMPLE_LENGTH)));
        
        sine_wave_table[i] = (val & 0xFFFF) | (val << 16);
    }
}

int main(void) {
	int ret;

    k_sleep(K_MSEC(100));	
    LOG_INF("Booting");

    int i = 0;

	const struct device *i2s0 = DEVICE_DT_GET(DT_NODELABEL(pio1_i2s0));

    if (!device_is_ready(i2s0)) {
        printf("I2S device not ready\n");
        return -ENODEV;
    }

    generate_sine_table();

    struct i2s_config i2s_cfg;
    i2s_cfg.word_size = 16U;
    i2s_cfg.channels = 2U;
    i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
    i2s_cfg.frame_clk_freq = 44100;
    i2s_cfg.block_size = BLOCK_SIZE;
    i2s_cfg.timeout = 100;
    i2s_cfg.mem_slab = &tx_0_mem_slab;

    ret = i2s_configure(i2s0, I2S_DIR_TX, &i2s_cfg);
    if (ret < 0) {
        printf("I2S config failed: %d\n", ret);
        return ret;
    }

    void *temp_ptr;
    for (int i = 0; i < NUM_BLOCKS; i++) {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &temp_ptr, K_FOREVER);
        if (ret < 0) {
            LOG_INF("k_mem_slab_alloc failed: %d\n", ret);
            return ret;
        }
        memcpy(temp_ptr, sine_wave_table, BLOCK_SIZE);
        i2s_write(i2s0, temp_ptr, BLOCK_SIZE);
    }

    ret = i2s_trigger(i2s0, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        printf("I2S trigger start failed\n");
        return ret;
    }

    while (1) {
        i++;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &temp_ptr, K_FOREVER);
        if (ret == 0) {
            /* Data is already in temp_ptr from the pre-fill stage! */
            i2s_write(i2s0, temp_ptr, BLOCK_SIZE);
        }
    }

	// int i = 0;
	// while (1) {
    //     LOG_INF("idle %d", i++);
	// 	k_sleep(K_MSEC(1000));	
	// }

	return 0;
}