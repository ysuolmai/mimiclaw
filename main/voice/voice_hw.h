#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t voice_hw_init(void);
bool voice_hw_is_ready(void);
void voice_hw_status(char *output, size_t output_size);
esp_err_t voice_hw_beep(int freq_hz, int duration_ms);
esp_err_t voice_hw_record_wav(const char *path, int seconds);
esp_err_t voice_hw_play_wav(const char *path);
esp_err_t voice_hw_read_pcm(int16_t *samples, size_t max_samples, size_t *samples_read, uint32_t timeout_ms);
esp_err_t voice_hw_write_pcm(const int16_t *samples, size_t sample_count, int sample_rate, uint32_t timeout_ms);
