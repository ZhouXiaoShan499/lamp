# LAMP-1 bring-up：按键开关 + 长按调光（PWM）
#
# 接线（2026-10-29 实测可用）：
#   HW-479 灯  ： - -> GND（共阴），G -> GPIO5，R/B 悬空
#   HW-483 按键： + -> 3V3，- -> GND，S -> GPIO4
#
# 行为：
#   短按（<400 ms）   开 / 关，记住当前亮度
#   长按（>=400 ms）  亮度档位 100% -> 25% -> 50% -> 75% -> 100% 循环，并自动点亮
#
# 为什么现在就用 PWM：正式灯板上的照明 LED 也是 PWM 调光，
# 这里先把驱动方式统一，以后把 GPIO5 换成灯板的驱动脚，逻辑不用重写。
#
# 出问题要退回"纯开关"版本：
#   python tools/mp_deploy.py --port COM6 --remote main.py firmware/micropython/main_toggle.py

from machine import Pin, PWM
import time

LED = PWM(Pin(5), freq=1000, duty=0)  # duty 0..1023
BTN = Pin(4, Pin.IN, Pin.PULL_UP)     # 按下读到 0

DEBOUNCE_MS = 30
LONG_MS = 400
LEVELS = (256, 512, 768, 1023)        # 25% / 50% / 75% / 100%
MAX_DUTY = 1023

level_idx = 3      # 从上电即 100% 开始
state = False


def apply():
    LED.duty(LEVELS[level_idx] if state else 0)


# 上电自检：闪 2 下
for _ in range(2):
    LED.duty(MAX_DUTY)
    time.sleep_ms(150)
    LED.duty(0)
    time.sleep_ms(150)

stable = BTN.value()
last_read = stable
t_change = time.ticks_ms()
press_at = 0
long_fired = False

while True:
    now = time.ticks_ms()
    v = BTN.value()

    if v != last_read:
        last_read = v
        t_change = now

    # 消抖后的状态跳变
    if time.ticks_diff(now, t_change) >= DEBOUNCE_MS and v != stable:
        stable = v
        if stable == 0:            # 按下
            press_at = now
            long_fired = False
        else:                      # 松开
            if not long_fired:     # 短按才切换开关
                state = not state
                apply()

    # 按住超过阈值：切一档亮度（每次长按只触发一次）
    if stable == 0 and not long_fired and time.ticks_diff(now, press_at) >= LONG_MS:
        long_fired = True
        level_idx = (level_idx + 1) % len(LEVELS)
        state = True
        apply()

    time.sleep_ms(5)
