#!/usr/bin/env python3
"""Minimal controller for ESP32-S3 Tailnet Speaker.

Runs on a machine already connected to your tailnet. Bind to 0.0.0.0:5055
or to your Tailscale 100.x address. Device connects to this TCP server through
MicroLink.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import threading
import time
import wave
from pathlib import Path

FRAME_JSON = 1
FRAME_AUDIO = 2
FRAME_MIC = 3
MAX_FRAME = 1024 * 1024


def frame(ftype: int, payload: bytes) -> bytes:
    return struct.pack("!BI", ftype, len(payload)) + payload


class Client:
    def __init__(self, sock: socket.socket, addr: tuple[str, int]):
        self.sock = sock
        self.addr = addr
        self.lock = threading.Lock()
        self.alive = True

    def send_json(self, obj: dict) -> None:
        data = json.dumps(obj, separators=(",", ":")).encode()
        self.send_frame(FRAME_JSON, data)

    def send_audio(self, data: bytes) -> None:
        self.send_frame(FRAME_AUDIO, data)

    def send_frame(self, ftype: int, data: bytes) -> None:
        with self.lock:
            self.sock.sendall(frame(ftype, data))

    def close(self) -> None:
        self.alive = False
        try:
            self.sock.close()
        except OSError:
            pass


def recv_exact(sock: socket.socket, n: int) -> bytes | None:
    chunks = []
    got = 0
    while got < n:
        try:
            part = sock.recv(n - got)
        except OSError:
            return None
        if not part:
            return None
        chunks.append(part)
        got += len(part)
    return b"".join(chunks)


def recv_loop(client: Client) -> None:
    try:
        while client.alive:
            hdr = recv_exact(client.sock, 5)
            if hdr is None:
                break
            ftype, length = struct.unpack("!BI", hdr)
            if length > MAX_FRAME:
                print(f"bad frame len={length}")
                break
            payload = recv_exact(client.sock, length)
            if payload is None:
                break
            if ftype == FRAME_JSON:
                try:
                    obj = json.loads(payload.decode())
                except Exception as exc:  # noqa: BLE001
                    print(f"bad json from {client.addr}: {exc}")
                    continue
                print(f"event {client.addr}: {json.dumps(obj, ensure_ascii=False)}")
            elif ftype == FRAME_MIC:
                print(f"mic frame {len(payload)} bytes")
            else:
                print(f"frame type={ftype} bytes={len(payload)}")
    finally:
        client.close()
        print(f"device disconnected: {client.addr}")


def audio_stream_wav(client: Client, path: Path, chunk_ms: int = 80) -> None:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        rate = wav.getframerate()
        if channels != 1 or sample_width != 2 or rate != 24000:
            raise SystemExit(
                f"{path} must be WAV PCM signed 16-bit mono 24000 Hz; "
                f"got channels={channels} width={sample_width} rate={rate}"
            )
        frames_per_chunk = max(1, rate * chunk_ms // 1000)
        client.send_json({"cmd": "play_audio", "format": "pcm_s16le_24k_mono"})
        while True:
            data = wav.readframes(frames_per_chunk)
            if not data:
                break
            client.send_audio(data)
            time.sleep(chunk_ms / 1000.0)


def generate_tone_wav(path: Path, seconds: float = 2.0, freq: float = 440.0) -> None:
    import math

    rate = 24000
    amp = 6500
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            beat = i % rate
            env = 1.0 if beat < rate / 3 else 0.0
            v = int(math.sin(2 * math.pi * freq * i / rate) * amp * env)
            frames += struct.pack("<h", v)
        wav.writeframes(frames)


def command_help() -> None:
    print(
        "commands:\n"
        "  status\n"
        "  led R G B\n"
        "  tone [seconds] [freq]\n"
        "  volume 0-100\n"
        "  alarm\n"
        "  stop\n"
        "  schedule HH:MM\n"
        "  wav /path/to/mono_24k_s16.wav\n"
        "  genwav /tmp/tone.wav [seconds] [freq]\n"
        "  quit\n"
    )


def repl(client_getter) -> None:
    command_help()
    while True:
        try:
            line = input("speaker> ").strip()
        except EOFError:
            return
        if not line:
            continue
        parts = line.split()
        cmd = parts[0].lower()
        client = client_getter()
        if cmd in {"quit", "exit"}:
            return
        if cmd == "help":
            command_help()
            continue
        if client is None or not client.alive:
            print("no device connected")
            continue
        try:
            if cmd == "status":
                client.send_json({"cmd": "status_request"})
            elif cmd == "led" and len(parts) == 4:
                client.send_json({"cmd": "set_led", "r": int(parts[1]), "g": int(parts[2]), "b": int(parts[3])})
            elif cmd == "tone":
                sec = float(parts[1]) if len(parts) > 1 else 3.0
                freq = float(parts[2]) if len(parts) > 2 else 440.0
                client.send_json({"cmd": "play_tone", "seconds": sec, "frequency": freq})
            elif cmd == "volume" and len(parts) == 2:
                client.send_json({"cmd": "set_volume", "volume": int(parts[1])})
            elif cmd == "alarm":
                client.send_json({"cmd": "alarm_start"})
            elif cmd == "stop":
                client.send_json({"cmd": "alarm_stop"})
                client.send_json({"cmd": "audio_stop"})
            elif cmd == "schedule" and len(parts) == 2:
                client.send_json({"cmd": "schedule_set", "time": parts[1]})
            elif cmd == "wav" and len(parts) == 2:
                audio_stream_wav(client, Path(parts[1]))
            elif cmd == "genwav" and len(parts) >= 2:
                sec = float(parts[2]) if len(parts) > 2 else 2.0
                freq = float(parts[3]) if len(parts) > 3 else 440.0
                generate_tone_wav(Path(parts[1]), sec, freq)
                print(f"wrote {parts[1]}")
            else:
                command_help()
        except Exception as exc:  # noqa: BLE001
            print(f"error: {exc}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5055)
    args = parser.parse_args()

    current: Client | None = None
    current_lock = threading.Lock()

    def get_client() -> Client | None:
        with current_lock:
            return current

    def accept_loop() -> None:
        nonlocal current
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((args.host, args.port))
            srv.listen(1)
            print(f"listening on {args.host}:{args.port}")
            while True:
                sock, addr = srv.accept()
                client = Client(sock, addr)
                with current_lock:
                    if current is not None:
                        current.close()
                    current = client
                print(f"device connected: {addr}")
                threading.Thread(target=recv_loop, args=(client,), daemon=True).start()

    threading.Thread(target=accept_loop, daemon=True).start()
    repl(get_client)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
