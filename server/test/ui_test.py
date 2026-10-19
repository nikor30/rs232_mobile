"""Drives the administrator's web interface in a real browser (Playwright).

Run by ui.sh in stages, because a simulated device has to act in between:
  invite    log in, create an invitation, print the token
  approve   type a wrong and then the right code for the pending device
  server    the server's state; change the server key and end the change
  command   one device: what it says of itself, what is on it, commands, settings, backup,
            firmware, the library, groups, the remote console
  accounts  create an account, set it up as its owner, sign in with password and code;
            a member of a group sees its devices and nothing else
  language  German and Portuguese: the page, what the script builds, what the server answers
  phone     the same pages on a phone: nothing wider than the screen, one thing at a time
  revoke    lock the device out
"""
import base64, hashlib, hmac, os, struct, sys, time
from playwright.sync_api import sync_playwright, expect

stage, base, admin = sys.argv[1], sys.argv[2], sys.argv[3]
arg = sys.argv[4] if len(sys.argv) > 4 else ""

with sync_playwright() as p:
    browser = getattr(p, os.environ.get("UI_BROWSER", "chromium")).launch()   # webkit: the engine of Safari
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
        snap(page, **kw)

    def snap(target, **kw):
        # In WebKit a screenshot puts a <style> of Playwright's own into the page, which the
        # page's policy refuses - that refusal is the test's, not the page's
        n = len(errors)
        target.screenshot(**kw)
        if browser.browser_type.name == "webkit":
            page.wait_for_timeout(100)
            errors[n:] = [e for e in errors[n:] if not e.startswith("Refused to apply a stylesheet")]

    def go(view, tab=None):                          # the places in the navigation, and the tabs within one
        page.click("#nav a[data-view=%s]" % view)
        expect(page.locator("#v-" + view)).to_be_visible()
        expect(page.locator("#nav a[aria-current]")).to_have_attribute("data-view", view)
        if tab:
            page.click("[data-tabs=%s] a[data-tab=%s]" % (view, tab))
            expect(page.locator("#%s-%s" % (view[0], tab))).to_be_visible()

    def tab(name):                                   # a tab of the device that is open
        page.click("#dTabs a[data-tab=%s]" % name)
        expect(page.locator("#p-" + name)).to_be_visible()

    def pick(name, to="overview"):                   # a device from the cards
        go("fleet")
        page.locator("#cards .dev", has_text=name).click()
        expect(page.locator("#v-device")).to_be_visible()
        if to != "overview":
            tab(to)

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
    expect(page.locator("#side .brand")).to_be_hidden()
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
    expect(page.locator("#side .brand img")).to_be_visible()
    if stage != "phone":
        # a wide screen: mark, name and the places stand at the side, one under the other
        expect(page.locator("#side .brand")).to_contain_text("RS232 Control Center")
        box = [page.locator("#nav a").nth(i).bounding_box() for i in (0, 1)]
        assert box[1]["y"] > box[0]["y"] and abs(box[1]["x"] - box[0]["x"]) < 1, box
        assert page.locator("#main").bounding_box()["x"] >= page.locator("#side").bounding_box()["width"], "the content lies under the side bar"

    if stage == "invite":
        expect(page.locator("#noDevices")).to_be_visible()
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        token = page.input_value("#tokenText")
        assert token.startswith("c2e1:"), token
        expect(page.locator("#tokenExp")).to_contain_text("valid for another")
        # the QR code must be a real picture: read it back and compare
        size = page.evaluate("document.querySelector('#qr').width")
        assert size > 100, size
        snap(page.locator("#qr"), path="/out/qr.png")
        shot(path="/out/ui-invite.png")
        # registration without an invitation: closed, opened for a time, closed again
        expect(page.locator("#openNote")).to_be_hidden()
        assert page.locator("#openFor option").count() == 12, "15 minutes to 3 hours in steps of 15"
        expect(page.locator("#openUrl")).to_contain_text("http")
        page.select_option("#openFor", "45")
        page.click("#openSet")
        expect(page.locator("#openNote")).to_be_visible()
        expect(page.locator("#openText")).to_contain_text("45 min")
        shot(path="/out/ui-open.png")
        page.click("#openEnd")
        expect(page.locator("#openNote")).to_be_hidden()
        # a profile for new devices, named in the invitation ("Open registration" has closed the dialog)
        go("library", "profiles")
        expect(page.locator("#noProfs")).to_be_visible()
        page.click("#profNew")
        page.fill("#profName", "Kunde A")
        page.fill("#profSite", "Lager 3")
        page.check("#profCtl")
        page.select_option("#profUn", "24")
        page.fill("#profSsid", "corp-dot1x")
        page.select_option("#profAuth", "2")
        expect(page.locator("#profUser")).to_be_visible()
        page.fill("#profUser", "rs232")
        page.fill("#profPass", "ein Passwort")
        shot(path="/out/ui-profile.png")
        page.click("#profForm button[type=submit]")
        row = page.locator("#profs tbody tr", has_text="Kunde A")
        expect(row).to_contain_text("corp-dot1x (PEAP)")
        expect(row).to_contain_text("unattended 24 h")
        go("fleet")
        page.click("#invite")
        expect(page.locator("#tokenText")).not_to_have_value("")
        before = page.input_value("#tokenText")
        page.select_option("#inviteProf", label="Kunde A")
        expect(page.locator("#tokenText")).not_to_have_value(before)
        import base64, json as _json
        t = page.input_value("#tokenText")[5:]
        pf = _json.loads(base64.urlsafe_b64decode(t + "=" * (-len(t) % 4)))["pf"]
        assert pf == {"n": "Kunde A", "ctl": True, "un": 24, "w": "corp-dot1x", "a": 2}, pf
        page.select_option("#inviteProf", "0")             # the device of the later steps enrolls without one
        page.wait_for_function("() => { const t = document.querySelector('#tokenText').value.slice(5).replace(/-/g, '+').replace(/_/g, '/');"
                               " try { return !JSON.parse(atob(t + '='.repeat((4 - t.length % 4) % 4))).pf; } catch (e) { return false; } }")
        token = page.input_value("#tokenText")
        print("TOKEN " + token)

    elif stage == "approve":
        card = page.locator("#cards .dev", has_text=arg.split(":")[0])
        expect(card).to_contain_text("waiting")
        expect(card).to_contain_text("Type in the code")
        expect(page.locator("#nPending")).to_have_text("1")
        card.click()
        assert page.url.endswith("#fleet/" + card.get_attribute("data-id")), page.url
        expect(page.locator("#approveForm")).to_be_visible()
        expect(page.locator("#dTabs")).to_be_hidden()    # nothing to do with it before it is confirmed
        code = arg.split(":")[1]
        assert code not in page.content(), "the page must not know the code"
        wrong = "%06d" % ((int(code) + 1) % 1000000)
        page.fill("#sas", wrong)
        page.click("#approveForm button")
        expect(page.locator("#toast")).to_contain_text("does not match")
        errors.clear()                               # the browser logs the 403, as intended
        page.fill("#sas", code)
        page.click("#approveForm button")
        expect(page.locator("#dState")).to_have_text("offline")   # confirmed, and not heard from yet
        expect(page.locator("#dTabs")).to_be_visible()
        expect(page.locator("#approveForm")).to_be_hidden()
        shot(path="/out/ui-approved.png")

    elif stage == "server":
        go("server")
        expect(page.locator("#stState")).to_have_text("all right")
        expect(page.locator("#stActive")).to_have_text("1")
        expect(page.locator("#alert")).to_be_hidden()
        expect(page.locator("#s-audit")).to_be_hidden()   # the record is there for who asks for it
        expect(page.locator("#navMark")).to_be_hidden()
        shot(path="/out/ui-status.png")
        go("server", "key")
        expect(page.locator("#keyChange")).to_be_hidden()
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
        go("server", "audit")
        expect(page.locator("#audit tbody")).to_contain_text("Server key changed")
        expect(page.locator("#audit tbody")).to_contain_text("Key change ended")
        go("fleet")
        row = page.locator("#cards .dev", has_text="Labor")
        expect(row).to_contain_text("online")
        expect(row.locator(".dot.on")).to_be_visible()   # connected right now, and it shows
        expect(row).not_to_contain_text("old server key")

    elif stage == "command":
        # the cards say what a device is doing, the overview says it in full
        card = page.locator("#cards .dev", has_text=arg)
        expect(card).to_contain_text("2 configurations")
        expect(card).to_contain_text("unattended")
        expect(card).to_contain_text("ports free")
        shot(path="/out/ui-fleet.png")
        pick(arg)
        expect(page.locator("#oConn")).to_have_text("online")
        expect(page.locator("#oUnatt")).to_have_text("on")
        expect(page.locator("#oCtl")).to_have_text("allowed")
        expect(page.locator("#oStore")).to_have_text("2 configurations")
        expect(page.locator("#oFw")).to_have_text("1.8.0")
        for hidden in ("#history", "#dId", "#setLoad", "#revoke", "#conForm"):
            expect(page.locator(hidden)).to_be_hidden()  # logs and details are a tab away
        shot(path="/out/ui-device.png")
        page.click("#p-overview button[data-cmd=ping]")
        tab("activity")
        hist = page.locator("#history tbody")
        expect(hist).to_contain_text("pong", timeout=20000)
        expect(page.locator("#dId")).to_be_visible()
        page.click("#p-activity button[data-cmd=configs]")
        expect(hist).to_contain_text("Uplink", timeout=20000)
        # what is on it: the stored configurations, to look at, play and delete
        tab("content")
        expect(page.locator("#cfgs tbody tr")).to_have_count(2)
        demo = page.locator("#cfgs tbody tr", has_text="Demo")
        demo.get_by_role("button", name="Play").click()
        page.select_option("#pickDev", "2")
        page.click("#pickOk")
        yes("on port 2")
        tab("activity")
        expect(hist).to_contain_text("\"result\": \"done\"", timeout=20000)
        expect(hist.locator("tr", has_text="Play").first).to_contain_text('"port":2')
        tab("content")
        page.locator("#cfgs tbody tr", has_text="Uplink").get_by_role("button", name="View").click()
        expect(page.locator("#cfgDlg")).to_be_visible(timeout=20000)
        expect(page.locator("#cfgText")).to_contain_text("GigabitEthernet0/1")
        page.click("#cfgKeep")
        expect(page.locator("#toast")).to_contain_text("Kept as a template")
        expect(page.locator("#cfgs tbody tr", has_text="Uplink")).to_contain_text("as in the library", timeout=20000)
        shot(path="/out/ui-content.png", full_page=True)
        tab("settings")
        page.click("#p-settings button[data-cmd=reboot]")
        yes("Restart the device")
        tab("activity")
        expect(hist.locator("tr", has_text="Restart").first).to_contain_text("done", timeout=20000)
        # device data is shown as text, never run: the simulated device is called <img src=x onerror=...>
        assert page.evaluate("window.pwned === undefined"), "markup from a device name was executed"
        assert page.locator("#v-device img, #cards img, #tree img").count() == 0
        shot(path="/out/ui-commands.png", full_page=True)
        tab("settings")
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
        tab("activity")
        row = hist.locator("tr", has_text="Change settings").first
        expect(row).to_contain_text("done")
        assert "geheim" not in page.content(), "a password stays in the history"
        tab("settings")
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
        page.click("#setLoad")
        expect(host).to_have_value("lab-8", timeout=20000)
        shot(path="/out/ui-backup.png", full_page=True)
        tab("content")
        expect(page.locator("#cfgs tbody tr")).to_have_count(3, timeout=20000)   # the device says so by itself
        # firmware: a signed package goes onto the shelf, the (simulated) device fetches and "installs" it.
        # The simulator does not check signatures - that is the firmware's part (tests/c2/run.sh).
        image = bytes((i * 7) & 255 for i in range(30000))
        manifest = json.dumps({"product": "rs232-console", "board": "waveshare-s3-lcd2", "fw": "9.9.9", "lang": "pt", "seq": 1791900000,
                               "size": len(image), "sha256": hashlib.sha256(image).hexdigest()}, separators=(",", ":")).encode()
        pkg = b"RSFW1\n" + manifest + b"\n" + base64.b64encode(b"\x5a" * 3309) + b"\n" + image
        expect(page.locator("#fwNone")).to_be_visible()
        go("library", "firmware")
        page.set_input_files("#fwFile", {"name": "x.rsfw", "mimeType": "application/octet-stream", "buffer": b"hello"})
        expect(page.locator("#libFwState")).to_contain_text("not a firmware package")
        page.set_input_files("#fwFile", {"name": "damaged.rsfw", "mimeType": "application/octet-stream", "buffer": pkg[:-1] + b"\x00"})
        expect(page.locator("#libFwState")).to_contain_text("damaged file")
        assert len(errors) == 2, errors                  # the browser logs the two refusals, as intended
        errors.clear()
        page.set_input_files("#fwFile", {"name": "good.rsfw", "mimeType": "application/octet-stream", "buffer": pkg})
        expect(page.locator("#libFwState")).to_contain_text("Uploaded: version 9.9.9")
        pick(arg, "content")
        fwrow = page.locator("#fwList tbody tr")
        expect(fwrow).to_have_count(1)
        expect(fwrow).to_contain_text("Português")
        shot(path="/out/ui-firmware.png", full_page=True)
        fwrow.get_by_role("button", name="Install").click()
        yes("9.9.9")
        expect(page.locator("#fwState")).to_contain_text("Installed: version 9.9.9", timeout=60000)
        tab("activity")
        expect(hist.locator("tr", has_text="Firmware update").first).to_contain_text("updated to 9.9.9 (pt), restarting", timeout=20000)
        go("library", "firmware")
        frow = page.locator("#libFw tbody tr", has_text="9.9.9")
        frow.get_by_role("button", name="Delete").click()
        yes("9.9.9")
        expect(page.locator("#noLibFw")).to_be_visible(timeout=20000)
        # where the device belongs: region, country, site - the side shows the tree and narrows the cards
        pick(arg, "settings")
        page.fill("#dRegion", "Nord")
        page.fill("#dCountry", "Deutschland")
        page.fill("#dSite", "Werk 2")
        page.click("#placeSave")
        expect(page.locator("#dPlace")).to_have_text("Nord › Deutschland › Werk 2")
        go("fleet")
        expect(page.locator("#tree a")).to_have_count(4)     # all, region, country, site
        page.locator("#tree a", has_text="Deutschland").click()
        expect(page.locator("#crumb")).to_have_text("Nord › Deutschland")
        expect(page.locator("#cards .dev")).to_have_count(1)
        expect(page.locator("#tree a", has_text="Werk 2")).to_contain_text("1/1")
        page.fill("#find", "no such device")
        expect(page.locator("#noMatch")).to_be_visible()
        page.fill("#find", "werk")
        expect(page.locator("#cards .dev")).to_have_count(1)
        page.fill("#find", "")
        page.locator("#tree a", has_text="All devices").click()
        # groups: which devices belong together (who may work with them: stage "accounts")
        go("access")
        expect(page.locator("#noGroups")).to_be_visible()
        page.click("#grpNew")
        page.fill("#grpName", "Werk Nord")
        page.fill("#grpNote", "Fertigung")
        page.locator("#grpDevices input").check()
        shot(path="/out/ui-group.png")
        page.click("#grpForm button[type=submit]")
        gcard = page.locator("#groups .grpcard", has_text="Werk Nord")
        expect(gcard).to_contain_text("1 device")
        expect(gcard).to_contain_text("Nobody yet")
        go("fleet")
        expect(page.locator("#cards .dev .chip", has_text="Werk Nord")).to_be_visible()
        page.locator("#grpNav a", has_text="Werk Nord").click()
        expect(page.locator("#crumb")).to_have_text("Group Werk Nord")
        expect(page.locator("#cards .dev")).to_have_count(1)
        page.locator("#tree a", has_text="All devices").click()
        # the library: a template is written, sorted into a folder, sent to the device and deleted
        go("library")
        expect(page.locator("#tpls tbody tr", has_text="Uplink")).to_be_visible()   # the one kept from the device
        page.click("#tplNew")
        page.fill("#tplName", "Uplink neu")
        page.fill("#tplFolder", "Cisco")
        page.fill("#tplText", "conf t\nint g0/1\n description uplink\n")
        page.click("#tplForm button[type=submit]")
        trow = page.locator("#tpls tbody tr", has_text="Uplink neu")
        expect(trow).to_be_visible()
        expect(page.locator("#tpls tr.grp", has_text="Cisco")).to_be_visible()
        trow.get_by_role("button", name="Edit").click()
        expect(page.locator("#tplText")).to_have_value("conf t\nint g0/1\n description uplink\n")
        page.fill("#tplFolder", "Cisco Edge")
        page.click("#tplForm button[type=submit]")
        expect(page.locator("#tpls tr.grp", has_text="Cisco Edge")).to_be_visible()
        page.locator("#tpls tr.grp", has_text="Cisco Edge").get_by_role("button", name="Rename").click()
        page.fill("#pickText", "Cisco Rand")
        page.click("#pickOk")
        expect(page.locator("#tpls tr.grp", has_text="Cisco Rand")).to_be_visible()
        page.locator("#tpls tr.grp", has_text="Cisco Rand").get_by_role("button", name="Rename").click()
        page.fill("#pickText", "Cisco Edge")
        page.click("#pickOk")
        expect(page.locator("#tpls tr.grp", has_text="Cisco Edge")).to_be_visible()
        trow.get_by_role("button", name="Send").click()
        expect(page.locator("#pickDev")).to_contain_text("Nord › Deutschland › Werk 2")
        page.click("#pickOk")
        go("library", "firmware")
        page.set_input_files("#fwFile", {"name": "good.rsfw", "mimeType": "application/octet-stream", "buffer": pkg})
        frow = page.locator("#libFw tbody tr", has_text="9.9.9")
        expect(frow).to_be_visible()
        frow.get_by_role("button", name="Folder").click()
        page.fill("#pickText", "Released")
        page.click("#pickOk")
        expect(page.locator("#libFw tr.grp")).to_contain_text("Released")
        shot(path="/out/ui-library.png", full_page=True)
        frow.get_by_role("button", name="Delete").click()
        yes("9.9.9")
        expect(page.locator("#noLibFw")).to_be_visible()
        go("library", "images")
        # another image: kept, renamed, sorted, fetched again byte for byte, deleted
        blob = bytes(range(256)) * 2000
        # with a wrong MD5 sum the server does not keep it; with the right one it does
        page.set_input_files("#imgFile", {"name": "cat9k_lite.17.09.bin", "mimeType": "application/octet-stream", "buffer": blob})
        page.fill("#pickText", "0" * 32)
        page.click("#pickOk")
        expect(page.locator("#imgState")).to_contain_text("does not have the MD5 sum")
        errors.clear()
        expect(page.locator("#noImgs")).to_be_visible()
        page.set_input_files("#imgFile", {"name": "cat9k_lite.17.09.bin", "mimeType": "application/octet-stream", "buffer": blob})
        page.fill("#pickText", hashlib.md5(blob).hexdigest().upper())
        page.click("#pickOk")
        expect(page.locator("#imgs tbody tr", has_text="cat9k")).to_contain_text(hashlib.md5(blob).hexdigest())
        expect(page.locator("#imgs tbody tr", has_text="cat9k").locator(".chip.ok")).to_contain_text("MD5 ok")
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
        expect(page.locator("#imgs tr.grp")).to_contain_text("Catalyst")
        # a folder can be renamed and removed; what is in it stays
        page.locator("#imgs tr.grp").get_by_role("button", name="Rename").click()
        page.fill("#pickText", "Cisco Catalyst")
        page.click("#pickOk")
        expect(page.locator("#imgs tr.grp")).to_contain_text("Cisco Catalyst")
        page.locator("#imgs tr.grp").get_by_role("button", name="Remove folder").click()
        yes("stays, without a folder")
        expect(page.locator("#imgs tr.grp")).to_have_count(0)
        expect(irow).to_be_visible()
        # from the library to the device's SD card
        pick(arg, "content")
        expect(page.locator("#sdFiles")).to_be_hidden()
        page.click("#sdForm button")
        yes("cat9k-lite.bin")
        expect(page.locator("#sdState")).to_contain_text("On the SD card: cat9k-lite.bin", timeout=60000)
        expect(page.locator("#sdFiles tbody tr")).to_contain_text("cat9k-lite.bin", timeout=20000)
        expect(page.locator("#sdFiles tbody tr .chip.ok")).to_contain_text("MD5 ok", timeout=20000)   # the device has checked it on its card
        shot(path="/out/ui-sdcard.png", full_page=True)
        page.locator("#sdFiles tbody tr", has_text="cat9k-lite.bin").get_by_role("button", name="Delete").click()
        yes("from the SD card")
        expect(page.locator("#sdFiles")).to_be_hidden(timeout=20000)
        go("library", "images")
        with page.expect_download() as dl:
            irow.get_by_role("button", name="Download").click()
        assert dl.value.suggested_filename == "cat9k-lite.bin", dl.value.suggested_filename
        assert open(dl.value.path(), "rb").read() == blob, "the image came back changed"
        shot(path="/out/ui-library.png", full_page=True)
        irow.get_by_role("button", name="Delete").click()
        yes("cat9k-lite.bin")
        expect(page.locator("#noImgs")).to_be_visible()
        pick(arg, "content")
        expect(page.locator("#cfgs tbody tr", has_text="Uplink neu")).to_contain_text("as in the library", timeout=20000)
        page.select_option("#tplPick", label="Cisco Edge / Uplink neu")
        page.click("#tplSendForm button")
        go("library")
        expect(trow).to_contain_text("Labor")            # the library knows where a template is
        trow.get_by_role("button", name="Delete").click()
        yes("Uplink neu")
        expect(trow).to_be_hidden()
        pick(arg, "console")
        expect(page.locator("#conHint")).to_contain_text("Unattended access is on")
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
        # recording: the device records on its card, the browser sends the text to the server
        page.check("#conRecOn")
        expect(page.locator("#conRecState")).to_contain_text("device: /logs/port1-001.log", timeout=20000)
        page.locator("#term .xterm-helper-textarea").focus()
        page.keyboard.type("show")
        page.keyboard.press("Enter")
        expect(page.locator("#conRecState")).not_to_contain_text("server: 0 kB", timeout=20000)
        if os.environ.get("UI_BROWSER", "chromium") == "chromium":   # WebKit gives a page no clipboard permission
            # as in PuTTY: selecting copies, the right button pastes
            ctx.grant_permissions(["clipboard-read", "clipboard-write"])
            box = page.locator("#term .xterm-screen").bounding_box()
            page.mouse.move(box["x"] + 4, box["y"] + 4)
            page.mouse.down()
            page.mouse.move(box["x"] + box["width"] - 4, box["y"] + 60, steps=5)
            page.mouse.up()
            page.wait_for_function("navigator.clipboard.readText().then((x) => x.includes('Switch>'))", timeout=5000)
            page.evaluate("navigator.clipboard.writeText('pasted-here')")
            page.locator("#term").click(button="right")
            expect(term).to_contain_text("pasted-here", timeout=20000)
        # a script under the terminal: asked twice, then played on the console's port
        scripts = page.locator("#conScripts")
        expect(scripts).to_be_visible()
        expect(scripts).to_contain_text("Played on port 1")
        row = page.locator("#conScriptList .script", has_text="Uplink").first
        row.get_by_role("button", name="Play").click()
        expect(page.locator("#askDlg")).to_contain_text("on port 1")
        page.click("#askYes")                       # the same dialog asks again at once
        yes("Really play")
        expect(page.locator("#conScriptState")).to_contain_text("Waiting for the device to fetch", timeout=20000)
        # the terminal's rows end inside its box: the last one is not cut off
        fit = page.evaluate("""() => { const b = document.querySelector('#term').getBoundingClientRect(), s = document.querySelector('#term .xterm-screen').getBoundingClientRect(); return [s.bottom, b.bottom]; }""")
        assert fit[0] <= fit[1] + 0.5, f"last terminal row cut off: screen ends at {fit[0]}, box at {fit[1]}"
        # not full_page: with the terminal focused that leaves the page scrolled somewhere
        # else than the browser believes, and the next click misses its button
        page.locator("#p-console").scroll_into_view_if_needed()
        shot(path="/out/ui-console.png")
        page.click("#conClose")
        expect(term).to_contain_text("closed by the administrator", timeout=20000)
        expect(page.locator("#conForm")).to_be_visible()
        # the simulator fetches commands only between consoles (the firmware also during one)
        expect(page.locator("#conScriptState")).to_contain_text("✓ Uplink", timeout=30000)
        rec = page.locator("#recList tbody tr").first
        expect(rec).to_be_visible(timeout=20000)
        with page.expect_download() as dl:
            rec.get_by_role("button", name="Download").click()
        text = open(dl.value.path(), "rb").read()
        assert b"GigabitEthernet0/40 is up" in text, f"recording on the server: {text[-200:]!r}"
        rec.get_by_role("button", name="Delete").click()
        yes("Delete the recording")
        expect(page.locator("#recCard")).to_be_hidden(timeout=20000)
        tab("activity")
        expect(page.locator("#conLog tbody tr").first).to_contain_text("closed by the administrator", timeout=20000)

    elif stage == "language":
        # English unless somebody chose otherwise - also when the browser asks for German (locale de-DE)
        expect(page.locator("#lang")).to_have_value("en")
        assert page.evaluate("document.documentElement.lang") == "en"
        pick(arg, "activity")
        expect(page.locator("#conLog tbody tr").first).to_contain_text("closed by the administrator")
        go("fleet")
        for lang, title, state, nav, reason, good, ask in (
                ("de", "Geräte", "online", "Zugang", "vom Administrator beendet", "in Ordnung", "Gerät neu starten?"),
                ("pt", "Dispositivos", "online", "Acesso", "encerrado pelo administrador", "em ordem", "Reiniciar o dispositivo?")):
            page.select_option("#lang", lang)            # the page loads again in that language
            expect(page.locator("#title")).to_have_text(title)
            expect(page.locator("#lang")).to_have_value(lang)
            expect(page.locator("#nav a[data-view=library]")).to_have_text({"de": "Bibliothek", "pt": "Biblioteca"}[lang])
            expect(page.locator("#tree a").first).to_contain_text({"de": "Alle Geräte", "pt": "Todos os dispositivos"}[lang])
            pick(arg, "activity")
            assert page.evaluate("document.documentElement.lang") == lang
            expect(page.locator("#dState")).to_have_text(state)
            expect(page.locator("#nav a[data-view=access]")).to_have_text(nav)
            expect(page.locator("#dTabs a[data-tab=content]")).to_have_text({"de": "Auf dem Gerät", "pt": "No dispositivo"}[lang])
            # what the server says in English arrives in the chosen language
            expect(page.locator("#conLog tbody tr").first).to_contain_text(reason)
            tab("settings")
            page.click("#p-settings button[data-cmd=reboot]")
            expect(page.locator("#askTitle")).to_have_text(ask)
            page.click("#askNo")
            tab("overview")
            shot(path="/out/lang-%s-device.png" % lang, full_page=True)
            go("server")
            expect(page.locator("#stState")).to_have_text(good)
            shot(path="/out/lang-%s-server.png" % lang)
            go("fleet")
        # the choice is kept: a new visit starts in it, the login page included
        page.reload()
        expect(page.locator("#title")).to_have_text("Dispositivos")
        page.click("#logout")
        expect(page.locator("#loginForm button[type=submit]")).to_have_text("Entrar")
        shot(path="/out/lang-pt-login.png")
        page.select_option("#lang", "en")
        expect(page.locator("#loginForm button[type=submit]")).to_have_text("Sign in")

    elif stage == "revoke":
        pick(arg, "settings")
        page.click("#revoke")
        expect(page.locator("#askNo")).to_be_focused()   # Enter is not the dangerous answer
        page.keyboard.press("Escape")
        expect(page.locator("#askDlg")).to_be_hidden()
        expect(page.locator("#dState")).to_have_text("online")
        page.click("#revoke")
        yes("at once and for good")
        expect(page.locator("#dState")).to_have_text("revoked")
        expect(page.locator("#dTabs")).to_be_hidden()
        expect(page.locator("#p-activity")).to_be_visible()

    elif stage == "phone":
        def fits(what):                              # nothing may be wider than the screen
            w = page.evaluate("[document.documentElement.scrollWidth, window.innerWidth]")
            assert w[0] <= w[1], "%s is %d wide on a screen of %d" % (what, w[0], w[1])
        def targets(what):                           # what a thumb has to hit is big enough for one
            small = page.evaluate("""[...document.querySelectorAll('.btn, nav a, a.dev, .back, .tree a, .seg button, select, input:not([type=checkbox]), label.check')]
                .filter(e => e.offsetParent && e.getBoundingClientRect().height < 43.5)
                .map(e => (e.id || e.textContent.trim().slice(0, 20)) + ' ' + Math.round(e.getBoundingClientRect().height))""")
            assert not small, "%s: too small to tap: %s" % (what, small)
        nav = page.locator("#nav").bounding_box()
        assert nav["y"] + nav["height"] == PHONE["height"] and nav["width"] == PHONE["width"], nav   # the places are under the thumb
        expect(page.locator("#tree")).to_be_hidden()     # the tree of locations waits behind one line
        fits("the cards"); targets("the cards")
        shot(path="/out/phone-devices.png")
        page.click("#treeBox summary")
        expect(page.locator("#tree a", has_text="Werk 2")).to_be_visible()
        fits("the tree"); targets("the tree")
        page.click("#treeBox summary")
        # one device: it takes the screen, and there is a way back
        page.locator("#cards .dev", has_text=arg).click()
        expect(page.locator("#v-device")).to_be_visible()
        expect(page.locator("#v-fleet")).to_be_hidden()
        expect(page.locator("#back")).to_be_visible()
        fits("a device"); targets("a device")
        shot(path="/out/phone-device.png", full_page=True)
        page.click("#back")
        expect(page.locator("#v-fleet")).to_be_visible()
        page.go_back()                               # the browser's own way back leads to the device again
        expect(page.locator("#v-device")).to_be_visible()
        for name in ("console", "content", "activity", "settings"):
            tab(name)
            fits("the tab " + name); targets("the tab " + name)
            shot(path="/out/phone-device-%s.png" % name, full_page=True)
        page.click("#setLoad")
        expect(page.locator("#setForm")).to_be_visible(timeout=20000)
        fits("the settings"); targets("the settings")
        shot(path="/out/phone-settings.png", full_page=True)
        page.click("#back")
        expect(page.locator("#v-fleet")).to_be_visible()
        for view, tabs in (("library", ("templates", "firmware", "images")), ("access", ("groups", "users")), ("server", ("status", "audit", "key"))):
            for name in tabs:
                go(view, name)
                fits(view + " " + name); targets(view + " " + name)
                shot(path="/out/phone-%s-%s.png" % (view, name), full_page=True)
        go("access", "groups")
        page.click("#grpNew")
        expect(page.locator("#grpDlg")).to_be_visible()
        page.wait_for_timeout(500)
        fits("the group"); targets("the group")
        shot(path="/out/phone-group.png")
        page.click("#grpCancel")
        go("server", "key")
        # a dialog comes up from the bottom edge, as wide as the screen
        page.click("#kcStart")
        expect(page.locator("#rotateDlg")).to_be_visible()
        page.wait_for_timeout(500)                   # until it has arrived
        box = page.locator("#rotateDlg").bounding_box()
        assert box["width"] == PHONE["width"] and abs(box["y"] + box["height"] - PHONE["height"]) < 1, box
        targets("the dialog")
        shot(path="/out/phone-dialog.png")
        page.click("#rotateCancel")
        go("fleet")
        page.click("#invite")
        expect(page.locator("#inviteDlg")).to_be_visible()
        page.wait_for_timeout(500)
        fits("the invitation")
        shot(path="/out/phone-invite.png")
        page.click("#closeInvite")
        # and in the dark
        page.emulate_media(color_scheme="dark")
        shot(path="/out/phone-devices-dark.png")
        page.locator("#cards .dev", has_text=arg).click()
        shot(path="/out/phone-device-dark.png")
        page.emulate_media(color_scheme="light")

    elif stage == "accounts":
        SLOW = 30000    # a password costs the server 600 000 rounds of hashing: seconds on a busy Raspberry Pi
        # the administrator creates the account and gets a setup code, shown once
        go("access", "users")
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
        go("access", "users")
        expect(page.locator("#users tbody tr", has_text="anna")).to_contain_text("active")
        expect(page.locator("#users tbody tr", has_text="anna").locator("select")).to_be_disabled()   # nobody changes the own role
        shot(path="/out/ui-accounts.png", full_page=True)
        go("server", "audit")
        audit = page.locator("#audit tbody")
        expect(audit).to_contain_text("Account set up")
        expect(audit).to_contain_text("User added")
        expect(audit.locator("tr.failed").first).to_contain_text("refused")
        shot(path="/out/ui-audit.png", full_page=True)

        # the session is a cookie: it survives a reload, and changing things works with it
        page.reload()
        expect(page.locator("#app")).to_be_visible()
        expect(page.locator("#whoami")).to_have_text("anna")
        expect(page.locator("#s-audit")).to_be_visible()   # a reload stays where one was
        go("fleet")
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
        go("server", "audit")
        expect(page.locator("#audit tbody tr").first).to_contain_text("Sign-in")
        # she cannot lock herself out
        go("access", "users")
        page.locator("#users tbody tr", has_text="anna").get_by_text("Delete").click()
        yes("Delete the account")
        expect(page.locator("#toast")).to_contain_text("own account")
        errors.clear()

        # a second account that is no administrator: it sees a device only through a group
        page.click("#addUser")
        page.fill("#userName", "bert")
        page.select_option("#userRole", "member")
        page.click("#userForm button[type=submit]")
        expect(page.locator("#codeDlg")).to_be_visible()
        setup2 = page.locator("#codeText").inner_text()
        expect(page.locator("#userPassHint")).to_contain_text("No password is set here")
        # "Set it up now": signed out, and the form is open with name and code in it
        page.click("#codeUse")
        expect(page.locator("#login")).to_be_visible()
        expect(page.locator("#setupForm")).to_be_visible()
        assert page.input_value("#setupName") == "bert" and page.input_value("#setupCode") == setup2
        page.fill("#setupPass", "noch ein langes Passwort")
        page.fill("#setupPass2", "noch ein langes Passwort")
        page.click("#setupForm button")
        expect(page.locator("#totpForm")).to_be_visible(timeout=SLOW)
        secret2 = page.locator("#totpSecret").inner_text()
        page.fill("#totpCode", totp(secret2))
        page.click("#totpForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        expect(page.locator("#whoami")).to_have_text("bert")
        expect(page.locator("#noDevices")).to_be_visible()   # in no group yet: nothing to see
        for gone in ("#nav a[data-view=access]", "#nav a[data-view=server]", "#invite"):
            expect(page.locator(gone)).to_be_hidden()
        page.goto(base + "/#server/audit")               # typing the address does not help
        expect(page.locator("#v-fleet")).to_be_visible()
        page.click("#logout")
        # the administrator puts him into the group, to use its devices
        page.fill("#loginName", "anna")
        page.fill("#loginPass", "ein langes Passwort 1")
        page.wait_for_timeout(31000)                     # her last code was the next one already: no code works twice, so a new one has to come up
        page.fill("#loginCode", totp(secret, 1))
        page.click("#loginForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        go("access")
        page.locator("#groups .grpcard", has_text="Werk Nord").get_by_role("button", name="Edit").click()
        page.locator("#grpMembers .line", has_text="bert").locator("select").select_option("use")
        page.click("#grpForm button[type=submit]")
        expect(page.locator("#groups .grpcard", has_text="Werk Nord")).to_contain_text("bert (use)")
        go("access", "users")
        expect(page.locator("#users tbody tr", has_text="bert")).to_contain_text("Werk Nord: use")
        shot(path="/out/ui-access.png", full_page=True)
        page.click("#logout")
        page.fill("#loginName", "bert")
        page.fill("#loginPass", "noch ein langes Passwort")
        page.fill("#loginCode", totp(secret2, 1))
        page.click("#loginForm button")
        expect(page.locator("#app")).to_be_visible(timeout=SLOW)
        expect(page.locator("#cards .dev")).to_have_count(1)
        pick("Labor")
        expect(page.locator("#dTabs a[data-tab=settings]")).to_be_hidden()   # to use, not to manage
        tab("content")
        expect(page.locator("#cfgs tbody tr").first).to_be_visible()
        expect(page.locator("#cfgs .btn")).to_have_count(0)
        expect(page.locator("#tplSendForm")).to_be_hidden()
        tab("console")
        expect(page.locator("#conForm")).to_be_visible()
        go("library")
        expect(page.locator("#tplNew")).to_be_hidden()
        shot(path="/out/ui-member.png")
        page.click("#logout")

    assert not errors, errors
    browser.close()
print("ok " + stage)
