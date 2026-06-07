#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Append a note to today's daily memory file.
 * Input JSON: {"note": "..."}
 */
esp_err_t tool_memory_append_execute(const char *input_json, char *output, size_t output_size);

/**
 * Search long-term memory, summary, daily notes, and session history.
 * Input JSON: {"query": "...", "max_results": 12}
 */
esp_err_t tool_memory_search_execute(const char *input_json, char *output, size_t output_size);

/**
 * Export memory and session files into a compact markdown backup file.
 * Input JSON: {"include_sessions": true, "max_bytes": 24576}
 */
esp_err_t tool_memory_export_execute(const char *input_json, char *output, size_t output_size);

/**
 * Build/update a local summary file from long-term memory, recent notes, and sessions.
 * Input JSON: {"days": 7, "include_sessions": true}
 */
esp_err_t tool_memory_summarize_execute(const char *input_json, char *output, size_t output_size);

/**
 * Delete old session files from SPIFFS.
 * Input JSON: {"older_than_days": 30, "dry_run": true}
 */
esp_err_t tool_session_cleanup_execute(const char *input_json, char *output, size_t output_size);
