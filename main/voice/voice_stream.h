#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char url[192];
    char codec[16];
    bool configured;
    bool active;
    bool connected;
    int input_sample_rate;
    int output_sample_rate;
    int frame_ms;
    int max_seconds;
} voice_stream_status_t;

esp_err_t voice_stream_init(void);
esp_err_t voice_stream_set_config(const char *url, const char *codec);
void voice_stream_get_config(char *url, size_t url_size, char *codec, size_t codec_size);
void voice_stream_get_status(voice_stream_status_t *status);
esp_err_t voice_stream_start(int seconds, char *output, size_t output_size);
esp_err_t voice_stream_stop(void);

