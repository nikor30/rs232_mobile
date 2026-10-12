"""Drives the administrator's web interface in a real browser (Playwright).

Run by ui.sh in stages, because a simulated device has to act in between:
  invite    log in, create an invitation, print the token
  approve   type a wrong and then the right code for the pending device
  server    the server's state; change the server key and end the change
  command   send commands and read their answers from the page, use the remote console
  accounts  create an account, set it up as its owner, sign in with password and code
  phone     the same pages on a phone: nothing wider than the screen, one thing at a time
  revoke    lock the device out
"""
import base64, hashlib, hmac, struct, sys, time
from playwright.sync_api import sync_playwright, expect

stage, base, admin = sys.argv[1], sys.argv[2], sys.argv[3]
arg = sys.argv[4] if len(sys.argv) > 4 else ""

with sync_playwright() as p:
    browser = p.chromium.launch()
    PHONE = {"width": 390, "height": 844}
    if stage == "phone":
        ctx = browser.new_context(ignore_https_errors=True, locale="de-DE", viewport=PHONE, device_scale_factor=2, is_mobile=True, has_touch=True)
    else:
        ctx = browser.new_context(ignore_https_errors=True, locale="de-DE", viewport={"width": 1200, "height": 900})
    page = ctx.new_page()
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))

    def shot(**kw):                                  # a picture of the page at rest, not of a transition
        page.wait_for_timeout(450)
        page.screenshot(**kw)

    def go(view):                                    # the places in the navigation
        page.click("#nav a[data-view=%s]" % view)
        expect(page.locator("#v-" + view)).to_be_visible()

    def yes(text):                                   # the question before something that cannot be taken back
        expect(page.locator("#askDlg")).to_be_visible()
        expect(page.locator("#askDlg")).to_contain_text(text)
        page.click("#askYes")
        expect(page.locator("#askDlg")).to_be_hidden()

    def totp(secret, step_offset=0):
        key = base64.b32decode(secret.replace(" ", ""))
        mac = hmac.new(key, struct.pack(">Q", int(time.time()) // 30 + step_offset), hashlib.sha1).digest()
        return "%06d" % ((struct.unpack(">I", mac[mac[-1] & 15:][:4])[0] & 0x7fffffff) % 1000000)

    page.goto(base + "/")
    expect(page.locator("#entraLogin")).to_be_hidden()   # not configured here
    page.click("#tokenBox summary")
    if stage == "invite":
        # a wrong token is refused and nothing is shown
        page.fill("#adminToken", "not-the-token-0123456789")
        page.click("#tokenForm button")
        expect(page.locator("#loginErr")).to_be_visible()
        expect(page.locator("#app")).to_be_hidden()
        errors.clear()                               # the 401 is the point of that step
    page.fill("#adminToken", admin)
    page.click("#tokenForm button")
    expect(page.locator("#app")).to_be_visible()
    expect(page.locator("#whoami")).to_have_text("Admin-Token")
    expect(page.locator("#title")).to_have_text("Geräte")
    expect(page.locator("#nav a[aria-current]")).to_have_text("Geräte")

    if stage == "invite":
        expect(page.locator("#noDevices")).to_be_visible()
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        token = page.input_value("#tokenText")
        assert token.startswith("c2e1:"), token
        expect(page.locator("#tokenExp")).to_contain_text("gilt noch")
        # the QR code must be a real picture: read it back and compare
        size = page.evaluate("document.querySelector('#qr').width")
        assert size > 100, size
        page.locator("#qr").screenshot(path="/out/qr.png")
        shot(path="/out/ui-invite.png")
        print("TOKEN " + token)

    elif stage == "approve":
        expect(page.locator("#pick")).to_be_visible()    # nothing picked yet, and the page says so
        row = page.locator("#devices .dev", has_text=arg.split(":")[0])
        expect(row).to_contain_text("wartet auf Bestätigung")
        row.click()
        expect(row).to_have_attribute("aria-current", "true")
        assert page.url.endswith("#geraete/" + row.get_attribute("data-id")), page.url
        expect(page.locator("#approveForm")).to_be_visible()
        expect(page.locator("#commands")).to_be_hidden()
        code = arg.split(":")[1]
        assert code not in page.content(), "the page must not know the code"
        wrong = "%06d" % ((int(code) + 1) % 1000000)
        page.fill("#sas", wrong)
        page.click("#approveForm button")
        expect(page.locator("#toast")).to_contain_text("stimmt nicht")
        errors.clear()                               # the browser logs the 403, as intended
        page.fill("#sas", code)
        page.click("#approveForm button")
        expect(page.locator("#dState")).to_have_text("verbunden")
        expect(page.locator("#commands")).to_be_visible()
        shot(path="/out/ui-approved.png")

    elif stage == "server":
        go("server")
        expect(page.locator("#stState")).to_have_text("in Ordnung")
        expect(page.locator("#stActive")).to_have_text("1")
        expect(page.locator("#alert")).to_be_hidden()
        expect(page.locator("#keyChange")).to_be_hidden()
        expect(page.locator("#navMark")).to_be_hidden()
        fp = page.locator("#srvFp").inner_text()
        assert len(fp) == 64, fp
        shot(path="/out/ui-server.png", full_page=True)
        # a new key: the page says what that means before it happens
        page.click("#kcStart")
        expect(page.locator("#rotateDlg")).to_be_visible()
        expect(page.locator("#rotateDlg")).to_contain_text("neu angemeldet")
        page.select_option("#rotateDays", "7")
        shot(path="/out/ui-rotate.png")
        page.click("#rotateCancel")
        expect(page.locator("#rotateDlg")).to_be_hidden()
        expect(page.locator("#srvFp")).to_have_text(fp)  # nothing happened
        page.click("#kcStart")
        page.select_option("#rotateDays", "7")
        page.click("#rotateForm button[type=submit]")
        expect(page.locator("#keyChange")).to_be_visible()
        expect(page.locator("#kcOld")).to_have_text(fp)
        expect(page.locator("#srvFp")).not_to_have_text(fp)
        expect(page.locator("#navMark")).to_be_visible()
        expect(page.locator("#kcStart")).to_be_hidden()  # one change at a time
        # the simulated device hears of the key with its next poll and confirms it
        expect(page.locator("#kcWaiting")).to_have_text("kein Gerät", timeout=20000)
        shot(path="/out/ui-keychange.png", full_page=True)
        page.click("#kcEnd")
        yes("Alle Geräte haben den neuen schon")
        expect(page.locator("#keyChange")).to_be_hidden()
        expect(page.locator("#navMark")).to_be_hidden()
        expect(page.locator("#kcStart")).to_be_visible()
        go("protokoll")
        expect(page.locator("#audit tbody")).to_contain_text("Serverschlüssel gewechselt")
        expect(page.locator("#audit tbody")).to_contain_text("Schlüsselwechsel beendet")
        go("geraete")
        row = page.locator("#devices .dev", has_text="Labor")
        expect(row).to_contain_text("verbunden")
        expect(row).not_to_contain_text("alter Serverschlüssel")

    elif stage == "command":
        page.locator("#devices .dev", has_text=arg).click()
        hist = page.locator("#history tbody")
        page.click("button[data-cmd=ping]")
        expect(hist).to_contain_text("pong", timeout=20000)
        page.click("button[data-cmd=configs]")
        expect(hist).to_contain_text("Uplink", timeout=20000)
        # the names offered for "Abspielen" come from that answer
        assert page.locator("#playNames option").count() == 2
        page.fill("#playName", "Demo")
        page.select_option("#playPort", "2")
        page.click("#playForm button")
        yes("auf Port 2 abspielen")
        expect(hist).to_contain_text("fertig", timeout=20000)
        expect(hist.locator("tr", has_text="Abspielen").first).to_contain_text('"port":2')
        page.click("button[data-cmd=reboot]")
        yes("neu starten")
        expect(hist.locator("tr", has_text="Neustart").first).to_contain_text("erledigt", timeout=20000)
        # device data is shown as text, never run: the simulated device is called <img src=x onerror=...>
        assert page.evaluate("window.pwned === undefined"), "markup from a device name was executed"
        assert page.locator("#devices img").count() == 0
        shot(path="/out/ui-commands.png", full_page=True)
        # remote console: ask, the (simulated) device agrees, type, read, BREAK, close
        page.select_option("#conMinutes", "5")
        page.click("#conForm button")
        expect(page.locator("#conState")).to_contain_text("Offen auf Port 1", timeout=20000)
        term = page.locator("#term .xterm-rows")
        expect(term).to_contain_text("Switch>", timeout=20000)
        page.locator("#term .xterm-helper-textarea").focus()
        page.keyboard.type("show")
        page.keyboard.press("Enter")
        expect(term).to_contain_text("GigabitEthernet0/40 is up", timeout=20000)
        page.click("#conBreak")
        expect(term).to_contain_text("[BREAK]", timeout=20000)
        expect(page.locator("#conInfo")).to_contain_text("noch")
        expect(page.locator("#conForm")).to_be_hidden()
        # not full_page: with the terminal focused that leaves the page scrolled somewhere
        # else than the browser believes, and the next click misses its button
        page.locator("#console").scroll_into_view_if_needed()
        shot(path="/out/ui-console.png")
        page.click("#conClose")
        expect(term).to_contain_text("vom Administrator beendet", timeout=20000)
        expect(page.locator("#conLog tbody tr").first).to_contain_text("vom Administrator beendet", timeout=20000)
        expect(page.locator("#conForm")).to_be_visible()

    elif stage == "revoke":
        page.locator("#devices .dev", has_text=arg).click()
        page.click("#revoke")
        expect(page.locator("#askNo")).to_be_focused()   # Enter is not the dangerous answer
        page.keyboard.press("Escape")
        expect(page.locator("#askDlg")).to_be_hidden()
        expect(page.locator("#dState")).to_have_text("verbunden")
        page.click("#revoke")
        yes("sofort und endgültig")
        expect(page.locator("#dState")).to_have_text("gesperrt")
        expect(page.locator("#commands")).to_be_hidden()

    elif stage == "phone":
        def fits(what):                              # nothing may be wider than the screen
            w = page.evaluate("[document.documentElement.scrollWidth, window.innerWidth]")
            assert w[0] <= w[1], "%s is %d wide on a screen of %d" % (what, w[0], w[1])
        def targets(what):                           # what a thumb has to hit is big enough for one
            small = page.evaluate("""[...document.querySelectorAll('.btn, nav a, .dev, .back, select, input')]
                .filter(e => e.offsetParent && e.getBoundingClientRect().height < 43.5)
                .map(e => (e.id || e.textContent.trim().slice(0, 20)) + ' ' + Math.round(e.getBoundingClientRect().height))""")
            assert not small, "%s: too small to tap: %s" % (what, small)
        nav = page.locator("#nav").bounding_box()
        assert nav["y"] + nav["height"] == PHONE["height"] and nav["width"] == PHONE["width"], nav   # the places are under the thumb
        fits("the device list"); targets("the device list")
        shot(path="/out/phone-devices.png")
        # one device: it takes the screen, and there is a way back
        page.locator("#devices .dev", has_text=arg).click()
        expect(page.locator("#detail")).to_be_visible()
        expect(page.locator("#listCard")).to_be_hidden()
        expect(page.locator("#back")).to_be_visible()
        expect(page.locator("#history")).to_be_visible()
        fits("a device"); targets("a device")
        shot(path="/out/phone-device.png", full_page=True)
        page.click("#back")
        expect(page.locator("#listCard")).to_be_visible()
        expect(page.locator("#detail")).to_be_hidden()
        page.go_back()                               # the browser's own way back leads to the device again
        expect(page.locator("#detail")).to_be_visible()
        page.click("#back")
        for view in ("benutzer", "protokoll", "server"):
            go(view)
            expect(page.locator("#title")).to_have_text({"benutzer": "Benutzer", "protokoll": "Protokoll", "server": "Server"}[view])
            fits(view); targets(view)
            shot(path="/out/phone-%s.png" % view, full_page=True)
        # a dialog comes up from the bottom edge, as wide as the screen
        page.click("#kcStart")
        expect(page.locator("#rotateDlg")).to_be_visible()
        page.wait_for_timeout(500)                   # until it has arrived
        box = page.locator("#rotateDlg").bounding_box()
        assert box["width"] == PHONE["width"] and abs(box["y"] + box["height"] - PHONE["height"]) < 1, box
        targets("the dialog")
        shot(path="/out/phone-dialog.png")
        page.click("#rotateCancel")
        go("geraete")
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        page.wait_for_timeout(500)
        fits("the invitation")
        shot(path="/out/phone-invite.png")
        page.click("#closeInvite")
        # and in the dark
        page.emulate_media(color_scheme="dark")
        shot(path="/out/phone-devices-dark.png")
        page.locator("#devices .dev", has_text=arg).click()
        shot(path="/out/phone-device-dark.png")
        page.emulate_media(color_scheme="light")

    elif stage == "accounts":
        SLOW = 30000    # a password costs the server 600 000 rounds of hashing: seconds on a busy Raspberry Pi
        # the administrator creates the account and gets a setup code, shown once
        go("benutzer")
        expect(page.locator("#noUsers")).to_be_visible()
        page.click("#addUser")
        page.fill("#userName", "Anna")
        page.click("#userForm button[type=submit]")
        expect(page.locator("#codeDlg")).to_be_visible()
        setup = page.locator("#codeText").inner_text()
        assert len(setup) == 19 and setup.count("-") == 3, setup
        page.click("#codeClose")
        row = page.locator("#users tbody tr", has_text="anna")
        expect(row).to_contain_text("wartet auf Einrichtung")
        assert setup not in page.content(), "the setup code stays on the page"
        page.click("#logout")
        expect(page.locator("#login")).to_be_visible()

        # the owner: setup code, password, authenticator
        page.click("#setupBox summary")
        page.fill("#setupName", "anna")
        page.fill("#setupCode", "AAAA-BBBB-CCCC-DDDD")
        page.fill("#setupPass", "ein langes Passwort 1")
        page.fill("#setupPass2", "ein langes Passwort 1")
        page.click("#setupForm button")
        expect(page.locator("#setupErr")).to_contain_text("stimmt nicht", timeout=SLOW)
        page.fill("#setupCode", setup.lower())           # typed as somebody would
        page.fill("#setupPass2", "ein anderes Passwort")
        page.click("#setupForm button")
        expect(page.locator("#setupErr")).to_contain_text("nicht gleich")
        page.fill("#setupPass2", "ein langes Passwort 1")
        page.click("#setupForm button")
        expect(page.locator("#totpForm")).to_be_visible(timeout=SLOW)
        secret = page.locator("#totpSecret").inner_text()
        assert page.evaluate("document.querySelector('#totpQr').width") > 100
        page.fill("#totpCode", "000000" if totp(secret) != "000000" else "111111")
        page.click("#totpForm button")
        expect(page.locator("#setupErr")).to_contain_text("Code stimmt nicht", timeout=SLOW)
        page.fill("#totpCode", totp(secret))
        page.click("#totpForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        expect(page.locator("#whoami")).to_have_text("anna")
        errors.clear()                               # the refusals above were the point
        expect(page.locator("#title")).to_have_text("Benutzer")   # back where she was
        expect(page.locator("#users tbody tr", has_text="anna")).to_contain_text("aktiv")
        shot(path="/out/ui-accounts.png", full_page=True)
        go("protokoll")
        audit = page.locator("#audit tbody")
        expect(audit).to_contain_text("Konto einrichten")
        expect(audit).to_contain_text("Benutzer angelegt")
        expect(audit.locator("tr.failed").first).to_contain_text("abgelehnt")
        shot(path="/out/ui-audit.png", full_page=True)

        # the session is a cookie: it survives a reload, and changing things works with it
        page.reload()
        expect(page.locator("#app")).to_be_visible()
        expect(page.locator("#whoami")).to_have_text("anna")
        expect(page.locator("#title")).to_have_text("Protokoll")   # a reload stays where one was
        go("geraete")
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        assert page.input_value("#tokenText").startswith("c2e1:")
        page.click("#closeInvite")
        cookies = ctx.cookies()
        assert len(cookies) == 1 and cookies[0]["httpOnly"] and cookies[0]["secure"] and cookies[0]["sameSite"] == "Strict" \
            and cookies[0]["name"].startswith("__Host-"), cookies
        assert page.evaluate("sessionStorage.length") == 0, "something is kept in the tab"
        page.click("#logout")
        expect(page.locator("#login")).to_be_visible()
        page.reload()
        expect(page.locator("#login")).to_be_visible()   # signed out at the server, not only on the page

        # signing in: wrong password, then right, with the next code (the one from the setup is spent)
        page.fill("#loginName", "anna")
        page.fill("#loginPass", "das falsche Passwort!")
        page.fill("#loginCode", totp(secret, 1))
        page.click("#loginForm button")
        expect(page.locator("#loginErr")).to_have_text("Anmeldung fehlgeschlagen", timeout=SLOW)
        errors.clear()
        page.fill("#loginPass", "ein langes Passwort 1")
        page.fill("#loginCode", totp(secret, 1))
        page.click("#loginForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        go("protokoll")
        expect(page.locator("#audit tbody tr").first).to_contain_text("Anmeldung")
        # she cannot lock herself out
        go("benutzer")
        page.locator("#users tbody tr", has_text="anna").get_by_text("Löschen").click()
        yes("löschen")
        expect(page.locator("#toast")).to_contain_text("eigene Konto")
        errors.clear()
        page.click("#logout")

    assert not errors, errors
    browser.close()
print("ok " + stage)
