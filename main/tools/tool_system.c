#include "tools/tool_system.h"
#include "wifi/wifi_manager.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "tool_system";

static void i64_to_str(int64_t value, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    char tmp[24];
    size_t pos = 0;
    bool negative = value < 0;
    uint64_t n = negative ? (uint64_t)(-(value + 1)) + 1ULL : (uint64_t)value;

    do {
        tmp[pos++] = (char)('0' + (n % 10));
        n /= 10;
    } while (n > 0 && pos < sizeof(tmp));

    size_t off = 0;
    if (negative && off + 1 < out_size) {
        out[off++] = '-';
    }
    while (pos > 0 && off + 1 < out_size) {
        out[off++] = tmp[--pos];
    }
    out[off] = '\0';
}

static const char *reset_reason_to_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

static void appendf(char *out, size_t out_size, size_t *off, const char *fmt, ...)
{
    if (!out || !off || *off >= out_size) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, out_size - *off, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n >= out_size - *off) {
        *off = out_size - 1;
        out[*off] = '\0';
        return;
    }

    *off += (size_t)n;
}

esp_err_t tool_system_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    size_t spiffs_total = 0;
    size_t spiffs_used = 0;
    esp_err_t spiffs_err = esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);

    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    char uptime_str[24];
    i64_to_str(uptime_s, uptime_str, sizeof(uptime_str));
    esp_reset_reason_t reset_reason = esp_reset_reason();

    size_t off = 0;
    appendf(output, output_size, &off, "System status\n");
    appendf(output, output_size, &off, "- uptime_s: %s\n", uptime_str);
    appendf(output, output_size, &off, "- reset_reason: %s\n", reset_reason_to_str(reset_reason));
    appendf(output, output_size, &off, "- app: %s %s\n", app->project_name, app->version);
    appendf(output, output_size, &off, "- build: %s %s\n", app->date, app->time);
    appendf(output, output_size, &off, "- idf: %s\n", app->idf_ver);
    appendf(output, output_size, &off, "- chip_cores: %u\n", (unsigned)chip.cores);
    appendf(output, output_size, &off, "- chip_revision: %u\n", (unsigned)chip.revision);
    appendf(output, output_size, &off, "- flash_bytes: %" PRIu32 "\n", flash_size);
    appendf(output, output_size, &off, "- psram_bytes: %u\n",
            (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    appendf(output, output_size, &off, "- wifi_connected: %s\n",
            wifi_manager_is_connected() ? "yes" : "no");
    appendf(output, output_size, &off, "- wifi_ip: %s\n", wifi_manager_get_ip());
    appendf(output, output_size, &off, "- heap_total_free: %u\n",
            (unsigned)esp_get_free_heap_size());
    appendf(output, output_size, &off, "- internal_heap_free: %u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    appendf(output, output_size, &off, "- internal_heap_min_free: %u\n",
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    appendf(output, output_size, &off, "- internal_heap_largest_block: %u\n",
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    appendf(output, output_size, &off, "- psram_heap_free: %u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    appendf(output, output_size, &off, "- psram_heap_min_free: %u\n",
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    appendf(output, output_size, &off, "- psram_heap_largest_block: %u\n",
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (spiffs_err == ESP_OK) {
        appendf(output, output_size, &off, "- spiffs_total: %u\n", (unsigned)spiffs_total);
        appendf(output, output_size, &off, "- spiffs_used: %u\n", (unsigned)spiffs_used);
        appendf(output, output_size, &off, "- spiffs_free: %u\n",
                (unsigned)(spiffs_total > spiffs_used ? spiffs_total - spiffs_used : 0));
    } else {
        appendf(output, output_size, &off, "- spiffs: unavailable (%s)\n",
                esp_err_to_name(spiffs_err));
    }

    ESP_LOGI(TAG, "system_status generated");
    output[output_size - 1] = '\0';
    return ESP_OK;
}
