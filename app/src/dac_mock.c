#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dac_mock, CONFIG_DAC_LOG_LEVEL);

static int dac_mock_write_value(const struct device *dev, uint8_t channel, uint32_t value)
{
    LOG_INF("Mock DAC [%s]: Channel %d set to %u", dev->name, channel, value);
    return 0;
}

static int dac_mock_channel_setup(const struct device *dev, const struct dac_channel_cfg *channel_cfg)
{
    LOG_INF("Mock DAC [%s]: Channel %d setup with %d-bit resolution", 
             dev->name, channel_cfg->channel_id, channel_cfg->resolution);
    return 0;
}

static const struct dac_driver_api dac_mock_api = {
    .channel_setup = dac_mock_channel_setup,
    .write_value = dac_mock_write_value,
};

#define DT_DRV_COMPAT zephyr_dac_mock

#define DAC_MOCK_INIT(n) \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, \
                          POST_KERNEL, CONFIG_DAC_INIT_PRIORITY, \
                          &dac_mock_api);

DT_INST_FOREACH_STATUS_OKAY(DAC_MOCK_INIT)
