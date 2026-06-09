#pragma once

#include "esp_err.h"

/**
 * Start BOOT button monitoring.
 * Holding BOOT for MIMI_BOOT_BUTTON_LONG_PRESS_MS requests AP-only WiFi
 * reconfiguration mode and restarts the device. Saved credentials are kept.
 */
esp_err_t boot_button_init(void);
