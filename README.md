# ESP32-S3 Tailnet Speaker

Custom ESP-IDF firmware for Waveshare ESP32-S3 AI Smart Speaker board.
It joins a Tailscale tailnet directly via MicroLink, then connects to a server
on the tailnet for remote alarms, LEDs, tones, and PCM audio playback.

## Current features

- ESP32-S3 native MicroLink/Tailscale-compatible client.
- Tailnet TCP framed protocol.
- Remote commands: status, LED color, volume, tone, PCM audio stream, alarm start/stop, schedule set.
- Local 24h math alarm fallback.
- ES8311 speaker playback on Waveshare board pins.
- WS2812 ring LEDs on GPIO38.
- LAN status page on normal Wi-Fi IP.

Mic streaming, wake word, and barge-in are planned next, not implemented yet.

## Setup

```bash
git clone --recurse-submodules <this-repo>
cd esp32-s3-tailnet-speaker
cp sdkconfig.credentials.example sdkconfig.credentials
$EDITOR sdkconfig.credentials
```

Set:

- `CONFIG_ML_WIFI_SSID`
- `CONFIG_ML_WIFI_PASSWORD`
- `CONFIG_ML_TAILSCALE_AUTH_KEY`
- `CONFIG_SPEAKER_SERVER_HOST` as your server `100.x.x.x` or MagicDNS name
- `CONFIG_ML_PRIORITY_PEER_IP` preferably same server `100.x.x.x`

Build:

```bash
. /path/to/esp-idf/export.sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.credentials" build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## Server

On your tailnet server:

```bash
python3 server/tailnet_speaker_server.py --host 0.0.0.0 --port 5055
```

Interactive commands:

```text
status
led 0 0 80
tone 3 440
volume 45
alarm
schedule 07:30
wav /path/to/mono_24k_s16.wav
stop
```

Generate test WAV:

```text
genwav /tmp/mellow.wav 3 440
wav /tmp/mellow.wav
```

## Security

- Tailscale auth key and Wi-Fi password stay in `sdkconfig.credentials`, ignored by git.
- Tailnet traffic is encrypted by MicroLink/WireGuard.
- No public endpoint, no gateway, no router port forward required.

## Hardware pins

- LED ring: GPIO38, WS2812/GRB, 7 LEDs
- I2C: SDA GPIO11, SCL GPIO10
- Speaker amp: TCA9555 IO8 high
- I2S speaker codec ES8311:
  - MCLK GPIO12
  - BCLK GPIO13
  - WS GPIO14
  - DOUT GPIO16
