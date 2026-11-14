#!/usr/bin/env python3
"""Talk to a serial port of the board through its WebSocket, from a script (standard library only).

    port_console.py 0 10 'enable\\r' 'show clock\\r'            send, wait 10 s, print the output
    port_console.py 0 120 --until 'md5\\) = [0-9a-f]{32}' 'verify /md5 usbflash0:x.bin\\r'

The board is $BOARD (default rs232.local). A new connection first replays what the
port printed before (the board keeps the last output), so the answer to a command
is what comes after it: --until only looks at output received after the last
command was sent. Used to drive a switch's console while the board sits at its USB
port (WAVESHARE.md, "SD-Karte als USB-Stick").
"""
import argparse, base64, json, os, re, socket, struct, sys, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("port", type=int)
ap.add_argument("seconds", type=float, help="how long to listen (at most; --until may end it sooner)")
ap.add_argument("send", nargs="*", help="texts to send, one every 1.5 s; \\r and \\n are understood")
ap.add_argument("--until", help="regular expression: stop as soon as it matches output received after the last text was sent")
a = ap.parse_args()
host = os.environ.get("BOARD", "rs232.local")
sends = [t.encode().decode("unicode_escape") for t in a.send]

token = json.load(urllib.request.urlopen(f"http://{host}/api/session", timeout=5))["token"]
sock = socket.create_connection((host, 81), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
sock.sendall((f"GET /?t={token} HTTP/1.1\r\nHost: {host}:81\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += sock.recv(4096)
head, buf = buf.split(b"\r\n\r\n", 1)
if b" 101 " not in head.split(b"\r\n")[0]:
    sys.exit("WebSocket refused: " + head.split(b"\r\n")[0].decode())


def send_frame(opcode, payload):
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x80 | opcode]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
    sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def frames():
    global buf
    while True:
        if len(buf) >= 2:
            op, n, off = buf[0] & 15, buf[1] & 127, 2
            if n == 126 and len(buf) >= 4:
                n, off = struct.unpack(">H", buf[2:4])[0], 4
            elif n == 127 and len(buf) >= 10:
                n, off = struct.unpack(">Q", buf[2:10])[0], 10
            elif n >= 126:
                n = None
            if n is not None and len(buf) >= off + n:
                payload, buf = buf[off:off + n], buf[off + n:]
                yield op, payload
                continue
        try:
            data = sock.recv(65536)
        except socket.timeout:
            yield None, b""
            continue
        if not data:
            return
        buf += data


send_frame(1, json.dumps({"cmd": "hello", "seq": [0, 0, 0, 0], "boot": 0, "time": int(time.time())}).encode())
sock.settimeout(0.3)
start = time.time()
next_send = start + 2.0
out, mark = bytearray(), 0
for op, payload in frames():
    now = time.time()
    if sends and now >= next_send:
        send_frame(2, bytes([a.port]) + sends.pop(0).encode())
        next_send, mark = now + 1.5, len(out)
    if op == 2 and payload and payload[0] == a.port:
        out += payload[1:]
    elif op == 9:
        send_frame(10, payload)
    if not sends and a.until and re.search(a.until.encode(), bytes(out[mark:])):
        break
    if now - start > a.seconds and not sends:
        break
sys.stdout.write(out.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n"))
