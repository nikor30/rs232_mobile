"""Drives the administrator's web interface in a real browser (Playwright).

Run by ui.sh in stages, because a simulated device has to act in between:
  invite    log in, create an invitation, print the token
  approve   type a wrong and then the right code for the pending device
  server    the server's state; change the server key and end the change
  command   send commands and read their answers from the page, use the remote console
  accounts  create an account, set it up as its owner, sign in with password and code
  language  German and Portuguese: the page, what the script builds, what the server answers
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
        expect(page.locator("#v-" + ("devices" if view == "manage" else view))).to_be_visible()
        expect(page.locator("#nav a[aria-current]")).to_have_attribute("data-view", view)

    def pick(name, view="manage"):                   # a device: under "Devices" to use it, under "Manage" to look after it
        go(view)
        row = page.locator("#devices .dev", has_text=name)
        row.click()
        expect(page.locator("#detail")).to_be_visible()
        return row

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
    # the product's mark: large over the sign-in form, and a real picture
    expect(page.locator(".hero")).to_contain_text("RS232 Control Center")
    assert page.evaluate("document.querySelector('.hero img').naturalWidth") > 0, "the logo did not load"
    expect(page.locator("header .brand")).to_be_hidden()
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
    expect(page.locator("#whoami")).to_have_text("Admin token")
    expect(page.locator("#title")).to_have_text("Devices")
    expect(page.locator("#nav a[aria-current]")).to_have_text("Devices")
    expect(page.locator("header .brand img")).to_be_visible()
    if stage != "phone":
        # a wide screen: mark, name and the places stand at the side, one under the other
        expect(page.locator("header .brand")).to_contain_text("RS232 Control Center")
        box = [page.locator("#nav a").nth(i).bounding_box() for i in (0, 1)]
        assert box[1]["y"] > box[0]["y"] and abs(box[1]["x"] - box[0]["x"]) < 1, box
        assert page.locator("main#app").bounding_box()["x"] >= page.locator("header").bounding_box()["width"], "the content lies under the side bar"

    if stage == "invite":
        expect(page.locator("#noDevices")).to_be_visible()
        expect(page.locator("#invite")).to_be_hidden()   # inviting is the administrator's business: under "Manage"
        go("manage")
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        token = page.input_value("#tokenText")
        assert token.startswith("c2e1:"), token
        expect(page.locator("#tokenExp")).to_contain_text("valid for another")
        # the QR code must be a real picture: read it back and compare
        size = page.evaluate("document.querySelector('#qr').width")
        assert size > 100, size
        page.locator("#qr").screenshot(path="/out/qr.png")
        shot(path="/out/ui-invite.png")
        print("TOKEN " + token)

    elif stage == "approve":
        expect(page.locator("#pick")).to_be_visible()    # nothing picked yet, and the page says so
        row = page.locator("#devices .dev", has_text=arg.split(":")[0])
        expect(row).to_contain_text("waiting for confirmation")
        # under "Devices" there is nothing to do with it yet, and the page says where to go
        row.click()
        expect(page.locator("#dNotActive")).to_contain_text("waits for its confirmation")
        expect(page.locator("#approveForm")).to_be_hidden()
        expect(page.locator("#console")).to_be_hidden()
        page.click("#toManage")
        expect(page.locator("#nav a[aria-current]")).to_have_text("Manage")
        expect(row).to_have_attribute("aria-current", "true")
        assert page.url.endswith("#manage/" + row.get_attribute("data-id")), page.url
        expect(page.locator("#approveForm")).to_be_visible()
        expect(page.locator("#commands")).to_be_hidden()
        code = arg.split(":")[1]
        assert code not in page.content(), "the page must not know the code"
        wrong = "%06d" % ((int(code) + 1) % 1000000)
        page.fill("#sas", wrong)
        page.click("#approveForm button")
        expect(page.locator("#toast")).to_contain_text("does not match")
        errors.clear()                               # the browser logs the 403, as intended
        page.fill("#sas", code)
        page.click("#approveForm button")
        expect(page.locator("#dState")).to_have_text("connected")
        expect(page.locator("#commands")).to_be_visible()
        shot(path="/out/ui-approved.png")

    elif stage == "server":
        go("server")
        expect(page.locator("#stState")).to_have_text("all right")
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
        expect(page.locator("#rotateDlg")).to_contain_text("enrolled again")
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
        expect(page.locator("#kcWaiting")).to_have_text("no device", timeout=20000)
        shot(path="/out/ui-keychange.png", full_page=True)
        page.click("#kcEnd")
        yes("All devices have the new one already")
        expect(page.locator("#keyChange")).to_be_hidden()
        expect(page.locator("#navMark")).to_be_hidden()
        expect(page.locator("#kcStart")).to_be_visible()
        go("audit")
        expect(page.locator("#audit tbody")).to_contain_text("Server key changed")
        expect(page.locator("#audit tbody")).to_contain_text("Key change ended")
        go("manage")
        row = page.locator("#devices .dev", has_text="Labor")
        expect(row).to_contain_text("online")
        expect(row).not_to_contain_text("old server key")

    elif stage == "command":
        # "Devices" is for using one: its ports and nothing of the rest
        pick(arg, "devices")
        expect(page.locator("#conForm")).to_be_visible()
        for hidden in ("#commands", "#settings", "#backup", "#history", "#revoke", "#dId"):
            expect(page.locator(hidden)).to_be_hidden()
        shot(path="/out/ui-device.png")
        pick(arg)
        expect(page.locator("#conForm")).to_be_hidden()
        hist = page.locator("#history tbody")
        page.click("button[data-cmd=ping]")
        expect(hist).to_contain_text("pong", timeout=20000)
        page.click("button[data-cmd=configs]")
        expect(hist).to_contain_text("Uplink", timeout=20000)
        # the names offered for "Play" come from that answer
        assert page.locator("#playNames option").count() == 2
        page.fill("#playName", "Demo")
        page.select_option("#playPort", "2")
        page.click("#playForm button")
        yes("on port 2")
        expect(hist).to_contain_text("\"result\": \"done\"", timeout=20000)
        expect(hist.locator("tr", has_text="Play").first).to_contain_text('"port":2')
        page.click("button[data-cmd=reboot]")
        yes("Restart the device")
        expect(hist.locator("tr", has_text="Restart").first).to_contain_text("done", timeout=20000)
        # device data is shown as text, never run: the simulated device is called <img src=x onerror=...>
        assert page.evaluate("window.pwned === undefined"), "markup from a device name was executed"
        assert page.locator("#devices img").count() == 0
        shot(path="/out/ui-commands.png", full_page=True)
        # settings: read from the device, change one, the device stores it; a wrong value is the device's to refuse
        page.click("#setLoad")
        host = page.locator("#setFields [data-path=hostname]")
        expect(host).to_have_value("rs232", timeout=20000)
        expect(page.locator("#setFields [data-path='ports.0.serial.baud']")).to_have_value("9600")
        expect(page.locator("#setFields [data-path='ports.0.enabled']")).to_be_disabled()
        shot(path="/out/ui-settings.png", full_page=True)
        page.click("#setForm button[type=submit]")
        expect(page.locator("#toast")).to_contain_text("Nothing was changed")
        host.fill("not a name")
        page.click("#setForm button[type=submit]")
        yes("stores them and restarts")
        expect(page.locator("#setState")).to_contain_text("Hostname", timeout=20000)
        host.fill("lab-7")
        page.fill("#setFields [data-path=apPass]", "geheim-geheim-1")
        page.select_option("#setFields [data-path='ports.0.serial.baud']", "115200")
        page.click("#setForm button[type=submit]")
        yes("stores them and restarts")
        expect(page.locator("#setState")).to_contain_text("Saved", timeout=20000)
        row = hist.locator("tr", has_text="Change settings").first
        expect(row).to_contain_text("done")
        assert "geheim" not in page.content(), "a password stays in the history"
        page.click("#setLoad")
        expect(host).to_have_value("lab-7", timeout=20000)
        expect(page.locator("#setFields [data-path='ports.0.serial.baud']")).to_have_value("115200")
        # backup: one file with the settings and both configurations; restoring it writes them back
        with page.expect_download(timeout=60000) as dl:
            page.click("#bkSave")
        expect(page.locator("#bkState")).to_contain_text("2 configurations")
        import json
        doc = json.load(open(dl.value.path()))
        assert doc["kind"] == "rs232-backup" and doc["settings"]["hostname"] == "lab-7", doc
        assert sorted(c["name"] for c in doc["configs"]) == ["Demo", "Uplink"] and "show version" in doc["configs"][0]["text"], doc["configs"]
        assert dl.value.suggested_filename.startswith("rs232-Labor") and dl.value.suggested_filename.endswith(".json"), dl.value.suggested_filename
        doc["settings"]["hostname"] = "lab-8"
        doc["configs"].append({"name": "Neu", "text": "show inventory\n"})
        page.set_input_files("#bkFile", {"name": "backup.json", "mimeType": "application/json", "buffer": json.dumps(doc).encode()})
        yes("3 configurations")
        expect(page.locator("#bkState")).to_contain_text("Restored", timeout=60000)
        page.set_input_files("#bkFile", {"name": "x.json", "mimeType": "application/json", "buffer": b"{}"})
        expect(page.locator("#bkState")).to_contain_text("not a backup")
        page.click("button[data-cmd=configs]")
        expect(hist.locator("tr", has_text="Configurations").first).to_contain_text("Neu", timeout=20000)
        page.click("#setLoad")
        expect(host).to_have_value("lab-8", timeout=20000)
        shot(path="/out/ui-backup.png", full_page=True)
        # firmware: a signed package goes onto the shelf, the (simulated) device fetches and "installs" it.
        # The simulator does not check signatures - that is the firmware's part (tests/c2/run.sh).
        image = bytes((i * 7) & 255 for i in range(30000))
        manifest = json.dumps({"product": "rs232-console", "board": "waveshare-s3-lcd2", "fw": "9.9.9", "lang": "pt", "seq": 1791900000,
                               "size": len(image), "sha256": hashlib.sha256(image).hexdigest()}, separators=(",", ":")).encode()
        pkg = b"RSFW1\n" + manifest + b"\n" + base64.b64encode(b"\x5a" * 3309) + b"\n" + image
        expect(page.locator("#fwNone")).to_be_visible()
        page.set_input_files("#fwFile", {"name": "x.rsfw", "mimeType": "application/octet-stream", "buffer": b"hello"})
        expect(page.locator("#fwState")).to_contain_text("not a firmware package")
        page.set_input_files("#fwFile", {"name": "damaged.rsfw", "mimeType": "application/octet-stream", "buffer": pkg[:-1] + b"\x00"})
        expect(page.locator("#fwState")).to_contain_text("damaged file")
        assert len(errors) == 2, errors                  # the browser logs the two refusals, as intended
        errors.clear()
        page.set_input_files("#fwFile", {"name": "good.rsfw", "mimeType": "application/octet-stream", "buffer": pkg})
        expect(page.locator("#fwState")).to_contain_text("Uploaded: version 9.9.9")
        fwrow = page.locator("#fwList tbody tr")
        expect(fwrow).to_have_count(1)
        expect(fwrow).to_contain_text("Português")
        shot(path="/out/ui-firmware.png", full_page=True)
        fwrow.get_by_role("button", name="Install").click()
        yes("9.9.9")
        expect(page.locator("#fwState")).to_contain_text("Installed: version 9.9.9", timeout=60000)
        expect(hist.locator("tr", has_text="Firmware update").first).to_contain_text("updated to 9.9.9 (pt), restarting", timeout=20000)
        fwrow.get_by_role("button", name="Delete").click()
        yes("9.9.9")
        expect(page.locator("#fwNone")).to_be_visible(timeout=20000)
        # where the device belongs: the list is grouped by it, and the search finds it
        page.fill("#dRegion", "Nord")
        page.fill("#dSite", "Werk 2")
        page.click("#placeSave")
        expect(page.locator("#devices .grp")).to_have_text("Nord · Werk 2")
        expect(page.locator("#devices .dev")).to_contain_text("online")
        # the library: a template is written, sorted into a folder, sent to the device and deleted
        go("library")
        expect(page.locator("#noTpls")).to_be_visible()
        page.click("#tplNew")
        page.fill("#tplName", "Uplink neu")
        page.fill("#tplFolder", "Cisco")
        page.fill("#tplText", "conf t\nint g0/1\n description uplink\n")
        page.click("#tplForm button[type=submit]")
        trow = page.locator("#tpls tbody tr", has_text="Uplink neu")
        expect(trow).to_be_visible()
        expect(page.locator("#tpls tr.grp")).to_have_text("Cisco")
        trow.get_by_role("button", name="Edit").click()
        expect(page.locator("#tplText")).to_have_value("conf t\nint g0/1\n description uplink\n")
        page.fill("#tplFolder", "Cisco Edge")
        page.click("#tplForm button[type=submit]")
        expect(page.locator("#tpls tr.grp")).to_have_text("Cisco Edge")
        trow.get_by_role("button", name="Send").click()
        expect(page.locator("#pickDev")).to_contain_text("Nord · Werk 2")
        page.click("#pickOk")
        page.set_input_files("#fwFile", {"name": "good.rsfw", "mimeType": "application/octet-stream", "buffer": pkg})
        frow = page.locator("#libFw tbody tr", has_text="9.9.9")
        expect(frow).to_be_visible()
        frow.get_by_role("button", name="Folder").click()
        page.fill("#pickText", "Released")
        page.click("#pickOk")
        expect(page.locator("#libFw tr.grp")).to_have_text("Released")
        shot(path="/out/ui-library.png", full_page=True)
        frow.get_by_role("button", name="Delete").click()
        yes("9.9.9")
        expect(page.locator("#noLibFw")).to_be_visible()
        # another image: kept, renamed, sorted, fetched again byte for byte, deleted
        blob = bytes(range(256)) * 2000
        page.set_input_files("#imgFile", {"name": "cat9k_lite.17.09.bin", "mimeType": "application/octet-stream", "buffer": blob})
        irow = page.locator("#imgs tbody tr", has_text="cat9k")
        expect(irow).to_contain_text("500 kB")
        expect(page.locator("#imgState")).to_contain_text("per file")
        irow.get_by_role("button", name="Rename").click()
        page.fill("#pickText", "cat9k-lite.bin")
        page.click("#pickOk")
        irow = page.locator("#imgs tbody tr", has_text="cat9k-lite.bin")
        irow.get_by_role("button", name="Folder").click()
        page.fill("#pickText", "Catalyst")
        page.click("#pickOk")
        expect(page.locator("#imgs tr.grp")).to_have_text("Catalyst")
        with page.expect_download() as dl:
            irow.get_by_role("button", name="Download").click()
        assert dl.value.suggested_filename == "cat9k-lite.bin", dl.value.suggested_filename
        assert open(dl.value.path(), "rb").read() == blob, "the image came back changed"
        shot(path="/out/ui-library.png", full_page=True)
        irow.get_by_role("button", name="Delete").click()
        yes("cat9k-lite.bin")
        expect(page.locator("#noImgs")).to_be_visible()
        pick(arg)
        expect(hist.locator("tr", has_text="Write configuration").first).to_contain_text("stored", timeout=20000)
        page.click("button[data-cmd=configs]")
        expect(hist.locator("tr", has_text="Configurations").first).to_contain_text("Uplink neu", timeout=20000)
        page.select_option("#tplPick", label="Cisco Edge / Uplink neu")
        page.click("#tplSendForm button")
        go("library")
        trow.get_by_role("button", name="Delete").click()
        yes("Uplink neu")
        expect(page.locator("#noTpls")).to_be_visible()
        pick(arg, "devices")
        expect(page.locator("#dLine")).to_contain_text("Nord · Werk 2")
        # remote console: ask, the (simulated) device agrees, type, read, BREAK, close
        page.select_option("#conMinutes", "5")
        page.click("#conForm button")
        expect(page.locator("#conState")).to_contain_text("Open on port 1", timeout=20000)
        term = page.locator("#term .xterm-rows")
        expect(term).to_contain_text("Switch>", timeout=20000)
        page.locator("#term .xterm-helper-textarea").focus()
        page.keyboard.type("show")
        page.keyboard.press("Enter")
        expect(term).to_contain_text("GigabitEthernet0/40 is up", timeout=20000)
        page.click("#conBreak")
        expect(term).to_contain_text("[BREAK]", timeout=20000)
        expect(page.locator("#conInfo")).to_contain_text("min left")
        expect(page.locator("#conForm")).to_be_hidden()
        # not full_page: with the terminal focused that leaves the page scrolled somewhere
        # else than the browser believes, and the next click misses its button
        page.locator("#console").scroll_into_view_if_needed()
        shot(path="/out/ui-console.png")
        page.click("#conClose")
        expect(term).to_contain_text("closed by the administrator", timeout=20000)
        expect(page.locator("#conForm")).to_be_visible()
        pick(arg)                                    # the record of it is the administrator's
        expect(page.locator("#conLog tbody tr").first).to_contain_text("closed by the administrator", timeout=20000)

    elif stage == "language":
        # English unless somebody chose otherwise - also when the browser asks for German (locale de-DE)
        expect(page.locator("#lang")).to_have_value("en")
        assert page.evaluate("document.documentElement.lang") == "en"
        pick(arg)
        expect(page.locator("#conLog tbody tr").first).to_contain_text("closed by the administrator")
        go("devices")
        for lang, title, state, nav, reason, good, ask in (
                ("de", "Geräte", "verbunden", "Protokoll", "vom Administrator beendet", "in Ordnung", "Gerät neu starten?"),
                ("pt", "Dispositivos", "conectado", "Registro", "encerrado pelo administrador", "em ordem", "Reiniciar o dispositivo?")):
            page.select_option("#lang", lang)            # the page loads again in that language
            expect(page.locator("#title")).to_have_text(title)
            expect(page.locator("#lang")).to_have_value(lang)
            expect(page.locator("#nav a[data-view=manage]")).to_have_text({"de": "Verwalten", "pt": "Gerenciar"}[lang])
            pick(arg)
            assert page.evaluate("document.documentElement.lang") == lang
            expect(page.locator("#dState")).to_have_text(state)
            expect(page.locator("#nav a[data-view=audit]")).to_have_text(nav)
            # what the server says in English arrives in the chosen language
            expect(page.locator("#conLog tbody tr").first).to_contain_text(reason)
            page.click("button[data-cmd=reboot]")
            expect(page.locator("#askTitle")).to_have_text(ask)
            page.click("#askNo")
            shot(path="/out/lang-%s-device.png" % lang, full_page=True)
            go("server")
            expect(page.locator("#stState")).to_have_text(good)
            shot(path="/out/lang-%s-server.png" % lang)
            go("devices")
        # the choice is kept: a new visit starts in it, the login page included
        page.reload()
        expect(page.locator("#title")).to_have_text("Dispositivos")
        page.click("#logout")
        expect(page.locator("#loginForm button[type=submit]")).to_have_text("Entrar")
        shot(path="/out/lang-pt-login.png")
        page.select_option("#lang", "en")
        expect(page.locator("#loginForm button[type=submit]")).to_have_text("Sign in")

    elif stage == "revoke":
        pick(arg)
        page.click("#revoke")
        expect(page.locator("#askNo")).to_be_focused()   # Enter is not the dangerous answer
        page.keyboard.press("Escape")
        expect(page.locator("#askDlg")).to_be_hidden()
        expect(page.locator("#dState")).to_have_text("connected")
        page.click("#revoke")
        yes("at once and for good")
        expect(page.locator("#dState")).to_have_text("revoked")
        expect(page.locator("#commands")).to_be_hidden()

    elif stage == "phone":
        def fits(what):                              # nothing may be wider than the screen
            w = page.evaluate("[document.documentElement.scrollWidth, window.innerWidth]")
            assert w[0] <= w[1], "%s is %d wide on a screen of %d" % (what, w[0], w[1])
        def targets(what):                           # what a thumb has to hit is big enough for one
            small = page.evaluate("""[...document.querySelectorAll('.btn, nav a, .dev, .back, select, input:not([type=checkbox]), label.check')]
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
        expect(page.locator("#conForm")).to_be_visible()
        fits("a device"); targets("a device")
        shot(path="/out/phone-device.png", full_page=True)
        page.click("#back")
        expect(page.locator("#listCard")).to_be_visible()
        expect(page.locator("#detail")).to_be_hidden()
        page.go_back()                               # the browser's own way back leads to the device again
        expect(page.locator("#detail")).to_be_visible()
        page.click("#back")
        # the same device, looked after: with its settings
        pick(arg)
        expect(page.locator("#back")).to_have_text("Manage")
        expect(page.locator("#history")).to_be_visible()
        page.click("#setLoad")
        expect(page.locator("#setForm")).to_be_visible(timeout=20000)
        fits("managing a device"); targets("managing a device")
        shot(path="/out/phone-manage.png", full_page=True)
        page.click("#back")
        expect(page.locator("#listCard")).to_be_visible()
        for view in ("library", "users", "audit", "server"):
            go(view)
            expect(page.locator("#title")).to_have_text({"library": "Library", "users": "Users", "audit": "Audit log", "server": "Server"}[view])
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
        go("manage")
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        page.wait_for_timeout(500)
        fits("the invitation")
        shot(path="/out/phone-invite.png")
        page.click("#closeInvite")
        # and in the dark
        page.emulate_media(color_scheme="dark")
        shot(path="/out/phone-devices-dark.png")
        go("devices")
        page.locator("#devices .dev", has_text=arg).click()
        shot(path="/out/phone-device-dark.png")
        page.emulate_media(color_scheme="light")

    elif stage == "accounts":
        SLOW = 30000    # a password costs the server 600 000 rounds of hashing: seconds on a busy Raspberry Pi
        # the administrator creates the account and gets a setup code, shown once
        go("users")
        expect(page.locator("#noUsers")).to_be_visible()
        page.click("#addUser")
        page.fill("#userName", "Anna")
        page.click("#userForm button[type=submit]")
        expect(page.locator("#codeDlg")).to_be_visible()
        setup = page.locator("#codeText").inner_text()
        assert len(setup) == 19 and setup.count("-") == 3, setup
        page.click("#codeClose")
        row = page.locator("#users tbody tr", has_text="anna")
        expect(row).to_contain_text("waiting to be set up")
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
        expect(page.locator("#setupErr")).to_contain_text("setup code is wrong", timeout=SLOW)
        page.fill("#setupCode", setup.lower())           # typed as somebody would
        page.fill("#setupPass2", "ein anderes Passwort")
        page.click("#setupForm button")
        expect(page.locator("#setupErr")).to_contain_text("not the same")
        page.fill("#setupPass2", "ein langes Passwort 1")
        page.click("#setupForm button")
        expect(page.locator("#totpForm")).to_be_visible(timeout=SLOW)
        secret = page.locator("#totpSecret").inner_text()
        assert page.evaluate("document.querySelector('#totpQr').width") > 100
        page.fill("#totpCode", "000000" if totp(secret) != "000000" else "111111")
        page.click("#totpForm button")
        expect(page.locator("#setupErr")).to_contain_text("code does not match", timeout=SLOW)
        page.fill("#totpCode", totp(secret))
        page.click("#totpForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        expect(page.locator("#whoami")).to_have_text("anna")
        errors.clear()                               # the refusals above were the point
        expect(page.locator("#title")).to_have_text("Users")   # back where she was
        expect(page.locator("#users tbody tr", has_text="anna")).to_contain_text("active")
        shot(path="/out/ui-accounts.png", full_page=True)
        go("audit")
        audit = page.locator("#audit tbody")
        expect(audit).to_contain_text("Account set up")
        expect(audit).to_contain_text("User added")
        expect(audit.locator("tr.failed").first).to_contain_text("refused")
        shot(path="/out/ui-audit.png", full_page=True)

        # the session is a cookie: it survives a reload, and changing things works with it
        page.reload()
        expect(page.locator("#app")).to_be_visible()
        expect(page.locator("#whoami")).to_have_text("anna")
        expect(page.locator("#title")).to_have_text("Audit log")   # a reload stays where one was
        go("devices")
        go("manage")
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
        expect(page.locator("#loginErr")).to_have_text("sign-in failed", timeout=SLOW)
        errors.clear()
        page.fill("#loginPass", "ein langes Passwort 1")
        page.fill("#loginCode", totp(secret, 1))
        page.click("#loginForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        go("audit")
        expect(page.locator("#audit tbody tr").first).to_contain_text("Sign-in")
        # she cannot lock herself out
        go("users")
        page.locator("#users tbody tr", has_text="anna").get_by_text("Delete").click()
        yes("Delete the account")
        expect(page.locator("#toast")).to_contain_text("own account")
        errors.clear()
        page.click("#logout")

    assert not errors, errors
    browser.close()
print("ok " + stage)
