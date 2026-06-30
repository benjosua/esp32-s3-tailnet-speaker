# Tailnet Speaker Frame Protocol

TCP stream over MicroLink/Tailscale.

Frame header:

```text
byte 0      frame type
bytes 1-4   uint32 big-endian payload length
bytes 5..   payload
```

Types:

- `1` JSON command/event UTF-8
- `2` audio playback payload: PCM signed 16-bit little-endian mono 24000 Hz
- `3` mic payload, reserved for later

Device commands:

```json
{"cmd":"status_request"}
{"cmd":"set_led","r":0,"g":0,"b":64}
{"cmd":"set_volume","volume":40}
{"cmd":"play_tone","seconds":3,"frequency":440}
{"cmd":"play_audio","format":"pcm_s16le_24k_mono"}
{"cmd":"audio_stop"}
{"cmd":"alarm_start"}
{"cmd":"alarm_stop"}
{"cmd":"schedule_set","time":"07:30"}
```

Audio streaming:

1. Send `play_audio` JSON.
2. Send one or more frame type `2` chunks.
3. Send `audio_stop` to interrupt/clear queue.

Device emits JSON events with shape:

```json
{
  "type":"event",
  "event":"hello",
  "device":"waveshare-speaker-2fa9",
  "vpn_ip":"100.x.y.z",
  "wifi":true,
  "tailnet":true,
  "controller":true,
  "alarm_active":false,
  "scheduled":"07:30",
  "volume":40
}
```
