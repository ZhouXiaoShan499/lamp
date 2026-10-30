# LAMP-1 bring-up：按键开关灯
#
# 实测结论（2026-10-29）：
#   HW-479 灯  ： - -> GND（公共端接地，共阴接法），G -> GPIO5
#                 高电平点亮。之前的反相测试里正是 GPIO5 为高时灯亮。
#   HW-483 按键： + -> 3V3，- -> GND，S -> GPIO4（内部上拉，按下读到 0）
#
# 驱动：GPIO5 高 = 亮。GPIO48 保持低电平（如果 R 还插在 48 上，它不会亮，
#       这样只显示绿色；想红绿一起亮就把 GPIO48 那一路也加进来）。
#
# 注意：这里没有 print。USB CDC 在没有上位机读取时，print 会写满 FIFO 后
# 阻塞主循环，表现出来就是"按键失灵"。需要日志时连上串口再临时加。
#
# 行为：上电自检闪 2 下，之后每按一次按键翻转一次。

from machine import Pin
import time

LED = Pin(5, Pin.OUT)              # G 通道，高电平点亮
Pin(48, Pin.OUT).value(0)          # 不用的通道保持熄灭
BTN = Pin(4, Pin.IN, Pin.PULL_UP)  # 按下读到 0

DEBOUNCE_MS = 30
LOOP_MS = 5


def led_set(on):
    LED.value(1 if on else 0)


for _ in range(2):  # 上电自检
    led_set(True)
    time.sleep_ms(150)
    led_set(False)
    time.sleep_ms(150)

state = False
stable = BTN.value()
last_read = stable
t_change = time.ticks_ms()

while True:
    v = BTN.value()
    if v != last_read:
        last_read = v
        t_change = time.ticks_ms()

    if time.ticks_diff(time.ticks_ms(), t_change) >= DEBOUNCE_MS and v != stable:
        stable = v
        if stable == 0:  # 按下沿
            state = not state
            led_set(state)

    time.sleep_ms(LOOP_MS)
