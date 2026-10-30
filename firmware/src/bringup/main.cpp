// ============================================================================
// LAMP-1 面包板 bring-up：按键 -> 灯（最小通路）
//
// 目标：只用这块 ESP32-S3 开发板 + HW-483 按键模块 + HW-479 RGB 模块，
//       验证「按键输入 -> GPIO 输出 -> 灯亮」这一条链路，不依赖 IR / 舵机 /
//       LittleFS，也不引用 config.h（那份引脚表是给成品 PCB 的，见 docs/06）。
//
// 接线（详细核对表见 docs/06-面包板点亮验证.md）：
//   HW-483 按键模块： +  -> 3V3      -  -> GND      S -> GPIO4
//   HW-479 RGB 模块： -  -> GND      G  -> GPIO5    R/B 悬空
//   注意 HW-479 的针序是  -  R  G  B（有 "-" 丝印那一端算第 1 脚），
//   棕线必须落在第 3 脚 G 上，插到 R 上绿灯永远不会亮。
//
// 用法：改 BRINGUP_MODE 后重新烧录，三种模式分别验证一段链路
//   0 = 只点灯：GPIO5 每 500 ms 翻转一次
//   1 = 只看键：串口打印 GPIO4 原始电平（判断极性 / 接触）
//   2 = 合起来：按一下开灯、再按一下关灯（默认）
//
// 编译：pio run -e bringup -t upload      （PlatformIO，见 firmware/platformio.ini）
//       Arduino IDE 用户：把本文件复制成 button_led/button_led.ino 即可直接编译。
// ============================================================================

#include <Arduino.h>

// ------------------------------- 可调参数 -----------------------------------

#define BRINGUP_MODE 2  // 0=只点灯 1=只看键 2=按键开关灯

static constexpr uint8_t PIN_BTN = 4;
static constexpr uint8_t PIN_LED = 5;

// 极性开关：先跑 MODE 1 看串口波形，再按实际情况改这里（默认值适配常见模块）
#define BTN_ACTIVE_LOW 1   // 1: 按下读到 LOW（模块上拉到 3V3，最常见）
                           // 0: 按下读到 HIGH（模块下拉到 GND）
#define BTN_USE_PULLUP 1   // 1: 打开芯片内部上拉；模块自带下拉时改 0，
                           //    否则"按不按都读到 HIGH"，永远触发不了
#define LED_ACTIVE_HIGH 1  // 1: 高电平点亮（HW-479 共阴，公共端接 GND）
                           // 0: 低电平点亮（共阳，公共端要接 3V3）

static constexpr uint16_t DEBOUNCE_MS = 25;  // 抖动窗口；按一下亮两下就加到 40~50

// ------------------------------- 小工具 -------------------------------------

static inline void ledWrite(bool on) {
#if LED_ACTIVE_HIGH
    digitalWrite(PIN_LED, on ? HIGH : LOW);
#else
    digitalWrite(PIN_LED, on ? LOW : HIGH);
#endif
}

static inline bool btnPressed() {
    const int v = digitalRead(PIN_BTN);
#if BTN_ACTIVE_LOW
    return v == LOW;
#else
    return v == HIGH;
#endif
}

// ------------------------------- 初始化 -------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);  // 等串口监视器接上，不影响功能

    pinMode(PIN_LED, OUTPUT);
    ledWrite(false);

    // 开机自检：先让灯闪 2 次。灯不闪就先别管按键，问题一定在灯这一段。
    for (uint8_t i = 0; i < 2; ++i) {
        ledWrite(true);
        delay(150);
        ledWrite(false);
        delay(150);
    }

#if BTN_USE_PULLUP
    pinMode(PIN_BTN, INPUT_PULLUP);
#else
    pinMode(PIN_BTN, INPUT);
#endif
    delay(10);

    Serial.println();
    Serial.println("[bringup] LAMP-1 button -> led");
    Serial.printf("[bringup] mode=%d  BTN=GPIO%d  LED=GPIO%d\n",
                  static_cast<int>(BRINGUP_MODE), static_cast<int>(PIN_BTN),
                  static_cast<int>(PIN_LED));
    Serial.printf("[bringup] idle: raw=%d pressed=%d\n", digitalRead(PIN_BTN),
                  btnPressed() ? 1 : 0);
    Serial.println("[bringup] 提示: 松开应为 raw=1, 按下应为 raw=0");
}

// --------------------------------- 主循环 -----------------------------------

void loop() {
#if BRINGUP_MODE == 0
    // ---------------- 只点灯：1 Hz 方波 ----------------
    static uint32_t lastToggle = 0;
    static bool on = false;
    if (millis() - lastToggle >= 500) {
        lastToggle = millis();
        on = !on;
        ledWrite(on);
    }

#elif BRINGUP_MODE == 1
    // ---------------- 只看键：电平一变就打印 ----------------
    static int lastRaw = -1;
    static uint32_t lastBeat = 0;
    const int raw = digitalRead(PIN_BTN);
    if (raw != lastRaw) {
        lastRaw = raw;
        Serial.printf("[btn] raw=%d (%s) pressed=%d\n", raw, raw == LOW ? "LOW" : "HIGH",
                      btnPressed() ? 1 : 0);
    }
    if (millis() - lastBeat >= 2000) {
        lastBeat = millis();
        Serial.printf("[btn] heartbeat raw=%d pressed=%d\n", raw, btnPressed() ? 1 : 0);
    }
    delay(5);

#else
    // ---------------- 合起来：按一下开、再按一下关 ----------------
    static bool inited = false;
    static bool stable = false;    // 消抖后的按下状态
    static bool lastRead = false;  // 上一次的原始状态
    static uint32_t lastChange = 0;
    static bool ledOn = false;

    if (!inited) {  // 上电时如果按键已经按着，不要误触发一次翻转
        inited = true;
        stable = lastRead = btnPressed();
        lastChange = millis();
    }

    const bool reading = btnPressed();
    if (reading != lastRead) {
        lastRead = reading;
        lastChange = millis();
    }

    if ((millis() - lastChange) >= DEBOUNCE_MS && reading != stable) {
        stable = reading;
        if (stable) {  // 只在"按下"的瞬间动作
            ledOn = !ledOn;
            ledWrite(ledOn);
            Serial.printf("[btn] pressed -> LED %s\n", ledOn ? "ON" : "OFF");
        }
    }

    delay(5);  // 没有别的事可做；正式固件里这里改成非阻塞的 millis() 轮询
#endif
}
