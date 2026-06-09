#include "board/boot_button.h"

#include "mimi_config.h"
#include "wifi/wifi_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

static const char *TAG = "boot_button";

static void boot_button_task(void *arg)
{
    uint32_t held_ms = 0;
    bool fired = false;

    while (1) {
        int level = gpio_get_level((gpio_num_t)MIMI_BOOT_BUTTON_GPIO);

        if (level == 0) {
            if (held_ms < MIMI_BOOT_BUTTON_LONG_PRESS_MS) {
                held_ms += MIMI_BOOT_BUTTON_POLL_MS;
            }

            if (!fired && held_ms >= MIMI_BOOT_BUTTON_LONG_PRESS_MS) {
                fired = true;
                ESP_LOGW(TAG, "BOOT held for %d ms; entering AP-only WiFi reconfiguration mode",
                         MIMI_BOOT_BUTTON_LONG_PRESS_MS);
                wifi_manager_request_reconfigure();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0;
            fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(MIMI_BOOT_BUTTON_POLL_MS));
    }
}

esp_err_t boot_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << MIMI_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure BOOT GPIO%d: %s",
                 MIMI_BOOT_BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        boot_button_task, "boot_button",
        MIMI_BOOT_BUTTON_STACK, NULL,
        MIMI_BOOT_BUTTON_PRIO, NULL,
        MIMI_BOOT_BUTTON_CORE);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BOOT button task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BOOT GPIO%d long press (%d ms) armed",
             MIMI_BOOT_BUTTON_GPIO, MIMI_BOOT_BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}
