#pragma once

/* MimiClaw Global Configuration */

#include "sdkconfig.h"

/* Status LED driver types. Define these before mimi_secrets.h so local builds
 * can override MIMI_STATUS_LED_TYPE with the named constants below.
 */
#define MIMI_STATUS_LED_TYPE_GPIO     1
#define MIMI_STATUS_LED_TYPE_WS2812   2

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_LLM_BASE_URL
#define MIMI_SECRET_LLM_BASE_URL    ""
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_PROXY_TYPE
#define MIMI_SECRET_PROXY_TYPE      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_ID
#define MIMI_SECRET_FEISHU_APP_ID   ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_SECRET
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#endif
#ifndef MIMI_SECRET_TAVILY_KEY
#define MIMI_SECRET_TAVILY_KEY      ""
#endif
#ifndef MIMI_SECRET_VOICE_STREAM_URL
#define MIMI_SECRET_VOICE_STREAM_URL ""
#endif

/* Target feature profile */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define MIMI_TARGET_C3_LITE          1
#else
#define MIMI_TARGET_C3_LITE          0
#endif

#ifndef MIMI_ENABLE_FEISHU
#define MIMI_ENABLE_FEISHU           (!MIMI_TARGET_C3_LITE)
#endif
#ifndef MIMI_ENABLE_WEBSOCKET
#define MIMI_ENABLE_WEBSOCKET        (!MIMI_TARGET_C3_LITE)
#endif
#ifndef MIMI_ENABLE_WEB_SEARCH
#define MIMI_ENABLE_WEB_SEARCH       (!MIMI_TARGET_C3_LITE)
#endif
#ifndef MIMI_ENABLE_AUTO_MEMORY_SUMMARY
#define MIMI_ENABLE_AUTO_MEMORY_SUMMARY (!MIMI_TARGET_C3_LITE)
#endif
#ifndef MIMI_ENABLE_VOICE_HW
#define MIMI_ENABLE_VOICE_HW         (!MIMI_TARGET_C3_LITE)
#endif
#ifndef MIMI_ENABLE_VOICE_STREAM
#define MIMI_ENABLE_VOICE_STREAM     (!MIMI_TARGET_C3_LITE)
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* BOOT button: long press enters WiFi reconfiguration mode.
 * ESP32-S3 Super Mini uses GPIO0 for BOOT. ESP32-C3 Super Mini commonly uses
 * GPIO9; this keeps the lite build compiling even though S3 is the primary
 * hardware profile.
 */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define MIMI_BOOT_BUTTON_GPIO        9
#else
#define MIMI_BOOT_BUTTON_GPIO        0
#endif
#define MIMI_BOOT_BUTTON_POLL_MS     50
#define MIMI_BOOT_BUTTON_LONG_PRESS_MS 5000
#define MIMI_BOOT_BUTTON_STACK       (3 * 1024)
#define MIMI_BOOT_BUTTON_PRIO        4
#define MIMI_BOOT_BUTTON_CORE        0

/* Super Mini onboard status LED.
 * Common ESP32-S3 boards use WS2812/NeoPixel DIN on GPIO48; common ESP32-C3
 * boards use an active-low GPIO LED on GPIO8. Keep it quiet during normal
 * operation; use a slow blink only while the AP onboarding portal is up.
 */
#ifndef MIMI_ENABLE_STATUS_LED
#define MIMI_ENABLE_STATUS_LED       1
#endif
#ifndef MIMI_STATUS_LED_GPIO
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define MIMI_STATUS_LED_GPIO         8
#else
#define MIMI_STATUS_LED_GPIO         48
#endif
#endif
#ifndef MIMI_STATUS_LED_TYPE
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define MIMI_STATUS_LED_TYPE         MIMI_STATUS_LED_TYPE_GPIO
#else
#define MIMI_STATUS_LED_TYPE         MIMI_STATUS_LED_TYPE_WS2812
#endif
#endif
#ifndef MIMI_STATUS_LED_ACTIVE_LOW
#if MIMI_STATUS_LED_TYPE == MIMI_STATUS_LED_TYPE_GPIO
#define MIMI_STATUS_LED_ACTIVE_LOW   1
#else
#define MIMI_STATUS_LED_ACTIVE_LOW   0
#endif
#endif
#define MIMI_STATUS_LED_RMT_RES_HZ   (10 * 1000 * 1000)
#define MIMI_STATUS_LED_BLINK_MS     700
#define MIMI_STATUS_LED_STACK        (3 * 1024)
#define MIMI_STATUS_LED_PRIO         3
#define MIMI_STATUS_LED_CORE         0

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#if MIMI_TARGET_C3_LITE
#define MIMI_TG_POLL_STACK           (8 * 1024)
#else
#define MIMI_TG_POLL_STACK           (12 * 1024)
#endif
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0
#define MIMI_TG_CARD_SHOW_MS         3000
#define MIMI_TG_CARD_BODY_SCALE      3

/* Feishu Bot */
#define MIMI_FEISHU_MAX_MSG_LEN          4096
#define MIMI_FEISHU_POLL_STACK           (12 * 1024)
#define MIMI_FEISHU_POLL_PRIO            5
#define MIMI_FEISHU_POLL_CORE            0
#define MIMI_FEISHU_WEBHOOK_PORT         18790
#define MIMI_FEISHU_WEBHOOK_PATH         "/feishu/events"
#define MIMI_FEISHU_WEBHOOK_MAX_BODY     (16 * 1024)

