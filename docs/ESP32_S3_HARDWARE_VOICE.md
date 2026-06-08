# ESP32-S3 Hardware Voice

This is the hardware voice foundation for the ESP32-S3 Super Mini build.

It provides I2S hardware tests, local WAV capture/playback, and a Xiaozhi-style streaming voice skeleton for a small external ASR/TTS server such as a sherpa-onnx bridge. It does not yet implement local wake word detection, AEC, or on-device ASR/TTS.

## Default hardware

- I2S microphone: INMP441 or SPH0645 style module
- I2S speaker amplifier: MAX98357A style module
- Input format: mono 16 kHz, 16-bit PCM
- Output format: mono 24 kHz, 16-bit PCM preferred; 16 kHz WAV playback is resampled to 24 kHz

## Default pins

```text
INMP441 microphone:
WS    GPIO4
SCK   GPIO5
SD    GPIO6

MAX98357A speaker:
DIN   GPIO11
BCLK  GPIO12
LRC   GPIO13
```

These defaults can be changed in `main/mimi_config.h`.

## Test flow

Use the Web Admin `Voice Hardware` panel:

1. `Voice Status` checks whether the I2S module initialized.
2. `Test Beep` plays a tone through the I2S amplifier.
3. `Record WAV` records to `/spiffs/voice_last.wav`.
4. `Play WAV` plays the recorded file.
5. Configure `Streaming Server URL`, then use `Start Stream` for a WebSocket voice turn.

The same operations are available to the agent as tools:

- `voice_status`
- `voice_beep`
- `voice_record`
- `voice_play`
- `voice_stream_status`
- `voice_stream_config`
- `voice_stream_start`
- `voice_stream_stop`

## Streaming protocol

The streaming path is designed to be easy to bridge to sherpa-onnx:

1. ESP32 connects to the configured WebSocket URL.
2. ESP32 sends a JSON text frame:

```json
{
  "type": "hello",
  "device": "mimiclaw-esp32-s3-supermini",
  "transport": "websocket",
  "codec": "pcm16",
  "sample_rate": 16000,
  "output_sample_rate": 24000,
  "channels": 1,
  "frame_ms": 60,
  "seconds": 10
}
```

3. ESP32 sends `{"type":"listen","state":"start"}`.
4. ESP32 sends binary frames containing little-endian signed PCM16 at 16 kHz mono.
5. ESP32 sends `{"type":"listen","state":"stop"}`.
6. The server can return text JSON for diagnostics/transcripts and binary PCM16 frames at 24 kHz mono for immediate playback.

`opus` is exposed as a reserved codec option so the wire protocol can move closer to Xiaozhi later. The current firmware sends PCM16 fallback unless an Opus encoder/decoder component is linked.

## Next steps

The next layer can connect this foundation to:

- Server bridge: receive PCM16 frames, run VAD/STT, forward text to the agent API, run TTS, and return 24 kHz PCM16 frames.
- Opus: replace the PCM16 fallback with real Opus encode/decode when the component dependency is settled.
- Wake/press-to-talk: add a GPIO button or wake word front-end before recording.
