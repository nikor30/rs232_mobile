import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mp
from matplotlib.lines import Line2D

RED, GRN, BLK, MUT, ORA = "#d64545", "#2f7a3d", "#333333", "#6b7280", "#b45309"
fig, ax = plt.subplots(figsize=(11.5, 7.4), dpi=200)
ax.set_xlim(0, 11.5); ax.set_ylim(0, 7.4); ax.axis("off")

ax.text(5.75, 7.15, "Die fehlende Kreuzung: HW-044  ↔  blaues Cisco-Rollover-Kabel",
        ha="center", va="top", fontsize=15.5, fontweight="bold", color="#111")
ax.text(5.75, 6.75, "Beide Seiten sind DB9-Buchsen – Draufsicht auf die Steckseite",
        ha="center", va="top", fontsize=10, color=MUT, style="italic")

def db9(cx, cy, title, sub, roles):
    """Buchse von vorn: obere Reihe 5..1 (Pin 1 rechts!), untere Reihe 9..6."""
    w, h = 3.05, 1.5
    ax.add_patch(mp.FancyBboxPatch((cx - w/2, cy - h/2), w, h,
                 boxstyle="round,pad=0.02,rounding_size=0.12",
                 linewidth=2, edgecolor="#5b6470", facecolor="#eef1f5"))
    ax.text(cx, cy + h/2 + 0.42, title, ha="center", va="bottom", fontsize=11.5, fontweight="bold", color="#111")
    ax.text(cx, cy + h/2 + 0.12, sub, ha="center", va="bottom", fontsize=8.8, color=MUT)
    pos = {}
    top = [5, 4, 3, 2, 1]
    for i, pin in enumerate(top):
        x = cx - 1.16 + i * 0.58
        y = cy + 0.31
        c = roles.get(pin, (None, "#9aa3ad"))[1]
        ax.add_patch(plt.Circle((x, y), 0.135, facecolor=c, edgecolor="#333", linewidth=1.1, zorder=4))
        ax.text(x, y + 0.29, str(pin), ha="center", va="center", fontsize=8.5,
                color="#111", fontweight="bold" if pin in roles else "normal", zorder=5)
        if pin in roles:
            ax.text(x, y - 0.33, roles[pin][0], ha="center", va="center", fontsize=8.4,
                    color=c, fontweight="bold", zorder=7,
                    bbox=dict(boxstyle="round,pad=0.14", facecolor="#eef1f5", edgecolor="none"))
        pos[pin] = (x, y)
    for i, pin in enumerate([9, 8, 7, 6]):
        x = cx - 0.87 + i * 0.58
        y = cy - 0.34
        ax.add_patch(plt.Circle((x, y), 0.135, facecolor="#9aa3ad", edgecolor="#333", linewidth=1.1, zorder=4))
        ax.text(x, y - 0.30, str(pin), ha="center", va="center", fontsize=8.5, color="#111", zorder=5)
        pos[pin] = (x, y)
    return pos

left = db9(2.55, 4.25, "HW-044 (Pegelwandler)", "DB9-Buchse am Modul",
           {2: ("TX", RED), 3: ("RX", GRN), 5: ("GND", BLK)})
right = db9(8.95, 4.25, "blaues Cisco-Kabel", "DB9-Buchse Richtung Switch",
            {2: ("RXD", GRN), 3: ("TXD", RED), 5: ("GND", BLK)})


# drei Jumper: 2->3 (gekreuzt), 3->2 (gekreuzt), 5->5 (gerade)
def wire(a, b, color, dip, label):
    x1, y1 = left[a]; x2, y2 = right[b]
    ax.plot([x1, x1, x2, x2], [y1, dip, dip, y2], color=color, lw=2.6,
            solid_capstyle="round", zorder=3)
    ax.text((x1 + x2) / 2, dip + 0.1, label, ha="center", va="bottom",
            fontsize=9.5, color=color, fontweight="bold")

wire(2, 3, RED, 2.62, "Pin 2  →  Pin 3     (TX auf TXD)")
wire(3, 2, GRN, 2.12, "Pin 3  →  Pin 2     (RX auf RXD)")
wire(5, 5, BLK, 1.62, "Pin 5  →  Pin 5     (GND)")

for p in (2, 3, 5):
    for d in (left, right):
        ax.add_patch(plt.Circle(d[p], 0.135, facecolor="none", edgecolor="#111", linewidth=1.8, zorder=6))

# Merksatz Pin-Nummerierung
ax.text(5.75, 6.16, "Achtung: Bei einer Buchse von vorn liegt Pin 1 OBEN RECHTS – spiegelverkehrt zum Stecker.",
        ha="center", va="center", fontsize=9.3, color=ORA, fontweight="bold")

# Warum-Kasten
ax.add_patch(mp.FancyBboxPatch((0.55, 0.28), 10.4, 1.0,
             boxstyle="round,pad=0.06,rounding_size=0.08",
             linewidth=1.2, edgecolor="#d7dbe0", facecolor="#fafbfc"))
ax.text(0.85, 1.06, "Warum der straighte Gender-Changer nicht geht:", fontsize=9.6,
        fontweight="bold", color="#111", va="center")
ax.text(0.85, 0.76, "Er legt 2 auf 2 und 3 auf 3. Damit trifft der Sendeausgang des Moduls den Sendeausgang des Kabels – "
                    "und beide Empfänger hängen in der Luft.", fontsize=9, color="#333", va="center")
ax.text(0.85, 0.48, "Gegenprobe mit dem Multimeter: schwarz an Pin 5, der Pin mit ca. −5 V ist der Sendeausgang (RS232 ruht negativ).",
        fontsize=9, color="#333", va="center")

ax.legend(handles=[Line2D([0],[0],color=RED,lw=2.6,label="Senderichtung Modul → Switch"),
                   Line2D([0],[0],color=GRN,lw=2.6,label="Senderichtung Switch → Modul"),
                   Line2D([0],[0],color=BLK,lw=2.6,label="Masse")],
          loc="lower center", bbox_to_anchor=(0.5, -0.03), ncol=3, frameon=False, fontsize=9.3)

plt.tight_layout()
plt.savefig("/tmp/claude-0/-home-claude/830cb3ec-f4fd-513a-bde5-dc117f387d2b/scratchpad/db9/nullmodem.png",
            dpi=200, facecolor="white", bbox_inches="tight")
print("done")
