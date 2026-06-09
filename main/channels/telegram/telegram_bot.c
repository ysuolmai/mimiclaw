#include "telegram_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "heartbeat/heartbeat.h"
#include "tools/tool_registry.h"
#include "tools/tool_system.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "telegram";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;
static int64_t s_last_saved_offset = -1;
static int64_t s_last_offset_save_us = 0;

#define TG_OFFSET_NVS_KEY            "update_offset"
#define TG_DEDUP_CACHE_SIZE          64
#define TG_OFFSET_SAVE_INTERVAL_US   (5LL * 1000 * 1000)
#define TG_OFFSET_SAVE_STEP          10

static uint64_t s_seen_msg_keys[TG_DEDUP_CACHE_SIZE] = {0};
static size_t s_seen_msg_idx = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s) {
        return h;
    }
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t make_msg_key(const char *chat_id, int msg_id)
{
    uint64_t h = fnv1a64(chat_id);
    return (h << 16) ^ (uint64_t)(msg_id & 0xFFFF) ^ ((uint64_t)msg_id << 32);
}

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

static bool seen_msg_contains(uint64_t key)
{
    for (size_t i = 0; i < TG_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_msg_keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void seen_msg_insert(uint64_t key)
{
    s_seen_msg_keys[s_seen_msg_idx] = key;
    s_seen_msg_idx = (s_seen_msg_idx + 1) % TG_DEDUP_CACHE_SIZE;
}

static void save_update_offset_if_needed(bool force)
{
    if (s_update_offset <= 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    bool should_save = force;
    if (!should_save && s_last_saved_offset >= 0) {
        if ((s_update_offset - s_last_saved_offset) >= TG_OFFSET_SAVE_STEP) {
            should_save = true;
        } else if ((now - s_last_offset_save_us) >= TG_OFFSET_SAVE_INTERVAL_US) {
            should_save = true;
        }
    } else if (!should_save) {
        should_save = true;
    }

    if (!should_save) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    if (nvs_set_i64(nvs, TG_OFFSET_NVS_KEY, s_update_offset) == ESP_OK) {
        if (nvs_commit(nvs) == ESP_OK) {
            s_last_saved_offset = s_update_offset;
            s_last_offset_save_us = now;
        }
    }
    nvs_close(nvs);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *tg_api_call_via_proxy(const char *path, const char *post_data)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443,
                                          (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
    if (!conn) return NULL;

    /* Build HTTP request */
    char header[512];
    int hlen;
    if (post_data) {
        hlen = snprintf(header, sizeof(header),
            "POST /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path, (int)strlen(post_data));
    } else {
        hlen = snprintf(header, sizeof(header),
            "GET /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path);
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }
    if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }

    /* Read response — accumulate until connection close */
    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf) { proxy_conn_close(conn); return NULL; }

    int timeout = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000;
    while (1) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    /* Skip HTTP headers — find \r\n\r\n */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return NULL; }
    body += 4;

    /* Return just the body */
    char *result = strdup(body);
    free(buf);
    return result;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static char *tg_api_call_direct(const char *method, const char *post_data)
{
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token, method);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }

    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

static char *tg_api_call(const char *method, const char *post_data)
{
    if (http_proxy_is_enabled()) {
        return tg_api_call_via_proxy(method, post_data);
    }
    return tg_api_call_direct(method, post_data);
}

static bool tg_response_is_ok(const char *resp, char *out_desc, size_t out_desc_size)
{
    if (out_desc && out_desc_size > 0) {
        out_desc[0] = '\0';
    }
    if (!resp) {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
        bool ok = cJSON_IsTrue(ok_field);
        if (!ok && out_desc && out_desc_size > 0) {
            cJSON *desc = cJSON_GetObjectItem(root, "description");
            if (desc && cJSON_IsString(desc)) {
                strlcpy(out_desc, desc->valuestring, out_desc_size);
            }
        }
        cJSON_Delete(root);
        return ok;
    }

    /* Proxy or gateway can occasionally return non-standard payload framing. */
    if (strstr(resp, "\"ok\":true") != NULL) {
        return true;
    }

    return false;
}

static bool telegram_command_matches(const char *text, const char *cmd, const char **out_args)
{
    if (!text || !cmd || text[0] != '/') {
        return false;
    }

    const char *end = text;
    while (*end && !isspace((unsigned char)*end)) {
        end++;
    }

    size_t token_len = (size_t)(end - text);
    size_t cmd_len = strlen(cmd);
    if (token_len < cmd_len || strncmp(text, cmd, cmd_len) != 0) {
        return false;
    }
    if (token_len != cmd_len && text[cmd_len] != '@') {
        return false;
    }

    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (out_args) {
        *out_args = end;
    }
    return true;
}

static const char *telegram_read_token(const char *text, char *out, size_t out_size)
{
    if (!text || !out || out_size == 0) {
        return text;
    }

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    size_t n = 0;
    while (*text && !isspace((unsigned char)*text)) {
        if (n + 1 < out_size) {
            out[n++] = *text;
        }
        text++;
    }
    out[n] = '\0';

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static void telegram_send_tool_result(const char *chat_id, const char *tool_name,
                                      const char *input_json, size_t output_size)
{
    char *output = calloc(1, output_size);
    if (!output) {
        telegram_send_message(chat_id, "Out of memory while running command.");
        return;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json ? input_json : "{}", output, output_size);
    if (err != ESP_OK && output[0] == '\0') {
        snprintf(output, output_size, "%s failed: %s", tool_name, esp_err_to_name(err));
    }

    telegram_send_message(chat_id, output[0] ? output : "(empty result)");
    free(output);
}

static char *json_with_string_field(const char *key, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, key, value ? value : "");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_tail_request(const char *path, int max_bytes)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "path", path);
    if (max_bytes > 0) {
        cJSON_AddNumberToObject(obj, "max_bytes", max_bytes);
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_memory_search_request(const char *query, int max_results)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "query", query ? query : "");
    cJSON_AddNumberToObject(obj, "max_results", max_results > 0 ? max_results : 12);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_memory_summarize_request(int days)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "days", days > 0 ? days : 7);
    cJSON_AddBoolToObject(obj, "include_sessions", true);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_memory_export_request(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddBoolToObject(obj, "include_sessions", true);
    cJSON_AddNumberToObject(obj, "max_bytes", 24576);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_session_cleanup_request(int days, bool dry_run)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "older_than_days", days >= 0 ? days : 30);
    cJSON_AddBoolToObject(obj, "dry_run", dry_run);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static void telegram_send_help(const char *chat_id)
{
    telegram_send_message(chat_id,
        "MimiClaw commands:\n"
        "/help - show this help\n"
        "/id - show this chat id\n"
        "/status - show device status\n"
        "/tasks - list scheduled tasks\n"
        "/remove_task <id> - remove a scheduled task\n"
        "/memory - show long-term memory\n"
        "/summary - show memory summary\n"
        "/today - show today's daily memory\n"
        "/remember <note> - append a daily memory note\n"
        "/search_memory <query> - search memory and sessions\n"
        "/summarize_memory [days] - refresh memory summary\n"
        "/export_memory - export memory backup\n"
        "/cleanup_sessions [days] [apply] - preview/apply old session cleanup\n"
        "/files [prefix] - list SPIFFS files\n"
        "/tail <path> [bytes] - show the end of a SPIFFS file\n"
        "/logs [path] [bytes] - tail a SPIFFS log file\n"
        "/heartbeat - run heartbeat check now\n"
        "/forget - clear this chat session\n\n"
        "Send any normal message to chat with the AI agent.");
}

static bool handle_builtin_command(const char *chat_id, const char *text)
{
    const char *args = NULL;

    if (telegram_command_matches(text, "/start", &args) ||
        telegram_command_matches(text, "/help", &args)) {
        telegram_send_help(chat_id);
        return true;
    }

    if (telegram_command_matches(text, "/id", &args)) {
        char reply[96];
        snprintf(reply, sizeof(reply), "chat_id: %s", chat_id);
        telegram_send_message(chat_id, reply);
        return true;
    }

    if (telegram_command_matches(text, "/status", &args)) {
        char *status = calloc(1, 2048);
        if (!status) {
            telegram_send_message(chat_id, "Out of memory while building status.");
            return true;
        }

        esp_err_t err = tool_system_status_execute("{}", status, 2048);
        if (err != ESP_OK && status[0] == '\0') {
            snprintf(status, 2048, "system_status failed: %s", esp_err_to_name(err));
        }
        telegram_send_message(chat_id, status[0] ? status : "(empty status)");
        free(status);
        return true;
    }

    if (telegram_command_matches(text, "/tasks", &args)) {
        telegram_send_tool_result(chat_id, "cron_list", "{}", 4096);
        return true;
    }

    if (telegram_command_matches(text, "/remove_task", &args)) {
        char job_id[16];
        telegram_read_token(args, job_id, sizeof(job_id));
        if (job_id[0] == '\0') {
            telegram_send_message(chat_id, "Usage: /remove_task <job_id>");
            return true;
        }

        char *json = json_with_string_field("job_id", job_id);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "cron_remove", json, 512);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/memory", &args)) {
        char *mem = calloc(1, 4096);
        if (!mem) {
            telegram_send_message(chat_id, "Out of memory while reading MEMORY.md.");
            return true;
        }

        if (memory_read_long_term(mem, 4096) == ESP_OK && mem[0]) {
            size_t reply_size = strlen(mem) + 32;
            char *reply = calloc(1, reply_size);
            if (reply) {
                snprintf(reply, reply_size, "MEMORY.md\n\n%s", mem);
                telegram_send_message(chat_id, reply);
                free(reply);
            } else {
                telegram_send_message(chat_id, mem);
            }
        } else {
            telegram_send_message(chat_id, "MEMORY.md is empty or not found.");
        }

        free(mem);
        return true;
    }

    if (telegram_command_matches(text, "/summary", &args)) {
        char *json = json_with_string_field("path", MIMI_MEMORY_SUMMARY_FILE);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "read_file", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/today", &args)) {
        char *recent = calloc(1, 4096);
        if (!recent) {
            telegram_send_message(chat_id, "Out of memory while reading today's memory.");
            return true;
        }

        if (memory_read_recent(recent, 4096, 1) == ESP_OK && recent[0]) {
            telegram_send_message(chat_id, recent);
        } else {
            telegram_send_message(chat_id, "No daily memory found for today.");
        }

        free(recent);
        return true;
    }

    if (telegram_command_matches(text, "/remember", &args)) {
        if (!args || args[0] == '\0') {
            telegram_send_message(chat_id, "Usage: /remember <note>");
            return true;
        }

        esp_err_t err = memory_append_today(args);
        if (err == ESP_OK) {
            telegram_send_message(chat_id, "Saved to today's daily memory.");
        } else {
            char reply[96];
            snprintf(reply, sizeof(reply), "Failed to save memory: %s", esp_err_to_name(err));
            telegram_send_message(chat_id, reply);
        }
        return true;
    }

    if (telegram_command_matches(text, "/search_memory", &args)) {
        if (!args || args[0] == '\0') {
            telegram_send_message(chat_id, "Usage: /search_memory <query>");
            return true;
        }

        char *json = json_memory_search_request(args, 12);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "memory_search", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/summarize_memory", &args)) {
        int days = 7;
        if (args && args[0]) {
            int requested = atoi(args);
            if (requested > 0) {
                days = requested;
            }
        }

        char *json = json_memory_summarize_request(days);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "memory_summarize", json, 1024);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/export_memory", &args)) {
        char *json = json_memory_export_request();
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "memory_export", json, 1024);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/cleanup_sessions", &args)) {
        char days_arg[16];
        const char *rest = telegram_read_token(args, days_arg, sizeof(days_arg));
        int days = 30;
        if (days_arg[0]) {
            int requested = atoi(days_arg);
            if (requested >= 0) {
                days = requested;
            }
        }
        bool dry_run = true;
        if (rest && strncmp(rest, "apply", 5) == 0) {
            dry_run = false;
        }

        char *json = json_session_cleanup_request(days, dry_run);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "session_cleanup", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/files", &args)) {
        if (!args || args[0] == '\0') {
            telegram_send_tool_result(chat_id, "list_dir", "{}", 4096);
            return true;
        }

        char *json = json_with_string_field("prefix", args);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "list_dir", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/tail", &args)) {
        char path[160];
        const char *rest = telegram_read_token(args, path, sizeof(path));
        if (path[0] == '\0') {
            telegram_send_message(chat_id, "Usage: /tail <path> [bytes]");
            return true;
        }

        int max_bytes = 2048;
        if (rest && rest[0]) {
            int requested = atoi(rest);
            if (requested > 0) {
                max_bytes = requested;
            }
        }

        char *json = json_tail_request(path, max_bytes);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "tail_file", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/logs", &args)) {
        char path[160];
        const char *rest = telegram_read_token(args, path, sizeof(path));
        if (path[0] == '\0') {
            strlcpy(path, MIMI_SPIFFS_BASE "/logs/mimi.log", sizeof(path));
        }

        int max_bytes = 2048;
        if (rest && rest[0]) {
            int requested = atoi(rest);
            if (requested > 0) {
                max_bytes = requested;
            }
        }

        char *json = json_tail_request(path, max_bytes);
        if (!json) {
            telegram_send_message(chat_id, "Out of memory while building request.");
            return true;
        }
        telegram_send_tool_result(chat_id, "tail_file", json, 4096);
        free(json);
        return true;
    }

    if (telegram_command_matches(text, "/heartbeat", &args)) {
        if (heartbeat_trigger()) {
            telegram_send_message(chat_id, "Heartbeat found actionable tasks and prompted the agent.");
        } else {
            telegram_send_message(chat_id, "Heartbeat found no actionable tasks.");
        }
        return true;
    }

    if (telegram_command_matches(text, "/forget", &args)) {
        esp_err_t err = session_clear(chat_id);
        if (err == ESP_OK) {
            telegram_send_message(chat_id, "This chat session was cleared.");
        } else {
            telegram_send_message(chat_id, "No saved session was found for this chat.");
        }
        return true;
    }

    return false;
}

