#include "voice/voice_hw.h"
#include "mimi_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "voice_hw";

#define VOICE_CHUNK_SAMPLES 256
#define VOICE_CHUNK_BYTES   (VOICE_CHUNK_SAMPLES * sizeof(int16_t))

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_initialized = false;
static bool s_ready = false;
static bool s_tx_enabled = false;
static bool s_rx_enabled = false;
static esp_err_t s_last_error = ESP_OK;

static void write_le16(FILE *f, uint16_t value)
{
    fputc(value & 0xff, f);
    fputc((value >> 8) & 0xff, f);
}

static void write_le32(FILE *f, uint32_t value)
{
    fputc(value & 0xff, f);
    fputc((value >> 8) & 0xff, f);
    fputc((value >> 16) & 0xff, f);
    fputc((value >> 24) & 0xff, f);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    uint16_t channels = 1;
    uint16_t bits = MIMI_VOICE_BITS_PER_SAMPLE;
    uint32_t sample_rate = MIMI_VOICE_SAMPLE_RATE;
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;

    fwrite("RIFF", 1, 4, f);
    write_le32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    write_le32(f, 16);
    write_le16(f, 1);
    write_le16(f, channels);
    write_le32(f, sample_rate);
    write_le32(f, byte_rate);
    write_le16(f, block_align);
    write_le16(f, bits);
    fwrite("data", 1, 4, f);
    write_le32(f, data_bytes);
}

static esp_err_t ensure_ready(void)
{
    if (!s_initialized) {
        esp_err_t err = voice_hw_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    return s_ready ? ESP_OK : s_last_error;
}

static esp_err_t tx_enable(void)
{
    if (!s_tx_enabled) {
        esp_err_t err = i2s_channel_enable(s_tx_chan);
        if (err != ESP_OK) {
            return err;
        }
        s_tx_enabled = true;
    }
    return ESP_OK;
}

static void tx_disable(void)
{
    if (s_tx_enabled) {
        i2s_channel_disable(s_tx_chan);
        s_tx_enabled = false;
    }
}

static esp_err_t rx_enable(void)
{
    if (!s_rx_enabled) {
        esp_err_t err = i2s_channel_enable(s_rx_chan);
        if (err != ESP_OK) {
            return err;
        }
        s_rx_enabled = true;
    }
    return ESP_OK;
}

static void rx_disable(void)
{
    if (s_rx_enabled) {
        i2s_channel_disable(s_rx_chan);
        s_rx_enabled = false;
    }
}

esp_err_t voice_hw_init(void)
{
    if (s_initialized) {
        return s_last_error;
    }
    s_initialized = true;

    if (!MIMI_ENABLE_VOICE_HW) {
        s_last_error = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGI(TAG, "hardware voice disabled in this target profile");
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = VOICE_CHUNK_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        s_last_error = err;
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_VOICE_I2S_BCLK_GPIO,
            .ws = MIMI_VOICE_I2S_WS_GPIO,
            .dout = MIMI_VOICE_I2S_DOUT_GPIO,
            .din = MIMI_VOICE_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    }
    if (err != ESP_OK) {
        s_last_error = err;
        ESP_LOGE(TAG, "i2s std init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG,
             "hardware voice ready: bclk=%d ws=%d din=%d dout=%d rate=%d",
             MIMI_VOICE_I2S_BCLK_GPIO,
             MIMI_VOICE_I2S_WS_GPIO,
             MIMI_VOICE_I2S_DIN_GPIO,
             MIMI_VOICE_I2S_DOUT_GPIO,
             MIMI_VOICE_SAMPLE_RATE);
    return ESP_OK;
}

bool voice_hw_is_ready(void)
{
    return s_ready;
}

void voice_hw_status(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return;
    }
    snprintf(output, output_size,
             "Hardware voice\n"
             "- enabled: %s\n"
             "- initialized: %s\n"
             "- ready: %s\n"
             "- last_error: %s\n"
             "- sample_rate: %d\n"
             "- bits_per_sample: %d\n"
             "- bclk_gpio: %d\n"
             "- ws_gpio: %d\n"
             "- din_gpio: %d\n"
             "- dout_gpio: %d\n"
             "- default_file: %s\n",
             MIMI_ENABLE_VOICE_HW ? "yes" : "no",
             s_initialized ? "yes" : "no",
             s_ready ? "yes" : "no",
             esp_err_to_name(s_last_error),
             MIMI_VOICE_SAMPLE_RATE,
             MIMI_VOICE_BITS_PER_SAMPLE,
             MIMI_VOICE_I2S_BCLK_GPIO,
             MIMI_VOICE_I2S_WS_GPIO,
             MIMI_VOICE_I2S_DIN_GPIO,
             MIMI_VOICE_I2S_DOUT_GPIO,
             MIMI_VOICE_DEFAULT_FILE);
}

esp_err_t voice_hw_beep(int freq_hz, int duration_ms)
{
    esp_err_t err = ensure_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (freq_hz < 80) freq_hz = 80;
    if (freq_hz > 4000) freq_hz = 4000;
    if (duration_ms <= 0) duration_ms = 300;
    if (duration_ms > 5000) duration_ms = 5000;

    err = tx_enable();
    if (err != ESP_OK) {
        return err;
    }

    int16_t samples[VOICE_CHUNK_SAMPLES];
    int half_period = MIMI_VOICE_SAMPLE_RATE / (freq_hz * 2);
    if (half_period <= 0) half_period = 1;

    int total_samples = (MIMI_VOICE_SAMPLE_RATE * duration_ms) / 1000;
    int sample_index = 0;
    while (sample_index < total_samples) {
        int count = total_samples - sample_index;
        if (count > VOICE_CHUNK_SAMPLES) count = VOICE_CHUNK_SAMPLES;
        for (int i = 0; i < count; i++) {
            int phase = ((sample_index + i) / half_period) & 1;
            samples[i] = phase ? 9000 : -9000;
        }
        size_t written = 0;
        err = i2s_channel_write(s_tx_chan, samples, count * sizeof(int16_t),
                                &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            break;
        }
        sample_index += count;
    }

    tx_disable();
    ESP_LOGI(TAG, "beep freq=%d duration=%d err=%s", freq_hz, duration_ms, esp_err_to_name(err));
    return err;
}

esp_err_t voice_hw_record_wav(const char *path, int seconds)
{
    esp_err_t err = ensure_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!path || !path[0]) {
        path = MIMI_VOICE_DEFAULT_FILE;
    }
    if (seconds <= 0) seconds = 3;
    if (seconds > MIMI_VOICE_MAX_RECORD_SECONDS) {
        seconds = MIMI_VOICE_MAX_RECORD_SECONDS;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", path);
        return ESP_FAIL;
    }

    write_wav_header(f, 0);
    uint8_t *buf = malloc(VOICE_CHUNK_BYTES);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint32_t target_bytes = MIMI_VOICE_SAMPLE_RATE * seconds * sizeof(int16_t);
    uint32_t recorded = 0;
    err = rx_enable();
    while (err == ESP_OK && recorded < target_bytes) {
        size_t want = target_bytes - recorded;
        if (want > VOICE_CHUNK_BYTES) want = VOICE_CHUNK_BYTES;

        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, buf, want, &bytes_read, pdMS_TO_TICKS(1200));
        if (err != ESP_OK) {
            break;
        }
        if (bytes_read > 0) {
            fwrite(buf, 1, bytes_read, f);
            recorded += bytes_read;
        }
    }
    rx_disable();
    free(buf);

    fseek(f, 0, SEEK_SET);
    write_wav_header(f, recorded);
    fclose(f);

    ESP_LOGI(TAG, "recorded %u bytes to %s err=%s", (unsigned)recorded, path, esp_err_to_name(err));
    return err;
}

