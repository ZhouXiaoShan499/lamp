#pragma once
// Countdown / daily / one-shot task scheduling.
//
// Time source policy (see docs/03 section 8.1):
//   * epoch valid  -> all task types allowed
//   * epoch invalid -> countdown only; daily tasks are reported as unavailable
//     instead of silently drifting.
// A countdown that cannot be evaluated after a power cycle (no RTC, no network)
// is fired once at boot and flagged, rather than dropped silently.

#include <Arduino.h>

#include "config.h"

namespace timers {

enum Type : uint8_t {
    TypeCountdown = 0,
    TypeDaily = 1,
    TypeOnce = 2,
};

enum Action : uint8_t {
    ActionLightOn = 0x01,
    ActionLightOff = 0x02,
    ActionIr = 0x04,
    ActionMotion = 0x08,
};

enum Days : uint8_t {
    Mon = 0x01,
    Tue = 0x02,
    Wed = 0x04,
    Thu = 0x08,
    Fri = 0x10,
    Sat = 0x20,
    Sun = 0x40,
    EveryDay = 0x7F,
};

struct Task {
    uint8_t id;
    uint8_t type;
    uint8_t days;
    uint8_t action;
    uint16_t ir_slot;
    uint16_t minute_of_day;   // 0..1439, daily / once
    uint32_t duration_s;      // countdown
    uint32_t deadline_epoch;  // countdown: absolute; once: absolute
    uint32_t last_fire;       // epoch of last trigger, for de-duplication
    uint8_t enabled;
    uint8_t missed;           // fired at boot because its deadline passed unobserved
};

using FireHandler = void (*)(const Task& task);

void begin();
void setHandler(FireHandler handler);

void setEpoch(uint32_t epoch_utc);
uint32_t epoch();
bool timeValid();

uint8_t addCountdown(uint32_t seconds, uint8_t action, uint16_t ir_slot);
uint8_t addDaily(uint16_t minute_of_day, uint8_t days, uint8_t action, uint16_t ir_slot);
uint8_t addOnce(uint32_t epoch_at, uint8_t action, uint16_t ir_slot);
bool removeTask(uint8_t id);
bool setEnabled(uint8_t id, bool enabled);
void clearAll();

uint8_t count();
const Task* all();
const Task* find(uint8_t id);

// Call every 250 ms.
void tick(uint32_t now_ms);

const char* typeName(uint8_t type);
String describe(const Task& task);

}  // namespace timers