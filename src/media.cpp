#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_system.h>

#include "es8311.h"
#include "main.h"

#define EXAMPLE_NUM_CHANNELS 2
#define SLOT_MODE I2S_SLOT_MODE_STEREO
#define EXAMPLE_SAMPLE_RATE (16000)
#define EXAMPLE_BUFFER_SIZE \
  (EXAMPLE_SAMPLE_RATE * EXAMPLE_NUM_CHANNELS * sizeof(uint16_t) * 20 / 1000)

#define I2C_NUM_0 i2c_port_t(0)
#define I2S_NUM_0 i2s_port_t(0)
#define I2S_NUM_1 i2s_port_t(1)

#ifdef CONFIG_CODEC_ES8311_SUPPORT
#define EXAMPLE_MCLK_MULTIPLE i2s_mclk_multiple_t(384)
#define EXAMPLE_MCLK_FREQ_HZ (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME 90  // 80 CONFIG_EXAMPLE_VOICE_VOLUME
#define EXAMPLE_MIC_GAIN ES8311_MIC_GAIN_12DB
#define I2C_SCL_IO (GPIO_NUM_18)
#define I2C_SDA_IO (GPIO_NUM_17)
#define I2S_MCK_IO (GPIO_NUM_16)
#define I2S_BCK_IO (GPIO_NUM_2)
#define I2S_WS_IO (GPIO_NUM_1)
#define I2S_DO_IO (GPIO_NUM_8)
#define I2S_DI_IO (GPIO_NUM_10)
#else
#define I2S_MCK_IO (GPIO_NUM_0)
#define DAC_BCLK_IO (GPIO_NUM_15)
#define DAC_LRCLK_IO (GPIO_NUM_16)
#define DAC_DATA_IO (GPIO_NUM_17)
#define ADC_BCK_IO (GPIO_NUM_38)
#define ADC_WS_IO (GPIO_NUM_39)
#define ADC_DATA_IO (GPIO_NUM_40)
#define I2S_PIN_NO_CHANGE (GPIO_NUM_NC)
#endif

static const char *TAG = "media";
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static int16_t *capture_buffer = NULL;
static size_t bytes_captured = 0;

#ifdef CONFIG_CODEC_ES8311_SUPPORT
static esp_err_t es8311_codec_init(void) {
  /* Initialize I2C peripheral */
  const i2c_config_t es_i2c_cfg = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = I2C_SDA_IO,
      .scl_io_num = I2C_SCL_IO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master =
          {
              .clk_speed = 100000,
          },
      .clk_flags = 0,
  };
  ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM, &es_i2c_cfg), TAG,
                      "config i2c failed");
  ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM, I2C_MODE_MASTER, 0, 0, 0),
                      TAG, "install i2c driver failed");

  /* Initialize es8311 codec */
  es8311_handle_t es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
      .sample_frequency = EXAMPLE_SAMPLE_RATE};

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16,
                              ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(
      es8311_sample_frequency_config(
          es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE,
          EXAMPLE_SAMPLE_RATE),
      TAG, "set es8311 sample frequency failed");
  ESP_RETURN_ON_ERROR(
      es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG,
      "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG,
                      "set es8311 microphone failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(es_handle, EXAMPLE_MIC_GAIN),
                      TAG, "set es8311 microphone gain failed");
  return ESP_OK;
}
#endif
static esp_err_t i2s_driver_init(void) {
#ifdef CONFIG_CODEC_ES8311_SUPPORT
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;  // Auto clear the legacy data in the DMA buffer
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      SLOT_MODE),
      .gpio_cfg =
          {
              .mclk = I2S_MCK_IO,
              .bclk = I2S_BCK_IO,
              .ws = I2S_WS_IO,
              .dout = I2S_DO_IO,
              .din = I2S_DI_IO,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;
#else
  // TX Channel Configuration
  i2s_chan_config_t chan_cfg_tx =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg_tx.auto_clear = true;
  i2s_chan_config_t chan_cfg_rx =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg_tx, &tx_handle, NULL));
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg_rx, NULL, &rx_handle));

  i2s_std_config_t std_cfg_tx = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      SLOT_MODE),
      .gpio_cfg =
          {
              .mclk = I2S_MCK_IO,
              .bclk = DAC_BCLK_IO,
              .ws = DAC_LRCLK_IO,
              .dout = DAC_DATA_IO,
              .din = I2S_PIN_NO_CHANGE,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  i2s_std_config_t std_cfg_rx = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      SLOT_MODE),
      .gpio_cfg =
          {
              .mclk = I2S_MCK_IO,
              .bclk = ADC_BCK_IO,
              .ws = ADC_WS_IO,
              .dout = I2S_PIN_NO_CHANGE,
              .din = ADC_DATA_IO,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
#endif
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg_tx));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg_rx));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  return ESP_OK;
}

void lk_init_audio() {
  if (i2s_driver_init() != ESP_OK) {
    panic("i2s driver init failed");
  }
  ESP_LOGI(TAG, "i2s driver init success");

#ifdef CONFIG_CODEC_ES8311_SUPPORT
  if (es8311_codec_init() != ESP_OK) {
    panic("es8311 codec init failed");
  }
  ESP_LOGI(TAG, "es8311 codec init success");
#endif

  capture_buffer = (int16_t *)malloc(EXAMPLE_BUFFER_SIZE);
  assert(capture_buffer != NULL);
}

const int16_t *lk_capture_audio(size_t *bytes) {
  size_t bytes_to_read = EXAMPLE_BUFFER_SIZE - bytes_captured;
  size_t bytes_read = 0;
  int16_t *ptr = capture_buffer + bytes_captured / 2;
  i2s_channel_read(rx_handle, ptr, bytes_to_read, &bytes_read, 0);  // timeout?
  bytes_captured += bytes_read;
  if (bytes_read < bytes_to_read) {
    return NULL;
  }
  if (bytes) {
    *bytes = bytes_captured;
  }
  bytes_captured = 0;
  return capture_buffer;
}

void lk_render_audio(const int16_t *data, size_t bytes) {
  size_t bytes_written = 0;
  int ret = i2s_channel_write(tx_handle, data, bytes, &bytes_written, 1000);
  if (ret != ESP_OK) {
    panic("i2s write failed");
  }
  if (bytes_written != bytes) {
    panic("i2s write bytes mismatch");
  }
}
