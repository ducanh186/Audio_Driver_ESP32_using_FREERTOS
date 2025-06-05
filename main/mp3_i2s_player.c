
#include <stdio.h>
//#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include "audio_player.h"
#include "config.h"   

static const char *TAG = "mp3_i2s_player";
// extern "C" {
//     void app_main(void);
// }
// Đường dẫn file MP3 trong SPIFFS
#define MP3_FILE_PATH  "/spiffs/new_epic.mp3"

// File handle để có thể replay khi end-of-file
static FILE *mp3_fp = NULL;

/**
 * @brief write_fn cho audio_player: nhận buffer PCM và ghi qua I2S.
 */
static esp_err_t _audio_player_write_fn(void *audio_buffer,
                                        size_t len,
                                        size_t *bytes_written,
                                        uint32_t timeout_ms)
{
    size_t out_bytes = 0;
    // Ghi block đến khi xong toàn bộ len byte
    esp_err_t err = i2s_write(I2S_NUM_0, audio_buffer, len, &out_bytes, portMAX_DELAY);
    if (err == ESP_OK) {
        *bytes_written = out_bytes;
    } else {
        *bytes_written = 0;
    }
    return err;
}

/**
 * @brief clk_set_fn cho audio_player: được gọi khi MP3 frame có sample rate/bits khác.
 *        Chúng ta reinit I2S với thông số mới.
 */
static esp_err_t _audio_player_clk_set(uint32_t sample_rate,
                                       uint32_t bits_cfg,
                                       i2s_slot_mode_t ch)
{
    ESP_LOGI(TAG, "audio_player requests reconfig I2S: %d Hz, %d-bit, %s",
             sample_rate,
             bits_cfg,
             (ch == 2) ? "stereo" : "mono");

    // Gỡ driver cũ
    i2s_driver_uninstall(I2S_NUM_0);

    // Cập nhật lại cấu hình
    i2s_speaker_config.sample_rate = sample_rate;

    // Chuyển bits_cfg về enum phù hợp
    if (bits_cfg == 16) {
        i2s_speaker_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    } else if (bits_cfg == 24) {
        i2s_speaker_config.bits_per_sample = I2S_BITS_PER_SAMPLE_24BIT;
    } else if (bits_cfg == 32) {
        i2s_speaker_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    } else {
        ESP_LOGW(TAG, "Unsupported bits_cfg %d, defaulting to 16", bits_cfg);
        i2s_speaker_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    }

    // Mono (MAX98357A) hoặc stereo
    if (ch == 2) {
        i2s_speaker_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    } else {
        i2s_speaker_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    }

    // Cài đặt driver mới với config vừa cập nhật
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_speaker_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Gán chân I2S
    err = i2s_set_pin(I2S_NUM_0, &i2s_speaker_pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S reconfigured successfully");
    return ESP_OK;
}

/**
 * @brief callback event cho audio_player: khi file MP3 chạy hết, được gọi về IDLE.
 *        Ở đây ta đóng file cũ, mở lại để lặp phát.
 */
static void _audio_player_event_cb(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        ESP_LOGI(TAG, "Reached end of MP3, replaying...");
        if (mp3_fp) {
            fclose(mp3_fp);
        }
        mp3_fp = fopen(MP3_FILE_PATH, "rb");
        if (mp3_fp) {
            audio_player_play(mp3_fp);
        } else {
            ESP_LOGE(TAG, "Cannot reopen MP3: %s", MP3_FILE_PATH);
        }
    }
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "=== STARTING MP3 VIA I2S ===");

    // 1. Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true
    };
    err = esp_vfs_spiffs_register(&spiffs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SPIFFS mounted");

    // 2. Khởi tạo I2S (mặc định 44100 Hz, 16-bit, mono)
    {
        i2s_config_t init_conf = i2s_speaker_config;
        init_conf.sample_rate = SAMPLE_RATE;                   // 44100
        init_conf.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; // 16-bit
        init_conf.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;  // Mono

        // Gỡ driver cũ (nếu có)
        i2s_driver_uninstall(I2S_NUM_0);

        // Cài driver I2S
        err = i2s_driver_install(I2S_NUM_0, &init_conf, 0, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
            return;
        }
        // Gán chân
        err = i2s_set_pin(I2S_NUM_0, &i2s_speaker_pins);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "I2S initialized: %d Hz, 16-bit, mono", SAMPLE_RATE);
    }

    // 3. Khởi tạo audio_player (chỉ decode MP3 → I2S)
    {
        audio_player_config_t player_cfg = {
            .mute_fn    = NULL,                 // Không dùng mute hardware
            .write_fn   = _audio_player_write_fn,
            .clk_set_fn = _audio_player_clk_set,
            .priority   = 4
        };
        err = audio_player_new(player_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "audio_player_new failed: %s", esp_err_to_name(err));
            return;
        }
        audio_player_callback_register(_audio_player_event_cb, NULL);
    }

    // 4. Mở file MP3 và phát
    mp3_fp = fopen(MP3_FILE_PATH, "rb");
    if (!mp3_fp) {
        ESP_LOGE(TAG, "Cannot open MP3 file: %s", MP3_FILE_PATH);
        return;
    }
    ESP_LOGI(TAG, "Playing MP3: %s", MP3_FILE_PATH);
    audio_player_play(mp3_fp);

    // 5. Giữ app_main chạy để audio_player có thời gian decode & gửi I2S
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
