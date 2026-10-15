#!/usr/bin/env python3
"""
bd_sim.py —— 北斗模块模拟器（联调用，没有真北斗模块时代替它）

接线：PC 的 USB-TTL（3.3V）或 USB-RS422 接本机北斗口 USART6（PC6 发 / PC7 收），38400 8N1。
依赖：pip install pyserial
用法：python bd_sim.py COM5

自动行为：
  每秒输出一条 PWI（新体制波束数 = beams）和一条 GSS（可用卫星数 = sats）
  收到 $CCICR -> 回 ICP（卡号 1859836，区域 / 全球频度 60 s，长度等级 3）
  收到 TCQ    -> 打印解出的正文，按 fki 模式 1 s 后回 FKI
  收到 OFQ    -> 1 s 后回 FKI（OFQ,Y）
命令（回车执行）：
  sig <beams> <sats>   改每秒输出的波束数 / 卫星数（sig 0 0 -> 本机判离线）
  silent / talk        停止 / 恢复每秒输出（silent 5 s 后本机判异常）
  fki y|n1|n3|none     对 TCQ 的回执：成功 / 频度未到 / 其他原因 / 不回（测超时）
  tci <发信方> <正文>  发一条混编 TCI 给本机
  raw <语句>           发任意语句，自动补 $、校验和、换行，如 raw BDFKI,033000,OFQ,Y,0,0
  show                 是否打印每秒的 PWI / GSS（默认不打印）
  q                    退出
"""
import sys
import threading
import time

CARD_ID = "1859836"     # ICP 里的本机卡号，TCI 的收信方
ICP_BODY = "BDICP," + CARD_ID + ",0,0,3,0,N,3,N,1,0,0,3,1,60,3,1,1,60,1,0,0,0,0"


def cks(raw):
    """校验和：'$' 和 '*' 之间所有字节的异或"""
    s = 0
    for b in raw:
        s ^= b
    return s


def build(body):
    """'BDFKI,...' -> b'$BDFKI,...*hh\\r\\n'；正文按 GBK 编码，可以含汉字"""
    body = body.strip().lstrip("$").split("*")[0]
    raw = body.encode("gbk")
    return b"$" + raw + b"*%02X\r\n" % cks(raw)


def hhmmss():
    return time.strftime("%H%M%S", time.gmtime())


class Sim:
    def __init__(self, port):
        self.port = port
        self.lock = threading.Lock()
        self.beams = 3
        self.sats = 2
        self.talk = True
        self.show = False
        self.fki = "y"
        self.buf = b""

    def send(self, body, quiet=False):
        data = build(body)
        with self.lock:
            self.port.write(data)
        if not quiet:
            print(">>", data.decode("gbk", "replace").rstrip())

    def later(self, sec, body):
        t = threading.Timer(sec, self.send, args=(body,))
        t.daemon = True
        t.start()

    def periodic(self):
        """每秒调用一次：PWI + GSS"""
        if not self.talk:
            return
        pwi = "BDPWI,%s.00,00,%02d" % (hhmmss(), self.beams)
        pwi += "".join(",%02d,47,47,00" % (11 + i) for i in range(self.beams))
        gss = "BDGSS,%02d,%02d" % (self.sats, 20 if self.sats else 0)
        gss += "".join(",%02d,40,45,180" % (20 + i) for i in range(self.sats))
        self.send(pwi, quiet=not self.show)
        self.send(gss, quiet=not self.show)

    def feed(self, data):
        """本机发来的字节流，按换行切句"""
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self.on_line(line.strip().decode("gbk", "replace"))

    def on_line(self, line):
        if not line.startswith("$"):
            return
        print("<<", line)
        body, _, given = line[1:].partition("*")
        if given and given[:2].upper() != "%02X" % cks(body.encode("gbk")):
            print("   ! 校验和不对，应为 *%02X" % cks(body.encode("gbk")))
        f = body.split(",")
        kind = f[0][-3:]
        if kind == "ICR":
            self.later(0.2, ICP_BODY)
        elif kind == "TCQ" and len(f) >= 6:
            if f[4] == "3" and f[5][:2].upper() == "A4":
                try:
                    text = bytes.fromhex(f[5][2:]).decode("gbk", "replace")
                except ValueError:
                    text = "(混编数据不是十六进制)"
            else:
                text = f[5]
            print("   TCQ -> %s, 编码类型 %s, 正文: %s" % (f[1], f[4], text))
            reply = {"y": "TCQ,Y,0,0", "n1": "TCQ,N,1,7", "n3": "TCQ,N,3,0"}.get(self.fki)
            if reply:
                self.later(1.0, "BDFKI,%s,%s" % (hhmmss(), reply))
            else:
                print("   (fki none：不回执，本机约 10 s 后判超时)")
        elif kind == "OFQ":
            self.later(1.0, "BDFKI,%s,OFQ,Y,0,0" % hhmmss())

    def command(self, cmd):
        """处理一行命令，返回 False 表示退出"""
        parts = cmd.strip().split(maxsplit=2)
        if not parts:
            return True
        op = parts[0].lower()
        if op == "q":
            return False
        if op == "sig" and len(parts) >= 3:
            self.beams, self.sats = int(parts[1]), int(parts[2])
            print("   每秒输出：波束 %d，卫星 %d" % (self.beams, self.sats))
        elif op in ("silent", "talk"):
            self.talk = (op == "talk")
            print("   每秒输出：%s" % ("恢复" if self.talk else "停止"))
        elif op == "show":
            self.show = not self.show
            print("   打印每秒输出：%s" % ("开" if self.show else "关"))
        elif op == "fki" and len(parts) >= 2:
            self.fki = parts[1].lower()
            print("   TCQ 回执模式：%s" % self.fki)
        elif op == "tci" and len(parts) >= 3:
            data = "A4" + parts[2].encode("gbk").hex().upper()
            self.send("BDTCI,%s,%s,2,%s.00,3,0,%s" % (parts[1], CARD_ID, hhmmss(), data))
        elif op == "raw" and len(parts) >= 2:
            self.send(cmd.strip()[3:].strip())
        else:
            print(__doc__)
        return True


def main():
    import serial    # pip install pyserial

    if len(sys.argv) < 2:
        print(__doc__)
        return
    port = serial.Serial(sys.argv[1], 38400, timeout=0.05)
    sim = Sim(port)
    print("北斗模拟器已打开 %s，38400 8N1。输入 help 看命令。" % sys.argv[1])

    def reader():
        while True:
            data = port.read(256)
            if data:
                sim.feed(data)

    def ticker():
        while True:
            sim.periodic()
            time.sleep(1.0)

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=ticker, daemon=True).start()
    try:
        while sim.command(input()):
            pass
    except (KeyboardInterrupt, EOFError):
        pass
    port.close()


if __name__ == "__main__":
    main()
