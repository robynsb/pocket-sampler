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

// TODO: Consolidate naming between blocks, samples, sample slices, instruments.
#define SAMPLE_LENGTH 128
#define NUM_BLOCKS 5
#define BLOCK_SIZE (SAMPLE_LENGTH * sizeof(int32_t))
#define SAMPLE_BLOCK_SIZE SAMPLE_LENGTH * sizeof(int16_t)
#define SAMPLE_RATE 44100
#define TONE_FREQUENCY_HZ 440.0f

K_SEM_DEFINE(audio_start_sem, 0, 1);

// TODO: Think about these alignments
K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, BLOCK_SIZE, NUM_BLOCKS, 32);

K_MEM_SLAB_DEFINE_STATIC(sample_data_slab, SAMPLE_BLOCK_SIZE, NUM_BLOCKS, 32);

struct read_order_t {
    uint32_t restart_in; // number of sample points before restarting from beginning.
    bool play;
};

K_MSGQ_DEFINE(order_msgq, sizeof(struct read_order_t), NUM_BLOCKS, 1);

struct sample_read_result_t {
    void *data; 
};

K_MSGQ_DEFINE(sample_read_result_msgq, sizeof(struct sample_read_result_t), NUM_BLOCKS, 1);

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const struct device *i2s0_dev;


#define SAMPLE_PARTITION_NODE DT_NODELABEL(samples)
FS_FSTAB_DECLARE_ENTRY(SAMPLE_PARTITION_NODE);

// TODO: Make "current" beat configuration struct.
K_MUTEX_DEFINE(beat_config_mutex);
static volatile float bpm = 120;

static void orchestrator_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    int ret;

    uint32_t beats_per_measure = 4;
    uint32_t number_of_measures_in_loop = 4;
    uint32_t current_sample = 0;

    while(true) {
        ret = k_mutex_lock(&beat_config_mutex, K_MSEC(1000));
        if (ret < 0) {
            LOG_ERR("failed to acquire beat_config_mutex");
            break;
        }

        // TODO: Check floats vs int types in this expression
        uint32_t number_of_sample_points_per_beat = SAMPLE_RATE/(bpm/60.0f);

        ret = k_mutex_unlock(&beat_config_mutex);
        if (ret < 0) {
            LOG_ERR("failed to release beat_config_mutex");
            break;
        }

        uint32_t number_of_sample_points_per_loop = beats_per_measure*number_of_measures_in_loop*number_of_sample_points_per_beat;

        uint32_t number_of_sample_points_played_of_beat = current_sample % number_of_sample_points_per_beat;

        uint32_t number_of_sample_points_left_from_beat = number_of_sample_points_per_beat - number_of_sample_points_played_of_beat;

        struct read_order_t order = {
            .restart_in = number_of_sample_points_left_from_beat,
            .play = true
        };

        ret = k_msgq_put(&order_msgq, &order, K_MSEC(10000));
        if (ret < 0) {
            LOG_ERR("failed to enqueue order_msgq");
            break;
        }
        current_sample = (current_sample + SAMPLE_LENGTH) % number_of_sample_points_per_loop;
    }

    while(true) {
        LOG_ERR("orchestrator failed :((");
        k_sleep(K_MSEC(8000));
    }
}

