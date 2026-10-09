#!/usr/bin/env python3
"""Debug console client for boards with the colour touch display (lcd_ui.cpp).

Talks to the firmware over the USB serial port without resetting the board:

    lcd_debug.py status                 health of both tasks, SD, Bluetooth
    lcd_debug.py shot out.png           screenshot of what the display shows
    lcd_debug.py tap 160 200            a touch at that position
    lcd_debug.py drag 160 200 160 80    a finger moved from one position to another (scrolls, swipes)
    lcd_debug.py screen 1               0 Status, 1 Terminal, 2 Skripte, 3 WLAN, 4 Web-UI, 5 Bluetooth,
                                        6 System, 7 Info, 8 Setup
    lcd_debug.py cfgtest                store a small configuration "Demo" to try the Skripte page
    lcd_debug.py rot 0                  force a rotation (0..3); "rot auto" hands it back to the sensor
    lcd_debug.py wake | off
    lcd_debug.py bat                    battery: pin voltage, result, what the charge state rests on
    lcd_debug.py batcal full            the voltage measured right now is 100 % ("empty": 0 %, "reset": undo,
                                        "3400 4050": both points in mV, 0 = the curve's own)
    lcd_debug.py c2                     command-and-control server: state, code, timings, stack and heap
    lcd_debug.py saver 1                power saving as on battery: 1 on, 0 off, -1 automatic
    lcd_debug.py poweroff 15            switch off, wake by timer after 15 s (waits for the board to return).
                                        Without a time only the BOOT button switches it on again.
    lcd_debug.py listen 20              print the log for 20 seconds

Several commands can be chained: "screen 5 shot info.png status".
Needs pyserial; "shot" also needs Pillow.
"""
import base64, glob, os, sys, time
import serial


def find_port():
    """The board's USB serial port, or None while it is away (switched off, restarting).

    Not a fixed /dev/ttyACM0: when the board returns while something still holds
    the old device node, the kernel hands out the next number. LCD_PORT overrides.
    """
    if os.environ.get("LCD_PORT"):
        return os.environ["LCD_PORT"] if os.path.exists(os.environ["LCD_PORT"]) else None
    found = sorted(glob.glob("/dev/serial/by-id/usb-Espressif_USB_JTAG*")) or sorted(glob.glob("/dev/ttyACM*"))
    return found[0] if found else None


def open_port():
    """Open the USB serial port and keep the board from being reset by it.

    The chip takes certain DTR/RTS changes as "reset" or "enter the boot loader".
    A plain open raises both lines, which is harmless; what resets the board is
    the kernel dropping them again on close. HUPCL off keeps them where they are.
    """
    import termios
    for attempt in range(40):            # after a restart the port comes and goes for a moment
        try:
            s = serial.Serial(find_port() or "/dev/ttyACM0", 115200, timeout=0.1)
            break
        except serial.SerialException:
            if attempt == 39:
                raise
            time.sleep(0.25)
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
        elif c == "drag":
            s.write(("drag " + " ".join(argv[i + 1:i + 5]) + "\n").encode()); i += 4
            sys.stdout.write(read_until(s, lambda b: False, 1.2).decode("utf8", "replace"))
        elif c == "saver":
            s.write(f"saver {argv[i + 1]}\n".encode()); i += 1
            time.sleep(0.3)
        elif c == "poweroff":
            secs = int(argv[i + 1]) if i + 1 < len(argv) and argv[i + 1].isdigit() else 0
            s.write(f"poweroff {secs}\n".encode() if secs else b"poweroff\n")
            try:
                sys.stdout.write(read_until(s, lambda b: False, 2.5).decode("utf8", "replace"))
                s.close()
            except (serial.SerialException, OSError):
                pass                                 # the port went away under our hands: that is the point
            if not secs:
                return
            i += 1
            t0, gone = time.time(), False            # the USB port disappears while the board is off
            while time.time() - t0 < secs + 30:
                here = find_port() is not None
                if not here and not gone:
                    gone = True
                    print(f"port gone after {time.time() - t0:.1f} s")
                if here and gone:
                    break
                time.sleep(0.2)
            print(f"port {'back' if gone and find_port() else 'NOT back'} after {time.time() - t0:.1f} s")
            time.sleep(2.5)
            s = open_port()
            sys.stdout.write(read_until(s, lambda b: False, 4).decode("utf8", "replace"))
        elif c in ("screen", "rot"):
            s.write(f"{c} {argv[i + 1]}\n".encode()); i += 1
            time.sleep(0.5)
        elif c == "bat":
            s.write(b"bat\n")
            sys.stdout.write(read_until(s, lambda b: b"Akku:" in b and b.endswith(b"\n"), 2).decode("utf8", "replace"))
        elif c == "c2":
            s.write(b"c2\n")
            sys.stdout.write(read_until(s, lambda b: b"Leitstelle:" in b and b.endswith(b"\n"), 2).decode("utf8", "replace"))
        elif c == "batcal":
            args = [argv[i + 1]]; i += 1
            if args[0].isdigit():
                args.append(argv[i + 1]); i += 1
            s.write(f"batcal {' '.join(args)}\n".encode())
            sys.stdout.write(read_until(s, lambda b: b"Kalibrierung:" in b and b.endswith(b"\n"), 3).decode("utf8", "replace"))
        elif c == "cfgtest":
            s.write(b"cfgtest\n")
            sys.stdout.write(read_until(s, lambda b: False, 1.0).decode("utf8", "replace"))
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
