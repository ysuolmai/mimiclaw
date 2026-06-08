#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

esp_err_t voice_hw_init(void);
bool voice_hw_is_ready(void);
void voice_hw_status(char *output, size_t output_size);
esp_err_t voice_hw_beep(int freq_hz, int duration_ms);
esp_err_t voice_hw_record_wav(const char *path, int seconds);
esp_err_t voice_hw_play_wav(const char *path);
