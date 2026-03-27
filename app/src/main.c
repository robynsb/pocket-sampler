#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include <zephyr/drivers/i2s.h>
#include <math.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/sys/byteorder.h>

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
K_SEM_DEFINE(reader_start_sem, 0, 1);

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

#define SAMPLE_PARTITION_NODE DT_NODELABEL(samples)
FS_FSTAB_DECLARE_ENTRY(SAMPLE_PARTITION_NODE);

static void sample_flash_reader_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    int ret;


    // k_sem_take(&reader_start_sem, K_FOREVER);
    k_sem_take(&audio_start_sem, K_FOREVER);
	struct fs_mount_t *mountpoint = &FS_FSTAB_ENTRY(SAMPLE_PARTITION_NODE);
	struct fs_statvfs sbuf;
    ret = fs_statvfs(mountpoint->mnt_point, &sbuf);
	if (ret < 0) {
		LOG_ERR("FAIL: statvfs: %d\n", ret);
        return;
	}


	LOG_INF("Sucessfully mounted samples filesystem %s: bsize = %lu ; frsize = %lu ;"
		   " blocks = %lu ; bfree = %lu\n",
		   mountpoint->mnt_point,
		   sbuf.f_bsize, sbuf.f_frsize,
		   sbuf.f_blocks, sbuf.f_bfree);
        
    char fname[30];
    snprintf(fname, sizeof(fname), "%s/kick0.wav", mountpoint->mnt_point);

    struct fs_file_t file;
    fs_file_t_init(&file);
    ret = fs_open(&file, fname, FS_O_CREATE | FS_O_RDWR);
	if (ret < 0) {
		LOG_ERR("FAIL: open %s: %d", fname, ret);
		return;
	}

    // rc = fs_read(&file, &boot_count, sizeof(boot_count));
	// if (rc < 0) {
	// 	LOG_ERR("FAIL: read %s: [rd:%d]", fname, rc);
	// 	goto out;
	// }
	// LOG_PRINTK("%s read count:%u (bytes: %d)\n", fname, boot_count, rc);
    uint8_t header_data[0x4E];
    ret = fs_read(&file, &header_data, sizeof(header_data));
    if (ret < 0) {
        LOG_ERR("FAIL: read %s: [rd:%d]", fname, ret);
        return;
    }

    // https://en.wikipedia.org/wiki/WAV#WAV_file_header
    uint32_t header_file_size = sys_get_le32(&header_data[4]) + 8;
    LOG_INF("Reading wav file, with file size=%d", header_file_size);

    uint16_t audio_format = sys_get_le16(&header_data[0x14]);

    uint16_t number_of_channels = sys_get_le16(&header_data[0x16]);
    LOG_INF("AudioFormat=%d", audio_format);

    uint32_t sample_rate = sys_get_le32(&header_data[0x18]);
    
    uint16_t bit_depth = sys_get_le16(&header_data[0x22]);

    char data_string[5];
    memcpy(data_string, &header_data[0x46], 4); // TODO: Don't hard code this beginning?
    data_string[4] = '\0';

    uint32_t sample_data_size = sys_get_le32(&header_data[0x4A]);

    LOG_INF("WAV: file_size=%" PRIu32, header_file_size);
    LOG_INF("WAV: audio_format=%" PRIu16, audio_format);
    LOG_INF("WAV: channels=%" PRIu16, number_of_channels);
    LOG_INF("WAV: sample_rate=%" PRIu32, sample_rate);
    LOG_INF("WAV: bit_depth=%" PRIu16, bit_depth);
    LOG_INF("WAV: data_string=%s", data_string);
    LOG_INF("WAV: sample_data_size=%d", sample_data_size);

    // TODO: Put some errors stating assumptions on audio data.
    off_t start_position = fs_tell(&file);


    uint8_t data[2*SAMPLE_LENGTH];

    for (int i = 0; i < NUM_BLOCKS; i++) {
        int32_t *buffer;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, (void **) &buffer, K_MSEC(5000));
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc timeout failed: %d", ret);
            return;
        }

        ret = fs_read(&file, &data, sizeof(data));
        if (ret < 0) {
            LOG_ERR("fs_read failed: %d", ret);
            return;
        }

        for(int j = 0; j < SAMPLE_LENGTH; j++) {
            int16_t sample_point = sys_get_le16(&data[j]);
            int32_t stereo_sample = (sample_point & 0xFFFF) | (sample_point << 16);
            buffer[j] = stereo_sample;
        }

        // fill_audio_block((int32_t *)block, SAMPLE_LENGTH, atomic_get(&playing_tone));
        
        ret = i2s_write(i2s0_dev, buffer, BLOCK_SIZE);
        if (ret < 0) {
            k_mem_slab_free(&tx_0_mem_slab, buffer);
            LOG_ERR("Initial i2s_write failed: %d", ret);
            return;
        }
    }

    ret = i2s_trigger(i2s0_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        LOG_ERR("I2S trigger start failed: %d", ret);
        return;
    }

    LOG_INF("I2S continuous playback started from flash");

    while (1) {
        // k_sleep(K_MSEC(1000));

        int32_t *buffer;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, (void **) &buffer, K_MSEC(5000));
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc failed: %d", ret);
            continue;
        }

        // fill audio block
        ret = fs_read(&file, &data, sizeof(data));
        if (ret != sizeof(data)) {
            LOG_INF("fs_read failed: %d, starting again...", ret);

            ret = fs_seek(&file, start_position, FS_SEEK_SET);
            if (ret < 0) {
                LOG_ERR("fs_seek failed: %d", ret);
                break;
            }

            // retry
            ret = fs_read(&file, &data, sizeof(data));
            if (ret != sizeof(data)) {
                LOG_ERR("fs_read failed: %d", ret);
                break;
            }
        }

        for(int j = 0; j < SAMPLE_LENGTH; j++) {
            int16_t sample_point = sys_get_le16(&data[2*j]);
            int32_t stereo_sample = (sample_point & 0xFFFF) | (sample_point << 16);
            buffer[j] = stereo_sample;
        }

        ret = i2s_write(i2s0_dev, buffer, BLOCK_SIZE);
        if (ret < 0) {
            k_mem_slab_free(&tx_0_mem_slab, buffer);
            LOG_ERR("i2s_write failed: %d", ret);
            k_sleep(K_MSEC(1000));
        }
    }
    while(1) {
        LOG_ERR("failed :((");
        k_sleep(K_MSEC(8000));
    }
}

#define TOGGLE_THREAD_STACK_SIZE 512
#define AUDIO_THREAD_STACK_SIZE 1024
// K_THREAD_DEFINE(toggle_tid, TOGGLE_THREAD_STACK_SIZE, tone_toggle_thread, NULL, NULL, NULL, 7, 0, 0);
// K_THREAD_DEFINE(audio_tid, AUDIO_THREAD_STACK_SIZE, audio_enqueue_thread, NULL, NULL, NULL, 6, 0, 0);
K_THREAD_DEFINE(reader_tid, AUDIO_THREAD_STACK_SIZE, sample_flash_reader_thread, NULL, NULL, NULL, 5, 0, 0);

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
    k_sem_give(&reader_start_sem);

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}