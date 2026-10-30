# 通用点亮测试：不管灯接在 GPIO5 还是 GPIO48、不管共阴还是共阳，都应该以 1 Hz 闪烁
#
#   阶段 A（1 秒）：GPIO48 高 + GPIO5 低
#   阶段 B（1 秒）：GPIO48 低 + GPIO5 高
#
# 四种组合里总有一种会亮：
#   接 5 且共阴 -> 阶段 B 亮
#   接 5 且共阳（公共端在 3V3）-> 阶段 A 亮
#   接 48 且共阴 -> 阶段 A 亮
#   接 48 且共阳 -> 阶段 B 亮
# 完全不闪 = 灯回路是开路的（地或公共端没通），必须动线。

from machine import Pin
import time

A = Pin(48, Pin.OUT)  # 被内部上拉扫描发现"有负载"的那个脚
B = Pin(5, Pin.OUT)   # 你原本指定的 G 通道

# 开机先闪两下，两个脚同时动作，确认程序在跑
for _ in range(2):
    A.value(1)
    B.value(1)
    time.sleep_ms(200)
    A.value(0)
    B.value(0)
    time.sleep_ms(200)

while True:
    A.value(1)
    B.value(0)
    time.sleep_ms(1000)
    A.value(0)
    B.value(1)
    time.sleep_ms(1000)
