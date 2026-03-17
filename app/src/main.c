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
#define SAMPLE_RATE 31100

/* Chirp parameters */
#define FREQ_START 20.0f
#define FREQ_END 20000.0f
#define SWEEP_DURATION 5.0f
#define TOTAL_SWEEP_SAMPLES (SAMPLE_RATE * SWEEP_DURATION * 2)  /* 441,000 for 10 sec */

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, BLOCK_SIZE, NUM_BLOCKS, 32);

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Global state */
static uint32_t block_counter = 0;
static float phase = 0.0f;

/**
 * Calculate instantaneous frequency at a given time
 * Extremely fast - just one division and one multiply
 */
static inline float get_frequency_at_time(float t)
{
    /* Wrap time to 10-second cycle */
    float cycle_time = fmodf(t, SWEEP_DURATION * 2.0f);
    
    if (cycle_time < SWEEP_DURATION) {
        /* Sweep up: linear interpolation */
        return FREQ_START + (FREQ_END - FREQ_START) * (cycle_time / SWEEP_DURATION);
    } else {
        /* Sweep down: linear interpolation */
        return FREQ_END - (FREQ_END - FREQ_START) * ((cycle_time - SWEEP_DURATION) / SWEEP_DURATION);
    }
}

/**
 * Generate a block of stereo samples
 * Key optimization: Calculate frequency ONCE per block (128 samples)
 * instead of once per sample
 */
static void generate_chirp_block(int32_t *buffer, uint32_t num_samples)
{
    /* Time at start of this block */
    float time_ms = (float)(block_counter * SAMPLE_LENGTH) / SAMPLE_RATE;
    
    /* Get frequency for this block - only ONE calculation for 128 samples */
    float frequency = get_frequency_at_time(time_ms);
    
    /* Pre-calculate the phase increment per sample */
    float phase_increment = 2.0f * M_PI * frequency / SAMPLE_RATE;
    
    float volume = 0.05f;
    
    /* Generate 128 samples with pre-calculated constants */
    for (uint32_t i = 0; i < num_samples; i++) {
        /* Accumulate phase */
        phase += phase_increment;
        
        /* Wrap phase (cheaper than keeping it small every iteration) */
        if (phase > 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
        
        /* Generate sine sample */
        int16_t sample_int = (int16_t)(32767.0f * volume * sinf(phase));
        
        /* Create stereo sample */
        int32_t stereo_sample = (sample_int & 0xFFFF) | (sample_int << 16);
        buffer[i] = stereo_sample;
    }
    
    block_counter++;
    /* Wrap block counter to create infinite loop */
    if (block_counter * SAMPLE_LENGTH >= TOTAL_SWEEP_SAMPLES) {
        block_counter = 0;
        phase = 0.0f;
    }
}

int main(void)
{
    int ret;
    for (int i =3; i >=0; i--) {
        k_sleep(K_MSEC(1000));
        LOG_INF("Booting in %d...", i);

    }
    k_sleep(K_MSEC(100));
    LOG_INF("Booting - Optimized chirp version");
    
    const struct device *i2s0 = DEVICE_DT_GET(DT_NODELABEL(pio1_i2s0));
    if (!device_is_ready(i2s0)) {
        printf("I2S device not ready\n");
        return -ENODEV;
    }
    
    struct i2s_config i2s_cfg;
    i2s_cfg.word_size = 16U;
    i2s_cfg.channels = 2U;
    i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
    i2s_cfg.frame_clk_freq = SAMPLE_RATE;
    i2s_cfg.block_size = BLOCK_SIZE;
    i2s_cfg.timeout = 100;
    i2s_cfg.mem_slab = &tx_0_mem_slab;
    
    ret = i2s_configure(i2s0, I2S_DIR_TX, &i2s_cfg);
    if (ret < 0) {
        printf("I2S config failed: %d\n", ret);
        return ret;
    }
    
    LOG_INF("Starting frequency sweep: 20 Hz to 20 kHz over 10 seconds");
    
    void *temp_ptr;
    for (int i = 0; i < NUM_BLOCKS; i++) {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &temp_ptr, K_FOREVER);
        if (ret < 0) {
            LOG_INF("k_mem_slab_alloc failed: %d\n", ret);
            return ret;
        }
        generate_chirp_block((int32_t *)temp_ptr, SAMPLE_LENGTH);
        i2s_write(i2s0, temp_ptr, BLOCK_SIZE);
    }
    
    ret = i2s_trigger(i2s0, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        printf("I2S trigger start failed\n");
        return ret;
    }
    
    LOG_INF("I2S playback started");
    
    while (1) {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &temp_ptr, K_FOREVER);
        if (ret == 0) {
            generate_chirp_block((int32_t *)temp_ptr, SAMPLE_LENGTH);
            i2s_write(i2s0, temp_ptr, BLOCK_SIZE);
        }
    }
    
    return 0;
}