"""把本地 .py 文件写进 ESP32-S3 上的 MicroPython 并软重启。

用法：
    python tools/mp_deploy.py --port COM6 firmware/micropython/main.py

实现要点：MicroPython 的 raw REPL 单条命令有长度上限（~256 字节），
所以文件不能一次 f.write 进去，要用 base64 切成小段，每段一条命令压栈，
最后一次性解码写盘。这样中文注释也不会被转义撑爆缓冲区。
"""

import argparse
import base64
import sys
import time

import serial

CHUNK = 96  # base64 每段字符数；96 字符 -> 命令行长约 115 字节，安全


def _read_until(ser, token, timeout):
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            if token in buf:
                break
    return bytes(buf)


def enter_raw_repl(ser):
    ser.write(b"\r\x03\x03")  # 回车 + 两次 Ctrl-C：打断正在运行的程序
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"\x01")  # Ctrl-A：进入 raw REPL
    resp = _read_until(ser, b">", 5.0)
    if b"raw REPL" not in resp:
        raise RuntimeError("进入 raw REPL 失败，收到: %r" % resp[:200])
    ser.reset_input_buffer()


def exec_raw(ser, code, timeout=10.0):
    """在 raw REPL 里执行一段代码，返回 (stdout, error_text)。"""
    ser.write(code.encode("utf-8") + b"\x04")  # Ctrl-D：执行
    raw = _read_until(ser, b"\x04", timeout)
    out = raw.split(b"\x04")[0]
    if out.startswith(b"OK"):
        out = out[2:]
    status = ser.read(1)
    err = b""
    if status == b"\x05":
        err = _read_until(ser, b"\x04", timeout).split(b"\x04")[0]
    _read_until(ser, b">", 2.0)  # 吃掉下一个提示符
    return out.decode("utf-8", "replace").strip(), err.decode("utf-8", "replace").strip()


def put_file(ser, path, remote):
    payload = open(path, "rb").read()
    b64 = base64.b64encode(payload).decode("ascii")

    out, err = exec_raw(ser, "_p=[]")
    if err:
        raise RuntimeError("初始化失败: " + err)
    for i in range(0, len(b64), CHUNK):
        piece = b64[i : i + CHUNK]
        out, err = exec_raw(ser, "_p.append('%s')" % piece)
        if err:
            raise RuntimeError("第 %d 段写入失败: %s" % (i // CHUNK, err))

    code = (
        "import ubinascii\n"
        "f=open('%s','wb')\n" % remote
        + "n=f.write(ubinascii.a2b_base64(''.join(_p).encode()))\n"
        + "f.close()\n"
        + "print('WROTE', n)\n"
    )
    out, err = exec_raw(ser, code)
    if err:
        raise RuntimeError("落盘失败: " + err)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--remote", default="main.py")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("file")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    try:
        print("port %s opened" % args.port)
        enter_raw_repl(ser)
        ver, _ = exec_raw(ser, "import os; print(os.uname().machine)")
        print("board:", ver)

        print(put_file(ser, args.file, args.remote))

        head, _ = exec_raw(ser, "print(open('%s').read()[:60])" % args.remote)
        print("file head:", head.replace("\\n", " "))

        if args.no_reset:
            ser.write(b"\x02")
            return 0

        ser.write(b"\x02")  # Ctrl-B：退出 raw REPL
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"\x04")  # Ctrl-D：软重启，运行 main.py
        time.sleep(2.0)
        boot = ser.read(ser.in_waiting or 1)
        boot += ser.read(ser.in_waiting)
        print("board says:\n" + boot.decode("utf-8", "replace").strip())
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
