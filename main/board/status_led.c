#include "board/status_led.h"

#include "mimi_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

#if MIMI_ENABLE_STATUS_LED
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#endif

static const char *TAG = "status_led";

static bool s_initialized = false;
static volatile status_led_mode_t s_mode = STATUS_LED_MODE_OFF;
static TaskHandle_t s_task = NULL;

#if MIMI_ENABLE_STATUS_LED
static rmt_channel_handle_t s_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

static esp_err_t status_led_write_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_channel || !s_encoder) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t grb[3] = { green, red, blue };
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    esp_err_t err = rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_config);
    if (err != ESP_OK) {
        return err;
    }

    err = rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(1));
    return err;
}

static void status_led_task(void *arg)
{
    (void)arg;
    bool lit = false;

    while (1) {
        status_led_mode_t mode = s_mode;

        if (mode == STATUS_LED_MODE_ONBOARDING) {
            lit = !lit;
            if (lit) {
                status_led_write_rgb(8, 4, 0);
            } else {
                status_led_write_rgb(0, 0, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(MIMI_STATUS_LED_BLINK_MS));
        } else {
            lit = false;
            status_led_write_rgb(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
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
#else
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = MIMI_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = MIMI_STATUS_LED_RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LED RMT channel on GPIO%d: %s",
                 MIMI_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bytes_encoder_config, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LED RMT encoder: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable LED RMT channel: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    status_led_write_rgb(0, 0, 0);

    BaseType_t ok = xTaskCreatePinnedToCore(
        status_led_task, "status_led",
        MIMI_STATUS_LED_STACK, NULL,
        MIMI_STATUS_LED_PRIO, &s_task,
        MIMI_STATUS_LED_CORE);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to create status LED task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status LED GPIO%d initialized", MIMI_STATUS_LED_GPIO);
    return ESP_OK;
#endif
}

void status_led_set_mode(status_led_mode_t mode)
{
    s_mode = mode;

#if MIMI_ENABLE_STATUS_LED
    if (s_initialized && mode == STATUS_LED_MODE_OFF) {
        status_led_write_rgb(0, 0, 0);
    }
#endif
}
