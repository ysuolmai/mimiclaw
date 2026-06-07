#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Append a note to today's daily memory file.
 * Input JSON: {"note": "..."}
 */
esp_err_t tool_memory_append_execute(const char *input_json, char *output, size_t output_size);
