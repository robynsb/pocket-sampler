#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include <zephyr/drivers/i2s.h>
#include <math.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define SAMPLE_LENGTH 128
#define NUM_BLOCKS 5
#define BLOCK_SIZE (SAMPLE_LENGTH * sizeof(int32_t))
#define SAMPLE_RATE 31100
#define TONE_FREQUENCY_HZ 440.0f

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, BLOCK_SIZE, NUM_BLOCKS, 32);

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Shared state */
static float phase = 0.0f;
static atomic_t playing_tone = ATOMIC_INIT(1);
static const struct device *i2s0_dev;

K_SEM_DEFINE(audio_start_sem, 0, 1);

static void generate_tone_block(int32_t *buffer, uint32_t num_samples)
{
    float phase_increment = 2.0f * M_PI * TONE_FREQUENCY_HZ / SAMPLE_RATE;
    float volume = 0.05f;

    for (uint32_t i = 0; i < num_samples; i++) {
        phase += phase_increment;

        if (phase > 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }

        int16_t sample_int = (int16_t)(32767.0f * volume * sinf(phase));
        int32_t stereo_sample = (sample_int & 0xFFFF) | (sample_int << 16);
        buffer[i] = stereo_sample;
    }
}

static inline void fill_audio_block(int32_t *buffer, uint32_t num_samples, bool play_tone)
{
    if (play_tone) {
        generate_tone_block(buffer, num_samples);
    } else {
        memset(buffer, 0, num_samples * sizeof(buffer[0]));
    }
}

static void tone_toggle_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    while (1) {
        k_sleep(K_MSEC(1000));
        bool next_state = !atomic_get(&playing_tone);
        atomic_set(&playing_tone, next_state);
    }
}

static void audio_enqueue_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    k_sem_take(&audio_start_sem, K_FOREVER);

    int ret;
    void *block;

    for (int i = 0; i < NUM_BLOCKS; i++) {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc failed: %d", ret);
            return;
        }

        fill_audio_block((int32_t *)block, SAMPLE_LENGTH, atomic_get(&playing_tone));
        ret = i2s_write(i2s0_dev, block, BLOCK_SIZE);
        if (ret < 0) {
            k_mem_slab_free(&tx_0_mem_slab, block);
            LOG_ERR("Initial i2s_write failed: %d", ret);
            return;
        }
    }

    ret = i2s_trigger(i2s0_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        LOG_ERR("I2S trigger start failed: %d", ret);
        return;
    }

    LOG_INF("I2S continuous playback started");

    while (1) {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc failed: %d", ret);
            continue;
        }

        fill_audio_block((int32_t *)block, SAMPLE_LENGTH, atomic_get(&playing_tone));
        ret = i2s_write(i2s0_dev, block, BLOCK_SIZE);
        if (ret < 0) {
            k_mem_slab_free(&tx_0_mem_slab, block);
            LOG_ERR("i2s_write failed: %d", ret);
            k_sleep(K_MSEC(10));
        }
    }
}

#define TOGGLE_THREAD_STACK_SIZE 512
#define AUDIO_THREAD_STACK_SIZE 1024
K_THREAD_DEFINE(toggle_tid, TOGGLE_THREAD_STACK_SIZE, tone_toggle_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(audio_tid, AUDIO_THREAD_STACK_SIZE, audio_enqueue_thread, NULL, NULL, NULL, 6, 0, 0);

int main(void)
{
    int ret;
    for (int i = 3; i >= 0; i--) {
        k_sleep(K_MSEC(1000));
        LOG_INF("Booting in %d...", i);
    }

    k_sleep(K_MSEC(100));
    LOG_INF("Booting - Audio generator");

    i2s0_dev = DEVICE_DT_GET(DT_NODELABEL(pio1_i2s0));
    if (!device_is_ready(i2s0_dev)) {
        LOG_ERR("I2S device not ready");
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

    ret = i2s_configure(i2s0_dev, I2S_DIR_TX, &i2s_cfg);
    if (ret < 0) {
        LOG_ERR("I2S config failed: %d", ret);
        return ret;
    }

    LOG_INF("Configured I2S; starting tone/silence worker");
    k_sem_give(&audio_start_sem);

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}