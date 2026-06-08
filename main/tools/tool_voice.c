#include "tools/tool_voice.h"
#include "mimi_config.h"
#include "voice/voice_hw.h"
#include "voice/voice_stream.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static cJSON *parse_input_or_empty(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse((input_json && input_json[0]) ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return NULL;
    }
    return root;
}

static const char *json_string(cJSON *root, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    const char *value = cJSON_GetStringValue(item);
    return (value && value[0]) ? value : fallback;
}

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool validate_spiffs_path(const char *path)
{
    if (!path || !path[0]) return false;
    if (strncmp(path, MIMI_SPIFFS_BASE "/", strlen(MIMI_SPIFFS_BASE) + 1) != 0) return false;
    if (strstr(path, "..")) return false;
    return true;
}

esp_err_t tool_voice_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    voice_hw_status(output, output_size);
    return MIMI_ENABLE_VOICE_HW ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tool_voice_beep_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    int freq_hz = json_int(root, "freq_hz", 880);
    int duration_ms = json_int(root, "duration_ms", 300);
    esp_err_t err = voice_hw_beep(freq_hz, duration_ms);
    snprintf(output, output_size, "%s: voice beep freq_hz=%d duration_ms=%d",
             err == ESP_OK ? "OK" : "Error", freq_hz, duration_ms);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_voice_record_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    const char *path = json_string(root, "path", MIMI_VOICE_DEFAULT_FILE);
    int seconds = json_int(root, "seconds", 3);
    if (!validate_spiffs_path(path)) {
        snprintf(output, output_size, "Error: path must start with %s/ and must not contain '..'", MIMI_SPIFFS_BASE);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = voice_hw_record_wav(path, seconds);
    snprintf(output, output_size, "%s: recorded %d second WAV to %s",
             err == ESP_OK ? "OK" : "Error", seconds, path);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_voice_play_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    const char *path = json_string(root, "path", MIMI_VOICE_DEFAULT_FILE);
    if (!validate_spiffs_path(path)) {
        snprintf(output, output_size, "Error: path must start with %s/ and must not contain '..'", MIMI_SPIFFS_BASE);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = voice_hw_play_wav(path);
    snprintf(output, output_size, "%s: played WAV from %s",
             err == ESP_OK ? "OK" : "Error", path);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_voice_stream_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    voice_stream_status_t st;
    voice_stream_get_status(&st);
    snprintf(output, output_size,
             "Voice stream\n"
             "- enabled: %s\n"
             "- configured: %s\n"
             "- active: %s\n"
             "- connected: %s\n"
             "- url: %s\n"
             "- codec: %s\n"
             "- input_sample_rate: %d\n"
             "- output_sample_rate: %d\n"
             "- frame_ms: %d\n"
             "- max_seconds: %d\n",
             MIMI_ENABLE_VOICE_STREAM ? "yes" : "no",
             st.configured ? "yes" : "no",
             st.active ? "yes" : "no",
             st.connected ? "yes" : "no",
             st.url,
             st.codec,
             st.input_sample_rate,
             st.output_sample_rate,
             st.frame_ms,
             st.max_seconds);
    return MIMI_ENABLE_VOICE_STREAM ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tool_voice_stream_config_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    const char *url = json_string(root, "url", "");
    const char *codec = json_string(root, "codec", MIMI_VOICE_STREAM_DEFAULT_CODEC);
    esp_err_t err = voice_stream_set_config(url, codec);
    snprintf(output, output_size, "%s: voice stream config url=%s codec=%s",
             err == ESP_OK ? "OK" : "Error", url, codec);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_voice_stream_start_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    int seconds = json_int(root, "seconds", 10);
    esp_err_t err = voice_stream_start(seconds, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_voice_stream_stop_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    esp_err_t err = voice_stream_stop();
    snprintf(output, output_size, "%s: voice stream stop requested", err == ESP_OK ? "OK" : "Error");
    return err;
}
