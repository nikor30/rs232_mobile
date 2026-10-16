#!/usr/bin/env python3
"""Signs firmware for the update through the control center.

The private key lives on the owner's computer and nowhere else - not in the
repository, not on the server. A device accepts an update only if its signature
checks out against the public key the device's own firmware was built with
(src/update_key.h), so that header has to be in place *before* the firmware
that is to accept updates is built and flashed.

    fw_sign.py keygen              make the key (once) and write src/update_key.h
    fw_sign.py pubkey              write src/update_key.h again from the key
    fw_sign.py sign [de|en|pt]     sign the build of that language -> dist/*.rsfw
    fw_sign.py sign --image F --lang de
    fw_sign.py show FILE.rsfw      what a package says, and whether its signature holds

Key: ~/.config/rs232-console/firmware-signing-key.pem (or $RS232_FW_KEY).
Needs OpenSSL 3.5 or newer (ML-DSA-65). Losing the key means flashing every
device over USB once more with a new one; a copy of it in the wrong hands means
the same - keep a backup somewhere that is not this computer and not the server.

    package = "RSFW1\\n" manifest "\\n" base64(signature) "\\n" image
"""
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KEY = os.environ.get("RS232_FW_KEY") or os.path.expanduser("~/.config/rs232-console/firmware-signing-key.pem")
HEADER = os.path.join(ROOT, "src", "update_key.h")
CONTEXT = "rs232-console firmware"       # the same string is in src/c2_update.cpp
ALGORITHM = "ML-DSA-65"
PUB_SIZE, SIG_SIZE = 1952, 3309
ENVS = {"en": "waveshare-s3-lcd2", "de": "waveshare-s3-lcd2-de", "pt": "waveshare-s3-lcd2-pt"}
BOARDS = {"waveshare": "waveshare-s3-lcd2"}     # PlatformIO environment without the language
MAGIC = b"RSFW1\n"


def die(text):
    sys.exit("fw_sign: " + text)


def openssl(*args, data=None):
    r = subprocess.run(["openssl", *args], input=data, capture_output=True)
    if r.returncode:
        die("openssl %s: %s" % (args[0], r.stderr.decode(errors="replace").strip() or "failed"))
    return r.stdout


def need_key():
    if not os.path.exists(KEY):
        die("no key at %s - make one with: fw_sign.py keygen" % KEY)


def public_key():
    """The raw public key: the last PUB_SIZE bytes of the DER structure."""
    need_key()
    der = openssl("pkey", "-in", KEY, "-pubout", "-outform", "DER")
    if len(der) < PUB_SIZE:
        die("the key at %s is not an %s key" % (KEY, ALGORITHM))
    return der[-PUB_SIZE:]


def define(name):
    m = re.search(r'#define\s+%s\s+"([^"]+)"' % name, open(os.path.join(ROOT, "src", "config.h")).read())
    if not m:
        die("%s not found in src/config.h" % name)
    return m.group(1)


def write_header():
    pub = public_key()
    rows = [", ".join("0x%02x" % b for b in pub[i:i + 16]) for i in range(0, len(pub), 16)]
    text = (
        "// Public key that firmware updates from the control center are checked against\n"
        "// (%s). Written by tools/fw_sign.py - do not edit. The private half stays on\n"
        "// the owner's computer.\n"
        "#pragma once\n#include <stdint.h>\n\n"
        "#define UPDATE_KEY_PRESENT 1\n"
        "// SHA-256 of the key, first 8 bytes: %s\n"
        "static const uint8_t UPDATE_KEY[%d] = {\n  %s\n};\n"
    ) % (ALGORITHM, hashlib.sha256(pub).hexdigest()[:16], len(pub), ",\n  ".join(rows))
    with open(HEADER, "w") as f:
        f.write(text)
    print("wrote %s (key %s)" % (os.path.relpath(HEADER), hashlib.sha256(pub).hexdigest()[:16]))


def keygen():
    if os.path.exists(KEY):
        die("there is a key already at %s - it is not replaced (devices in the field trust it)" % KEY)
    os.makedirs(os.path.dirname(KEY), mode=0o700, exist_ok=True)
    old = os.umask(0o077)
    try:
        openssl("genpkey", "-algorithm", ALGORITHM, "-out", KEY)
    finally:
        os.umask(old)
    os.chmod(KEY, 0o600)
    print("made %s - keep a backup of it away from this computer" % KEY)
    write_header()
    print("now build and flash the firmware once over USB; from then on it takes signed updates")


def read_package(path):
    b = open(path, "rb").read()
    if not b.startswith(MAGIC):
        die("%s is not a firmware package" % path)
    manifest, _, rest = b[len(MAGIC):].partition(b"\n")
    sig64, _, image = rest.partition(b"\n")
    return manifest, base64.b64decode(sig64), image


