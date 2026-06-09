#include "wifi_onboard.h"
#include "onboard_html.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "heartbeat/heartbeat.h"
#include "memory/memory_store.h"
#include "tools/tool_registry.h"

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "onboard";
static httpd_handle_t s_server = NULL;
static bool s_captive_mode = false;

#define ADMIN_USER_MAX_LEN 32
#define ADMIN_PASS_MAX_LEN 64
#define ADMIN_BASIC_MAX_LEN 192

static bool nvs_get_string(const char *ns, const char *key, char *out, size_t out_size)
{
    if (!ns || !key || !out || out_size == 0) return false;
    out[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t len = out_size;
    bool found = (nvs_get_str(nvs, key, out, &len) == ESP_OK && out[0] != '\0');
    nvs_close(nvs);
    return found;
}

static void admin_fallback_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_size, "mimiclaw-%02X%02X", mac[4], mac[5]);
}

static void admin_get_user(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (nvs_get_string(MIMI_NVS_ADMIN, MIMI_NVS_KEY_ADMIN_USER, out, out_size)) {
        return;
    }
    if (MIMI_SECRET_ADMIN_USER[0] != '\0') {
        strlcpy(out, MIMI_SECRET_ADMIN_USER, out_size);
        return;
    }
    strlcpy(out, "admin", out_size);
}

static bool admin_password_is_custom(void)
{
    char value[ADMIN_PASS_MAX_LEN] = {0};
    return nvs_get_string(MIMI_NVS_ADMIN, MIMI_NVS_KEY_ADMIN_PASS, value, sizeof(value)) ||
           MIMI_SECRET_ADMIN_PASS[0] != '\0';
}

static void admin_get_password(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (nvs_get_string(MIMI_NVS_ADMIN, MIMI_NVS_KEY_ADMIN_PASS, out, out_size)) {
        return;
    }
    if (MIMI_SECRET_ADMIN_PASS[0] != '\0') {
        strlcpy(out, MIMI_SECRET_ADMIN_PASS, out_size);
        return;
    }
    admin_fallback_password(out, out_size);
}

static bool base64_encode_basic(const uint8_t *input, size_t input_len,
                                char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((input_len + 2) / 3) * 4 + 1;
    if (!input || !out || out_size < needed) {
        if (out && out_size) out[0] = '\0';
        return false;
    }

    size_t i = 0;
    size_t o = 0;
    while (i < input_len) {
        size_t remaining = input_len - i;
        uint32_t octet_a = input[i++];
        uint32_t octet_b = (remaining > 1) ? input[i++] : 0;
        uint32_t octet_c = (remaining > 2) ? input[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[o++] = alphabet[(triple >> 18) & 0x3F];
        out[o++] = alphabet[(triple >> 12) & 0x3F];
        out[o++] = (remaining > 1) ? alphabet[(triple >> 6) & 0x3F] : '=';
        out[o++] = (remaining > 2) ? alphabet[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    return true;
}

static esp_err_t http_send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"MimiClaw Admin\", charset=\"UTF-8\"");
    return httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
}

static bool http_admin_authorized(httpd_req_t *req)
{
    if (s_captive_mode) {
        return true;
    }

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len == 0 || auth_len >= ADMIN_BASIC_MAX_LEN) {
        http_send_auth_required(req);
        return false;
    }

    char auth[ADMIN_BASIC_MAX_LEN] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strncmp(auth, "Basic ", 6) != 0) {
        http_send_auth_required(req);
        return false;
    }

    char user[ADMIN_USER_MAX_LEN] = {0};
    char pass[ADMIN_PASS_MAX_LEN] = {0};
    char pair[ADMIN_USER_MAX_LEN + ADMIN_PASS_MAX_LEN + 2] = {0};
    char encoded[ADMIN_BASIC_MAX_LEN] = {0};
    admin_get_user(user, sizeof(user));
    admin_get_password(pass, sizeof(pass));
    snprintf(pair, sizeof(pair), "%s:%s", user, pass);

    if (!base64_encode_basic((const uint8_t *)pair, strlen(pair), encoded, sizeof(encoded)) ||
        strcmp(auth + 6, encoded) != 0) {
        http_send_auth_required(req);
        return false;
    }

    return true;
}

