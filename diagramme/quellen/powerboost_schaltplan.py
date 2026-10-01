import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mp

W    = "#1a1a1a"
BOX  = "#f4f6f9"
EDGE = "#33475b"
RED, BLK, GRN, ORA, VIO, BLU = "#c0392b", "#222222", "#1e7a3a", "#b45309", "#6b46c1", "#1f6feb"

fig, ax = plt.subplots(figsize=(15, 10), dpi=170)
ax.set_xlim(0, 15); ax.set_ylim(0, 10); ax.axis("off")

ax.text(7.5, 9.75, "Ladebooster am ESP32-DevKit-Mockup", ha="center", va="top",
        fontsize=17.5, fontweight="bold", color="#111")
ax.text(7.5, 9.38, "PowerBoost 1000C · LiPo 1S · ESP32 DevKit · MAX3232-Modul HW-044",
        ha="center", va="top", fontsize=10.5, color="#667", style="italic")

def box(x, y, w, h, title, sub=""):
    ax.add_patch(mp.FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.08",
                                   linewidth=1.8, edgecolor=EDGE, facecolor=BOX, zorder=2))
    ax.text(x + w/2, y + h - 0.28, title, ha="center", va="top", fontsize=12,
            fontweight="bold", color="#14365e", zorder=3)
    if sub:
        ax.text(x + w/2, y + h - 0.62, sub, ha="center", va="top", fontsize=8.6, color="#667", zorder=3)

# Pin: Punkt auf der Kante, Name IM Symbol -> Leitung nach aussen kreuzt nie den Text
def pin(x, y, name, inward, col=W, fs=9.2):
    ax.add_patch(plt.Circle((x, y), 0.062, facecolor=col, edgecolor="#111", linewidth=0.9, zorder=6))
    if inward == "right":
        ax.text(x + 0.17, y, name, ha="left", va="center", fontsize=fs, fontweight="bold", color=col, zorder=6)
    elif inward == "left":
        ax.text(x - 0.17, y, name, ha="right", va="center", fontsize=fs, fontweight="bold", color=col, zorder=6)
    elif inward == "up":
        ax.text(x, y + 0.16, name, ha="center", va="bottom", fontsize=fs, fontweight="bold", color=col, zorder=6)
    else:
        ax.text(x, y - 0.16, name, ha="center", va="top", fontsize=fs, fontweight="bold", color=col, zorder=6)

def wire(pts, col=W, lw=1.9):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=col, lw=lw,
            solid_capstyle="round", zorder=4)

def gnd(x, y, col=BLK):
    wire([(x, y), (x, y - 0.20)], col)
    for w_, dy in [(0.19, 0.20), (0.12, 0.28), (0.055, 0.36)]:
        ax.plot([x - w_, x + w_], [y - dy, y - dy], color=col, lw=1.8, zorder=4)

def junction(x, y, col):
    ax.add_patch(plt.Circle((x, y), 0.07, facecolor=col, edgecolor=col, zorder=7))

# ---------------- BT1 ----------------
bx, byc = 1.05, 6.75
for ln, off, lw in [(0.34, 0.26, 2.4), (0.17, 0.11, 3.2), (0.34, -0.04, 2.4), (0.17, -0.19, 3.2)]:
    ax.plot([bx - ln, bx + ln], [byc + off, byc + off], color=W, lw=lw, zorder=4)
wire([(bx, byc + 0.26), (bx, 7.55)])
wire([(bx, byc - 0.19), (bx, 6.05)])
ax.text(bx - 0.52, byc + 0.26, "+", ha="right", va="center", fontsize=13, fontweight="bold", color=RED)
ax.text(bx - 0.52, byc - 0.19, "−", ha="right", va="center", fontsize=13, fontweight="bold", color=BLK)
ax.text(bx, byc - 1.15, "BT1   LiPo 1S", ha="center", fontsize=9.6, fontweight="bold", color="#14365e")
ax.text(bx, byc - 1.42, "3,7 V / 1000 mAh", ha="center", fontsize=8.6, color="#667")

