"""Drives the administrator's web interface in a real browser (Playwright).

Run by ui.sh in stages, because a simulated device has to act in between:
  invite    log in, create an invitation, print the token
  approve   type a wrong and then the right code for the pending device
  command   send commands and read their answers from the page
  accounts  create an account, set it up as its owner, sign in with password and code
"""
import base64, hashlib, hmac, struct, sys, time
from playwright.sync_api import sync_playwright, expect

stage, base, admin = sys.argv[1], sys.argv[2], sys.argv[3]
arg = sys.argv[4] if len(sys.argv) > 4 else ""

with sync_playwright() as p:
    browser = p.chromium.launch()
    ctx = browser.new_context(ignore_https_errors=True, locale="de-DE", viewport={"width": 1200, "height": 900})
    page = ctx.new_page()
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("dialog", lambda d: d.accept())          # the confirm() questions

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
    expect(page.locator("#srvInfo")).to_contain_text("Serverschlüssel")

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
        page.screenshot(path="/out/ui-invite.png")
        print("TOKEN " + token)

    elif stage == "approve":
        row = page.locator("#devices tbody tr", has_text=arg.split(":")[0])
        expect(row).to_contain_text("wartet auf Bestätigung")
        row.click()
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
        page.screenshot(path="/out/ui-approved.png")

    elif stage == "command":
        page.locator("#devices tbody tr", has_text=arg).click()
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
        expect(hist).to_contain_text("fertig", timeout=20000)
        expect(hist.locator("tr", has_text="Abspielen").first).to_contain_text('"port":2')
        page.click("button[data-cmd=reboot]")
        expect(hist.locator("tr", has_text="Neustart").first).to_contain_text("erledigt", timeout=20000)
        # device data is shown as text, never run: the simulated device is called <img src=x onerror=...>
        assert page.evaluate("window.pwned === undefined"), "markup from a device name was executed"
        assert page.locator("#devices img").count() == 0
        page.screenshot(path="/out/ui-commands.png", full_page=True)
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
        page.screenshot(path="/out/ui-console.png")
        page.click("#conClose")
        expect(term).to_contain_text("vom Administrator beendet", timeout=20000)
        expect(page.locator("#conLog tbody tr").first).to_contain_text("vom Administrator beendet", timeout=20000)
        expect(page.locator("#conForm")).to_be_visible()
        page.click("#revoke")
        expect(page.locator("#dState")).to_have_text("gesperrt")
        expect(page.locator("#commands")).to_be_hidden()

    elif stage == "accounts":
        SLOW = 30000    # a password costs the server 600 000 rounds of hashing: seconds on a busy Raspberry Pi
        # the administrator creates the account and gets a setup code, shown once
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
        expect(page.locator("#users tbody tr", has_text="anna")).to_contain_text("aktiv")
        audit = page.locator("#audit tbody")
        expect(audit).to_contain_text("Konto einrichten")
        expect(audit).to_contain_text("Benutzer angelegt")
        expect(audit.locator("tr.failed").first).to_contain_text("abgelehnt")
        page.screenshot(path="/out/ui-accounts.png", full_page=True)

        # the session is a cookie: it survives a reload, and changing things works with it
        page.reload()
        expect(page.locator("#app")).to_be_visible()
        expect(page.locator("#whoami")).to_have_text("anna")
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
        expect(page.locator("#audit tbody tr").first).to_contain_text("Anmeldung")
        # she cannot lock herself out
        page.locator("#users tbody tr", has_text="anna").get_by_text("Löschen").click()
        expect(page.locator("#toast")).to_contain_text("eigene Konto")
        errors.clear()
        page.click("#logout")

    assert not errors, errors
    browser.close()
print("ok " + stage)