static void admin_log_auth_hint(void)
{
    char user[ADMIN_USER_MAX_LEN] = {0};
    admin_get_user(user, sizeof(user));
    if (admin_password_is_custom()) {
        ESP_LOGI(TAG, "STA Web Admin auth enabled: username=%s, password=<configured>", user);
    } else {
        char pass[ADMIN_PASS_MAX_LEN] = {0};
        admin_get_password(pass, sizeof(pass));
        ESP_LOGW(TAG, "STA Web Admin auth enabled: username=%s, generated password=%s", user, pass);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_in_place(char *s)
{
    if (!s) return;

    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool http_get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    if (!req || !key || !out || out_size == 0) return false;
    out[0] = '\0';

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) return false;

    char *query = calloc(1, query_len + 1);
    if (!query) return false;

    bool found = false;
    if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK &&
        httpd_query_key_value(query, key, out, out_size) == ESP_OK) {
        url_decode_in_place(out);
        found = true;
    }

    free(query);
    return found;
}

static char *http_read_body(httpd_req_t *req, int max_len)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > max_len) {
        return NULL;
    }

    char *buf = calloc(1, total_len + 1);
    if (!buf) {
        return NULL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        received += ret;
    }

    buf[total_len] = '\0';
    return buf;
}

static esp_err_t http_send_json_object(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t http_send_text_result(httpd_req_t *req, bool ok, const char *result)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "result", result ? result : "");
    esp_err_t ret = http_send_json_object(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t http_send_tool_result(httpd_req_t *req, const char *tool_name,
                                       const char *input_json, size_t output_size)
{
    char *output = calloc(1, output_size);
    if (!output) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json ? input_json : "{}", output, output_size);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(output);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "result", output);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }

    esp_err_t ret = http_send_json_object(req, root);
    cJSON_Delete(root);
    free(output);
    return ret;
}