esp_err_t voice_hw_play_wav(const char *path)
{
    esp_err_t err = ensure_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!path || !path[0]) {
        path = MIMI_VOICE_DEFAULT_FILE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot read %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[44];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0 ||
        memcmp(header + 12, "fmt ", 4) != 0 ||
        memcmp(header + 36, "data", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t format = read_le16(header + 20);
    uint16_t channels = read_le16(header + 22);
    uint32_t sample_rate = read_le32(header + 24);
    uint16_t bits = read_le16(header + 34);
    uint32_t data_bytes = read_le32(header + 40);
    if (format != 1 || channels != 1 || sample_rate != MIMI_VOICE_SAMPLE_RATE || bits != 16) {
        fclose(f);
        ESP_LOGE(TAG, "unsupported wav format fmt=%u channels=%u rate=%u bits=%u",
                 format, channels, (unsigned)sample_rate, bits);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *buf = malloc(VOICE_CHUNK_BYTES);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    err = tx_enable();
    uint32_t played = 0;
    while (err == ESP_OK && played < data_bytes) {
        size_t want = data_bytes - played;
        if (want > VOICE_CHUNK_BYTES) want = VOICE_CHUNK_BYTES;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;

        size_t written = 0;
        err = i2s_channel_write(s_tx_chan, buf, got, &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) break;
        played += written;
    }
    tx_disable();

    free(buf);
    fclose(f);
    ESP_LOGI(TAG, "played %u bytes from %s err=%s", (unsigned)played, path, esp_err_to_name(err));
    return err;
}