static void process_updates(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        /* Track offset and skip stale/duplicate updates */
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        int64_t uid = -1;
        if (cJSON_IsNumber(update_id)) {
            uid = (int64_t)update_id->valuedouble;
        }
        if (uid >= 0) {
            if (uid < s_update_offset) {
                continue;
            }
            s_update_offset = uid + 1;
            save_update_offset_if_needed(false);
        }

        /* Extract message */
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (!text || !cJSON_IsString(text)) continue;

        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        if (!chat) continue;

        cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
        if (!chat_id) continue;

        int msg_id_val = -1;
        cJSON *message_id = cJSON_GetObjectItem(message, "message_id");
        if (cJSON_IsNumber(message_id)) {
            msg_id_val = (int)message_id->valuedouble;
        }

        char chat_id_str[32];
        if (cJSON_IsString(chat_id) && chat_id->valuestring) {
            strncpy(chat_id_str, chat_id->valuestring, sizeof(chat_id_str) - 1);
            chat_id_str[sizeof(chat_id_str) - 1] = '\0';
        } else if (cJSON_IsNumber(chat_id)) {
            snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);
        } else {
            continue;
        }

        if (msg_id_val >= 0) {
            uint64_t msg_key = make_msg_key(chat_id_str, msg_id_val);
            if (seen_msg_contains(msg_key)) {
                char uid_str[24];
                i64_to_str(uid, uid_str, sizeof(uid_str));
                ESP_LOGW(TAG, "Drop duplicate message update_id=%s chat=%s message_id=%d",
                         uid_str, chat_id_str, msg_id_val);
                continue;
            }
            seen_msg_insert(msg_key);
        }

        char uid_str[24];
        i64_to_str(uid, uid_str, sizeof(uid_str));
        ESP_LOGI(TAG, "Message update_id=%s message_id=%d from chat %s: %.40s...",
                 uid_str, msg_id_val, chat_id_str, text->valuestring);

        if (handle_builtin_command(chat_id_str, text->valuestring)) {
            continue;
        }

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);
        msg.content = strdup(text->valuestring);
        if (msg.content) {
            if (message_bus_push_inbound(&msg) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop telegram message");
                free(msg.content);
            }
        }
    }

    cJSON_Delete(root);
}

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram polling task started");

    while (1) {
        if (s_bot_token[0] == '\0') {
            ESP_LOGW(TAG, "No bot token configured, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char offset_str[24];
        i64_to_str(s_update_offset, offset_str, sizeof(offset_str));
        char params[128];
        snprintf(params, sizeof(params),
                 "getUpdates?offset=%s&timeout=%d",
                 offset_str, MIMI_TG_POLL_TIMEOUT_S);

        char *resp = tg_api_call(params, NULL);
        if (resp) {
            process_updates(resp);
            free(resp);
        } else {
            /* Back off on error */
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

/* --- Public API --- */

esp_err_t telegram_bot_init(void)
{
    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
        }

        int64_t offset = 0;
        if (nvs_get_i64(nvs, TG_OFFSET_NVS_KEY, &offset) == ESP_OK && offset > 0) {
            s_update_offset = offset;
            s_last_saved_offset = offset;
            char offset_str[24];
            i64_to_str(s_update_offset, offset_str, sizeof(offset_str));
            ESP_LOGI(TAG, "Loaded Telegram update offset: %s", offset_str);
        }
        nvs_close(nvs);
    }

    /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

    if (s_bot_token[0]) {
        ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)", (int)strlen(s_bot_token));
    } else {
        ESP_LOGW(TAG, "No Telegram bot token. Use CLI: set_tg_token <TOKEN>");
    }
    return ESP_OK;
}

esp_err_t telegram_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        telegram_poll_task, "tg_poll",
        MIMI_TG_POLL_STACK, NULL,
        MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text)
{
    if (s_bot_token[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no bot token");
        return ESP_ERR_INVALID_STATE;
    }

    /* Split long messages at 4096-char boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_TG_MAX_MSG_LEN) {
            chunk = MIMI_TG_MAX_MSG_LEN;
        }

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);

        /* Create null-terminated chunk */
        char *segment = malloc(chunk + 1);
        if (!segment) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON_AddStringToObject(body, "text", segment);
        cJSON_AddStringToObject(body, "parse_mode", "Markdown");

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);

        if (!json_str) {
            all_ok = 0;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending telegram chunk to %s (%d bytes)", chat_id, (int)chunk);
        char *resp = tg_api_call("sendMessage", json_str);
        free(json_str);

        int sent_ok = 0;
        bool markdown_failed = false;
        if (resp) {
            char desc[160];
            sent_ok = tg_response_is_ok(resp, desc, sizeof(desc));
            if (!sent_ok) {
                markdown_failed = true;
                ESP_LOGI(TAG, "Markdown rejected by Telegram for %s: %s",
                         chat_id, desc[0] ? desc : "unknown");
            }
        }

        if (!sent_ok) {
            /* Retry without parse_mode */
            cJSON *body2 = cJSON_CreateObject();
            cJSON_AddStringToObject(body2, "chat_id", chat_id);
            char *seg2 = malloc(chunk + 1);
            if (seg2) {
                memcpy(seg2, text + offset, chunk);
                seg2[chunk] = '\0';
                cJSON_AddStringToObject(body2, "text", seg2);
                free(seg2);
            }
            char *json2 = cJSON_PrintUnformatted(body2);
            cJSON_Delete(body2);
            if (json2) {
                char *resp2 = tg_api_call("sendMessage", json2);
                free(json2);
                if (resp2) {
                    char desc2[160];
                    sent_ok = tg_response_is_ok(resp2, desc2, sizeof(desc2));
                    if (!sent_ok) {
                        ESP_LOGE(TAG, "Plain send failed: %s", desc2[0] ? desc2 : "unknown");
                        ESP_LOGE(TAG, "Telegram raw response: %.300s", resp2);
                    }
                    free(resp2);
                } else {
                    ESP_LOGE(TAG, "Plain send failed: no HTTP response");
                }
            } else {
                ESP_LOGE(TAG, "Plain send failed: no JSON body");
            }
        }

        if (!sent_ok) {
            all_ok = 0;
        } else {
            if (markdown_failed) {
                ESP_LOGI(TAG, "Plain-text fallback succeeded for %s", chat_id);
            }
            ESP_LOGI(TAG, "Telegram send success to %s (%d bytes)", chat_id, (int)chunk);
        }

        free(resp);
        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_set_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_TG_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    ESP_LOGI(TAG, "Telegram bot token saved");
    return ESP_OK;
}
