// main.c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include "audio_player.h"
#include "config.h"

#define TAG "mp3_i2s_player"

static FILE *mp3_fp = NULL;
static bool i2s_initialized = false;
static bool is_paused = false;
static TaskHandle_t button_task_handle = NULL;

/*----------------- Audio Player Callbacks -----------------*/

// Mute/unmute callback (not strictly necessary for I2S-only setup)
static esp_err_t _audio_player_mute_fn(AUDIO_PLAYER_MUTE_SETTING setting) {
    ESP_LOGI(TAG, "Mute callback: %s", (setting == AUDIO_PLAYER_UNMUTE) ? "UNMUTE" : "MUTE");
    return ESP_OK;
}

// Write PCM data to I2S
static esp_err_t _audio_player_write_fn(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms) {
    if (!i2s_initialized) {
        *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    size_t out_bytes = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, audio_buffer, len, &out_bytes, portMAX_DELAY);
    if (err == ESP_OK) {
        *bytes_written = out_bytes;
    } else {
        *bytes_written = 0;
    }
    return err;
}

// Reconfigure I2S when MP3 decoder changes sample rate / bit depth / channels
static esp_err_t _audio_player_clk_set(uint32_t sample_rate, uint32_t bits_cfg, i2s_slot_mode_t ch) {
    ESP_LOGI(TAG, "Reconfiguring I2S: %u Hz, %u-bit, %s",
             sample_rate,
             bits_cfg,
             (ch == 2) ? "stereo" : "mono");

    // Uninstall old driver if already initialized
    if (i2s_initialized) {
        i2s_driver_uninstall(I2S_NUM_0);
        i2s_initialized = false;
    }

    // Update I2S configuration
    i2s_speaker_config.sample_rate = sample_rate;
    i2s_speaker_config.bits_per_sample =
        (bits_cfg == 24) ? I2S_BITS_PER_SAMPLE_24BIT :
        (bits_cfg == 32) ? I2S_BITS_PER_SAMPLE_32BIT :
                           I2S_BITS_PER_SAMPLE_16BIT;
    i2s_speaker_config.channel_format = (ch == 2)
        ? I2S_CHANNEL_FMT_RIGHT_LEFT
        : I2S_CHANNEL_FMT_ONLY_LEFT;

    // Install new I2S driver and set pins
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_speaker_config, 0, NULL);
    if (err != ESP_OK) return err;
    err = i2s_set_pin(I2S_NUM_0, &i2s_speaker_pins);
    if (err == ESP_OK) i2s_initialized = true;
    return err;
}

/*----------------- Audio Player Event Callback -----------------*/

// Called when playback state changes; we only care about IDLE (end of file)
static void _audio_player_event_cb(audio_player_cb_ctx_t *ctx) {
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        ESP_LOGI(TAG, "MP3 file has finished playing.");
        // No replay logic here
    }
}

/*----------------- GPIO ISR for Pause/Resume -----------------*/

// Interrupt Service Routine for the pause button
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    BaseType_t higher_woken = pdFALSE;
    // Notify the button task that the button was pressed
    vTaskNotifyGiveFromISR(button_task_handle, &higher_woken);
    if (higher_woken) {
        portYIELD_FROM_ISR();
    }
}

/*----------------- Button Task -----------------*/

// Waits for notification from ISR, then toggles pause/resume
static void button_task(void *arg) {
    // Delay a moment so audio_task is fully initialized
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        // Block until ISR sends a notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Toggle pause/resume
        if (is_paused) {
            audio_player_resume();
            is_paused = false;
            ESP_LOGI(TAG, "Resumed playback");
        } else {
            audio_player_pause();
            is_paused = true;
            ESP_LOGI(TAG, "Paused playback");
        }
    }
}

/*----------------- GPIO Configuration -----------------*/

// Configure BTN_PAUSE_GPIO as input with pull-up and attach ISR
static void gpio_buttons_init() {
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_NEGEDGE,    // Trigger on falling edge (HIGH->LOW)
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_PAUSE_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    // Install ISR service (only once per application)
    gpio_install_isr_service(0);
    // Attach ISR handler to BTN_PAUSE_GPIO
    gpio_isr_handler_add(BTN_PAUSE_GPIO, gpio_isr_handler, NULL);
}

/*----------------- Main Application -----------------*/

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "== STARTING AUDIO PLAYER ==");

    // 1. Mount SPIFFS so we can open the MP3 file
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&(esp_vfs_spiffs_conf_t) {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true
    }));
    ESP_LOGI(TAG, "SPIFFS mounted");

    // 2. Initialize I2S with default 44.1 kHz, 16-bit, mono
    i2s_config_t init_conf = i2s_speaker_config;
    init_conf.sample_rate     = SAMPLE_RATE;
    init_conf.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    init_conf.channel_format  = I2S_CHANNEL_FMT_ONLY_LEFT;
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &init_conf, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &i2s_speaker_pins));
    i2s_initialized = true;
    ESP_LOGI(TAG, "I2S initialized: %d Hz, 16-bit, mono", SAMPLE_RATE);

    // 3. Create audio_player instance and register callbacks
    audio_player_config_t cfg = {
        .mute_fn    = _audio_player_mute_fn,
        .write_fn   = _audio_player_write_fn,
        .clk_set_fn = _audio_player_clk_set,
        .priority   = 4
    };
    ESP_ERROR_CHECK(audio_player_new(cfg));
    audio_player_callback_register(_audio_player_event_cb, NULL);

    // 4. Initialize the Pause/Resume button and its ISR
    gpio_buttons_init();
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, &button_task_handle);

    // 5. Open and play a single MP3 file from SPIFFS
    mp3_fp = fopen("/spiffs/new_epic.mp3", "rb");
    if (!mp3_fp) {
        ESP_LOGE(TAG, "Failed to open file: /spiffs/new_epic.mp3");
        return;
    }
    ESP_LOGI(TAG, "Playing: /spiffs/new_epic.mp3");
    audio_player_play(mp3_fp);

    // 6. Keep main alive; audio/task runs in background
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
