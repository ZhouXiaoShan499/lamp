#pragma once
// ESP32 <-> PC voice link (task 3 of the voice spike).
//
//   V829 PDM mic -> 16 kHz s16le frames -> WebSocket (binary)
//                -> PC: Vosk ASR -> Ollama/DeepSeek -> text reply -> Serial
//
// The module owns the VAD state machine and streams only the spoken part of an
// utterance; the PC keeps the audio buffer and runs ASR when audio_end arrives.
// Configuration (WiFi + server address) comes from include/secrets.h.

#include <Arduino.h>

#include "config.h"

namespace voice {

void begin();
// Call from loop() as often as possible. Never blocks longer than one frame.
void tick(uint32_t now_ms);

bool connected();
bool listening();
bool monitor();

// Prints the RMS level of every frame (task 1 verification: breath/clap test).
void setMonitor(bool on);

// Records the next utterance even if it is quieter than MIC_VAD_RMS.
void requestListen();

// Sends plain text to the PC (task 2 verification without the mic).
void sendText(const char* text);

void printStatus();

}  // namespace voice