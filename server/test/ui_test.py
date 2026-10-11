"""Drives the administrator's web interface in a real browser (Playwright).

Run by ui.sh in three stages, because a simulated device has to act in between:
  invite   log in, create an invitation, print the token
  approve  type a wrong and then the right code for the pending device
  command  send commands and read their answers from the page
"""
import sys
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

    page.goto(base + "/")
    if stage == "invite":
        # a wrong token is refused and nothing is shown
        page.fill("#adminToken", "not-the-token-0123456789")
        page.click("#loginForm button")
        expect(page.locator("#loginErr")).to_be_visible()
        expect(page.locator("#app")).to_be_hidden()
        errors.clear()                               # the 401 is the point of that step
    page.fill("#adminToken", admin)
    page.click("#loginForm button")
    expect(page.locator("#app")).to_be_visible()
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

    assert not errors, errors
    browser.close()
print("ok " + stage)
