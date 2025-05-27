#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include <dirent.h>
#include <string.h>
#include "I2SOutput.h"
#include "SDCard.h"
#include "SPIFFS.h"
#include "WAVFileReader.h"
#include "config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#ifdef CONFIG_AUDIO_SUPPORT_MP3_DECODER
#include "mp3_decoder.h"
#elif (CONFIG_AUDIO_SUPPORT_AMRNB_DECODER ||    \
        CONFIG_AUDIO_SUPPORT_AMRWB_DECODER)
#include "amr_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_OPUS_DECODER
#include "opus_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_OGG_DECODER
#include "ogg_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_FLAC_DECODER
#include "flac_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_WAV_DECODER
#include "wav_decoder.h"
#elif ((CONFIG_AUDIO_SUPPORT_AAC_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_M4A_DECODER) ||   \
        (CONFIG_AUDIO_SUPPORT_TS_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_MP4_DECODER))
#include "aac_decoder.h"
#endif
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "board.h"


static const char *TAG = "app";
extern "C" {
  void app_main(void);
}

/*Task:
+ audio_processing_task: Đọc file WAV, mix âm thanh, gửi dữ liệu vào queue.
+ i2s_output_task: Lấy dữ liệu từ queue và phát qua I2S.
+ button_task: Xử lý sự kiện nút bấm qua ISR.
*/
// Định nghĩa hằng số
#define SAMPLE_RATE 44100
#define BUFFER_SIZE 1024
#define QUEUE_SIZE 20 // Tăng kích thước queue để giảm underrun
#define DEBOUNCE_TIME_MS 500 //Thời gian debounce (500ms) để loại bỏ nhiễu khi nhấn nút.

// Biến toàn cục FreeRTOS
static QueueHandle_t audio_queue;
static EventGroupHandle_t event_group; //EventGroup để đồng bộ hóa trạng thái (phát, mix, dừng).
static TimerHandle_t debounce_timer; //Timer phần mềm để debounce nút bấm.

// Event Bits
#define BIT_MUSIC_PLAYING (1 << 0)
#define BIT_MIX_REQUESTED (1 << 1)
#define BIT_BUTTON_PLAY (1 << 2)
#define BIT_BUTTON_MIX (1 << 3)
#define BIT_STOP_REQUESTED (1 << 4)

// ISR cho nút bấm
/*   ISR (Interrupt Service Routine)
Phản ứng nhanh với sự kiện phần cứng (như nhấn nút) mà không cần thăm dò (polling) liên tục, tiết kiệm CPU.
IRAM (Internal RAM) 
ISR tiêu tốn IRAM, giới hạn trên ESP32 (128 KB IRAM).
*/
void IRAM_ATTR button_play_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_PLAY, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void IRAM_ATTR button_mix_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_MIX, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// Callback cho timer debounce
void debounce_timer_callback(TimerHandle_t xTimer) {
}

