"""在已连好的 ESP32-S3 MicroPython 上执行一段代码，把 stdout / 报错打印出来。

用法：
    python tools/mp_eval.py --port COM6 "import sys; print(sys.version)"
    python tools/mp_eval.py --port COM6 --timeout 40 --file firmware/micropython/mic_test.py
    python tools/mp_eval.py --port COM6 --reset          # 只软重启，看开机输出

和 mp_deploy.py 的区别：那个负责把文件写进板子并重启，这个只"跑一句 / 跑一个文件"，
用来做硬件探测（引脚、I2S、传感器），不改板上的 main.py。
"""

import argparse
import sys
import time

import serial

from mp_deploy import enter_raw_repl, exec_raw


def open_board(port):
    ser = serial.Serial(port, 115200, timeout=0.05)
    ser.write(b"\r\x03\x03")  # 回车 + 两次 Ctrl-C：打断正在跑的程序
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--reset", action="store_true")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--file")
    g.add_argument("--code")
    ap.add_argument("positional_code", nargs="?")
    args = ap.parse_args()

    ser = open_board(args.port)
    try:
        if args.reset:
            ser.write(b"\x04")
            time.sleep(2.0)
            boot = ser.read(ser.in_waiting or 1)
            boot += ser.read(ser.in_waiting)
            print("board says:\n" + boot.decode("utf-8", "replace").strip())
            return 0

        code = args.code or args.positional_code
        if args.file:
            with open(args.file, encoding="utf-8") as fh:
                code = fh.read()
        if not code:
            ap.error("需要 --file、--code 或一段位置参数代码")

        enter_raw_repl(ser)
        out, err = exec_raw(ser, code, timeout=args.timeout)
        if out:
            print(out)
        if err:
            print("!! " + err, file=sys.stderr)
            return 1
        return 0
    finally:
        ser.write(b"\x02")  # 退出 raw REPL，回到普通 REPL
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
