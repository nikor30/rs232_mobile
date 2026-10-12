#!/usr/bin/env python3
"""Every text of the device's web interface has to exist in German and in
Portuguese: what stands in web/index.html, and what web/app.js builds with
tr('...'). A text that is the same in a language is listed in SAME, so that
leaving one out by accident does not pass for that.

    tools/i18n_check.py
"""
import html, html.parser, json, os, re, sys

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")
SAME = {
    "de": {"RS232 Console", "rs232", "Break", "Ports", "Setup", "Port 1", "None", "Even", "Odd", "CR+LF", "LF", "XMODEM-1K", "YMODEM",
           "Name", "SSID", "MSCHAPv2", "PAP", "Server", "Code", "HTTPS", "Firmware", "Hardware", "Hostname", "Pause (ms)",
           "Auto-baud …", "“{0}”: {1}", "CR (\\r) – Cisco, Fortinet …", "Port ", "Port {0}", "Port {0} RX", "Port {0} TX",
           '<div class="grid3"><label class="wide">Name <input data-k="name" maxlength="24" placeholder="Port {0}"></label>',
           '<div class="pcard-head"><label class="check"><input type="checkbox" data-k="enabled"> Port {0}</label>'},
    "pt": {"RS232 Console", "rs232", "Break", "Setup", "None", "Even", "Odd", "CR+LF", "LF", "XMODEM-1K", "YMODEM", "Serial", "Config",
           "SSID", "MSCHAPv2", "PAP", "HTTPS", "Firmware", "Hardware", "Hostname", "Auto-baud", "Auto-baud …", "“{0}”: {1}",
           "Wi-Fi (AP)", "WPA2/WPA3 Enterprise · PEAP-MSCHAPv2", "WPA2/WPA3 Enterprise · EAP-TTLS", "CR (\\r) – Cisco, Fortinet …",
           "Menu", "XMODEM (128 bytes)"},
}
# not language: code samples, units, what a device prints
NO_TEXT = re.compile(r"^(….*\.bin|\d[\d ,.]*(ms|px|dBm|kB)?|[.{}%@].*|\(?[a-z0-9_.:\- {}<>/]+\)?|GPIO.*|RX.*|TX.*|…|,|–|.*\{\{.*|enable\n.*|end|loadx|loady|copy xmodem: flash:|, IOS|, U-Boot|8N1)$", re.S)


class Texts(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.out, self.skip = set(), 0

    def handle_starttag(self, tag, attrs):
        self.skip += tag in ("script", "style", "svg")
        for k, v in attrs:
            if k in ("placeholder", "aria-label", "title") and v:
                self.out.add(v)

    def handle_endtag(self, tag):
        self.skip -= tag in ("script", "style", "svg")

    def handle_data(self, data):
        data = re.sub(r"\s+", " ", data).strip()
        if data and not self.skip:
            self.out.add(data)


def main():
    p = Texts()
    p.feed(open(os.path.join(WEB, "index.html"), encoding="utf-8").read())
    there = {"page": p.out}
    page = {t for t in p.out if re.search(r"[A-Za-z]{2}", t) and not NO_TEXT.match(t)}
    script = open(os.path.join(WEB, "app.js"), encoding="utf-8").read()
    built = {k.replace("\\'", "'").replace("\\n", "\n").replace("\\\\", "\\") for k in re.findall(r"\btr\('((?:[^'\\]|\\.)*)'", script)}
    bad = 0
    for lang in ("de", "pt"):
        d = json.load(open(os.path.join(WEB, "lang", lang + ".json"), encoding="utf-8"))
        for where, keys, have in (("page", page, d["page"]), ("script", built, d["script"])):
            missing = sorted(k for k in keys if k not in have and k not in SAME[lang])
            for k in missing:
                print("%s, %s: no translation for %r" % (lang, where, k))
            bad += len(missing)
            for k, v in have.items():                # the values a text takes must survive the translation
                if sorted(re.findall(r"\{\d+\}", k)) != sorted(re.findall(r"\{\d+\}", v)):
                    print("%s, %s: placeholders differ in %r -> %r" % (lang, where, k, v))
                    bad += 1
            stale = sorted(k for k in have if k not in there.get(where, keys))
            for k in stale:
                print("%s, %s: entry for a text that is not there (any more): %r" % (lang, where, k))
            bad += len(stale)
        print("%s: %d texts in the page, %d built by the script" % (lang, len(page), len(built)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