/* Agent Loop */
#if MIMI_TARGET_C3_LITE
#define MIMI_AGENT_STACK             (12 * 1024)
#define MIMI_AGENT_CORE              0
#define MIMI_AGENT_MAX_HISTORY       6
#define MIMI_AGENT_MAX_TOOL_ITER     3
#define MIMI_MAX_TOOL_CALLS          2
#else
#define MIMI_AGENT_STACK             (24 * 1024)
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4
#endif
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_SEND_WORKING_STATUS 1

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_PROVIDER_DEFAULT    "anthropic"
#if MIMI_TARGET_C3_LITE
#define MIMI_LLM_MAX_TOKENS          1024
#define MIMI_LLM_STREAM_BUF_SIZE     (12 * 1024)
#else
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)
#endif
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD 0
#define MIMI_LLM_LOG_PREVIEW_BYTES   160

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           16
#if MIMI_TARGET_C3_LITE
#define MIMI_OUTBOUND_STACK          (8 * 1024)
#else
#define MIMI_OUTBOUND_STACK          (12 * 1024)
#endif
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       MIMI_SPIFFS_BASE "/config"
#define MIMI_SPIFFS_MEMORY_DIR       MIMI_SPIFFS_BASE "/memory"
#define MIMI_SPIFFS_SESSION_DIR      MIMI_SPIFFS_BASE "/sessions"
#define MIMI_MEMORY_FILE             MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define MIMI_MEMORY_SUMMARY_FILE     MIMI_SPIFFS_MEMORY_DIR "/SUMMARY.md"
#define MIMI_SOUL_FILE               MIMI_SPIFFS_CONFIG_DIR "/SOUL.md"
#define MIMI_USER_FILE               MIMI_SPIFFS_CONFIG_DIR "/USER.md"
#if MIMI_TARGET_C3_LITE
#define MIMI_CONTEXT_BUF_SIZE        (8 * 1024)
#define MIMI_SESSION_MAX_MSGS        6
#else
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20
#endif

/* Cron / Heartbeat */
#define MIMI_CRON_FILE               MIMI_SPIFFS_BASE "/cron.json"
#define MIMI_CRON_MAX_JOBS           16
#define MIMI_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define MIMI_HEARTBEAT_FILE          MIMI_SPIFFS_BASE "/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* GPIO */
#define MIMI_GPIO_CONFIG_SECTION     1   /* enable GPIO tools */

/* Hardware voice (ESP32-S3 profile).
 * Pins match the Xiaozhi ESP32-S3 Super Mini board config:
 * - INMP441/SPH0645 microphone: WS GPIO4, SCK GPIO5, SD GPIO6
 * - MAX98357A speaker amp: DIN GPIO11, BCLK GPIO12, LRC GPIO13
 */
#define MIMI_VOICE_MIC_WS_GPIO       4
#define MIMI_VOICE_MIC_SCK_GPIO      5
#define MIMI_VOICE_MIC_DIN_GPIO      6
#define MIMI_VOICE_SPK_DOUT_GPIO     11
#define MIMI_VOICE_SPK_BCLK_GPIO     12
#define MIMI_VOICE_SPK_LRCK_GPIO     13
#define MIMI_VOICE_INPUT_SAMPLE_RATE 16000
#define MIMI_VOICE_OUTPUT_SAMPLE_RATE 24000
#define MIMI_VOICE_SAMPLE_RATE       MIMI_VOICE_INPUT_SAMPLE_RATE
#define MIMI_VOICE_BITS_PER_SAMPLE   16
#define MIMI_VOICE_MAX_RECORD_SECONDS 30
#define MIMI_VOICE_DEFAULT_FILE      MIMI_SPIFFS_BASE "/voice_last.wav"
#define MIMI_VOICE_TTS_FILE          MIMI_SPIFFS_BASE "/voice_tts.wav"
#define MIMI_VOICE_STREAM_DEFAULT_CODEC "pcm16"
#define MIMI_VOICE_STREAM_FRAME_MS   60
#define MIMI_VOICE_STREAM_MAX_SECONDS 60
#define MIMI_VOICE_STREAM_STACK      (12 * 1024)
#define MIMI_VOICE_STREAM_PRIO       5
#define MIMI_VOICE_STREAM_CORE       0

/* Skills */
#define MIMI_SKILLS_PREFIX           MIMI_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#if MIMI_TARGET_C3_LITE
#define MIMI_WS_MAX_CLIENTS          1
#else
#define MIMI_WS_MAX_CLIENTS          4
#endif

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_FEISHU              "feishu_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"
#define MIMI_NVS_VOICE               "voice_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_FEISHU_APP_ID   "app_id"
#define MIMI_NVS_KEY_FEISHU_APP_SECRET "app_secret"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_TAVILY_KEY      "tavily_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_LLM_BASE_URL    "base_url"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
#define MIMI_NVS_KEY_PROXY_TYPE      "proxy_type"
#define MIMI_NVS_KEY_VOICE_STREAM_URL "stream_url"
#define MIMI_NVS_KEY_VOICE_CODEC     "codec"
#define MIMI_NVS_KEY_FORCE_ONBOARD   "force_onboard"

/* WiFi Onboarding (Captive Portal) */
#define MIMI_ONBOARD_AP_PREFIX    "MimiClaw-"
#define MIMI_ONBOARD_AP_PASS      ""          /* open network */
#define MIMI_ONBOARD_HTTP_PORT    80
#define MIMI_ONBOARD_DNS_STACK    (4 * 1024)
#define MIMI_ONBOARD_MAX_SCAN     20
