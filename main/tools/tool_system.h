#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Report device status for diagnostics and self-checks.
 * Input JSON: {}
 */
esp_err_t tool_system_status_execute(const char *input_json, char *output, size_t output_size);
