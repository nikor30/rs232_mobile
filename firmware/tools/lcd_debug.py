#!/usr/bin/env python3
"""Debug console client for boards with the colour touch display (lcd_ui.cpp).

Talks to the firmware over the USB serial port without resetting the board:

    lcd_debug.py status                 health of both tasks, SD, Bluetooth
    lcd_debug.py shot out.png           screenshot of what the display shows
    lcd_debug.py tap 160 200            a touch at that position
    lcd_debug.py screen 1               0 Status, 1 Terminal, 2 WLAN, 3 Web-UI, 4 Bluetooth, 5 Info
    lcd_debug.py rot 0                  force a rotation (0..3)
    lcd_debug.py wake | off
    lcd_debug.py listen 20              print the log for 20 seconds

Several commands can be chained: "screen 5 shot info.png status".
Needs pyserial; "shot" also needs Pillow.
"""
import base64, sys, time
import serial

PORT = "/dev/ttyACM0"


def open_port():
    """Open the USB serial port and keep the board from being reset by it.

    The chip takes certain DTR/RTS changes as "reset" or "enter the boot loader".
    A plain open raises both lines, which is harmless; what resets the board is
    the kernel dropping them again on close. HUPCL off keeps them where they are.
    """
    import termios
    s = serial.Serial(PORT, 115200, timeout=0.1)
    attrs = termios.tcgetattr(s.fd)
    attrs[2] &= ~termios.HUPCL
    termios.tcsetattr(s.fd, termios.TCSANOW, attrs)
    return s


def read_until(s, done, timeout, idle=None):
    """Read until done(buf), the timeout, or (if given) `idle` seconds of silence after the first data."""
    buf, t, last = b"", time.time(), None
    while time.time() - t < timeout:
        d = s.read(4096)
        if d:
            buf += d
            last = time.time()
            if done(buf):
                break
        elif idle and last and time.time() - last > idle:
            break
    return buf


def shot(s, path, swap):
    """Rows arrive run-length coded with a checksum; damaged rows are asked for again."""
    from PIL import Image
    rows, w, h = {}, 0, 0
    request = b"shot\n"
    for attempt in range(60):
        s.write(request)
        raw = read_until(s, lambda b: b"[SHOT] end" in b, 30, idle=1.0).decode("ascii", "replace")
        for line in raw.splitlines():
            if line.startswith("[SHOT] ") and "end" not in line:
                try:
                    w, h = (int(v) for v in line.split()[1:3])
                except ValueError:
                    pass
                continue
            if len(line) < 10 or line[3] != ":" or "#" not in line or not line[:3].isdigit():
                continue
            body, _, check = line[4:].rpartition("#")
            try:
                runs = [(int(r[:4], 16), int(r[5:], 16)) for r in body.split(",") if r]
                total = 0
                for color, n in runs:
                    total = (total * 31 + color * 7 + n) & 0xFFFFFFFF
                if total & 0xFFFF != int(check, 16) or sum(n for _, n in runs) != w:
                    continue
            except ValueError:
                continue
            rows[int(line[:3])] = runs
        missing = [y for y in range(h) if y not in rows]
        if w and not missing:
            break
        if missing:                      # ask again for the first gap only
            last = missing[0]
            while last + 1 in missing:
                last += 1
            request = f"shot {missing[0]} {last}\n".encode()
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y, runs in rows.items():
        x = 0
        for v, n in runs:
            if swap:
                v = (v >> 8 | v << 8) & 0xFFFF
            rgb = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
            for _ in range(n):
                px[x, y] = rgb
                x += 1
    img.save(path)
    print(f"{path}: {w}x{h}, {len(rows)} of {h} rows received")


def main(argv):
    swap = "--noswap" not in argv        # the canvas holds the pixels byte-swapped
    argv = [a for a in argv if a != "--noswap"]
    if not argv:
        print(__doc__)
        return
    s = open_port()
    s.reset_input_buffer()
    i = 0
    while i < len(argv):
        c = argv[i]
        if c == "status":
            s.write(b"?\n")
            sys.stdout.write(read_until(s, lambda b: b"Bluetooth: Zustand" in b and b.endswith(b"\n"), 3).decode("utf8", "replace"))
        elif c == "shot":
            shot(s, argv[i + 1], swap); i += 1
        elif c == "tap":
            s.write(f"tap {argv[i + 1]} {argv[i + 2]}\n".encode()); i += 2
            sys.stdout.write(read_until(s, lambda b: False, 0.8).decode("utf8", "replace"))
        elif c in ("screen", "rot"):
            s.write(f"{c} {argv[i + 1]}\n".encode()); i += 1
            time.sleep(0.5)
        elif c in ("wake", "off"):
            s.write(c.encode() + b"\n"); time.sleep(0.5)
        elif c == "listen":
            sys.stdout.write(read_until(s, lambda b: False, float(argv[i + 1])).decode("utf8", "replace")); i += 1
        elif c == "sleep":
            time.sleep(float(argv[i + 1])); i += 1
        else:
            print("unknown command:", c)
        i += 1


if __name__ == "__main__":
    main(sys.argv[1:])
