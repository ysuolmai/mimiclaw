# Voice Server Handoff

This note captures the deployment progress for the external MimiClaw voice
server. It is intentionally sanitized: no SSH key material, private key paths,
server IPs, usernames, or other machine-identifying details are included.

## Goal

Run a small LAN/VPN WebSocket voice bridge for the ESP32-S3 Super Mini firmware:

- ESP32 sends 16 kHz mono signed little-endian PCM16 over WebSocket.
- Server runs sherpa-onnx ASR/TTS.
- Server returns diagnostic JSON and 24 kHz mono signed little-endian PCM16 audio.
- ESP32 Web Admin `Voice Hardware -> Streaming Server URL` should point at:

```text
ws://<server-host-or-ip>:8765/mimi
```

## Firmware Protocol

The firmware connects to `/mimi` and sends:

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

Then:

1. Text frame: `{"type":"listen","state":"start"}`
2. Binary PCM16 frames, 16 kHz mono.
3. Text frame: `{"type":"listen","state":"stop"}`
4. Server may send JSON transcript/status frames.
5. Server may send binary PCM16 frames at 24 kHz mono for playback.

## Server Progress

On the target Ubuntu 24.04 x86_64 server:

- Python 3.12 is available.
- `python3.12-venv` / pip were made available.
- A deployment directory was created at:

```text
~/mimiclaw-voice-server
```

- Python venv was created under:

```text
~/mimiclaw-voice-server/.venv
```

- Python packages installed:

```text
websockets
numpy
sherpa-onnx
```

- A user-level systemd service was installed and enabled:

```text
~/.config/systemd/user/mimiclaw-voice.service
```

- The service name is:

```text
mimiclaw-voice.service
```

- User lingering was already enabled on that machine, so the user service should
  be able to start without an active login session.

## Model Progress

The deployment downloaded and extracted these sherpa-onnx models:

```text
models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/
models/vits-melo-tts-zh_en/
```

Expected important files:

```text
models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/model.int8.onnx
models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/tokens.txt
models/vits-melo-tts-zh_en/model.onnx
models/vits-melo-tts-zh_en/model.int8.onnx
models/vits-melo-tts-zh_en/tokens.txt
models/vits-melo-tts-zh_en/lexicon.txt
```

After restarting the service, logs showed:

```text
ASR=True TTS=True
Listening on ws://0.0.0.0:8765/mimi
```

The process used roughly 550 MB RAM after loading both models.

## Service Files

The server-side service files were prepared outside the repository during the
deployment session:

```text
voice_bridge.py
requirements.txt
download_models.sh
voice.env
mimiclaw-voice.service
self_test.py
```

If continuing from another machine, inspect the remote deployment directory
first before overwriting anything:

```bash
ssh <server> 'ls -la ~/mimiclaw-voice-server && systemctl --user status mimiclaw-voice.service --no-pager'
```

## Useful Remote Commands

Check service status:

```bash
ssh <server> 'systemctl --user --no-pager --full status mimiclaw-voice.service'
```

View recent logs:

```bash
ssh <server> 'journalctl --user -u mimiclaw-voice.service -n 120 --no-pager'
```

Restart after editing:

```bash
ssh <server> 'systemctl --user restart mimiclaw-voice.service'
```

Verify the port is listening:

```bash
ssh <server> 'ss -ltnp | grep 8765 || true'
```

Run a local WebSocket self-test from the server:

```bash
ssh <server> 'cd ~/mimiclaw-voice-server && . .venv/bin/activate && python self_test.py'
```

## Remaining Work

1. Complete a WebSocket self-test from the server itself.
2. From the ESP32 Web Admin, set:

```text
Voice Hardware -> Streaming Server URL = ws://<server-host-or-ip>:8765/mimi
Voice Hardware -> Streaming Codec = pcm16
```

3. Use `Voice Status` to confirm the ESP32 I2S side is ready.
4. Use `Test Beep`, `Record WAV`, and `Play WAV` to verify local mic/speaker.
5. Use `Start Stream` to verify end-to-end mic -> server ASR/TTS -> speaker.

## Notes

- Keep the WebSocket endpoint on LAN/VPN. Do not expose it directly to the
  public internet without adding authentication and TLS.
- The current bridge behavior is simple: ASR transcript is wrapped into a TTS
  reply like `Heard: <text>`. It does not yet call the MimiClaw agent/LLM.
- The firmware currently expects PCM16 fallback. Opus is reserved in the UI but
  not implemented in the firmware build.