static void sample_flash_reader_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    int ret;


    // k_sem_take(&reader_start_sem, K_FOREVER);
    // k_sem_take(&audio_start_sem, K_FOREVER);
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
    ret = fs_read(&file, header_data, sizeof(header_data));
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

    LOG_INF("Starting reader");

    while (1) {
        // k_sleep(K_MSEC(1000));

        struct read_order_t order;

        ret = k_msgq_get(&order_msgq, &order, K_MSEC(10000));
        if (ret < 0) {
            LOG_ERR("k_msgq_get failed: %d", ret);
            break;
        }

        uint8_t *buffer;
        ret = k_mem_slab_alloc(&sample_data_slab, (void **) &buffer, K_MSEC(5000));
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc failed sample_data_slab: %d", ret);
            break;
        }


        // fill audio block
        if(order.restart_in >= SAMPLE_LENGTH) {
            ret = fs_read(&file, buffer, SAMPLE_BLOCK_SIZE);
            if (ret != SAMPLE_BLOCK_SIZE) {
                // fill the rest with 0s
                for (int i = ret; i < SAMPLE_BLOCK_SIZE; i++) {
                    buffer[i] = 0;
                }
            }

        } else {
            ret = fs_seek(&file, start_position, FS_SEEK_SET);
            if (ret < 0) {
                LOG_ERR("fs_seek failed: %d", ret);
                break;
            }

            uint32_t num_bytes_to_read = order.restart_in * sizeof(uint16_t);

            ret = fs_read(&file, buffer, num_bytes_to_read);
            if (ret != num_bytes_to_read) {
                LOG_ERR("fs_read failed: %d. Sample too short?!", ret);
                break;
            }
        }

        //enqueue
        struct sample_read_result_t read_result = {
            .data = buffer
        };

        ret = k_msgq_put(&sample_read_result_msgq, &read_result, K_MSEC(1000));
        if (ret < 0) {
            LOG_ERR("failed to enqueue sample_read_result_msgq");
            break;
        }
    }
    while(1) {
        LOG_ERR("reader failed :((");
        k_sleep(K_MSEC(8000));
    }
}

static void sound_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    int ret;


    // k_sem_take(&reader_start_sem, K_FOREVER);
    k_sem_take(&audio_start_sem, K_FOREVER);


    for (int i = 0; i < NUM_BLOCKS; i++) {

        int32_t *buffer;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, (void **) &buffer, K_MSEC(5000));
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc timeout failed: %d", ret);
            return;
        }


        for(int j = 0; j < SAMPLE_LENGTH; j++) {
            int16_t sample_point = 0;
            int32_t stereo_sample = (sample_point & 0xFFFF) | (sample_point << 16);
            buffer[j] = stereo_sample;
        }
        
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
        struct sample_read_result_t read_result;
        ret = k_msgq_get(&sample_read_result_msgq, &read_result, K_MSEC(10000));
        if (ret < 0) {
            LOG_ERR("k_msgq_get failed: %d", ret);
            break;
        }

        int32_t *buffer;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, (void **) &buffer, K_MSEC(5000));
        if (ret < 0) {
            LOG_ERR("k_mem_slab_alloc failed tx_0_mem_slab: %d", ret);
            break;
        }


        uint8_t *sample_data = read_result.data;

        for(int j = 0; j < SAMPLE_LENGTH; j++) {
            int16_t sample_point = sys_get_le16(&sample_data[2*j]);
            int32_t stereo_sample = (sample_point & 0xFFFF) | (sample_point << 16);
            buffer[j] = stereo_sample;
        }
        k_mem_slab_free(&sample_data_slab, read_result.data);

        ret = i2s_write(i2s0_dev, buffer, BLOCK_SIZE);
        if (ret < 0) {
            k_mem_slab_free(&tx_0_mem_slab, buffer);
            LOG_ERR("i2s_write failed: %d", ret);
            k_sleep(K_MSEC(1000));
            break;
        }
    }
    while(1) {
        LOG_ERR("reader failed :((");
        k_sleep(K_MSEC(8000));
    }
}

#define AUDIO_THREAD_STACK_SIZE 1024
K_THREAD_DEFINE(reader_tid, AUDIO_THREAD_STACK_SIZE, sample_flash_reader_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(sounder_tid, AUDIO_THREAD_STACK_SIZE, sound_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(orchestrator_tid, AUDIO_THREAD_STACK_SIZE, orchestrator_thread, NULL, NULL, NULL, 5, 0, 0);

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
    // k_sleep(K_MSEC(1000));
    k_sem_give(&audio_start_sem);

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}