# ESP32-S3 Hardware Voice

This is the hardware voice foundation for the ESP32-S3 Super Mini build.

It currently provides I2S hardware tests and local WAV capture/playback. It does not yet implement cloud ASR, cloud TTS, wake word detection, or a full hands-free conversation loop.

## Default hardware

- I2S microphone: INMP441 or SPH0645 style module
- I2S speaker amplifier: MAX98357A style module
- Sample format: mono 16 kHz, 16-bit PCM WAV

## Default pins

```text
BCLK  GPIO4
WS    GPIO5
DIN   GPIO6  microphone data into ESP32-S3
DOUT  GPIO7  speaker data out from ESP32-S3
```

These defaults can be changed in `main/mimi_config.h`.

## Test flow

Use the Web Admin `Voice Hardware` panel:

1. `Voice Status` checks whether the I2S module initialized.
2. `Test Beep` plays a tone through the I2S amplifier.
3. `Record WAV` records to `/spiffs/voice_last.wav`.
4. `Play WAV` plays the recorded file.

The same operations are available to the agent as tools:

- `voice_status`
- `voice_beep`
- `voice_record`
- `voice_play`

## Next steps

The next layer can connect this foundation to:

- ASR: record WAV, send it to a speech-to-text service, inject transcript into the agent.
- TTS: call a text-to-speech service, store/play a compatible WAV stream.
- Wake/press-to-talk: add a GPIO button or wake word front-end before recording.
