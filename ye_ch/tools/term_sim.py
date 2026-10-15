#!/usr/bin/env python3
"""
term_sim.py —— 终端模拟器（扮演"通信综合管理软件"，经 UDP 和本机交互）

PC 网卡设成 10.9.0.24/24（= app_config.h 的 CFG_TERM_IP）；本机的回复固定发到 10.9.0.24:5000，
所以本脚本绑定 5000 端口（SSCOM 等占着 5000 时先关掉）。
用法：
  python term_sim.py                      交互模式，命令：
      hb                                  发 42201 心跳，打印 42202 里的北斗状态
      bd <号码> <正文>                    发 42212 北斗短报文，等 42213
      raw <json>                          原样发一段 JSON
      q                                   退出
  python term_sim.py hb                   只发一次心跳，等 3 s 后退出
  python term_sim.py bd 4217580 12一二    只发一条短报文，等 15 s 后退出
  --board IP:端口                         本机地址，默认 10.9.0.57:4220
收到 42214（北斗收报）时自动回 42215 确认。
"""
import json
import socket
import sys
import threading
import time
import uuid

BOARD = ("10.9.0.57", 4220)    # 本机 CFG_LOCAL_IP : CFG_TERM_LOCAL_PORT
LOCAL_PORT = 5000              # CFG_TERM_PORT


def make(msg_type, content, msg_id=None):
    return {"msg_id": msg_id or str(uuid.uuid4()), "timestamp": int(time.time() * 1000),
            "msg_type": str(msg_type), "content": content}


def send(sock, obj):
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    sock.sendto(data, BOARD)
    print(">>> %s" % data.decode("utf-8"))


def show(sock, obj):
    t = str(obj.get("msg_type"))
    c = obj.get("content") or {}
    if t == "42202":
        bd = (c.get("Config") or {}).get("BD") or {}
        print("    BD status=%s SQ=%s CD=%s maxlength=%s taskstate=%s" % (
            bd.get("status"), bd.get("SQ"), bd.get("CD"), bd.get("maxlength"), c.get("taskstate")))
    elif t == "42213":
        err = c.get("errormsg", c.get("error_msg"))
        print("    发送结果 status=%s errormsg=%r" % (c.get("status"), err))
    elif t == "42214":
        print("    北斗收报 来自 %s：%s" % (c.get("target_id"), c.get("msg")))
        send(sock, make(42215, {"taskid": c.get("taskid", ""), "status": 0}, obj.get("msg_id")))


def rx_loop(sock):
    while True:
        try:
            data, addr = sock.recvfrom(8192)
        except OSError:
            return
        text = data.decode("utf-8", "replace")
        print("<<< %s:%d %s" % (addr[0], addr[1], text))
        try:
            show(sock, json.loads(text))
        except ValueError:
            print("    (不是合法 JSON)")


def command(sock, line):
    parts = line.strip().split(maxsplit=2)
    if not parts:
        return True
    op = parts[0].lower()
    if op == "q":
        return False
    if op == "hb":
        send(sock, make(42201, {"taskid": str(uuid.uuid4())}))
    elif op == "bd" and len(parts) >= 3:
        send(sock, make(42212, {"taskid": str(uuid.uuid4()), "target_id": parts[1], "msg": parts[2]}))
    elif op == "raw" and len(parts) >= 2:
        sock.sendto(line.strip()[3:].strip().encode("utf-8"), BOARD)
    else:
        print(__doc__)
    return True


def main():
    global BOARD
    args = sys.argv[1:]
    if "--board" in args:
        i = args.index("--board")
        ip, port = args[i + 1].split(":")
        BOARD = (ip, int(port))
        del args[i:i + 2]
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", LOCAL_PORT))
    threading.Thread(target=rx_loop, args=(sock,), daemon=True).start()
    print("终端模拟器：本地 UDP %d <-> 本机 %s:%d" % (LOCAL_PORT, BOARD[0], BOARD[1]))

    if args:                                    # 一次性命令
        command(sock, " ".join(args))
        time.sleep(15 if args[0] == "bd" else 3)
    else:                                       # 交互模式
        try:
            while command(sock, input()):
                pass
        except (KeyboardInterrupt, EOFError):
            pass
    sock.close()


if __name__ == "__main__":
    main()
