#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_voice_status_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_beep_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_record_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_play_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_stream_status_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_stream_config_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_stream_start_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_voice_stream_stop_execute(const char *input_json, char *output, size_t output_size);
