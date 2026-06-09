#include "board/status_led.h"

#include "mimi_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

#if MIMI_ENABLE_STATUS_LED && (MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_WS2812)
#include "led_strip.h"
#elif MIMI_ENABLE_STATUS_LED && (MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_GPIO)
#include "driver/gpio.h"
#endif

static const char *TAG = "status_led";

static bool s_initialized = false;
static volatile status_led_mode_t s_mode = STATUS_LED_MODE_OFF;
static TaskHandle_t s_task = NULL;

#if MIMI_ENABLE_STATUS_LED && (MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_WS2812)
static led_strip_handle_t s_strip = NULL;

static esp_err_t status_led_write_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    if (red == 0 && green == 0 && blue == 0) {
        return led_strip_clear(s_strip);
    }

    esp_err_t err = led_strip_set_pixel(s_strip, 0, red, green, blue);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    return err;
}
#endif

#if MIMI_ENABLE_STATUS_LED
static esp_err_t status_led_write_lit(bool lit)
{
#if MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_WS2812
    return status_led_write_rgb(lit ? 8 : 0, lit ? 4 : 0, 0);
#elif MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_GPIO
    const int on_level = MIMI_STATUS_LED_ACTIVE_LOW ? 0 : 1;
    const int off_level = MIMI_STATUS_LED_ACTIVE_LOW ? 1 : 0;
    return gpio_set_level(MIMI_STATUS_LED_GPIO, lit ? on_level : off_level);
#else
    (void)lit;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void status_led_task(void *arg)
{
    (void)arg;
    bool lit = false;
    status_led_mode_t last_mode = STATUS_LED_MODE_OFF;

    while (1) {
        status_led_mode_t mode = s_mode;

        if (mode == STATUS_LED_MODE_ONBOARDING) {
            lit = !lit;
            status_led_write_lit(lit);
            last_mode = mode;
            vTaskDelay(pdMS_TO_TICKS(MIMI_STATUS_LED_BLINK_MS));
        } else {
            if (last_mode != STATUS_LED_MODE_OFF || lit) {
                lit = false;
                status_led_write_lit(false);
            }
            last_mode = STATUS_LED_MODE_OFF;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static esp_err_t status_led_start_task(const char *driver_name)
{
    s_initialized = true;
    status_led_write_lit(false);

    BaseType_t ok = xTaskCreatePinnedToCore(
        status_led_task, "status_led",
        MIMI_STATUS_LED_STACK, NULL,
        MIMI_STATUS_LED_PRIO, &s_task,
        MIMI_STATUS_LED_CORE);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create status LED task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status LED GPIO%d initialized (%s)",
             MIMI_STATUS_LED_GPIO, driver_name);
    return ESP_OK;
}
#endif

esp_err_t status_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

#if !MIMI_ENABLE_STATUS_LED
    s_initialized = true;
    return ESP_OK;
#elif MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_WS2812
    led_strip_config_t strip_config = {
        .strip_gpio_num = MIMI_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = MIMI_STATUS_LED_RMT_RES_HZ,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LED strip on GPIO%d: %s",
                 MIMI_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    return status_led_start_task("led_strip ws2812");
#elif MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << MIMI_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure LED GPIO%d: %s",
                 MIMI_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    return status_led_start_task(MIMI_STATUS_LED_ACTIVE_LOW ?
                                 "gpio active-low" : "gpio active-high");
#else
    ESP_LOGW(TAG, "Unsupported status LED type %d", MIMI_STATUS_LED_TYPE);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void status_led_set_mode(status_led_mode_t mode)
{
    s_mode = mode;
}
