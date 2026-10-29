#pragma once
// ---------------------------------------------------------------------------
// Compile-time configuration: pins, limits, feature flags.
// Every pin here must be re-checked against the real PCB before flashing:
// GPIO 0/3/45/46 are strapping pins, GPIO 26..32 are used by flash/PSRAM,
// GPIO 19/20 are USB D-/D+. Do not reuse them.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// ---- feature flags (override with build_flags) -----------------------------
#ifndef LAMP_USE_OLED
#define LAMP_USE_OLED 0
#endif
#ifndef LAMP_HAS_SERVOS
#define LAMP_HAS_SERVOS 1
#endif
#ifndef LAMP_HAS_VOICE
#define LAMP_HAS_VOICE 0
#endif

// ---- pins -----------------------------------------------------------------
constexpr uint8_t PIN_LED_WARM = 4;
constexpr uint8_t PIN_LED_COOL = 5;
constexpr uint8_t PIN_SERVO_TILT = 6;
constexpr uint8_t PIN_SERVO_YAW = 7;
constexpr uint8_t PIN_BUZZER = 8;
constexpr uint8_t PIN_IR_RECV = 9;
constexpr uint8_t PIN_IR_SEND = 10;
constexpr uint8_t PIN_OLED_SDA = 11;
constexpr uint8_t PIN_OLED_SCL = 12;
constexpr uint8_t PIN_BTN_MODE = 13;
constexpr uint8_t PIN_BTN_LIGHT = 14;
constexpr uint8_t PIN_BTN_LEARN = 15;
constexpr uint8_t PIN_VOICE_TX = 16;
constexpr uint8_t PIN_VOICE_RX = 17;
constexpr uint8_t PIN_STATUS_LED = 18;

// ---- LEDC channels (ESP32-C3/S3 low-speed group; one timer per group) ------
// channels 8 and 9 share timer 4 (both 4 kHz) -> OK
// channels 10 and 11 share timer 5 (both 50 Hz) -> OK
// channel 12 uses timer 6 (4 kHz buzzer)
constexpr uint8_t CH_LED_WARM = 8;
constexpr uint8_t CH_LED_COOL = 9;
constexpr uint8_t CH_SERVO_TILT = 10;
constexpr uint8_t CH_SERVO_YAW = 11;
constexpr uint8_t CH_BUZZER = 12;

constexpr uint32_t LED_PWM_HZ = 4000;
constexpr uint8_t LED_PWM_BITS = 12;   // 4096 steps
constexpr uint32_t SERVO_PWM_HZ = 50;
constexpr uint8_t SERVO_PWM_BITS = 14; // 16384 steps -> 1.22 us per tick

// ---- lighting -------------------------------------------------------------
constexpr uint8_t LIGHT_MIN_PCT = 1;
constexpr uint8_t LIGHT_MAX_PCT = 100;
constexpr uint16_t LIGHT_RAMP_MS = 600;   // full 0..100 % ramp time

// ---- infrared ------------------------------------------------------------
constexpr uint16_t IR_CAPTURE_BUFFER = 1024;  // must equal irstore::kMaxRawLen
constexpr uint8_t IR_CAPTURE_TIMEOUT_MS = 50; // IRrecv gap timeout
constexpr uint16_t IR_MIN_RAW_LEN = 20;
constexpr uint16_t IR_LEARN_TIMEOUT_MS = 15000;
constexpr uint8_t IR_TOLERANCE_PCT = 5;       // two captures must agree within 5 %
constexpr uint8_t IR_SEND_REPEATS = 2;
constexpr uint16_t IR_SEND_GAP_MS = 40;
constexpr uint8_t IR_BLOCK_RX_MS = 120;       // do not hear our own transmission

// ---- timers --------------------------------------------------------------
constexpr uint8_t TASK_MAX = 8;
constexpr uint16_t TASK_MIN_DURATION_S = 10;
constexpr uint32_t TASK_MAX_DURATION_S = 24UL * 3600UL;

// ---- motion --------------------------------------------------------------
constexpr float SERVO_TILT_MIN_DEG = 10.0f;
constexpr float SERVO_TILT_MAX_DEG = 55.0f;
constexpr float SERVO_TILT_HOME_DEG = 30.0f;
constexpr float SERVO_YAW_MIN_DEG = -60.0f;
constexpr float SERVO_YAW_MAX_DEG = 60.0f;
constexpr float SERVO_YAW_HOME_DEG = 0.0f;
constexpr float SERVO_STEP_DEG = 1.5f;    // per 20 ms tick
constexpr uint32_t MOTION_TICK_MS = 20;

// ---- buttons -------------------------------------------------------------
constexpr uint16_t BTN_DEBOUNCE_MS = 20;
constexpr uint16_t BTN_LONGPRESS_MS = 1200;
constexpr uint16_t BTN_LEARN_LONGPRESS_MS = 3000;

// ---- misc ----------------------------------------------------------------
constexpr const char* FW_VERSION = "0.1.0-mvp";
constexpr const char* NVS_NAMESPACE = "lamp1";
constexpr const char* IR_DIR = "/ir";
constexpr const char* LOG_DIR = "/log";
constexpr size_t LOG_MAX_BYTES = 32 * 1024;
constexpr bool DEFAULT_QUIET_AT_NIGHT = true;