# ---------------- PowerBoost ----------------
PX, PY, PW, PH = 2.9, 4.75, 3.1, 2.9
box(PX, PY, PW, PH, "PowerBoost 1000C", "TPS61090 + MCP73871")
p_batp = (PX, 7.10); pin(*p_batp, "BAT +", "right", RED, 8.8)
p_batm = (PX, 6.60); pin(*p_batm, "BAT −", "right", BLK, 8.8)
p_usb  = (PX + 1.55, PY); pin(*p_usb, "USB", "up", ORA, 8.8)
PXR = PX + PW
p_5vo  = (PXR, 7.20); pin(*p_5vo,  "5Vo",  "left", RED, 8.8)
p_gnd  = (PXR, 6.75); pin(*p_gnd,  "GND",  "left", BLK, 8.8)
p_lbo  = (PXR, 6.30); pin(*p_lbo,  "LBO",  "left", GRN, 8.8)
p_en   = (PXR, 5.85); pin(*p_en,   "EN",   "left", ORA, 8.8)
p_lipo = (PXR, 5.40); pin(*p_lipo, "LiPo", "left", VIO, 8.8)
ax.text(PX + 1.55, PY - 0.42, "Micro-USB\nzum Laden", ha="center", va="top", fontsize=8.4, color="#667")

wire([(bx, 7.55), (2.35, 7.55), (2.35, 7.10), p_batp], RED)
wire([(bx, 6.05), (2.55, 6.05), (2.55, 6.60), p_batm], BLK)

# ---------------- DevKit ----------------
DX, DY, DW, DH = 10.1, 4.35, 3.3, 3.3
box(DX, DY, DW, DH, "ESP32 DevKit", "WROOM-32, 30 Pin")
d_vin = (DX, 7.20); pin(*d_vin, "VIN", "right", RED, 8.8)
d_gnd = (DX, 6.75); pin(*d_gnd, "GND", "right", BLK, 8.8)
d_23  = (DX, 6.30); pin(*d_23,  "D23", "right", GRN, 8.8)
d_35  = (DX, 5.85); pin(*d_35,  "D35", "right", VIO, 8.8)
d_3v3 = (DX + 0.65, DY); pin(*d_3v3, "3V3", "up", RED, 8.6)
d_g2  = (DX + 1.35, DY); pin(*d_g2,  "GND", "up", BLK, 8.6)
d_17  = (DX + 2.05, DY); pin(*d_17,  "D17", "up", BLU, 8.6)
d_16  = (DX + 2.75, DY); pin(*d_16,  "D16", "up", BLU, 8.6)

# ---------------- D1 ----------------
dxp = 8.05
wire([p_5vo, (dxp - 0.30, 7.20)], RED)
ax.add_patch(plt.Polygon([[dxp - 0.30, 7.38], [dxp - 0.30, 7.02], [dxp + 0.10, 7.20]],
                         closed=True, facecolor="white", edgecolor=W, linewidth=1.8, zorder=5))
ax.plot([dxp + 0.10, dxp + 0.10], [7.38, 7.02], color=W, lw=2.6, zorder=5)
wire([(dxp + 0.10, 7.20), d_vin], RED)
ax.text(dxp - 0.10, 7.92, "D1   1N5817", ha="center", fontsize=9.4, fontweight="bold", color="#14365e")
ax.text(dxp - 0.10, 7.66, "Ring (Kathode) zum DevKit", ha="center", fontsize=8.4, color="#667")

wire([p_gnd, d_gnd], BLK)
wire([p_lbo, d_23], GRN)
ax.text(8.05, 6.40, "Akku leer < 3,2 V", ha="center", va="bottom", fontsize=8.6, color=GRN)

# ---------------- S1 ----------------
wire([p_en, (6.75, 5.85)], ORA)
ax.add_patch(plt.Circle((6.75, 5.85), 0.055, facecolor=ORA, edgecolor=ORA, zorder=6))
ax.plot([6.75, 7.18], [5.85, 6.14], color=ORA, lw=2.2, zorder=5)
ax.add_patch(plt.Circle((7.28, 5.85), 0.055, facecolor=ORA, edgecolor=ORA, zorder=6))
wire([(7.28, 5.85), (7.28, 5.52)], ORA)
gnd(7.28, 5.52, ORA)
ax.text(7.02, 4.92, "S1  Ein/Aus", ha="center", fontsize=9.4, fontweight="bold", color=ORA)
ax.text(7.02, 4.68, "offen = ein", ha="center", fontsize=8.4, color="#667")

