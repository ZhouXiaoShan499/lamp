#pragma once
// Serial command shell + event log + optional OLED status line.
//
// The serial console is not a debug afterthought: it is the only way to
// diagnose a unit in the field without an app, and it is what the automated
// acceptance tests drive. Keep every user-facing action reachable from it.

#include <Arduino.h>

#include "config.h"

namespace ui {

// argv[0] is the command, argv[argc] is guaranteed to be nullptr.
using CommandHandler = void (*)(int argc, char** argv);

void begin();
void tick(uint32_t now_ms);

void setCommandHandler(CommandHandler handler);

// Mirrors to Serial and appends one JSON line to /log/events.log.
void logf(const char* format, ...);

// Short line shown on the OLED (and in `status`).
void setStatus(const char* text);
const char* status();

// Learn-progress feedback: "LEARN 2/6" + a bar.
void setLearnHint(const char* text, int8_t progress_pct);

void printHelp();

}  // namespace ui