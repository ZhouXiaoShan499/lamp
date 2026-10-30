# 临时诊断用：判断 HW-479 的信号线到底接在哪个 GPIO 上
#
# 背景：把每个候选 GPIO 配成"内部上拉"再读，只有 GPIO48 读 0。
#       这说明 GPIO48 上有东西把它拉低（很可能就是你的灯模块，150Ω + LED 到地）。
#       而 GPIO5 读 1，表示那条支路是开的。
#
# 运行后循环两组闪烁，用来肉眼确认：
#   第 1 组：GPIO48 快闪（0.4 秒亮 / 0.4 秒灭，共 10 次）  —— 约 8 秒
#   停顿 3 秒
#   第 2 组：GPIO5  慢闪（0.8 秒亮 / 0.8 秒灭，共 4 次）   —— 约 6 秒
#   停顿 3 秒，然后重复

from machine import Pin
import time

PIN_A = Pin(48, Pin.OUT)  # 第 1 组：被拉低的那个
PIN_B = Pin(5, Pin.OUT)   # 第 2 组：你原本说的 G 通道

PIN_A.value(0)
PIN_B.value(0)


def group_fast():
    for _ in range(10):
        PIN_A.value(1)
        time.sleep_ms(400)
        PIN_A.value(0)
        time.sleep_ms(400)


def group_slow():
    for _ in range(4):
        PIN_B.value(1)
        time.sleep_ms(800)
        PIN_B.value(0)
        time.sleep_ms(800)


while True:
    group_fast()
    time.sleep_ms(3000)
    group_slow()
    time.sleep_ms(3000)
