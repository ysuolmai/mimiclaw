#pragma once

#include "esp_err.h"

typedef enum {
    WIFI_ONBOARD_MODE_CAPTIVE = 0,
    WIFI_ONBOARD_MODE_ADMIN,
    WIFI_ONBOARD_MODE_STA_ADMIN,
} wifi_onboard_mode_t;

/**
 * Start WiFi onboarding/configuration portal.
 * CAPTIVE mode opens DNS hijack + config page and blocks forever.
 * ADMIN mode keeps a local config hotspot alive without captive redirects.
 * STA_ADMIN serves the same admin page on the connected STA IP without AP.
 */
esp_err_t wifi_onboard_start(wifi_onboard_mode_t mode);
