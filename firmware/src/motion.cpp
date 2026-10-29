#include "motion.h"

#include "pwm.h"

namespace motion {
namespace {

float tiltCur = SERVO_TILT_HOME_DEG;
float yawCur = SERVO_YAW_HOME_DEG;
float tiltTarget = SERVO_TILT_HOME_DEG;
float yawTarget = SERVO_YAW_HOME_DEG;

bool attached = false;
bool isMuted = false;
bool isNight = false;
uint32_t settleAt = 0;      // do not release PWM before this timestamp
uint32_t returnHomeAt = 0;  // 0 = no pending return
uint32_t lastTick = 0;

float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

void writeAngle(uint8_t pin, uint8_t channel, float angle_deg) {
    // 500..2500 us maps to 0..180 deg for a standard hobby servo.
    const float pulse = 500.0f + clampf(angle_deg, 0.0f, 180.0f) * (2000.0f / 180.0f);
    pwm::write(pin, channel, pwm::servoDuty(static_cast<uint16_t>(pulse + 0.5f)));
}

void attachServos() {
    if (attached) return;
    pwm::attach(PIN_SERVO_TILT, CH_SERVO_TILT, SERVO_PWM_HZ, SERVO_PWM_BITS);
    pwm::attach(PIN_SERVO_YAW, CH_SERVO_YAW, SERVO_PWM_HZ, SERVO_PWM_BITS);
    attached = true;
}

void releaseServos() {
    if (!attached) return;
    // Duty 0 -> no pulse train, the servo goes limp and stops humming.
    pwm::write(PIN_SERVO_TILT, CH_SERVO_TILT, 0);
    pwm::write(PIN_SERVO_YAW, CH_SERVO_YAW, 0);
    attached = false;
}

// Servo horn zero offset: a yaw of -60 deg corresponds to servo angle 30 deg.
float yawToServo(float yaw_deg) { return 90.0f + yaw_deg; }

}  // namespace

void begin() {
    tiltCur = tiltTarget = SERVO_TILT_HOME_DEG;
    yawCur = yawTarget = SERVO_YAW_HOME_DEG;
    attached = false;
    settleAt = 0;
    returnHomeAt = 0;
#if !LAMP_HAS_SERVOS
    isMuted = true;
#endif
    Serial.printf("[motion] tilt=%u yaw=%u muted=%d\n", PIN_SERVO_TILT, PIN_SERVO_YAW, isMuted ? 1 : 0);
}

bool request(Move move) {
    if (isMuted) return false;
    if (isNight && move != Move::Home) return false;

    attachServos();
    const uint32_t now = millis();
    returnHomeAt = 0;

    switch (move) {
        case Move::Home:
            tiltTarget = SERVO_TILT_HOME_DEG;
            yawTarget = SERVO_YAW_HOME_DEG;
            break;
        case Move::Nod:
            tiltTarget = SERVO_TILT_MAX_DEG;
            returnHomeAt = now + 700;
            break;
        case Move::TurnLeft:
            yawTarget = clampf(SERVO_YAW_MIN_DEG * 0.6f, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
            returnHomeAt = now + 1000;
            break;
        case Move::TurnRight:
            yawTarget = clampf(SERVO_YAW_MAX_DEG * 0.6f, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
            returnHomeAt = now + 1000;
            break;
        case Move::Alert:
            tiltTarget = SERVO_TILT_MAX_DEG;
            yawTarget = clampf(SERVO_YAW_MAX_DEG * 0.5f, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
            returnHomeAt = now + 1200;
            break;
    }
    Serial.printf("[motion] move=%s tilt=%.1f yaw=%.1f\n", moveName(move), tiltTarget, yawTarget);
    return true;
}

void tick(uint32_t now_ms) {
    if (!attached) return;
    if (now_ms - lastTick < MOTION_TICK_MS) return;
    lastTick = now_ms;

    const auto approach = [](float cur, float target) -> float {
        if (cur < target) return (cur + SERVO_STEP_DEG < target) ? cur + SERVO_STEP_DEG : target;
        if (cur > target) return (cur - SERVO_STEP_DEG > target) ? cur - SERVO_STEP_DEG : target;
        return cur;
    };
    tiltCur = approach(tiltCur, tiltTarget);
    yawCur = approach(yawCur, yawTarget);
    writeAngle(PIN_SERVO_TILT, CH_SERVO_TILT, tiltCur);
    writeAngle(PIN_SERVO_YAW, CH_SERVO_YAW, yawToServo(yawCur));

    const bool arrived = (tiltCur == tiltTarget) && (yawCur == yawTarget);
    if (arrived) {
        if (settleAt == 0) settleAt = now_ms + 350;  // hold briefly so the head does not wobble
        if (returnHomeAt != 0 && now_ms >= returnHomeAt) {
            tiltTarget = SERVO_TILT_HOME_DEG;
            yawTarget = SERVO_YAW_HOME_DEG;
            returnHomeAt = 0;
            settleAt = 0;
            return;
        }
        if (returnHomeAt == 0 && now_ms >= settleAt) {
            releaseServos();
            settleAt = 0;
        }
    } else {
        settleAt = 0;
    }
}

bool busy() {
    if (!attached) return false;
    return (tiltCur != tiltTarget) || (yawCur != yawTarget) || returnHomeAt != 0;
}

void setMuted(bool muted) {
    isMuted = muted;
    if (muted) {
        isNight = false;
        releaseServos();
    }
}

bool muted() { return isMuted; }

void setNightMode(bool night) { isNight = night; }
bool nightMode() { return isNight; }

void emergencyStop() {
    tiltTarget = tiltCur;
    yawTarget = yawCur;
    returnHomeAt = 0;
    releaseServos();
    Serial.println("[motion] emergency stop");
}

const char* moveName(Move move) {
    switch (move) {
        case Move::Home: return "home";
        case Move::Nod: return "nod";
        case Move::TurnLeft: return "left";
        case Move::TurnRight: return "right";
        case Move::Alert: return "alert";
    }
    return "unknown";
}

}  // namespace motion