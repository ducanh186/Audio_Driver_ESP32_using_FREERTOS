#include "audio_player.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "audio_player.h"
#include "main.h"
#include "driver/i2s.h"
#include "driver/gpio.h"

static const char *TAG = "mp3_demo";
extern "C" {
    void app_main(void);
}
#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define SPIFFS_BASE             "/spiffs"
#define MP3_FILE_NAME           "/For_Elise.mp3"
#define DEFAULT_VOLUME          60

// I2S Configuration
#define I2S_NUM                 I2S_NUM_0
#define I2S_BCK_PIN             GPIO_NUM_26
#define I2S_WS_PIN              GPIO_NUM_25
#define I2S_DATA_OUT_PIN        GPIO_NUM_22
#define I2S_SAMPLE_RATE         44100
#define I2S_SAMPLE_BITS         16

static audio_player_t audio_player_type = AUDIO_PLAYER_I2S;
static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_audio_player_handle = NULL;
static audio_player_config_t player_config = {0};
static FILE *s_fp = NULL;
static file_iterator_instance_t *file_iterator = NULL;
static uint8_t current_volume = DEFAULT_VOLUME;

/**
 * @brief event group
 */
typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

// I2S initialization function
static esp_err_t i2s_init(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .tx_desc_auto_clear = true,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_pin(I2S_NUM, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S pins: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM);
        return ret;
    }

    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

// Simple I2S write function
static esp_err_t i2s_write_data(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = i2s_write(I2S_NUM, audio_buffer, len, bytes_written, pdMS_TO_TICKS(timeout_ms));
    return ret;
}

// Simple volume control (software scaling)
static void apply_volume_scaling(int16_t *samples, size_t sample_count, uint8_t volume)
{
    if (volume > 100) volume = 100;
    float scale = (float)volume / 100.0f;
    
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)((float)samples[i] * scale);
    }
}

static esp_err_t _audio_player_mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    esp_err_t ret = ESP_OK;
    if (audio_player_type == AUDIO_PLAYER_I2S) {
        // For I2S, we can implement mute by setting volume to 0 or stopping I2S
        if (setting == AUDIO_PLAYER_MUTE) {
            ESP_LOGI(TAG, "Audio muted");
            // Could pause I2S or set a mute flag
        } else {
            ESP_LOGI(TAG, "Audio unmuted");
        }
        ret = ESP_OK;
    } else {
        if (s_audio_player_handle == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "mute setting: %s", setting == 0 ? "mute" : "unmute");
        ret = uac_host_device_set_mute(s_audio_player_handle, (setting == 0 ? true : false));
    }
    return ret;
}

