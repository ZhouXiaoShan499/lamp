#pragma once
// Thin wrapper over the LEDC API so the same source builds on both the
// arduino-esp32 2.x API (channel based) and the 3.x API (pin based).

#include <Arduino.h>

#include "config.h"

namespace pwm {

inline void attach(uint8_t pin, uint8_t channel, uint32_t freq_hz, uint8_t bits) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    (void)channel;
    ledcAttach(pin, freq_hz, bits);
#else
    ledcSetup(channel, freq_hz, bits);
    ledcAttachPin(pin, channel);
#endif
}

inline void write(uint8_t pin, uint8_t channel, uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    (void)channel;
    ledcWrite(pin, duty);
#else
    (void)pin;
    ledcWrite(channel, duty);
#endif
}

inline void detach(uint8_t pin, uint8_t channel) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    (void)channel;
    ledcDetach(pin);
#else
    (void)pin;
    ledcDetachPin(channel);
#endif
}

// 50 Hz servo helper: 0 .. 20000 us pulse width -> duty for a 14-bit channel.
inline uint32_t servoDuty(uint16_t pulse_us) {
    const uint32_t period_us = 1000000UL / SERVO_PWM_HZ;
    if (pulse_us > period_us) pulse_us = period_us;
    return (static_cast<uint32_t>(pulse_us) * ((1UL << SERVO_PWM_BITS) - 1)) / period_us;
}

}  // namespace pwm