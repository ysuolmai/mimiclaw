# ESP32-C3 Super Mini Lite

This profile targets ESP32-C3 Super Mini boards with 4 MB flash and no PSRAM.

It is a constrained runtime profile, not the full ESP32-S3 build.

## Enabled by default

- WiFi and captive/admin configuration portal
- Telegram bot
- LLM agent loop
- Small persistent memory and session files
- File, memory, cron, heartbeat, GPIO, and system status tools
- SPIFFS image as a separate artifact

## Disabled by default

- Feishu/Lark runtime startup
- Local WebSocket gateway startup
- Agent web_search tool registration
- Automatic periodic memory summary refresh
- OTA A/B partitions

## Current limitations

- No voice input or output pipeline is implemented.
- No Telegram voice download, ASR, TTS, I2S speaker, or microphone path is present.
- The LLM path is memory constrained. Prefer smaller models, short answers, and short history.

## Flash layout

```text
0x0000    bootloader
0x8000    partition table
0x9000    nvs        24KB
0xF000    phy_init   4KB
0x10000   factory    0x250000
0x260000  spiffs     0x190000
0x3F0000  coredump   64KB
```

The merged firmware artifact contains bootloader, partition table, and app only.
SPIFFS is uploaded as a separate `spiffs.bin` artifact.
