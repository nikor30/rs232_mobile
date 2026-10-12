#!/usr/bin/env python3
"""Every text of the administrator's interface has to exist in German and in
Portuguese: what stands in index.html, and what app.js builds with t('...').
A text that is the same in a language is listed in SAME, so that leaving one
out by accident does not pass for that.

    ./test/i18n_check.py        (run by ui.sh)
"""
import html.parser, json, os, re, sys

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "internal", "server", "web")
SAME = {  # English word = German word / Portuguese word, or nothing to translate
    "de": {"Server", "Ping", "Port", "Name", "BREAK", "Status", "Admin-Token", "Microsoft Entra ID", "firmware {v}"},
    "pt": {"Ping", "BREAK", "Microsoft Entra ID", "firmware {v}"},
}


class Texts(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.out, self.skip = set(), 0

    def handle_starttag(self, tag, attrs):
        self.skip += tag in ("script", "style", "svg", "select")     # the language menu names languages
        for k, v in attrs:
            if k in ("placeholder", "aria-label", "title") and v and re.search(r"[A-Za-z]{2}", v):
                self.out.add(v)

    def handle_endtag(self, tag):
        self.skip -= tag in ("script", "style", "svg", "select")

    def handle_data(self, data):
        data = re.sub(r"\s+", " ", data).strip()
        if data and not self.skip and re.search(r"[A-Za-z]{2}", data):
            self.out.add(data)


def main():
    p = Texts()
    page = open(os.path.join(WEB, "index.html"), encoding="utf-8").read()
    p.feed(page)
    # <select> was skipped for the language menu; the other menus do have texts
    for options in re.findall(r"<select id=\"(?!lang)[^>]*>(.*?)</select>", page, re.S):
        for o in re.findall(r"<option[^>]*>([^<]+)</option>", options):
            if re.search(r"[A-Za-z]{3}", o) and not re.fullmatch(r"\d+ min", o.strip()):
                p.out.add(o.strip())
    keys = set(p.out)
    script = open(os.path.join(WEB, "app.js"), encoding="utf-8").read()
    for k in re.findall(r"\bt\('((?:[^'\\]|\\.)*)'", script):
        keys.add(k.replace("\\'", "'"))
    src = open(os.path.join(WEB, "i18n.js"), encoding="utf-8").read()
    bad = 0
    for lang in ("de", "pt"):
        body = re.search(r"\b%s: (\{.*?\n    \})" % lang, src, re.S).group(1)
        have = json.loads(re.sub(r",\s*\}", "}", body))
        missing = sorted(k for k in keys if k not in have and k not in SAME[lang])
        for k in missing:
            print("%s: no translation for %r" % (lang, k))
        # placeholders must survive the translation
        for k, v in have.items():
            if sorted(re.findall(r"\{\w+\}", k)) != sorted(re.findall(r"\{\w+\}", v)):
                print("%s: placeholders differ in %r -> %r" % (lang, k, v))
                bad += 1
        bad += len(missing)
        print("%s: %d texts, %d translated" % (lang, len(keys), len(have)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