// Task đọc và mixing âm thanh
void audio_processing_task(void *pvParameters) {
    I2SOutput *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
    int16_t *main_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    int16_t *mix_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    int16_t *output_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!main_buf || !mix_buf || !output_buf) {
        ESP_LOGE(TAG, "Not enough memory for buffers");
        delete output;
        vTaskDelete(NULL);
    }

    FILE *main_fp = fopen("/sdcard/gong.wav", "rb");
    FILE *mix_fp = fopen("/sdcard/huh.wav", "rb");
    if (!main_fp || !mix_fp) {
        ESP_LOGE(TAG, "Cannot open WAV files");
        if (main_fp) fclose(main_fp);
        if (mix_fp) fclose(mix_fp);
        free(main_buf); free(mix_buf); free(output_buf);
        delete output;
        vTaskDelete(NULL);
    }

    WAVFileReader *main_reader = new WAVFileReader(main_fp);
    WAVFileReader *mix_reader = new WAVFileReader(mix_fp);

    ESP_LOGI(TAG, "Sample rate: %d", main_reader->sample_rate());
    output->start(main_reader->sample_rate());

    while (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
        if (xEventGroupGetBits(event_group) & BIT_STOP_REQUESTED) {
            break;
        }

        int main_samples = main_reader->read(main_buf, BUFFER_SIZE);
        if (main_samples <= 0) break;

        if (xEventGroupGetBits(event_group) & BIT_MIX_REQUESTED) {
            int mix_samples = mix_reader->read(mix_buf, main_samples);
            for (int i = 0; i < main_samples; i++) {
                if (i < mix_samples) {
                    int32_t mixed = (int32_t)main_buf[i] + (int32_t)mix_buf[i];
                    output_buf[i] = (int16_t)(mixed / 2);
                } else {
                    output_buf[i] = main_buf[i];
                }
            }
            if (mix_samples < main_samples) {
                xEventGroupClearBits(event_group, BIT_MIX_REQUESTED);
                fseek(mix_fp, 44, SEEK_SET);
            }
        } else {
            memcpy(output_buf, main_buf, main_samples * sizeof(int16_t));
        }

        // Dùng xQueueOverwrite để ưu tiên dữ liệu mới nếu queue đầy
        if (!xQueueOverwrite(audio_queue, output_buf)) {
            ESP_LOGW(TAG, "Queue full, overwriting data");
        }
    }

    output->stop();
    delete main_reader;
    delete mix_reader;
    fclose(main_fp);
    fclose(mix_fp);
    free(main_buf); free(mix_buf); free(output_buf);
    delete output;
    xEventGroupClearBits(event_group, BIT_MUSIC_PLAYING | BIT_STOP_REQUESTED);
    vTaskDelete(NULL);
}

// Task phát âm thanh qua I2S
void i2s_output_task(void *pvParameters) {
    int16_t *buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "Not enough memory for buffer");
        vTaskDelete(NULL);
    }

    while (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
        if (xQueueReceive(audio_queue, buffer, portMAX_DELAY) == pdTRUE) {
            size_t bytes_written;
            i2s_write(I2S_NUM_0, buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_written, portMAX_DELAY);
            if (bytes_written != BUFFER_SIZE * sizeof(int16_t)) {
                ESP_LOGW(TAG, "Not all data written to I2S");
            }
        }
    }
    free(buffer);
    vTaskDelete(NULL);
}

// Task xử lý nút bấm
void button_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(event_group, BIT_BUTTON_PLAY | BIT_BUTTON_MIX, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & BIT_BUTTON_PLAY) {
            ESP_LOGI(TAG, "GPIO_BUTTON pressed");
            if (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
                ESP_LOGI(TAG, "Main music stopping");
                xEventGroupSetBits(event_group, BIT_STOP_REQUESTED);
            } else {
                ESP_LOGI(TAG, "Main music starting");
                xEventGroupSetBits(event_group, BIT_MUSIC_PLAYING);
                xTaskCreate(audio_processing_task, "audio_processing_task", 4096, NULL, 5, NULL);
                xTaskCreate(i2s_output_task, "i2s_output_task", 4096, NULL, 5, NULL);
            }
            xTimerStart(debounce_timer, 0); // Bắt đầu timer debounce
        }
        if (bits & BIT_BUTTON_MIX) {
            ESP_LOGI(TAG, "GPIO_BUTTON_1 pressed - mix requested");
            xEventGroupSetBits(event_group, BIT_MIX_REQUESTED);
            xTimerStart(debounce_timer, 0);
        }
        // Dùng vTaskDelayUntil để kiểm soát tần suất
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
    }
}

void app_main(void) {

    ESP_LOGI(TAG, "Starting audio player with deep OS integration");

#ifdef USE_SPIFFS
    ESP_LOGI(TAG, "Mounting SPIFFS on /sdcard");
    new SPIFFS("/sdcard");
#else
    ESP_LOGI(TAG, "Mounting SDCard on /sdcard");
    new SDCard("/sdcard", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
#endif

    // Kiểm tra nội dung thẻ SD
    DIR *dir = opendir("/sdcard");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "Found file: %s", ent->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "Cannot open /sdcard directory!");
    }

    // Khởi tạo FreeRTOS components
    event_group = xEventGroupCreate();
    audio_queue = xQueueCreate(QUEUE_SIZE, BUFFER_SIZE * sizeof(int16_t));
    debounce_timer = xTimerCreate("debounce_timer", pdMS_TO_TICKS(DEBOUNCE_TIME_MS), pdFALSE, NULL, debounce_timer_callback);

    // Cấu hình GPIO cho nút bấm
    gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(GPIO_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON_1, GPIO_PULLDOWN_ONLY);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON, button_play_isr_handler, NULL);
    gpio_isr_handler_add(GPIO_BUTTON_1, button_mix_isr_handler, NULL);

    // Tạo task xử lý nút bấm
    xTaskCreate(button_task, "button_task", 2048, NULL, 2, NULL);
     audio_pipeline_handle_t pipeline;
    audio_element_handle_t fatfs_stream_reader, i2s_stream_writer, music_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    // Initialize SD Card peripheral
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[3.1] Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

