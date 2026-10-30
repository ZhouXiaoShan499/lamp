#!/usr/bin/env python3
"""Run the local Vosk model on a wav file (voice task 3, ASR half only).

    python asr_test.py                       # uses server/test_zh.wav
    python asr_test.py path/to/record.wav

Accepted input: any PCM wav; channels/width/rate are converted to 16 kHz mono
s16le with audioop, so a phone recording works without ffmpeg.
"""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import audioop  # deprecated in 3.13, present in 3.11
import vosk

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL = BASE_DIR / "models" / "vosk-model-small-cn-0.22"
DEFAULT_WAV = BASE_DIR / "test_zh.wav"
SAMPLE_RATE = 16000


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)
    print(f"input : {path.name} {channels}ch {width * 8}bit {rate}Hz {frames / rate:.2f}s")
    if width != 2:
        raw = audioop.lin2lin(raw, width, 2)
    if channels == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    elif channels != 1:
        raise SystemExit(f"unsupported channel count: {channels}")
    if rate != SAMPLE_RATE:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, SAMPLE_RATE, None)
        print(f"resample: {rate} -> {SAMPLE_RATE} Hz")
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description="Vosk ASR smoke test")
    parser.add_argument("wav", nargs="?", default=str(DEFAULT_WAV))
    parser.add_argument("--model", default=str(DEFAULT_MODEL))
    args = parser.parse_args()

    model_path = Path(args.model)
    if not model_path.exists():
        raise SystemExit(f"model not found: {model_path}")

    vosk.SetLogLevel(-1)
    print(f"model : {model_path}")
    model = vosk.Model(str(model_path))
    recognizer = vosk.KaldiRecognizer(model, SAMPLE_RATE)
    recognizer.SetWords(True)

    pcm = load_pcm(Path(args.wav))
    chunk_size = 4000  # 125 ms
    for offset in range(0, len(pcm), chunk_size):
        if recognizer.AcceptWaveform(pcm[offset : offset + chunk_size]):
            text = json.loads(recognizer.Result()).get("text", "")
            if text:
                print(f"chunk : {text}")

    final = json.loads(recognizer.FinalResult())
    print(f"\ntext  : {final.get('text', '') or '(empty)'}")
    for word in final.get("result", []):
        print(f"  {word['start']:6.2f}-{word['end']:6.2f}s  {word['word']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())