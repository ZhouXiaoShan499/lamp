#pragma once
// Dual-channel (warm/cool) LED control with perceptual dimming.
//
// Brightness is gamma-corrected: linear PWM looks like it "jumps" between 5 %
// and 20 %, while a 2.2 gamma ramp feels even. Colour temperature snaps to
// one of five calibrated presets - real CCT interpolation needs per-unit
// calibration data we do not have at MVP stage.

#include <Arduino.h>

#include "config.h"

namespace lighting {

enum class Preset : uint8_t { Study, Office, Rest, Night, Manual };

void begin();

void setBrightness(uint8_t pct);   // 0..100 (0 turns the lamp off)
void setCct(uint16_t kelvin);      // snaps to the nearest calibrated preset
void setPreset(Preset preset);
void toggle();
void tick(uint32_t now);

// Temporary global dim while a servo moves or IR is being transmitted, so the
// 5 V rail keeps its headroom (see docs/03 section 4).
void setScale(float scale);        // 0.0 .. 1.0
void clearScale();

uint8_t brightness();              // target brightness in percent
uint16_t cct();                    // target colour temperature in kelvin
bool isOn();
Preset preset();
const char* presetName(Preset value);

}  // namespace lighting