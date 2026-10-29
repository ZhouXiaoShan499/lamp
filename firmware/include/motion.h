#pragma once
// Two-axis motion (tilt + yaw) built on LEDC-driven hobby servos.
//
// Design rules that matter for the product (docs/03 section 7.2):
//   * one servo moves at a time (power interlock), the caller dims the lamp
//   * movement is eased, never a step change
//   * PWM is released once the head settles, otherwise the servo hums forever
//   * muted / night mode degrades to "no motion", the lamp still works

#include <Arduino.h>

#include "config.h"

namespace motion {

enum class Move : uint8_t { Home, Nod, TurnLeft, TurnRight, Alert };

void begin();

// Returns false when the move was suppressed (muted, night mode, or no servos).
bool request(Move move);
void tick(uint32_t now_ms);

bool busy();
void setMuted(bool muted);
bool muted();

// Night mode is set by the caller from the real clock; when active, motion is
// suppressed unless the user explicitly triggered it from the buttons.
void setNightMode(bool night);
bool nightMode();

// Releases both servos immediately (stall / hand-forced protection).
void emergencyStop();

const char* moveName(Move move);

}  // namespace motion