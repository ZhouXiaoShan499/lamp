#pragma once
// INMP441 I2S 麦克风前端（注意：INMP441 是标准 I2S，不是 PDM）。
//
// 接线（模块丝印 -> ESP32-S3，面包板）：
//   VDD  -> 3V3        GND -> GND
//   L/R  -> GND（选左声道）
//   SCK  -> GPIO16     WS  -> GPIO18     SD -> GPIO17
//
// ESP32 做主模式：自己产生 BCLK/WS，麦克风把 24 bit 采样左对齐地推过来。
// 这里按 16 kHz / 16 bit / 单声道左槽读取，正好是 PC 端 Vosk 要的格式。

#include <Arduino.h>

#include "config.h"

namespace pdmmic {

// Opens the I2S RX channel. Safe to call repeatedly; returns false on error.
bool begin();
void end();
bool ready();

// Reads up to `count` samples. Returns the number actually read (0 on timeout).
size_t read(int16_t* samples, size_t count, uint32_t timeout_ms = 60);

// Root-mean-square level of a frame, DC removed. Use `mic test` to calibrate:
// a quiet room should sit well under MIC_VAD_RMS, speech at 20 cm far above it.
float rms(const int16_t* samples, size_t count);

// 硬件自检（`mic probe`）：采 1 秒并打印 min/max/均值/非零比例，
// 用来区分"没声音"和"读回来是常量"。约 1.5 秒。
void diagnose();

}  // namespace pdmmic
