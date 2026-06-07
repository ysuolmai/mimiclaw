#include "tools/tool_memory.h"
#include "memory/memory_store.h"
#include "mimi_config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_memory";

#define MEMORY_BACKUP_FILE       MIMI_SPIFFS_MEMORY_DIR "/BACKUP.md"
#define MEMORY_MAX_LINE          512
#define MEMORY_EXPORT_MAX_BYTES  (48 * 1024)

static cJSON *parse_input_or_empty(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse((input_json && input_json[0]) ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return NULL;
    }
    return root;
}

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) return fallback;
    return cJSON_IsTrue(item);
}

static void build_spiffs_path(const char *name, char *path, size_t path_size)
{
    if (!name || !path || path_size == 0) return;
    if (strncmp(name, MIMI_SPIFFS_BASE, strlen(MIMI_SPIFFS_BASE)) == 0) {
        strlcpy(path, name, path_size);
    } else {
        snprintf(path, path_size, "%s/%s", MIMI_SPIFFS_BASE, name);
    }
}

static bool is_memory_or_session_path(const char *path)
{
    if (!path) return false;
    return strncmp(path, MIMI_SPIFFS_MEMORY_DIR, strlen(MIMI_SPIFFS_MEMORY_DIR)) == 0 ||
           strncmp(path, MIMI_SPIFFS_SESSION_DIR, strlen(MIMI_SPIFFS_SESSION_DIR)) == 0 ||
           strstr(path, "/tg_") != NULL ||
           strstr(path, "tg_") == path + strlen(MIMI_SPIFFS_BASE) + 1;
}

static bool is_session_path(const char *path)
{
    return path && strstr(path, "tg_") && strstr(path, ".jsonl");
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;

    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static size_t append_text(char *output, size_t output_size, size_t off, const char *text)
{
    if (!output || output_size == 0 || off >= output_size - 1 || !text) return off;
    int n = snprintf(output + off, output_size - off, "%s", text);
    if (n < 0) return off;
    if ((size_t)n >= output_size - off) return output_size - 1;
    return off + (size_t)n;
}

static size_t append_fmt(char *output, size_t output_size, size_t off, const char *fmt, ...)
{
    if (!output || output_size == 0 || off >= output_size - 1 || !fmt) return off;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(output + off, output_size - off, fmt, ap);
    va_end(ap);

    if (n < 0) return off;
    if ((size_t)n >= output_size - off) return output_size - 1;
    return off + (size_t)n;
}

static void trim_line(char *line)
{
    if (!line) return;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

static int search_file(const char *path, const char *query, int max_results,
                       char *output, size_t output_size, size_t *off)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[MEMORY_MAX_LINE];
    int line_no = 0;
    int matches = 0;

    while (fgets(line, sizeof(line), f) && matches < max_results && *off < output_size - 1) {
        line_no++;
        trim_line(line);
        if (!contains_ci(line, query)) continue;

        *off = append_fmt(output, output_size, *off,
                          "%s:%d: %s\n", path, line_no, line);
        matches++;
    }

    fclose(f);
    return matches;
}

static size_t copy_file_section(FILE *out, const char *path, size_t written, size_t max_bytes)
{
    if (!out || !path || written >= max_bytes) return written;

    FILE *in = fopen(path, "r");
    if (!in) return written;

    int n = fprintf(out, "\n## %s\n\n", path);
    if (n > 0) written += (size_t)n;

    char buf[256];
    while (written < max_bytes) {
        size_t room = max_bytes - written;
        size_t want = room < sizeof(buf) ? room : sizeof(buf);
        size_t got = fread(buf, 1, want, in);
        if (got == 0) break;
        fwrite(buf, 1, got, out);
        written += got;
    }

    fputc('\n', out);
    written++;
    fclose(in);
    return written;
}

static int summarize_session_file(const char *path, FILE *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int messages = 0;
    int user_msgs = 0;
    int assistant_msgs = 0;
    time_t first_ts = 0;
    time_t last_ts = 0;
    char last_user[160] = {0};
    char line[MEMORY_MAX_LINE * 2];

    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "content"));
        cJSON *ts_item = cJSON_GetObjectItem(obj, "ts");
        time_t ts = cJSON_IsNumber(ts_item) ? (time_t)ts_item->valuedouble : 0;

        messages++;
        if (ts > 0) {
            if (first_ts == 0) first_ts = ts;
            last_ts = ts;
        }
        if (role && strcmp(role, "user") == 0) {
            user_msgs++;
            if (content) {
                strlcpy(last_user, content, sizeof(last_user));
            }
        } else if (role && strcmp(role, "assistant") == 0) {
            assistant_msgs++;
        }
        cJSON_Delete(obj);
    }

    fclose(f);

    if (messages > 0 && out) {
        fprintf(out,
                "- `%s`: %d messages (%d user, %d assistant), first=%ld, last=%ld",
                path, messages, user_msgs, assistant_msgs, (long)first_ts, (long)last_ts);
        if (last_user[0]) {
            fprintf(out, ", last_user=\"%.120s\"", last_user);
        }
        fputc('\n', out);
    }

    return messages;
}

