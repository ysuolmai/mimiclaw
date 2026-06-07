#include "tools/tool_memory.h"
#include "memory/memory_store.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_memory";

esp_err_t tool_memory_append_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *note = cJSON_GetStringValue(cJSON_GetObjectItem(root, "note"));
    if (!note || note[0] == '\0') {
        snprintf(output, output_size, "Error: missing 'note' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = memory_append_today(note);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to append memory note: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: appended %d bytes to today's daily memory", (int)strlen(note));
    ESP_LOGI(TAG, "memory_append: %d bytes", (int)strlen(note));
    cJSON_Delete(root);
    return ESP_OK;
}
