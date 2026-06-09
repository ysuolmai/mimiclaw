#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_MODE_OFF = 0,
    STATUS_LED_MODE_ONBOARDING,
} status_led_mode_t;

/**
 * Initialize the board status LED. On ESP32-S3 Super Mini this drives the
 * onboard WS2812/RGB LED on GPIO48 and turns it off immediately.
 */
esp_err_t status_led_init(void);

/**
 * Set LED behavior. Normal connected runtime should stay OFF; AP onboarding
 * uses a slow blink.
 */
void status_led_set_mode(status_led_mode_t mode);