static char *json_with_string_field(const char *key, const char *value)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, key, value ? value : "");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static char *json_tail_request(const char *path, int max_bytes)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "path", path ? path : "");
    cJSON_AddNumberToObject(obj, "max_bytes", max_bytes > 0 ? max_bytes : 4096);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static void read_text_file_or_empty(const char *path, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!path) {
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void json_add_effective_config(cJSON *root, const char *json_key,
                                      const char *ns, const char *nvs_key,
                                      const char *build_val)
{
    char value[256] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(value);
        if (nvs_get_str(nvs, nvs_key, value, &len) == ESP_OK) {
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

static void json_add_effective_config_u16(cJSON *root, const char *json_key,
                                          const char *ns, const char *nvs_key,
                                          const char *build_val)
{
    char value[16] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t port = 0;
        if (nvs_get_u16(nvs, nvs_key, &port) == ESP_OK && port > 0) {
            snprintf(value, sizeof(value), "%u", (unsigned)port);
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

/* ── DNS hijack ─────────────────────────────────────────────────── */

/* Minimal DNS response: always answer 192.168.4.1 */
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket error");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack listening on :53");

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len;

    while (1) {
        client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* too short for DNS header */

        /* Build response: copy query, set response flags, append answer */
        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;

        memcpy(resp, buf, len);

        /* Set QR=1 (response), AA=1 (authoritative), RA=1 */
        resp[2] = 0x85;  /* QR=1, Opcode=0, AA=1, TC=0, RD=1 */
        resp[3] = 0x80;  /* RA=1, Z=0, RCODE=0 (no error) */

        /* Answer count = 1 */
        resp[6] = 0x00;
        resp[7] = 0x01;

        /* Append answer: pointer to name + A record with 192.168.4.1 */
        int off = len;
        resp[off++] = 0xC0;  /* pointer */
        resp[off++] = 0x0C;  /* offset to question name */
        resp[off++] = 0x00; resp[off++] = 0x01;  /* type A */
        resp[off++] = 0x00; resp[off++] = 0x01;  /* class IN */
        resp[off++] = 0x00; resp[off++] = 0x00;
        resp[off++] = 0x00; resp[off++] = 0x3C;  /* TTL = 60 */
        resp[off++] = 0x00; resp[off++] = 0x04;  /* data length = 4 */
        resp[off++] = 192; resp[off++] = 168;
        resp[off++] = 4;   resp[off++] = 1;

        sendto(sock, resp, off, 0,
               (struct sockaddr *)&client, client_len);
    }
}

/* ── HTTP handlers ──────────────────────────────────────────────── */

static esp_err_t http_get_root(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, ONBOARD_HTML, sizeof(ONBOARD_HTML) - 1);
}

/* Captive portal detection endpoints → redirect to root */
static esp_err_t http_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_get_scan(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MIMI_ONBOARD_MAX_SCAN) ap_count = MIMI_ONBOARD_MAX_SCAN;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    uint16_t ap_max = ap_count;
    esp_wifi_scan_get_ap_records(&ap_max, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_max; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;  /* skip hidden */
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(obj, "ch", ap_list[i].primary);
        cJSON_AddBoolToObject(obj, "auth", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, obj);
    }
    free(ap_list);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t http_get_config(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    json_add_effective_config(root, "ssid", MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID, MIMI_SECRET_WIFI_SSID);
    cJSON_AddStringToObject(root, "password", "");
    cJSON_AddStringToObject(root, "api_key", "");
    json_add_effective_config(root, "model", MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL);
    json_add_effective_config(root, "provider", MIMI_NVS_LLM, MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER);
    json_add_effective_config(root, "base_url", MIMI_NVS_LLM, MIMI_NVS_KEY_LLM_BASE_URL, MIMI_SECRET_LLM_BASE_URL);
    cJSON_AddStringToObject(root, "tg_token", "");
    json_add_effective_config(root, "feishu_app_id", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, MIMI_SECRET_FEISHU_APP_ID);
    cJSON_AddStringToObject(root, "feishu_app_secret", "");
    json_add_effective_config(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST);
    json_add_effective_config_u16(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT);
    json_add_effective_config(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE, MIMI_SECRET_PROXY_TYPE);
    cJSON_AddStringToObject(root, "search_key", "");
    cJSON_AddStringToObject(root, "tavily_key", "");
    json_add_effective_config(root, "voice_stream_url", MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_STREAM_URL, MIMI_SECRET_VOICE_STREAM_URL);
    json_add_effective_config(root, "voice_codec", MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_CODEC, MIMI_VOICE_STREAM_DEFAULT_CODEC);
    char admin_user[ADMIN_USER_MAX_LEN] = {0};
    admin_get_user(admin_user, sizeof(admin_user));
    cJSON_AddStringToObject(root, "admin_user", admin_user);
    cJSON_AddStringToObject(root, "admin_password", "");
    cJSON_AddBoolToObject(root, "admin_auth_required", !s_captive_mode);
    cJSON_AddBoolToObject(root, "admin_password_custom", admin_password_is_custom());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t http_get_api_status(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;
    return http_send_tool_result(req, "system_status", "{}", 2048);
}

static esp_err_t http_get_api_tasks(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;
    return http_send_tool_result(req, "cron_list", "{}", 4096);
}

static esp_err_t http_post_api_task_remove(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char *body = http_read_body(req, 512);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || job_id[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing job_id");
        return ESP_FAIL;
    }

    char *json = json_with_string_field("job_id", job_id);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t ret = http_send_tool_result(req, "cron_remove", json, 512);
    free(json);
    return ret;
}

static esp_err_t http_get_api_files(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char prefix[192];
    if (!http_get_query_value(req, "prefix", prefix, sizeof(prefix)) || prefix[0] == '\0') {
        return http_send_tool_result(req, "list_dir", "{}", 4096);
    }

    char *json = json_with_string_field("prefix", prefix);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t ret = http_send_tool_result(req, "list_dir", json, 4096);
    free(json);
    return ret;
}

static esp_err_t http_get_api_file(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char path[192];
    if (!http_get_query_value(req, "path", path, sizeof(path)) || path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char mode[16];
    bool tail = http_get_query_value(req, "mode", mode, sizeof(mode)) && strcmp(mode, "tail") == 0;
    char bytes_arg[16];
    int max_bytes = 4096;
    if (http_get_query_value(req, "bytes", bytes_arg, sizeof(bytes_arg)) && bytes_arg[0]) {
        int requested = atoi(bytes_arg);
        if (requested > 0) {
            max_bytes = requested;
        }
    }

    char *json = tail ? json_tail_request(path, max_bytes) : json_with_string_field("path", path);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    esp_err_t ret = http_send_tool_result(req, tail ? "tail_file" : "read_file", json, 8192);
    free(json);
    return ret;
}

static esp_err_t http_post_api_file(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char *body = http_read_body(req, 8192);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
    if (!path || !content) {
        cJSON_Delete(root);
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path or content");
        return ESP_FAIL;
    }

    const char *tool = (mode && strcmp(mode, "append") == 0) ? "append_file" : "write_file";
    esp_err_t ret = http_send_tool_result(req, tool, body, 512);
    cJSON_Delete(root);
    free(body);
    return ret;
}

static esp_err_t http_get_api_memory(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char *long_term = calloc(1, 4096);
    char *recent = calloc(1, 4096);
    char *summary = calloc(1, 4096);
    if (!long_term || !recent || !summary) {
        free(long_term);
        free(recent);
        free(summary);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    memory_read_long_term(long_term, 4096);
    memory_read_recent(recent, 4096, 3);
    read_text_file_or_empty(MIMI_MEMORY_SUMMARY_FILE, summary, 4096);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(long_term);
        free(recent);
        free(summary);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "long_term", long_term);
    cJSON_AddStringToObject(root, "recent", recent);
    cJSON_AddStringToObject(root, "summary", summary);
    esp_err_t ret = http_send_json_object(req, root);
    cJSON_Delete(root);
    free(long_term);
    free(recent);
    free(summary);
    return ret;
}

static esp_err_t http_post_api_memory(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char *body = http_read_body(req, 8192);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (action && action[0]) {
        const char *tool = NULL;
        size_t output_size = 2048;

        if (strcmp(action, "search") == 0) {
            tool = "memory_search";
            output_size = 4096;
        } else if (strcmp(action, "summarize") == 0) {
            tool = "memory_summarize";
        } else if (strcmp(action, "export") == 0) {
            tool = "memory_export";
        } else if (strcmp(action, "cleanup_sessions") == 0) {
            tool = "session_cleanup";
            output_size = 4096;
        } else if (strcmp(action, "tail_log") == 0) {
            tool = "tail_file";
            output_size = 4096;
        }

        if (!tool) {
            cJSON_Delete(root);
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown memory action");
            return ESP_FAIL;
        }

        esp_err_t ret = http_send_tool_result(req, tool, body, output_size);
        cJSON_Delete(root);
        free(body);
        return ret;
    }

    const char *note = cJSON_GetStringValue(cJSON_GetObjectItem(root, "note"));
    const char *long_term = cJSON_GetStringValue(cJSON_GetObjectItem(root, "long_term"));

    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *message = "No memory field provided";
    if (note && note[0]) {
        err = memory_append_today(note);
        message = (err == ESP_OK) ? "Appended note to daily memory" : "Failed to append daily memory";
    } else if (long_term) {
        err = memory_write_long_term(long_term);
        message = (err == ESP_OK) ? "Updated long-term memory" : "Failed to update long-term memory";
    }

    cJSON_Delete(root);
    free(body);
    return http_send_text_result(req, err == ESP_OK, message);
}

static esp_err_t http_post_api_voice(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    char *body = http_read_body(req, 1024);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    const char *tool = NULL;
    size_t output_size = 2048;

    if (action && strcmp(action, "status") == 0) {
        tool = "voice_status";
    } else if (action && strcmp(action, "beep") == 0) {
        tool = "voice_beep";
    } else if (action && strcmp(action, "record") == 0) {
        tool = "voice_record";
    } else if (action && strcmp(action, "play") == 0) {
        tool = "voice_play";
    } else if (action && strcmp(action, "stream_status") == 0) {
        tool = "voice_stream_status";
    } else if (action && strcmp(action, "stream_config") == 0) {
        tool = "voice_stream_config";
    } else if (action && strcmp(action, "stream_start") == 0) {
        tool = "voice_stream_start";
    } else if (action && strcmp(action, "stream_stop") == 0) {
        tool = "voice_stream_stop";
    }

    if (!tool) {
        cJSON_Delete(root);
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown voice action");
        return ESP_FAIL;
    }

    esp_err_t ret;
    if (!MIMI_ENABLE_VOICE_HW) {
        ret = http_send_text_result(req, false, "Hardware voice is disabled in this target profile");
    } else {
        ret = http_send_tool_result(req, tool, body, output_size);
    }

    cJSON_Delete(root);
    free(body);
    return ret;
}

static esp_err_t http_post_api_heartbeat(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;
    bool triggered = heartbeat_trigger();
    return http_send_text_result(req, true,
                                 triggered ? "Heartbeat prompted the agent" : "No actionable heartbeat tasks");
}

/*
 * Sync one JSON string field into NVS.
 * - missing field: leave current NVS value unchanged
 * - empty string: erase current NVS value
 * - non-empty string: save/update current NVS value
 */
static void nvs_sync_field(cJSON *root, const char *json_key,
                           const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            nvs_set_str(nvs, nvs_key, item->valuestring);
            ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/*
 * Sync a secret field submitted from Web Admin.
 * Empty secret inputs are treated as "keep existing" so a partially loaded page
 * cannot accidentally erase API keys or bot tokens.
 */
static void nvs_sync_secret_field(cJSON *root, const char *json_key,
                                  const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    if (item->valuestring[0] == '\0') {
        ESP_LOGI(TAG, "Kept %s/%s (empty secret field ignored)", ns, nvs_key);
        return;
    }

    nvs_sync_field(root, json_key, ns, nvs_key);
}

static void nvs_sync_limited_field(cJSON *root, const char *json_key,
                                   const char *ns, const char *nvs_key,
                                   size_t max_len, bool keep_empty)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    if (item->valuestring[0] == '\0') {
        if (keep_empty) {
            ESP_LOGI(TAG, "Kept %s/%s (empty field ignored)", ns, nvs_key);
            return;
        }
        nvs_sync_field(root, json_key, ns, nvs_key);
        return;
    }

    if (strlen(item->valuestring) >= max_len) {
        ESP_LOGW(TAG, "Ignoring %s: value is too long", json_key);
        return;
    }

    nvs_sync_field(root, json_key, ns, nvs_key);
}

static void nvs_sync_u16_field(cJSON *root, const char *json_key,
                               const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            char *end = NULL;
            unsigned long value = strtoul(item->valuestring, &end, 10);
            if (end == item->valuestring || *end != '\0' || value > UINT16_MAX) {
                ESP_LOGW(TAG, "Ignoring invalid %s value: %s", json_key, item->valuestring);
            } else {
                ESP_ERROR_CHECK(nvs_set_u16(nvs, nvs_key, (uint16_t)value));
                ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    if (!http_admin_authorized(req)) return ESP_OK;

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 3072) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_FAIL;
    }

    char *buf = calloc(1, total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
            return ESP_FAIL;
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* WiFi (required) */
    nvs_sync_field(root, "ssid",     MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID);
    nvs_sync_secret_field(root, "password", MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS);

    /* LLM */
    nvs_sync_secret_field(root, "api_key",  MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY);
    nvs_sync_field(root, "model",    MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL);
    nvs_sync_field(root, "provider", MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER);
    nvs_sync_field(root, "base_url", MIMI_NVS_LLM,    MIMI_NVS_KEY_LLM_BASE_URL);

    /* Telegram */
    nvs_sync_secret_field(root, "tg_token", MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN);

    /* Feishu */
    nvs_sync_field(root, "feishu_app_id",     MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID);
    nvs_sync_secret_field(root, "feishu_app_secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET);

    /* Proxy */
    nvs_sync_field(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST);
    nvs_sync_u16_field(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT);
    nvs_sync_field(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE);

    /* Search */
    nvs_sync_secret_field(root, "search_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY);
    nvs_sync_secret_field(root, "tavily_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY);

    /* Voice stream */
    nvs_sync_field(root, "voice_stream_url", MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_STREAM_URL);
    nvs_sync_field(root, "voice_codec", MIMI_NVS_VOICE, MIMI_NVS_KEY_VOICE_CODEC);

    /* Web Admin auth */
    nvs_sync_limited_field(root, "admin_user", MIMI_NVS_ADMIN, MIMI_NVS_KEY_ADMIN_USER,
                           ADMIN_USER_MAX_LEN, false);
    nvs_sync_limited_field(root, "admin_password", MIMI_NVS_ADMIN, MIMI_NVS_KEY_ADMIN_PASS,
                           ADMIN_PASS_MAX_LEN, true);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);

    ESP_LOGI(TAG, "Configuration saved, restarting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

/* ── Soft AP + HTTP server startup ──────────────────────────────── */

static esp_err_t start_softap(bool keep_sta)
{
    /* Get last 2 bytes of MAC for unique SSID suffix */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", MIMI_ONBOARD_AP_PREFIX, mac[4], mac[5]);

    /* Create AP netif if not already present */
    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* APSTA lets the local config AP coexist with WiFi scanning/STA usage. */
    (void)keep_sta;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && !(keep_sta && err == ESP_ERR_WIFI_CONN)) {
        return err;
    }

    ESP_LOGI(TAG, "Soft AP started: %s (open)", ssid);
    return ESP_OK;
}

static httpd_handle_t start_http_server(bool captive)
{
    if (s_server) {
        if (captive && !s_captive_mode) {
            ESP_LOGW(TAG, "HTTP server already running without captive redirects");
        }
        return s_server;
    }

    s_captive_mode = captive;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_ONBOARD_HTTP_PORT;
    config.max_uri_handlers = captive ? 28 : 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    /* Main page */
    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = http_get_root,
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_config = {
        .uri = "/config", .method = HTTP_GET, .handler = http_get_config,
    };
    httpd_register_uri_handler(s_server, &uri_config);

    /* WiFi scan */
    httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = http_get_scan,
    };
    httpd_register_uri_handler(s_server, &uri_scan);

    /* Save config */
    httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = http_post_save,
    };
    httpd_register_uri_handler(s_server, &uri_save);

    httpd_uri_t uri_api_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = http_get_api_status,
    };
    httpd_register_uri_handler(s_server, &uri_api_status);

    httpd_uri_t uri_api_tasks = {
        .uri = "/api/tasks", .method = HTTP_GET, .handler = http_get_api_tasks,
    };
    httpd_register_uri_handler(s_server, &uri_api_tasks);

    httpd_uri_t uri_api_task_remove = {
        .uri = "/api/task/remove", .method = HTTP_POST, .handler = http_post_api_task_remove,
    };
    httpd_register_uri_handler(s_server, &uri_api_task_remove);

    httpd_uri_t uri_api_files = {
        .uri = "/api/files", .method = HTTP_GET, .handler = http_get_api_files,
    };
    httpd_register_uri_handler(s_server, &uri_api_files);

    httpd_uri_t uri_api_file_get = {
        .uri = "/api/file", .method = HTTP_GET, .handler = http_get_api_file,
    };
    httpd_register_uri_handler(s_server, &uri_api_file_get);

    httpd_uri_t uri_api_file_post = {
        .uri = "/api/file", .method = HTTP_POST, .handler = http_post_api_file,
    };
    httpd_register_uri_handler(s_server, &uri_api_file_post);

    httpd_uri_t uri_api_memory_get = {
        .uri = "/api/memory", .method = HTTP_GET, .handler = http_get_api_memory,
    };
    httpd_register_uri_handler(s_server, &uri_api_memory_get);

    httpd_uri_t uri_api_memory_post = {
        .uri = "/api/memory", .method = HTTP_POST, .handler = http_post_api_memory,
    };
    httpd_register_uri_handler(s_server, &uri_api_memory_post);

    httpd_uri_t uri_api_voice = {
        .uri = "/api/voice", .method = HTTP_POST, .handler = http_post_api_voice,
    };
    httpd_register_uri_handler(s_server, &uri_api_voice);

    httpd_uri_t uri_api_heartbeat = {
        .uri = "/api/heartbeat", .method = HTTP_POST, .handler = http_post_api_heartbeat,
    };
    httpd_register_uri_handler(s_server, &uri_api_heartbeat);

    if (captive) {
        /* Captive portal detection endpoints */
        const char *captive_uris[] = {
            "/generate_204",           /* Android */
            "/gen_204",                /* Android alt */
            "/hotspot-detect.html",    /* iOS/macOS */
            "/library/test/success.html", /* iOS alt */
            "/connecttest.txt",        /* Windows */
            "/redirect",               /* Windows alt */
        };
        for (int i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
            httpd_uri_t uri_captive = {
                .uri = captive_uris[i],
                .method = HTTP_GET,
                .handler = http_captive_redirect,
            };
            httpd_register_uri_handler(s_server, &uri_captive);
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", MIMI_ONBOARD_HTTP_PORT);
    if (!captive) {
        admin_log_auth_hint();
    }
    return s_server;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t wifi_onboard_start(wifi_onboard_mode_t mode)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi Configuration Portal");
    ESP_LOGI(TAG, "========================================");

    bool captive = (mode == WIFI_ONBOARD_MODE_CAPTIVE);
    bool use_softap = (mode != WIFI_ONBOARD_MODE_STA_ADMIN);
    if (captive) {
        /* Stop STA retries before starting captive portal. */
        wifi_manager_set_reconnect_enabled(false);
        wifi_manager_stop();
    }

    if (use_softap) {
        esp_err_t err = start_softap(!captive);
        if (err != ESP_OK) return err;
    }

    if (captive) {
        /* Start DNS hijack only for true captive portal mode. */
        xTaskCreate(dns_hijack_task, "dns_hijack",
                    MIMI_ONBOARD_DNS_STACK, NULL, 5, NULL);
    }

    /* Start HTTP server */
    httpd_handle_t server = start_http_server(captive);
    if (!server) return ESP_FAIL;

    if (use_softap) {
        ESP_LOGI(TAG, "Connect to MimiClaw-XXXX WiFi, then open http://192.168.4.1");
    } else {
        ESP_LOGI(TAG, "Admin portal is available on STA IP: http://%s",
                 wifi_manager_get_ip());
    }

    if (!captive) {
        if (use_softap) {
            ESP_LOGI(TAG, "Local admin portal stays available while STA is connected");
        } else {
            ESP_LOGI(TAG, "Soft AP is off while STA WiFi is connected");
        }
        return ESP_OK;
    }

    /* Block forever — onboarding ends with esp_restart() in /save handler */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_OK;  /* unreachable */
}
