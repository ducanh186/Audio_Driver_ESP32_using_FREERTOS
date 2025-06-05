#include "config.h"

// I2S config for MAX98357A (speaker only, no mic)
i2s_config_t i2s_speaker_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
};

// I2S speaker pins
i2s_pin_config_t i2s_speaker_pins = {
    .bck_io_num   = I2S_SPEAKER_SERIAL_CLOCK,
    .ws_io_num    = I2S_SPEAKER_LEFT_RIGHT_CLOCK,
    .data_out_num = I2S_SPEAKER_SERIAL_DATA,
    .data_in_num  = I2S_PIN_NO_CHANGE
};
static void gpio_buttons_init() {
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BTN_PAUSE_GPIO) 
                      | (1ULL<<BTN_NEXT_GPIO) 
                      | (1ULL<<BTN_BACK_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);
}
