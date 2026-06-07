# ESP32-S3 Super Mini Build

This fork targets the ESP32-S3 Super Mini / ESP32-S3FH4R2 profile used by `ysuolmai/esp-claw`.

## Hardware Profile

- Chip: ESP32-S3
- Flash: 4 MB Quad SPI, 80 MHz
- PSRAM: 2 MB Quad SPI, 80 MHz
- Console: native USB Serial/JTAG
- Partition layout: single factory app plus SPIFFS

## Partition Layout

| Offset | Size | Partition |
| --- | --- | --- |
| `0x9000` | 24 KB | `nvs` |
| `0xF000` | 4 KB | `phy_init` |
| `0x20000` | 3200 KB | `factory` |
| `0x340000` | 704 KB | `spiffs` |
| `0x3F0000` | 64 KB | `coredump` |

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Flash Merged Firmware

The GitHub Actions build uploads `mimiclaw-esp32-s3-supermini-merged.bin`, which includes the bootloader, partition table, app, and SPIFFS image.

```bash
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 mimiclaw-esp32-s3-supermini-merged.bin
```
