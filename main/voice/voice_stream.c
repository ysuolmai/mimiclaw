#include "voice/voice_stream.h"
#include "mimi_config.h"
#include "voice/voice_hw.h"

#if MIMI_ENABLE_VOICE_STREAM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "voice_stream";

#define VOICE_STREAM_URL_MAX       192
#define VOICE_STREAM_CODEC_MAX     16
#define VOICE_STREAM_RX_TEXT_MAX   512

static char s_url[VOICE_STREAM_URL_MAX] = MIMI_SECRET_VOICE_STREAM_URL;
static char s_codec[VOICE_STREAM_CODEC_MAX] = MIMI_VOICE_STREAM_DEFAULT_CODEC;
static bool s_configured = false;
static bool s_active = false;
static bool s_connected = false;
static esp_websocket_client_handle_t s_client = NULL;
static TaskHandle_t s_task = NULL;
static int s_run_seconds = 0;
static char s_last_text[VOICE_STREAM_RX_TEXT_MAX];

static bool codec_is_supported(const char *codec)
{
    return !codec || codec[0] == '\0' ||
           strcmp(codec, "pcm16") == 0 ||
           strcmp(codec, "opus") == 0;
}

static void load_config_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_url);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_STREAM_URL, s_url, &len) != ESP_OK &&
            MIMI_SECRET_VOICE_STREAM_URL[0]) {
            strlcpy(s_url, MIMI_SECRET_VOICE_STREAM_URL, sizeof(s_url));
        }

        len = sizeof(s_codec);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_CODEC, s_codec, &len) != ESP_OK) {
            strlcpy(s_codec, MIMI_VOICE_STREAM_DEFAULT_CODEC, sizeof(s_codec));
        }
        nvs_close(nvs);
    }

    if (!codec_is_supported(s_codec)) {
        strlcpy(s_codec, MIMI_VOICE_STREAM_DEFAULT_CODEC, sizeof(s_codec));
    }
    s_configured = s_url[0] != '\0';
}

esp_err_t voice_stream_init(void)
{
    if (!MIMI_ENABLE_VOICE_STREAM) {
        ESP_LOGI(TAG, "voice stream disabled in this target profile");
        return ESP_OK;
    }

    load_config_from_nvs();
    ESP_LOGI(TAG, "voice stream %s, codec=%s", s_configured ? "configured" : "not configured", s_codec);
    return ESP_OK;
}

esp_err_t voice_stream_set_config(const char *url, const char *codec)
{
    if (!codec_is_supported(codec)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    if (url && url[0]) {
        err = nvs_set_str(nvs, MIMI_NVS_KEY_VOICE_STREAM_URL, url);
    } else {
        esp_err_t erase_err = nvs_erase_key(nvs, MIMI_NVS_KEY_VOICE_STREAM_URL);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
    }
    if (err == ESP_OK) {
        const char *chosen_codec = (codec && codec[0]) ? codec : MIMI_VOICE_STREAM_DEFAULT_CODEC;
        err = nvs_set_str(nvs, MIMI_NVS_KEY_VOICE_CODEC, chosen_codec);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strlcpy(s_url, url ? url : "", sizeof(s_url));
        strlcpy(s_codec, (codec && codec[0]) ? codec : MIMI_VOICE_STREAM_DEFAULT_CODEC, sizeof(s_codec));
        s_configured = s_url[0] != '\0';
    }
    return err;
}

void voice_stream_get_config(char *url, size_t url_size, char *codec, size_t codec_size)
{
    if (url && url_size) strlcpy(url, s_url, url_size);
    if (codec && codec_size) strlcpy(codec, s_codec, codec_size);
}

void voice_stream_get_status(voice_stream_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    strlcpy(status->url, s_url, sizeof(status->url));
    strlcpy(status->codec, s_codec, sizeof(status->codec));
    status->configured = s_configured;
    status->active = s_active;
    status->connected = s_connected;
    status->input_sample_rate = MIMI_VOICE_INPUT_SAMPLE_RATE;
    status->output_sample_rate = MIMI_VOICE_OUTPUT_SAMPLE_RATE;
    status->frame_ms = MIMI_VOICE_STREAM_FRAME_MS;
    status->max_seconds = MIMI_VOICE_STREAM_MAX_SECONDS;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "voice stream connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "voice stream disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        if (e->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            size_t copy = e->data_len;
            if (copy >= sizeof(s_last_text)) copy = sizeof(s_last_text) - 1;
            memcpy(s_last_text, e->data_ptr, copy);
            s_last_text[copy] = '\0';
            ESP_LOGI(TAG, "voice stream text: %.120s", s_last_text);
        } else if (e->op_code == WS_TRANSPORT_OPCODES_BINARY && e->data_len > 0) {
            if (strcmp(s_codec, "pcm16") == 0) {
                voice_hw_write_pcm((const int16_t *)e->data_ptr, e->data_len / sizeof(int16_t),
                                   MIMI_VOICE_OUTPUT_SAMPLE_RATE, 500);
            } else {
                ESP_LOGW(TAG, "binary downlink requires codec decoder: %s", s_codec);
            }
        }
    }
}

static char *build_hello_json(int seconds)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device", "mimiclaw-esp32-s3-supermini");
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON_AddStringToObject(root, "codec", s_codec);
    cJSON_AddNumberToObject(root, "sample_rate", MIMI_VOICE_INPUT_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "output_sample_rate", MIMI_VOICE_OUTPUT_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", 1);
    cJSON_AddNumberToObject(root, "frame_ms", MIMI_VOICE_STREAM_FRAME_MS);
    cJSON_AddNumberToObject(root, "seconds", seconds);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void send_text_json(const char *type, const char *state)
{
    if (!s_client || !s_connected) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", type);
    if (state) cJSON_AddStringToObject(root, "state", state);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(1000));
    free(json);
}

