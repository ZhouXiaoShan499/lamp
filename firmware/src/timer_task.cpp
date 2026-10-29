#include "timer_task.h"

#include <Preferences.h>

namespace timers {
namespace {

Preferences prefs;
Task tasks[TASK_MAX];
uint8_t used = 0;           // number of allocated slots (id == index)
bool epochValid = false;
uint32_t epochBase = 0;
uint32_t millisBase = 0;
uint32_t lastCheckMs = 0;
FireHandler handler = nullptr;

constexpr const char* kKeyTasks = "tasks";
constexpr const char* kKeyUsed = "used";

void persist() {
    prefs.putBytes(kKeyTasks, tasks, sizeof(tasks));
    prefs.putUChar(kKeyUsed, used);
}

// Monotonic seconds: real epoch when synced, otherwise seconds since boot.
// Countdown tasks work in both modes; daily/once tasks need a real clock.
uint32_t nowSeconds() {
    if (epochValid) return epochBase + (millis() - millisBase) / 1000UL;
    return millis() / 1000UL;
}

void fire(Task& task) {
    task.last_fire = nowSeconds();
    if (task.type != TypeDaily) task.enabled = 0;
    Serial.printf("[timer] fire id=%u type=%s action=0x%02x slot=%u\n", static_cast<unsigned>(task.id),
                  typeName(task.type), task.action, static_cast<unsigned>(task.ir_slot));
    persist();
    if (handler != nullptr) handler(task);
}

}  // namespace

const char* typeName(uint8_t type) {
    switch (type) {
        case TypeCountdown: return "countdown";
        case TypeDaily: return "daily";
        case TypeOnce: return "once";
        default: return "unknown";
    }
}

void begin() {
    prefs.begin(NVS_NAMESPACE, false);
    const size_t got = prefs.getBytesLength(kKeyTasks);
    if (got == sizeof(tasks)) {
        prefs.getBytes(kKeyTasks, tasks, sizeof(tasks));
        used = prefs.getUChar(kKeyUsed, 0);
        if (used > TASK_MAX) used = 0;
    } else {
        memset(tasks, 0, sizeof(tasks));
        used = 0;
    }

    // Without a real clock a stored countdown deadline is meaningless after a
    // reboot, so re-arm it with its original duration instead of firing late
    // (or never). Daily/once tasks simply wait for a time sync.
    for (uint8_t i = 0; i < used; ++i) {
        if (tasks[i].type == TypeCountdown && tasks[i].enabled) {
            tasks[i].deadline_epoch = tasks[i].duration_s;
            tasks[i].missed = 1;  // "re-armed after power loss", surfaced in the UI
        }
    }
    Serial.printf("[timer] loaded %u task(s)\n", static_cast<unsigned>(used));
}

void setHandler(FireHandler value) { handler = value; }

void setEpoch(uint32_t epoch_utc) {
    const uint32_t oldNow = nowSeconds();
    epochValid = true;
    epochBase = epoch_utc;
    millisBase = millis();
    const uint32_t newNow = nowSeconds();

    // Preserve the remaining time of every running countdown, whether we are
    // jumping from uptime-seconds to a real epoch or just re-syncing the clock.
    for (uint8_t i = 0; i < used; ++i) {
        Task& task = tasks[i];
        if (task.type != TypeCountdown || !task.enabled || task.deadline_epoch == 0) continue;
        int32_t remaining = static_cast<int32_t>(task.deadline_epoch - oldNow);
        if (remaining < 0) remaining = 0;
        task.deadline_epoch = newNow + static_cast<uint32_t>(remaining);
    }
    persist();
    Serial.printf("[timer] epoch=%lu synced\n", static_cast<unsigned long>(epoch_utc));
}

uint32_t epoch() { return epochValid ? nowSeconds() : 0; }
bool timeValid() { return epochValid; }

uint8_t addCountdown(uint32_t seconds, uint8_t action, uint16_t ir_slot) {
    if (seconds < TASK_MIN_DURATION_S) seconds = TASK_MIN_DURATION_S;
    if (seconds > TASK_MAX_DURATION_S) seconds = TASK_MAX_DURATION_S;
    if (used >= TASK_MAX) return 0xFF;

    Task& task = tasks[used];
    memset(&task, 0, sizeof(task));
    task.id = used;
    task.type = TypeCountdown;
    task.days = EveryDay;
    task.action = action;
    task.ir_slot = ir_slot;
    task.duration_s = seconds;
    task.deadline_epoch = nowSeconds() + seconds;
    task.enabled = 1;
    ++used;
    persist();
    Serial.printf("[timer] add countdown id=%u %lus action=0x%02x\n", static_cast<unsigned>(task.id),
                  static_cast<unsigned long>(seconds), action);
    return task.id;
}

uint8_t addDaily(uint16_t minute_of_day, uint8_t days, uint8_t action, uint16_t ir_slot) {
    if (minute_of_day >= 1440) return 0xFF;
    if (days == 0) days = EveryDay;
    if (used >= TASK_MAX) return 0xFF;

    Task& task = tasks[used];
    memset(&task, 0, sizeof(task));
    task.id = used;
    task.type = TypeDaily;
    task.days = days;
    task.action = action;
    task.ir_slot = ir_slot;
    task.minute_of_day = minute_of_day;
    task.enabled = 1;
    ++used;
    persist();
    Serial.printf("[timer] add daily id=%u %02u:%02u days=0x%02x\n", static_cast<unsigned>(task.id),
                  minute_of_day / 60, minute_of_day % 60, days);
    return task.id;
}

uint8_t addOnce(uint32_t epoch_at, uint8_t action, uint16_t ir_slot) {
    if (used >= TASK_MAX) return 0xFF;
    Task& task = tasks[used];
    memset(&task, 0, sizeof(task));
    task.id = used;
    task.type = TypeOnce;
    task.days = EveryDay;
    task.action = action;
    task.ir_slot = ir_slot;
    task.deadline_epoch = epoch_at;
    task.enabled = 1;
    ++used;
    persist();
    return task.id;
}

bool removeTask(uint8_t id) {
    if (id >= used) return false;
    for (uint8_t i = id; i + 1 < used; ++i) tasks[i] = tasks[i + 1];
    --used;
    memset(&tasks[used], 0, sizeof(Task));
    for (uint8_t i = 0; i < used; ++i) tasks[i].id = i;
    persist();
    return true;
}

bool setEnabled(uint8_t id, bool enabled) {
    if (id >= used) return false;
    tasks[id].enabled = enabled ? 1 : 0;
    persist();
    return true;
}

void clearAll() {
    memset(tasks, 0, sizeof(tasks));
    used = 0;
    persist();
}

uint8_t count() { return used; }
const Task* all() { return tasks; }
const Task* find(uint8_t id) { return (id < used) ? &tasks[id] : nullptr; }

void tick(uint32_t now_ms) {
    if (now_ms - lastCheckMs < 250) return;
    lastCheckMs = now_ms;

    const uint32_t now = nowSeconds();
    for (uint8_t i = 0; i < used; ++i) {
        Task& task = tasks[i];
        if (!task.enabled) continue;

        if (task.type == TypeCountdown) {
            if (task.deadline_epoch != 0 && now >= task.deadline_epoch) fire(task);
            continue;
        }

        if (!epochValid) continue;  // daily/once need a real clock

        if (task.type == TypeOnce) {
            if (now >= task.deadline_epoch) fire(task);
            continue;
        }

        // Daily: same minute-of-day, allowed weekday, at most once per 60 s.
        if (task.last_fire != 0 && (now - task.last_fire) < 60) continue;
        const uint32_t minute_of_day = (now / 60) % 1440;
        const uint8_t dow = static_cast<uint8_t>(((now / 86400UL) + 4) % 7);  // 1970-01-01 = Thu
        const uint8_t mask = static_cast<uint8_t>(1U << dow);
        if ((task.days & mask) && minute_of_day == task.minute_of_day) fire(task);
    }
}

String describe(const Task& task) {
    String out;
    out.reserve(96);
    out += "#";
    out += String(static_cast<unsigned>(task.id));
    out += " ";
    out += typeName(task.type);
    out += task.enabled ? " on" : " off";
    out += " act=0x";
    if (task.action < 0x10) out += "0";
    out += String(task.action, HEX);
    if (task.type == TypeDaily) {
        char buf[24];
        snprintf(buf, sizeof(buf), " %02u:%02u d=0x%02x", task.minute_of_day / 60,
                 task.minute_of_day % 60, task.days);
        out += buf;
        if (!timeValid()) out += " (needs time sync)";
    } else if (task.type == TypeCountdown) {
        out += " ";
        out += String(static_cast<unsigned long>(task.duration_s));
        out += "s";
        if (task.missed) out += " (re-armed)";
    } else {
        out += " at ";
        out += String(static_cast<unsigned long>(task.deadline_epoch));
    }
    if (task.action & ActionIr) {
        out += " slot=";
        out += String(static_cast<unsigned>(task.ir_slot));
    }
    return out;
}

}  // namespace timers