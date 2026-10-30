"""不接硬件也能把闭环测通：模拟 ESP32 的 TCP 客户端 / 模拟浏览器 WebSocket。

用法：
    python server/test_client.py ws  "用一句话介绍你自己"
    python server/test_client.py tcp --wav server/test_zh.wav
    python server/test_client.py tcp --pcm server/test_zh.raw
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import sys
import wave

sys.stdout.reconfigure(encoding="utf-8")


async def run_ws(question: str):
    import websockets

    async with websockets.connect("ws://127.0.0.1:8080/ws") as ws:
        await ws.send(json.dumps({"type": "ask", "text": question}))
        while True:
            data = json.loads(await asyncio.wait_for(ws.recv(), timeout=300))
            if data["type"] == "token":
                print(data["text"], end="", flush=True)
            elif data["type"] == "done":
                print("\n[DONE]")
                return
            else:
                print("\n[%s] %s" % (data["type"], data.get("text", "")))


def load_wav(path: str) -> bytes:
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1, "需要单声道"
        assert w.getsampwidth() == 2, "需要 16-bit"
        rate = w.getframerate()
        pcm = w.readframes(w.getnframes())
    if rate != 16000:
        raise SystemExit("示例 wav 需要 16 kHz，当前 %d" % rate)
    return pcm


async def run_tcp(pcm: bytes):
    reader, writer = await asyncio.open_connection("127.0.0.1", 8766)
    chunk = 3200  # 100 ms
    for i in range(0, len(pcm), chunk):
        piece = pcm[i : i + chunk]
        writer.write(("PCM %d\n" % len(piece)).encode() + piece)
        await writer.drain()
        await asyncio.sleep(0.05)  # 模拟实时采集节奏
    writer.write(b"END\n")
    await writer.drain()

    while True:
        line = await asyncio.wait_for(reader.readline(), timeout=300)
        if not line:
            print("[连接断开]")
            break
        text = line.decode("utf-8", "replace").rstrip("\n")
        print(text)
        if text.startswith(("DONE", "ERR")):
            break
    writer.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["ws", "tcp"])
    ap.add_argument("question", nargs="?", default="用一句话介绍你自己")
    ap.add_argument("--wav")
    ap.add_argument("--pcm")
    args = ap.parse_args()

    if args.mode == "ws":
        asyncio.run(run_ws(args.question))
    else:
        if args.pcm:
            pcm = open(args.pcm, "rb").read()
        elif args.wav:
            pcm = load_wav(args.wav)
        else:
            raise SystemExit("tcp 模式需要 --wav 或 --pcm")
        print("发送 %d 字节 PCM（%.2f 秒）" % (len(pcm), len(pcm) / 32000.0))
        asyncio.run(run_tcp(pcm))


if __name__ == "__main__":
    main()