static time_t session_last_timestamp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    time_t last_ts = 0;
    char line[MEMORY_MAX_LINE * 2];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        cJSON *ts_item = cJSON_GetObjectItem(obj, "ts");
        if (cJSON_IsNumber(ts_item) && (time_t)ts_item->valuedouble > last_ts) {
            last_ts = (time_t)ts_item->valuedouble;
        }
        cJSON_Delete(obj);
    }

    fclose(f);
    return last_ts;
}

esp_err_t tool_memory_append_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

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

esp_err_t tool_memory_search_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(root, "query"));
    if (!query || !query[0]) {
        snprintf(output, output_size, "Error: missing 'query' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int max_results = json_int(root, "max_results", 12);
    if (max_results <= 0) max_results = 12;
    if (max_results > 30) max_results = 30;

    output[0] = '\0';
    size_t off = append_fmt(output, output_size, 0,
                            "Memory search: \"%s\" (max %d)\n\n", query, max_results);
    int matches = 0;

    matches += search_file(MIMI_MEMORY_FILE, query, max_results - matches, output, output_size, &off);
    matches += search_file(MIMI_MEMORY_SUMMARY_FILE, query, max_results - matches, output, output_size, &off);

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && matches < max_results && off < output_size - 1) {
            char path[256];
            build_spiffs_path(ent->d_name, path, sizeof(path));
            if (!is_memory_or_session_path(path)) continue;
            if (strcmp(path, MIMI_MEMORY_FILE) == 0 || strcmp(path, MIMI_MEMORY_SUMMARY_FILE) == 0) continue;
            matches += search_file(path, query, max_results - matches, output, output_size, &off);
        }
        closedir(dir);
    }

    if (matches == 0) {
        append_text(output, output_size, off, "(no matches found)");
    }

    ESP_LOGI(TAG, "memory_search: query=%s matches=%d", query, matches);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_memory_export_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    bool include_sessions = json_bool(root, "include_sessions", true);
    int max_bytes = json_int(root, "max_bytes", 24576);
    if (max_bytes <= 4096) max_bytes = 4096;
    if (max_bytes > MEMORY_EXPORT_MAX_BYTES) max_bytes = MEMORY_EXPORT_MAX_BYTES;

    FILE *out = fopen(MEMORY_BACKUP_FILE, "w");
    if (!out) {
        snprintf(output, output_size, "Error: cannot write %s", MEMORY_BACKUP_FILE);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    size_t written = (size_t)fprintf(out,
        "# MimiClaw Memory Backup\n\n"
        "- created_epoch: %ld\n"
        "- include_sessions: %s\n"
        "- max_bytes: %d\n",
        (long)now, include_sessions ? "true" : "false", max_bytes);

    written = copy_file_section(out, MIMI_MEMORY_FILE, written, (size_t)max_bytes);
    written = copy_file_section(out, MIMI_MEMORY_SUMMARY_FILE, written, (size_t)max_bytes);
    written = copy_file_section(out, MIMI_HEARTBEAT_FILE, written, (size_t)max_bytes);

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    int files = 0;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && written < (size_t)max_bytes) {
            char path[256];
            build_spiffs_path(ent->d_name, path, sizeof(path));
            if (strcmp(path, MEMORY_BACKUP_FILE) == 0 ||
                strcmp(path, MIMI_MEMORY_FILE) == 0 ||
                strcmp(path, MIMI_MEMORY_SUMMARY_FILE) == 0 ||
                strcmp(path, MIMI_HEARTBEAT_FILE) == 0) {
                continue;
            }
            if (strncmp(path, MIMI_SPIFFS_MEMORY_DIR, strlen(MIMI_SPIFFS_MEMORY_DIR)) == 0 ||
                (include_sessions && is_session_path(path))) {
                written = copy_file_section(out, path, written, (size_t)max_bytes);
                files++;
            }
        }
        closedir(dir);
    }

    fclose(out);
    snprintf(output, output_size,
             "OK: exported memory backup to %s (%d bytes budget, %d extra files included)",
             MEMORY_BACKUP_FILE, max_bytes, files);
    ESP_LOGI(TAG, "memory_export: %s", output);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_memory_summarize_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    int days = json_int(root, "days", 7);
    if (days <= 0) days = 7;
    if (days > 30) days = 30;
    bool include_sessions = json_bool(root, "include_sessions", true);

    char *long_term = calloc(1, 3072);
    char *recent = calloc(1, 4096);
    if (!long_term || !recent) {
        free(long_term);
        free(recent);
        snprintf(output, output_size, "Error: out of memory while summarizing memory");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    FILE *out = fopen(MIMI_MEMORY_SUMMARY_FILE, "w");
    if (!out) {
        free(long_term);
        free(recent);
        snprintf(output, output_size, "Error: cannot write %s", MIMI_MEMORY_SUMMARY_FILE);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    fprintf(out,
            "# MimiClaw Memory Summary\n\n"
            "- updated_epoch: %ld\n"
            "- source_long_term: %s\n"
            "- source_recent_days: %d\n\n",
            (long)now, MIMI_MEMORY_FILE, days);

    if (memory_read_long_term(long_term, 3072) == ESP_OK && long_term[0]) {
        fprintf(out, "## Long-term Memory Snapshot\n\n%s\n\n", long_term);
    } else {
        fprintf(out, "## Long-term Memory Snapshot\n\n(empty)\n\n");
    }

    if (memory_read_recent(recent, 4096, days) == ESP_OK && recent[0]) {
        fprintf(out, "## Recent Daily Notes\n\n%s\n\n", recent);
    } else {
        fprintf(out, "## Recent Daily Notes\n\n(empty)\n\n");
    }

    free(long_term);
    free(recent);

    int session_files = 0;
    int session_messages = 0;
    if (include_sessions) {
        fprintf(out, "## Session Digest\n\n");
        DIR *dir = opendir(MIMI_SPIFFS_BASE);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                char path[256];
                build_spiffs_path(ent->d_name, path, sizeof(path));
                if (!is_session_path(path)) continue;
                int messages = summarize_session_file(path, out);
                if (messages > 0) {
                    session_files++;
                    session_messages += messages;
                }
            }
            closedir(dir);
        }
        if (session_files == 0) {
            fprintf(out, "(no sessions found)\n");
        }
    }

    fprintf(out,
            "\n## Maintenance Notes\n\n"
            "- Use memory_search to find old notes before editing long-term memory.\n"
            "- Use session_cleanup with dry_run=true before deleting old sessions.\n"
            "- This is an on-device summary. The AI can refine it into concise facts when needed.\n");
    fclose(out);

    snprintf(output, output_size,
             "OK: updated %s from %d days of notes (%d session files, %d messages)",
             MIMI_MEMORY_SUMMARY_FILE, days, session_files, session_messages);
    ESP_LOGI(TAG, "memory_summarize: %s", output);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_session_cleanup_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_input_or_empty(input_json, output, output_size);
    if (!root) return ESP_ERR_INVALID_ARG;

    int older_than_days = json_int(root, "older_than_days", 30);
    if (older_than_days < 0) older_than_days = 30;
    bool dry_run = json_bool(root, "dry_run", true);

    time_t now = time(NULL);
    time_t cutoff = (older_than_days == 0) ? now + 1 : now - (time_t)older_than_days * 86400;

    size_t off = append_fmt(output, output_size, 0,
                            "Session cleanup (%s, older_than_days=%d)\n\n",
                            dry_run ? "dry-run" : "apply", older_than_days);
    int matched = 0;
    int deleted = 0;
    int skipped_unknown_age = 0;

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        snprintf(output, output_size, "Error: cannot open %s", MIMI_SPIFFS_BASE);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && off < output_size - 1) {
        char path[256];
        build_spiffs_path(ent->d_name, path, sizeof(path));
        if (!is_session_path(path)) continue;

        struct stat st;
        time_t file_time = 0;
        if (stat(path, &st) == 0 && st.st_mtime > 0) {
            file_time = st.st_mtime;
        } else {
            file_time = session_last_timestamp(path);
        }

        if (file_time <= 0) {
            skipped_unknown_age++;
            off = append_fmt(output, output_size, off, "skip unknown age: %s\n", path);
            continue;
        }
        if (file_time > cutoff) continue;

        matched++;
        if (!dry_run && remove(path) == 0) {
            deleted++;
            off = append_fmt(output, output_size, off, "deleted: %s\n", path);
        } else {
            off = append_fmt(output, output_size, off, "would delete: %s (mtime=%ld)\n",
                             path, (long)file_time);
        }
    }
    closedir(dir);

    append_fmt(output, output_size, off,
               "\nmatched=%d deleted=%d skipped_unknown_age=%d", matched, deleted, skipped_unknown_age);

    ESP_LOGI(TAG, "session_cleanup: matched=%d deleted=%d dry_run=%d",
             matched, deleted, dry_run);
    cJSON_Delete(root);
    return ESP_OK;
}
