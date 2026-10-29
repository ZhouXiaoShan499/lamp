#pragma once
// IR learn / transmit front-end.
//
// Learning deliberately captures twice and compares: a single capture often
// misses the first mark because of power ripple or a shaky hand, and silently
// storing a truncated frame is worse than asking the user to press again.

#include <Arduino.h>

#include "config.h"
#include "ir_store.h"

namespace irengine {

enum class LearnState : uint8_t { Idle, WaitFirst, WaitSecond };

enum class LearnError : uint8_t {
    None = 0,
    Timeout,
    TooShort,
    TooLong,
    Unstable,
    SaveFailed,
};

// Called when a learn attempt finishes. `len` is the stored frame length.
using LearnHandler = void (*)(uint8_t slot, bool ok, LearnError error, uint16_t len);

void begin();
void setHandler(LearnHandler handler);

void startLearn(uint8_t slot);
void cancelLearn();
bool isLearning();
uint8_t learningSlot();
uint32_t learnRemainingMs();

// Call from loop() as often as possible. Never blocks.
void poll();

// Loads the slot and transmits it (blocking: a few hundred milliseconds).
bool send(uint8_t slot);
bool hasCode(uint8_t slot);

const char* errorText(LearnError error);
const char* stateText(LearnState state);

}  // namespace irengine