# ---------------- Teiler ----------------
tx = 8.85
wire([p_lipo, (tx, 5.40), (tx, 4.62)], VIO)
ax.add_patch(mp.Rectangle((tx - 0.17, 4.15), 0.34, 0.47, linewidth=1.7, edgecolor=VIO,
                          facecolor="white", zorder=5))
ax.text(tx - 0.30, 4.38, "R1\n100 k", ha="right", va="center", fontsize=8.6, color=VIO)
wire([(tx, 4.15), (tx, 3.72)], VIO)
junction(tx, 3.72, VIO)
ax.add_patch(mp.Rectangle((tx - 0.17, 3.25), 0.34, 0.47, linewidth=1.7, edgecolor=VIO,
                          facecolor="white", zorder=5))
ax.text(tx - 0.30, 3.48, "R2\n100 k", ha="right", va="center", fontsize=8.6, color=VIO)
wire([(tx, 3.25), (tx, 2.95)], VIO)
gnd(tx, 2.95, VIO)
wire([(tx, 3.72), (9.65, 3.72), (9.65, 5.85), d_35], VIO)

# ---------------- MAX3232 ----------------
MX, MY, MW, MH = 10.1, 1.55, 3.3, 1.45
box(MX, MY, MW, MH, "MAX3232-Modul HW-044", "DB9-Buchse zum Gerät")
m_vcc = (MX + 0.65, MY + MH); pin(*m_vcc, "VCC", "down", RED, 8.6)
m_gnd = (MX + 1.35, MY + MH); pin(*m_gnd, "GND", "down", BLK, 8.6)
m_txd = (MX + 2.05, MY + MH); pin(*m_txd, "TXD", "down", BLU, 8.6)
m_rxd = (MX + 2.75, MY + MH); pin(*m_rxd, "RXD", "down", BLU, 8.6)
for a, b, col in [(d_3v3, m_vcc, RED), (d_g2, m_gnd, BLK), (d_17, m_txd, BLU), (d_16, m_rxd, BLU)]:
    wire([a, b], col)

# ---------------- Hinweise ----------------
ax.add_patch(mp.FancyBboxPatch((0.5, 0.45), 7.6, 2.55, boxstyle="round,pad=0.06,rounding_size=0.08",
                               linewidth=1.3, edgecolor="#d7dbe0", facecolor="#fbfbfd"))
yy = 2.72
for head, txt in [
    ("D1 macht den Aufbau USB-sicher.", "Der Schottky sperrt die Rückspeisung. Damit dürfen USB-Buchse"),
    ("", "des DevKits und 5Vo gleichzeitig aktiv sein – flashen mit Akku dran."),
    ("5Vo auf VIN, nie auf 3V3.", "Der AMS1117 im DevKit macht aus 4,9 V die 3,3 V."),
    ("LBO ist offener Kollektor.", "Interner Pull-up von D23 genügt, kein Widerstand nötig."),
    ("R1/R2 sind optional.", "Nur für die Prozentanzeige; ohne sie warnt das Gerät über LBO."),
    ("Akku nur 1 Zelle.", "7,4-V-Packs zerstören den Laderegler."),
]:
    if head:
        ax.text(0.75, yy, head, fontsize=9.3, fontweight="bold", color="#14365e", va="top")
    ax.text(3.35, yy, txt, fontsize=9.1, color="#333", va="top")
    yy -= 0.38

ax.text(14.55, 0.55, "Web-UI: Menü → Ports\nWarnleitung (LBO):  D23\nAkku-Messung:  D35, LiPo 1 Zelle, 2:1",
        ha="right", va="bottom", fontsize=9, color="#14365e",
        bbox=dict(boxstyle="round,pad=0.55", facecolor="#eef7ee", edgecolor="#bcd9bc"))

plt.tight_layout()
plt.savefig("/tmp/claude-0/-home-claude/830cb3ec-f4fd-513a-bde5-dc117f387d2b/scratchpad/sch/schaltplan.png",
            dpi=170, facecolor="white", bbox_inches="tight")
print("done")