static void voice_stream_task(void *arg)
{
    (void)arg;
    s_active = true;
    s_last_text[0] = '\0';

    int seconds = s_run_seconds;
    if (seconds <= 0) seconds = 10;
    if (seconds > MIMI_VOICE_STREAM_MAX_SECONDS) seconds = MIMI_VOICE_STREAM_MAX_SECONDS;

    esp_websocket_client_config_t ws_cfg = {
        .uri = s_url,
        .buffer_size = 2048,
        .task_stack = MIMI_VOICE_STREAM_STACK,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "failed to init voice websocket");
        goto done;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_client);

    int64_t connect_deadline = esp_timer_get_time() + 8000000LL;
    while (!s_connected && esp_timer_get_time() < connect_deadline && s_active) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_connected || !s_active) {
        ESP_LOGW(TAG, "voice stream connect timeout");
        goto done;
    }

    char *hello = build_hello_json(seconds);
    if (hello) {
        esp_websocket_client_send_text(s_client, hello, strlen(hello), pdMS_TO_TICKS(1000));
        free(hello);
    }
    send_text_json("listen", "start");

    const int frame_samples = (MIMI_VOICE_INPUT_SAMPLE_RATE * MIMI_VOICE_STREAM_FRAME_MS) / 1000;
    int16_t *pcm = malloc(frame_samples * sizeof(int16_t));
    if (!pcm) {
        ESP_LOGE(TAG, "voice stream OOM");
        goto done;
    }

    int64_t end_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    size_t frames = 0;
    while (s_active && s_connected && esp_timer_get_time() < end_us) {
        size_t samples_read = 0;
        esp_err_t err = voice_hw_read_pcm(pcm, frame_samples, &samples_read, MIMI_VOICE_STREAM_FRAME_MS + 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "voice read failed: %s", esp_err_to_name(err));
            break;
        }
        if (samples_read == 0) continue;

        if (strcmp(s_codec, "pcm16") == 0) {
            esp_websocket_client_send_bin(s_client, (const char *)pcm,
                                          samples_read * sizeof(int16_t),
                                          pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGW(TAG, "opus selected but encoder is not linked; sending pcm16 fallback");
            esp_websocket_client_send_bin(s_client, (const char *)pcm,
                                          samples_read * sizeof(int16_t),
                                          pdMS_TO_TICKS(1000));
        }
        frames++;
    }

    free(pcm);
    send_text_json("listen", "stop");
    ESP_LOGI(TAG, "voice stream finished, frames=%u", (unsigned)frames);

done:
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    s_active = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_stream_start(int seconds, char *output, size_t output_size)
{
    if (!MIMI_ENABLE_VOICE_STREAM) {
        snprintf(output, output_size, "Error: voice stream disabled in this target profile");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_configured) {
        snprintf(output, output_size, "Error: configure voice_stream_url first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active || s_task) {
        snprintf(output, output_size, "Error: voice stream already active");
        return ESP_ERR_INVALID_STATE;
    }
    if (!voice_hw_is_ready()) {
        esp_err_t err = voice_hw_init();
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: voice hardware init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (seconds <= 0) seconds = 10;
    if (seconds > MIMI_VOICE_STREAM_MAX_SECONDS) seconds = MIMI_VOICE_STREAM_MAX_SECONDS;
    s_run_seconds = seconds;

    BaseType_t ok = xTaskCreatePinnedToCore(voice_stream_task, "voice_stream",
                                            MIMI_VOICE_STREAM_STACK, NULL,
                                            MIMI_VOICE_STREAM_PRIO, &s_task,
                                            MIMI_VOICE_STREAM_CORE);
    if (ok != pdPASS) {
        s_task = NULL;
        snprintf(output, output_size, "Error: failed to start voice stream task");
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size,
             "OK: started %d second %s voice stream to %s",
             seconds, s_codec, s_url);
    return ESP_OK;
}

esp_err_t voice_stream_stop(void)
{
    if (!s_active) return ESP_OK;
    s_active = false;
    if (s_client && s_connected) {
        send_text_json("listen", "stop");
    }
    return ESP_OK;
}

#else

#include <string.h>

esp_err_t voice_stream_init(void)
{
    return ESP_OK;
}

esp_err_t voice_stream_set_config(const char *url, const char *codec)
{
    (void)url;
    (void)codec;
    return ESP_ERR_NOT_SUPPORTED;
}

void voice_stream_get_config(char *url, size_t url_size, char *codec, size_t codec_size)
{
    if (url && url_size) url[0] = '\0';
    if (codec && codec_size) {
        strlcpy(codec, MIMI_VOICE_STREAM_DEFAULT_CODEC, codec_size);
    }
}

void voice_stream_get_status(voice_stream_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    strlcpy(status->codec, MIMI_VOICE_STREAM_DEFAULT_CODEC, sizeof(status->codec));
    status->input_sample_rate = MIMI_VOICE_INPUT_SAMPLE_RATE;
    status->output_sample_rate = MIMI_VOICE_OUTPUT_SAMPLE_RATE;
    status->frame_ms = MIMI_VOICE_STREAM_FRAME_MS;
    status->max_seconds = MIMI_VOICE_STREAM_MAX_SECONDS;
}

esp_err_t voice_stream_start(int seconds, char *output, size_t output_size)
{
    (void)seconds;
    if (output && output_size) {
        snprintf(output, output_size, "Error: voice stream disabled in this target profile");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_stream_stop(void)
{
    return ESP_OK;
}

#endif