#ifdef CONFIG_AUDIO_SUPPORT_MP3_DECODER
    ESP_LOGI(TAG, "[3.3] Create mp3 decoder");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    music_decoder = mp3_decoder_init(&mp3_cfg);
#elif (CONFIG_AUDIO_SUPPORT_AMRNB_DECODER ||    \
        CONFIG_AUDIO_SUPPORT_AMRWB_DECODER)
    ESP_LOGI(TAG, "[3.3] Create amr decoder");
    amr_decoder_cfg_t  amr_dec_cfg  = DEFAULT_AMR_DECODER_CONFIG();
    music_decoder = amr_decoder_init(&amr_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    ESP_LOGI(TAG, "[3.3] Create opus decoder");
    opus_decoder_cfg_t opus_dec_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    music_decoder = decoder_opus_init(&opus_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_OGG_DECODER
    ESP_LOGI(TAG, "[3.3] Create ogg decoder");
    ogg_decoder_cfg_t  ogg_dec_cfg  = DEFAULT_OGG_DECODER_CONFIG();
    music_decoder = ogg_decoder_init(&ogg_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_FLAC_DECODER
    ESP_LOGI(TAG, "[3.3] Create flac decoder");
    flac_decoder_cfg_t flac_dec_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    music_decoder = flac_decoder_init(&flac_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_WAV_DECODER
    ESP_LOGI(TAG, "[3.3] Create wav decoder");
    wav_decoder_cfg_t  wav_dec_cfg  = DEFAULT_WAV_DECODER_CONFIG();
    music_decoder = wav_decoder_init(&wav_dec_cfg);
#elif ((CONFIG_AUDIO_SUPPORT_AAC_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_M4A_DECODER) ||   \
        (CONFIG_AUDIO_SUPPORT_TS_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_MP4_DECODER))
    ESP_LOGI(TAG, "[3.3] Create aac decoder");
    aac_decoder_cfg_t  aac_dec_cfg  = DEFAULT_AAC_DECODER_CONFIG();
    music_decoder = aac_decoder_init(&aac_dec_cfg);
#endif

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, music_decoder, "dec");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[3.5] Link it together [sdcard]-->fatfs_stream-->music_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"file", "dec", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

#ifdef CONFIG_AUDIO_SUPPORT_MP3_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.mp3 ");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.mp3");
#elif CONFIG_AUDIO_SUPPORT_AMRNB_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /test.amr");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.amr");
#elif CONFIG_AUDIO_SUPPORT_AMRWB_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.Wamr");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.Wamr");
#elif CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.opus");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.opus");
#elif CONFIG_AUDIO_SUPPORT_OGG_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.ogg");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.ogg");
#elif CONFIG_AUDIO_SUPPORT_FLAC_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.flac");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.flac");
#elif CONFIG_AUDIO_SUPPORT_WAV_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.wav");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.wav");
#elif CONFIG_AUDIO_SUPPORT_AAC_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.aac");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.aac");
#elif CONFIG_AUDIO_SUPPORT_M4A_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.m4a");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.m4a");
#elif CONFIG_AUDIO_SUPPORT_TS_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.ts");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.ts");
#elif CONFIG_AUDIO_SUPPORT_MP4_DECODER
    ESP_LOGI(TAG, "[3.6] Set up uri: /sdcard/test.mp4");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.mp4");
#endif

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);
    // Example of linking elements into an audio pipeline -- END

    ESP_LOGI(TAG, "[ 6 ] Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) music_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(music_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from music decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, fatfs_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, music_decoder);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(fatfs_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(music_decoder);
    esp_periph_set_destroy(set);
}