static esp_err_t _audio_player_write_fn(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    if (audio_player_type == AUDIO_PLAYER_I2S) {
        // Apply volume scaling for I2S output
        apply_volume_scaling((int16_t*)audio_buffer, len / sizeof(int16_t), current_volume);
        ret = i2s_write_data(audio_buffer, len, bytes_written, timeout_ms);
    } else {
        if (s_audio_player_handle == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        *bytes_written = 0;
        ret = uac_host_device_write(s_audio_player_handle, audio_buffer, len, timeout_ms);
        if (ret == ESP_OK) {
            *bytes_written = len;
        }
    }
    return ret;
}

static esp_err_t _audio_player_std_clock(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    if (audio_player_type == AUDIO_PLAYER_I2S) {
        // Reconfigure I2S with new sample rate
        i2s_set_sample_rates(I2S_NUM, rate);
        ESP_LOGI(TAG, "I2S reconfigured: rate %"PRIu32", bits %"PRIu32", channels %d", 
                 rate, bits_cfg, (int)ch);
    } else {
        if (s_audio_player_handle == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Re-config: speaker rate %"PRIu32", bits %"PRIu32", mode %s", 
                 rate, bits_cfg, ch == 1 ? "MONO" : (ch == 2 ? "STEREO" : "INVALID"));
        ESP_ERROR_CHECK(uac_host_device_stop(s_audio_player_handle));
        const uac_host_stream_config_t stm_config = {
            .channels = ch,
            .bit_resolution = bits_cfg,
            .sample_freq = rate,
        };
        ret = uac_host_device_start(s_audio_player_handle, &stm_config);
    }
    return ret;
}

static void _audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "ctx->audio_event = %d", ctx->audio_event);
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE: {
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_IDLE");
        if (s_audio_player_handle != NULL) {
            ESP_ERROR_CHECK(uac_host_device_suspend(s_audio_player_handle));
        }
        ESP_LOGI(TAG, "Play in loop");
        s_fp = fopen(SPIFFS_BASE MP3_FILE_NAME, "rb");
        if (s_fp) {
            ESP_LOGI(TAG, "Playing '%s'", MP3_FILE_NAME);
            audio_player_play(s_fp);
        } else {
            ESP_LOGE(TAG, "unable to open filename '%s'", MP3_FILE_NAME);
        }
        break;
    }
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PLAY");
        if (s_audio_player_handle != NULL) {
            ESP_ERROR_CHECK(uac_host_device_resume(s_audio_player_handle));
            uac_host_device_set_volume(s_audio_player_handle, current_volume);
        }
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PAUSE");
        break;
    default:
        break;
    }
}

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        audio_player_type = AUDIO_PLAYER_I2S;
        s_audio_player_handle = NULL;
        ESP_LOGI(TAG, "UAC Device disconnected, switching to I2S");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
    }
    
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL2,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    
    s_event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, pdMS_TO_TICKS(100))) {
            if (UAC_DRIVER_EVENT == evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                    audio_player_type = AUDIO_PLAYER_USB;
                    uac_host_dev_info_t dev_info;
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = 16000,
                        .buffer_threshold = 4000,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    ESP_ERROR_CHECK(uac_host_get_device_info(uac_device_handle, &dev_info));
                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                    uac_host_printf_device_param(uac_device_handle);
                    
                    const uac_host_stream_config_t stm_config = {
                        .channels = 2,
                        .bit_resolution = 16,
                        .sample_freq = 48000,
                    };
                    ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &stm_config));
                    s_audio_player_handle = uac_device_handle;
                    uac_host_device_set_volume(s_audio_player_handle, current_volume);
                    
                    s_fp = fopen(SPIFFS_BASE MP3_FILE_NAME, "rb");
                    if (s_fp) {
                        ESP_LOGI(TAG, "Playing '%s'", MP3_FILE_NAME);
                        audio_player_play(s_fp);
                    } else {
                        ESP_LOGE(TAG, "unable to open filename '%s'", MP3_FILE_NAME);
                    }
                    break;
                }
                case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                    ESP_LOGI(TAG, "UAC Device connected: MIC (not supported in this example)");
                    break;
                }
                default:
                    break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                uac_host_device_event_t event = evt_queue.device_evt.event;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    break;
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                    break;
                default:
                    break;
                }
            } else if (APP_EVENT == evt_queue.event_group) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
}

// Simple SPIFFS mount function
static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    
    return ESP_OK;
}

// Public API functions
audio_player_t get_audio_player_type(void)
{
    return audio_player_type;
}

uac_host_device_handle_t get_audio_player_handle(void)
{
    return s_audio_player_handle;
}

uint8_t get_sys_volume(void)
{
    return current_volume;
}

void set_sys_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    current_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", volume);
    
    if (s_audio_player_handle != NULL) {
        uac_host_device_set_volume(s_audio_player_handle, volume);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting MP3 Audio Player");
    
    // Create event queue
    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);

    // Initialize SPIFFS
    ESP_ERROR_CHECK(spiffs_init());

    // Initialize file iterator
    file_iterator = file_iterator_new(SPIFFS_BASE);
    assert(file_iterator != NULL);

    // Initialize I2S
    ESP_ERROR_CHECK(i2s_init());

    // Initialize audio player
    player_config.mute_fn = _audio_player_mute_fn;
    player_config.write_fn = _audio_player_write_fn;
    player_config.clk_set_fn = _audio_player_std_clock;
    player_config.priority = 1;

    ESP_ERROR_CHECK(audio_player_new(player_config));
    ESP_ERROR_CHECK(audio_player_callback_register(_audio_player_callback, NULL));

    // Create UAC and USB tasks
    static TaskHandle_t uac_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                             USER_TASK_PRIORITY, &uac_task_handle, 1);
    assert(ret == pdTRUE);
    
    ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                  USB_HOST_TASK_PRIORITY, NULL, 1);
    assert(ret == pdTRUE);

    // Start playing audio file automatically
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for initialization
    
    s_fp = fopen(SPIFFS_BASE MP3_FILE_NAME, "rb");
    if (s_fp) {
        ESP_LOGI(TAG, "Auto-playing '%s'", MP3_FILE_NAME);
        audio_player_play(s_fp);
    } else {
        ESP_LOGE(TAG, "Unable to open filename '%s'", MP3_FILE_NAME);
    }

    ESP_LOGI(TAG, "MP3 Audio Player initialized successfully");
    
    // Main loop - could add console commands or other control logic here
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Add any periodic tasks or status updates here
    }
}