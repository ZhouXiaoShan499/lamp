#!/usr/bin/env python3
"""Fake ESP32: exercise the whole PC chain without the board.

Sends test_zh.wav (or a text question) to voice_server.py over the same
WebSocket protocol the firmware uses, and prints every reply.

    python test_client_voice.py                    # wav -> Vosk -> Ollama
    python test_client_voice.py --text "你好"       # text -> Ollama
    python test_client_voice.py --url ws://192.168.1.100:8765
"""

from __future__ import annotations

import argparse
import asyncio
import json
import wave
from pathlib import Path

import audioop
import websockets

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_WAV = BASE_DIR / "test_zh.wav"


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        raw = wav.readframes(wav.getnframes())
    if width != 2:
        raw = audioop.lin2lin(raw, width, 2)
    if channels == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    elif channels != 1:
        raise SystemExit(f"unsupported channel count: {channels}")
    if rate != 16000:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, 16000, None)
    return raw


async def run(args) -> int:
    async with websockets.connect(args.url, max_size=2 * 1024 * 1024) as ws:
        ready = json.loads(await ws.recv())
        print(f"server: {json.dumps(ready, ensure_ascii=False)}")

        if args.text:
            await ws.send(json.dumps({"type": "text", "content": args.text}, ensure_ascii=False))
        else:
            pcm = load_pcm(Path(args.wav))
            await ws.send(
                json.dumps(
                    {
                        "type": "audio_start",
                        "sample_rate": 16000,
                        "format": "s16le",
                        "channels": 1,
                    }
                )
            )
            chunk = 3200  # 100 ms
            for offset in range(0, len(pcm), chunk):
                await ws.send(pcm[offset : offset + chunk])
            await ws.send(json.dumps({"type": "audio_end", "reason": "test_client"}))
            print(f"sent  : {len(pcm) / 32000:.2f}s of audio")

        while True:
            message = await asyncio.wait_for(ws.recv(), timeout=args.timeout)
            payload = json.loads(message)
            print(f"recv  : {json.dumps(payload, ensure_ascii=False)}")
            if payload.get("type") in ("reply", "error"):
                return 0 if payload.get("type") == "reply" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="WebSocket test client for voice_server.py")
    parser.add_argument("--url", default="ws://127.0.0.1:8765")
    parser.add_argument("--wav", default=str(DEFAULT_WAV))
    parser.add_argument("--text", default="", help="send a text question instead of audio")
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