def verify(manifest, sig):
    with tempfile.TemporaryDirectory() as d:
        paths = {n: os.path.join(d, n) for n in ("m", "s", "pub")}
        open(paths["m"], "wb").write(manifest)
        open(paths["s"], "wb").write(sig)
        open(paths["pub"], "wb").write(openssl("pkey", "-in", KEY, "-pubout"))
        r = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", paths["pub"], "-in", paths["m"],
                            "-sigfile", paths["s"], "-pkeyopt", "context-string:" + CONTEXT], capture_output=True)
        return r.returncode == 0


def sign(args):
    need_key()
    image_path, lang, board = None, None, "waveshare"
    while args:
        a = args.pop(0)
        if a == "--image":
            image_path = args.pop(0)
        elif a == "--lang":
            lang = args.pop(0)
        elif a == "--board":
            board = args.pop(0)
            if board not in BOARDS:
                die("board: " + " or ".join(BOARDS))
        elif a in ENVS and lang is None:
            lang = a
        else:
            die("sign: do not know what to do with %r" % a)
    lang = lang or "en"
    if lang not in ENVS:
        die("language: en, de or pt")
    env = BOARDS[board] + ("" if lang == "en" else "-" + lang)
    if image_path is None:
        image_path = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
    if not os.path.exists(image_path):
        die("no image at %s - build it first: pio run -e %s" % (image_path, env))
    image = open(image_path, "rb").read()
    pub = public_key()
    if pub[:64] not in image:
        die("this image was not built with the public key of %s:\n"
            "         a device running it would refuse every later update. Run fw_sign.py pubkey and build again." % KEY)
    marker = ("UI_LANG=%s;" % lang).encode()
    if b"UI_LANG=" in image and marker not in image:
        die("this image is not the %s build" % lang)
    # Which hardware the image is for is read from the image itself: config.h
    # names more than one board, and a package must never claim another one.
    m = re.search(rb"FW_BOARD=([a-z0-9-]+);", image)
    if not m:
        die("this image does not say which board it is for (built before the FW_BOARD marker) - build it again")
    board_id = m.group(1).decode()
    seq = int(time.time())
    manifest = json.dumps({"product": "rs232-console", "board": board_id, "fw": define("FW_VERSION"),
                           "lang": lang, "seq": seq, "size": len(image),
                           "sha256": hashlib.sha256(image).hexdigest()}, separators=(",", ":")).encode()
    with tempfile.TemporaryDirectory() as d:
        m, s = os.path.join(d, "m"), os.path.join(d, "s")
        open(m, "wb").write(manifest)
        openssl("pkeyutl", "-sign", "-inkey", KEY, "-in", m, "-out", s, "-pkeyopt", "context-string:" + CONTEXT)
        sig = open(s, "rb").read()
    if len(sig) != SIG_SIZE or not verify(manifest, sig):
        die("the signature just made does not verify")
    out_dir = os.path.join(ROOT, "dist")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "rs232-console-%s-%s-%s-%d.rsfw" % (define("FW_VERSION"), board_id, lang, seq))
    with open(out, "wb") as f:
        f.write(MAGIC + manifest + b"\n" + base64.b64encode(sig) + b"\n" + image)
    print(manifest.decode())
    print("wrote %s (%d bytes)" % (os.path.relpath(out), os.path.getsize(out)))


def show(path):
    manifest, sig, image = read_package(path)
    info = json.loads(manifest)
    for k, v in info.items():
        print("%-8s %s" % (k, v))
    print("%-8s %s" % ("signed", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(info.get("seq", 0)))))
    ok_image = len(image) == info.get("size") and hashlib.sha256(image).hexdigest() == info.get("sha256")
    print("image    %s" % ("matches the manifest" if ok_image else "DOES NOT MATCH the manifest"))
    if os.path.exists(KEY):
        ok_sig = len(sig) == SIG_SIZE and verify(manifest, sig)
        print("signature %s" % ("holds for the key at " + KEY if ok_sig else "DOES NOT HOLD for the key at " + KEY))
    else:
        ok_sig = True
        print("signature not checked (no key here)")
    sys.exit(0 if ok_image and ok_sig else 1)


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
    elif argv[0] == "keygen":
        keygen()
    elif argv[0] == "pubkey":
        write_header()
    elif argv[0] == "sign":
        sign(argv[1:])
    elif argv[0] == "show" and len(argv) == 2:
        show(argv[1])
    else:
        die("unknown command; see --help")


if __name__ == "__main__":
    main(sys.argv[1:])
