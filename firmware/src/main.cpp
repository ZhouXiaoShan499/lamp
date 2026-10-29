// LAMP-1 MVP firmware: IR learn/replay + dual-CCT lighting + timers + motion.
// Single non-blocking loop; no delay() outside of short hardware sequences.
// See docs/03-技术方案.md for the design rationale behind each module.

#include <Arduino.h>

#include "config.h"
#include "ir_engine.h"
#include "ir_store.h"
#include "lighting.h"
#include "motion.h"
#include "pwm.h"
#include "timer_task.h"
#include "ui.h"

#if defined(LAMP_WIFI_SSID) && defined(LAMP_WIFI_PASS)
#include <WiFi.h>
#include <time.h>
#define LAMP_WIFI_ENABLED 1
#else
#define LAMP_WIFI_ENABLED 0
#endif

namespace {

// ---------------------------------------------------------------- buttons ---
enum class BtnEvent : uint8_t { None, Short, Long };

struct Button {
    uint8_t pin;
    bool stable;
    bool lastRead;
    bool longFired;
    uint32_t lastChange;
    uint32_t pressedAt;
};

Button btnMode{PIN_BTN_MODE, false, false, false, 0, 0};
Button btnLight{PIN_BTN_LIGHT, false, false, false, 0, 0};
Button btnLearn{PIN_BTN_LEARN, false, false, false, 0, 0};

void buttonInit(Button& button, uint8_t pin) {
    button.pin = pin;
    pinMode(pin, INPUT_PULLUP);
    button.stable = (digitalRead(pin) == LOW);
    button.lastRead = button.stable;
    button.longFired = false;
    button.lastChange = millis();
    button.pressedAt = 0;
}

BtnEvent buttonPoll(Button& button, uint32_t now, uint16_t longMs) {
    const bool raw = (digitalRead(button.pin) == LOW);  // active low
    if (raw != button.lastRead) {
        button.lastRead = raw;
        button.lastChange = now;
    }
    if ((now - button.lastChange) >= BTN_DEBOUNCE_MS && raw != button.stable) {
        button.stable = raw;
        if (button.stable) {
            button.pressedAt = now;
            button.longFired = false;
        } else {
            if (button.longFired) {
                button.longFired = false;
                return BtnEvent::None;
            }
            return BtnEvent::Short;
        }
    }
    if (button.stable && !button.longFired && (now - button.pressedAt) >= longMs) {
        button.longFired = true;
        return BtnEvent::Long;
    }
    return BtnEvent::None;
}

// ------------------------------------------------------------------ buzzer ---
uint8_t beepLeft = 0;
bool beepOn = false;
uint32_t beepPhaseEnd = 0;

void beepRequest(uint8_t count) {
    if (count > 3) count = 3;
    beepLeft = count;
    beepOn = false;
    beepPhaseEnd = 0;
}

void beepTick(uint32_t now) {
    if (beepLeft == 0) {
        if (beepOn) {
            pwm::write(PIN_BUZZER, CH_BUZZER, 0);
            beepOn = false;
        }
        return;
    }
    if (now < beepPhaseEnd) return;
    if (beepOn) {
        pwm::write(PIN_BUZZER, CH_BUZZER, 0);
        beepOn = false;
        --beepLeft;
        beepPhaseEnd = now + 70;
    } else {
        pwm::write(PIN_BUZZER, CH_BUZZER, 1u << 9);  // ~50 % duty at 4 kHz
        beepOn = true;
        beepPhaseEnd = now + 80;
    }
}

// ------------------------------------------------------------- learn session ---
constexpr uint8_t kLearnSlots = 6;
bool learnActive = false;
uint8_t learnBase = 0;
uint8_t learnSlot = 0;

void exitLearn(const char* reason) {
    if (!learnActive) return;
    learnActive = false;
    irengine::cancelLearn();
    ui::setLearnHint("", -1);
    ui::logf("{\"ev\":\"learn_exit\",\"reason\":\"%s\"}", reason);
    ui::setStatus("ready");
}

void enterLearn(uint8_t base_slot) {
    if (base_slot + kLearnSlots > irstore::kSlots) {
        base_slot = irstore::kSlots - kLearnSlots;
    }
    learnBase = base_slot;
    learnSlot = base_slot;
    learnActive = true;
    char hint[24];
    snprintf(hint, sizeof(hint), "LEARN %u/%u", 1, kLearnSlots);
    ui::setLearnHint(hint, 0);
    ui::setStatus("learning");
    irengine::startLearn(learnSlot);
    motion::request(motion::Move::Nod);  // "I am listening"
}

void onLearnResult(uint8_t slot, bool ok, irengine::LearnError error, uint16_t len) {
    ui::logf("{\"ev\":\"learn_result\",\"slot\":%u,\"ok\":%d,\"reason\":\"%s\",\"len\":%u}",
             static_cast<unsigned>(slot), ok ? 1 : 0, irengine::errorText(error),
             static_cast<unsigned>(len));

    if (ok) {
        beepRequest(1);
        const uint8_t next = static_cast<uint8_t>(slot + 1);
        if (next >= learnBase + kLearnSlots) {
            ui::setLearnHint("SAVED", 100);
            exitLearn("complete");
            motion::request(motion::Move::Nod);
            return;
        }
        learnSlot = next;
        char hint[24];
        snprintf(hint, sizeof(hint), "LEARN %u/%u", static_cast<unsigned>(next - learnBase + 1),
                 static_cast<unsigned>(kLearnSlots));
        ui::setLearnHint(hint, static_cast<int8_t>(((next - learnBase) * 100) / kLearnSlots));
        irengine::startLearn(next);
        return;
    }

    if (error == irengine::LearnError::Timeout) {
        exitLearn("timeout");
        return;
    }

    beepRequest(2);
    ui::setLearnHint(irengine::errorText(error), -1);
    // Retry the same slot: the most common causes are distance, angle and a
    // weak remote battery, all of which the user can fix in two seconds.
    irengine::startLearn(slot);
}

// --------------------------------------------------------------- night mode ---
bool isNightNow() {
    if (!timers::timeValid()) return false;
    const uint32_t minuteOfDay = (timers::epoch() / 60) % 1440;
    return minuteOfDay >= (22 * 60) || minuteOfDay < (7 * 60);
}

// ------------------------------------------------------------------- actions ---
bool sendIrSlot(uint16_t slot) {
    // Drop the LED to 70 % while the IR burst runs: 240 mA on top of a 1 A LED
    // load dips the 5 V rail and can brown-out the ESP32 (docs/03 section 4.1).
    const bool wasOn = lighting::isOn();
    if (wasOn) {
        lighting::setScale(0.7f);
        lighting::flush();
    }
    const bool ok = irengine::send(static_cast<uint8_t>(slot));
    if (wasOn) {
        lighting::clearScale();
        lighting::flush();
    }
    ui::logf("{\"ev\":\"ir_send\",\"slot\":%u,\"ok\":%d}", static_cast<unsigned>(slot), ok ? 1 : 0);
    if (ok) beepRequest(1);
    return ok;
}

void applyAction(uint8_t action, uint16_t irSlot) {
    if (action & timers::ActionLightOn) lighting::setBrightness(70);
    if (action & timers::ActionLightOff) lighting::setBrightness(0);
    if (action & timers::ActionIr) sendIrSlot(irSlot);
    if (action & timers::ActionMotion) motion::request(motion::Move::Alert);
}

void onTimerFire(const timers::Task& task) {
    ui::logf("{\"ev\":\"timer_fire\",\"id\":%u,\"action\":%u,\"night\":%d}",
             static_cast<unsigned>(task.id), task.action, isNightNow() ? 1 : 0);
    motion::setNightMode(isNightNow());
    applyAction(task.action, task.ir_slot);
    beepRequest(1);
}

// ------------------------------------------------------------------- buttons ---
void onLightButton(BtnEvent event) {
    if (event == BtnEvent::Short) {
        static const uint8_t steps[] = {25, 50, 75, 100};
        const uint8_t current = lighting::brightness();
        uint8_t next = 60;
        for (uint8_t i = 0; i < sizeof(steps); ++i) {
            if (steps[i] > current) {
                next = steps[i];
                break;
            }
        }
        if (current >= 100) next = 25;
        if (!lighting::isOn()) next = 60;
        lighting::setBrightness(next);
        ui::logf("{\"ev\":\"light_set\",\"pct\":%u,\"src\":\"button\"}", next);
    } else if (event == BtnEvent::Long) {
        static const lighting::Preset cycle[] = {lighting::Preset::Study, lighting::Preset::Office,
                                                 lighting::Preset::Rest, lighting::Preset::Night};
        static uint8_t index = 0;
        index = static_cast<uint8_t>((index + 1) % (sizeof(cycle) / sizeof(cycle[0])));
        lighting::setPreset(cycle[index]);
        ui::logf("{\"ev\":\"light_preset\",\"name\":\"%s\"}", lighting::presetName(cycle[index]));
    }
}

void onModeButton(BtnEvent event) {
    if (event == BtnEvent::Short) {
        sendIrSlot(0);  // slot 0 is the primary key, normally the AC power toggle
        motion::request(motion::Move::Nod);
    } else if (event == BtnEvent::Long) {
        motion::setMuted(!motion::muted());
        ui::logf("{\"ev\":\"motion_mute\",\"muted\":%d}", motion::muted() ? 1 : 0);
        ui::setStatus(motion::muted() ? "motion muted" : "motion on");
        beepRequest(1);
    }
}

void onLearnButton(BtnEvent event) {
    if (learnActive) {
        if (event == BtnEvent::Short) {
            exitLearn("user");
        }
        return;
    }
    if (event == BtnEvent::Long) {
        // Start on the first slot that is still empty, so re-learning does not
        // silently overwrite a working key.
        uint8_t base = 0;
        while (base < irstore::kSlots && irstore::exists(base)) ++base;
        if (base >= irstore::kSlots - kLearnSlots) base = 0;
        enterLearn(base);
    } else if (event == BtnEvent::Short) {
        ui::setStatus(irstore::usedSlots() == 0 ? "no keys: hold LEARN" : "ready");
    }
}

void handleButtons(uint32_t now) {
    const BtnEvent light = buttonPoll(btnLight, now, BTN_LONGPRESS_MS);
    if (light != BtnEvent::None) onLightButton(light);

    const BtnEvent mode = buttonPoll(btnMode, now, BTN_LONGPRESS_MS);
    if (mode != BtnEvent::None) onModeButton(mode);

    const BtnEvent learn = buttonPoll(btnLearn, now, BTN_LEARN_LONGPRESS_MS);
    if (learn != BtnEvent::None) onLearnButton(learn);
}

// --------------------------------------------------------------------- status ---
void statusTick(uint32_t now) {
    static uint32_t lastBlink = 0;
    static bool ledOn = false;
    if (learnActive) {
        if (now - lastBlink >= 200) {
            lastBlink = now;
            ledOn = !ledOn;
            digitalWrite(PIN_STATUS_LED, ledOn ? HIGH : LOW);
        }
    } else if (ledOn) {
        ledOn = false;
        digitalWrite(PIN_STATUS_LED, LOW);
    }
    motion::setNightMode(isNightNow());
}

#if LAMP_WIFI_ENABLED
void wifiTick(uint32_t now) {
    static uint32_t lastAttempt = 0;
    static uint32_t lastSync = 0;
    static bool started = false;

    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastAttempt >= 10000) {
            lastAttempt = now;
            WiFi.begin(LAMP_WIFI_SSID, LAMP_WIFI_PASS);  // non-blocking
            if (!started) {
                started = true;
                ui::logf("{\"ev\":\"wifi_begin\",\"ssid\":\"%s\"}", LAMP_WIFI_SSID);
            }
        }
        return;
    }
    if (now - lastSync >= 5000) {
        lastSync = now;
        const time_t value = time(nullptr);
        if (value > 1600000000L) {
            const uint32_t epoch = static_cast<uint32_t>(value);
            if (!timers::timeValid() || (timers::epoch() > epoch + 5) || (timers::epoch() + 5 < epoch)) {
                timers::setEpoch(epoch);
            }
        }
    }
}
#endif